#include "cam_vision.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <mbedtls/base64.h>

#include "cam_config.h"
#include "cam_pins.h"

// The main board is not in this picture at all
// --------------------------------------------
// It would be natural to ship the JPEG to the main board and let it talk to the
// cloud, since it already holds a TLS session. It cannot: with the assistant
// speaking, its largest free heap block is 2,036 bytes and a VGA frame is
// ~30,000. It has no PSRAM. A real log line from that board:
//
//   ALLOC FAILED #1: 2308 bytes ... task 'wifi' | free: 7964, largest: 2036
//
// ...and the firmware is built -fno-exceptions, so a failed allocation aborts.
//
// This board, by contrast, has 4MB of PSRAM. So it does the whole round trip
// itself and hands back a sentence. The largest thing the main board ever holds
// is that sentence.

// The root store the Arduino core embeds. Declared as a pair so its length can
// be computed: setCACertBundle() wants an explicit size.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace cam_vision {
namespace {

// VGA at quality 12 lands around 25-35KB. SVGA recognises a little more detail
// but costs ~2x the upload time on a board whose antenna is a PCB trace, and
// the models do not notice the difference for "what is this".
constexpr framesize_t kCaptureSize = FRAMESIZE_VGA;
constexpr int kCaptureQuality = 12;

constexpr uint32_t kHttpTimeoutMs = 20000;

// Turn the sensor into a JPEG camera, take one shot, and put it back. The
// tracker needs raw RGB565 and this needs compressed JPEG, and the two cannot
// coexist: changing pixformat means tearing the driver down.
//
// Returns a PSRAM buffer the caller must free(), or nullptr.
uint8_t* CaptureJpeg(size_t* out_len) {
  *out_len = 0;

  sensor_t* s = esp_camera_sensor_get();
  if (s == nullptr) {
    return nullptr;
  }

  esp_camera_deinit();

  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer = LEDC_TIMER_0;
  cfg.pin_d0 = Y2_GPIO_NUM;
  cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2 = Y4_GPIO_NUM;
  cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4 = Y6_GPIO_NUM;
  cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6 = Y8_GPIO_NUM;
  cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk = XCLK_GPIO_NUM;
  cfg.pin_pclk = PCLK_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM;
  cfg.pin_href = HREF_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn = PWDN_GPIO_NUM;
  cfg.pin_reset = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size = kCaptureSize;
  cfg.jpeg_quality = kCaptureQuality;
  cfg.fb_count = 1;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&cfg) != ESP_OK) {
    Serial.println("[vision] jpeg reinit failed");
    return nullptr;
  }

  // The first frame after a mode change is exposed for the old mode and comes
  // out black or blown out. Throw a couple away and let AEC settle.
  for (int i = 0; i < 3; ++i) {
    camera_fb_t* warmup = esp_camera_fb_get();
    if (warmup != nullptr) {
      esp_camera_fb_return(warmup);
    }
    delay(60);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    Serial.println("[vision] capture failed");
    return nullptr;
  }

  // Copy out before we tear the driver down again - fb points into the driver's
  // own buffer, which esp_camera_deinit() frees.
  uint8_t* copy = static_cast<uint8_t*>(ps_malloc(fb->len));
  if (copy != nullptr) {
    memcpy(copy, fb->buf, fb->len);
    *out_len = fb->len;
  }
  esp_camera_fb_return(fb);
  return copy;
}

// Pull the assistant message out of an OpenAI-shaped response without dragging
// in a JSON library. We are looking for exactly one field and the shape is
// fixed, so a scanner is smaller and allocates nothing beyond the result.
//
// Handles \\uXXXX because the API escapes CJK depending on the gateway, and a
// mojibake answer is worse than no answer.
bool ExtractContent(const String& body, String* out) {
  int i = body.indexOf("\"content\"");
  if (i < 0) {
    return false;
  }
  i = body.indexOf('"', i + 9);  // opening quote of the value
  if (i < 0) {
    return false;
  }
  ++i;

  out->reserve(128);
  while (i < static_cast<int>(body.length())) {
    const char c = body[i];
    if (c == '"') {
      return out->length() > 0;
    }
    if (c != '\\') {
      *out += c;
      ++i;
      continue;
    }
    if (i + 1 >= static_cast<int>(body.length())) {
      break;
    }
    const char e = body[i + 1];
    i += 2;
    switch (e) {
      case 'n':
      case 'r':
      case 't':
        *out += ' ';
        break;
      case 'u': {
        if (i + 4 > static_cast<int>(body.length())) {
          return out->length() > 0;
        }
        uint32_t cp = 0;
        for (int k = 0; k < 4; ++k) {
          const char h = body[i + k];
          cp <<= 4;
          if (h >= '0' && h <= '9') {
            cp |= static_cast<uint32_t>(h - '0');
          } else if (h >= 'a' && h <= 'f') {
            cp |= static_cast<uint32_t>(h - 'a' + 10);
          } else if (h >= 'A' && h <= 'F') {
            cp |= static_cast<uint32_t>(h - 'A' + 10);
          } else {
            return out->length() > 0;
          }
        }
        i += 4;
        // Surrogate pair. Rare here (CJK is in the BMP) but an emoji would
        // otherwise emit two broken halves.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= static_cast<int>(body.length()) && body[i] == '\\' && body[i + 1] == 'u') {
          uint32_t lo = 0;
          bool ok = true;
          for (int k = 0; k < 4; ++k) {
            const char h = body[i + 2 + k];
            lo <<= 4;
            if (h >= '0' && h <= '9') {
              lo |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              lo |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              lo |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
              ok = false;
              break;
            }
          }
          if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          }
        }
        if (cp < 0x80) {
          *out += static_cast<char>(cp);
        } else if (cp < 0x800) {
          *out += static_cast<char>(0xC0 | (cp >> 6));
          *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
          *out += static_cast<char>(0xE0 | (cp >> 12));
          *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          *out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
          *out += static_cast<char>(0xF0 | (cp >> 18));
          *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
          *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          *out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        break;
      }
      default:
        *out += e;  // covers \" and \\ and \/
        break;
    }
  }
  return out->length() > 0;
}

}  // namespace

bool Describe(String* out) {
  if (WiFi.status() != WL_CONNECTED) {
    *out = "wifi down";
    return false;
  }

  size_t jpeg_len = 0;
  uint8_t* jpeg = CaptureJpeg(&jpeg_len);
  if (jpeg == nullptr || jpeg_len == 0) {
    free(jpeg);
    *out = "capture failed";
    return false;
  }
  Serial.printf("[vision] captured %u bytes\n", static_cast<unsigned>(jpeg_len));

  const size_t b64_cap = ((jpeg_len + 2) / 3) * 4 + 1;
  const size_t body_cap = b64_cap + sizeof(CAM_VISION_PROMPT) + sizeof(CAM_VISION_MODEL) + 256;

  // Both of these live in PSRAM. ~45KB of base64 plus the JSON around it would
  // not fit in this chip's internal DRAM alongside the TLS session.
  char* body = static_cast<char*>(ps_malloc(body_cap));
  if (body == nullptr) {
    free(jpeg);
    *out = "out of memory";
    return false;
  }

  int n = snprintf(body, body_cap,
                   "{\"model\":\"" CAM_VISION_MODEL
                   "\",\"max_tokens\":80,\"messages\":[{\"role\":\"user\",\"content\":["
                   "{\"type\":\"text\",\"text\":\"" CAM_VISION_PROMPT "\"},"
                   "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,");
  if (n < 0 || static_cast<size_t>(n) >= body_cap) {
    free(body);
    free(jpeg);
    *out = "body overflow";
    return false;
  }

  size_t written = 0;
  const int rc = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(body + n), body_cap - static_cast<size_t>(n), &written, jpeg, jpeg_len);
  free(jpeg);
  if (rc != 0) {
    free(body);
    *out = "base64 failed";
    return false;
  }

  const int tail = snprintf(body + n + written, body_cap - static_cast<size_t>(n) - written, "\"}}]}]}");
  if (tail < 0) {
    free(body);
    *out = "body overflow";
    return false;
  }
  const size_t body_len = static_cast<size_t>(n) + written + static_cast<size_t>(tail);

  NetworkClientSecure client;
  // Validate against the bundled root store rather than setInsecure(). The API
  // key travels in a header on this connection; skipping validation would hand
  // it to anyone able to MITM the Wi-Fi.
  client.setCACertBundle(rootca_crt_bundle_start, static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
  client.setTimeout(kHttpTimeoutMs / 1000);

  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  http.setConnectTimeout(kHttpTimeoutMs);
  if (!http.begin(client, CAM_VISION_ENDPOINT)) {
    free(body);
    *out = "http begin failed";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " CAM_VISION_API_KEY);

  const uint32_t t0 = millis();
  const int status = http.POST(reinterpret_cast<uint8_t*>(body), body_len);
  free(body);

  if (status != 200) {
    Serial.printf("[vision] http %d after %lums\n", status, static_cast<unsigned long>(millis() - t0));
    http.end();
    *out = "api error " + String(status);
    return false;
  }

  const String payload = http.getString();
  http.end();
  Serial.printf("[vision] ok in %lums\n", static_cast<unsigned long>(millis() - t0));

  String content;
  if (!ExtractContent(payload, &content)) {
    *out = "unparseable reply";
    return false;
  }
  content.trim();
  *out = content;
  return true;
}

}  // namespace cam_vision
