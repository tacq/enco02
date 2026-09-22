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
  const int vflip = s->status.vflip;
  const int hmirror = s->status.hmirror;

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
  if (sensor_t* js = esp_camera_sensor_get()) {
    js->set_vflip(js, vflip);
    js->set_hmirror(js, hmirror);
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

// Pull a named string field out of a JSON response without dragging in a JSON
// library. We are looking for exactly one field and the shape is fixed, so a
// scanner is smaller and allocates nothing beyond the result.
//
// Handles \\uXXXX because the API escapes CJK depending on the gateway, and a
// mojibake answer is worse than no answer.
//
// `key` is given with its quotes, e.g. "\"result\"".
bool ExtractString(const String& body, const char* key, String* out) {
  const int key_len = static_cast<int>(strlen(key));
  int i = body.indexOf(key);
  if (i < 0) {
    return false;
  }
  i = body.indexOf('"', i + key_len);  // opening quote of the value
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

// Runtime endpoint handed over by the main board. Fixed buffers: this module
// must not allocate at request time on a board that is also holding a camera
// frame buffer.
char g_url[128] = {0};
char g_token[80] = {0};
char g_device_id[24] = {0};

constexpr char kBoundary[] = "----ESP32_CAMERA_BOUNDARY";

// Runs one POST, choosing TLS or plaintext from the url scheme.
//
// The two client classes cannot share a declaration - NetworkClientSecure has
// to outlive the request - so the request body lives in a lambda that takes the
// base class and each branch supplies its own stack object. No heap, no
// duplicated request code, and no TLS context constructed for a plain http URL.
//
// Returns the HTTP status, or a negative HTTPClient error.
int PostBody(const char* url, const char* content_type, const uint8_t* body, size_t body_len, String* reply) {
  auto run = [&](NetworkClient& client) -> int {
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    if (!http.begin(client, url)) {
      return -1000;
    }
    http.addHeader("Content-Type", content_type);
    if (g_device_id[0] != '\0') {
      // The token is issued against the main board's MAC; the server checks
      // that this header matches it.
      http.addHeader("Device-Id", g_device_id);
    }
    if (g_token[0] != '\0') {
      http.addHeader("Authorization", String("Bearer ") + g_token);
    } else if (g_url[0] == '\0') {
      // Only the compiled-in OpenAI route uses the baked key.
      http.addHeader("Authorization", "Bearer " CAM_VISION_API_KEY);
    }

    const int status = http.POST(const_cast<uint8_t*>(body), body_len);
    if (status == 200) {
      *reply = http.getString();
    }
    http.end();
    return status;
  };

  if (strncmp(url, "https://", 8) == 0) {
    NetworkClientSecure client;
    // Validate against the bundled root store rather than setInsecure(). A
    // bearer token travels in a header on this connection; skipping validation
    // would hand it to anyone able to MITM the Wi-Fi.
    client.setCACertBundle(rootca_crt_bundle_start, static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
    client.setTimeout(kHttpTimeoutMs / 1000);
    return run(client);
  }

  NetworkClient client;
  client.setTimeout(kHttpTimeoutMs / 1000);
  return run(client);
}

// Read whatever the server chose to call the answer.
//
// The xiaozhi endpoint returns {"success":true,"result":"..."}; an
// OpenAI-compatible gateway returns the text under "content". Upstream
// xiaozhi-esp32 does not parse this at all - it hands the raw body to the LLM
// and lets it work the shape out. We cannot: the answer has to fit in one UART
// line and then be spoken aloud.
bool ExtractAnswer(const String& body, String* out) {
  return ExtractString(body, "\"result\"", out) || ExtractString(body, "\"content\"", out) ||
         ExtractString(body, "\"text\"", out);
}

// POST the frame to the xiaozhi vision service as multipart/form-data.
//
// The field names, the boundary and the header set are copied from upstream
// xiaozhi-esp32 (main/boards/common/esp32_camera.cc, Esp32Camera::Explain) so
// the server sees exactly what it sees from a first-party device.
//
// One deliberate difference: upstream sends Transfer-Encoding: chunked because
// it streams JPEG chunks out of an encoder queue as they are produced. We
// already hold the whole frame, so we send a plain Content-Length body, which
// is both simpler and one less thing for a proxy to mishandle.
bool DescribeViaEndpoint(const uint8_t* jpeg, size_t jpeg_len, const char* question, String* out) {
  const size_t q_len = strlen(question);
  const size_t cap = jpeg_len + q_len + 512;

  // PSRAM: ~35KB of JPEG plus the envelope does not fit in this chip's internal
  // DRAM alongside an open socket.
  uint8_t* body = static_cast<uint8_t*>(ps_malloc(cap));
  if (body == nullptr) {
    *out = "out of memory";
    return false;
  }

  size_t n = 0;
  n += snprintf(reinterpret_cast<char*>(body + n), cap - n,
                "--%s\r\n"
                "Content-Disposition: form-data; name=\"question\"\r\n"
                "\r\n"
                "%s\r\n"
                "--%s\r\n"
                "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
                "Content-Type: image/jpeg\r\n"
                "\r\n",
                kBoundary, question, kBoundary);
  if (n >= cap) {
    free(body);
    *out = "body overflow";
    return false;
  }

  memcpy(body + n, jpeg, jpeg_len);
  n += jpeg_len;

  const int tail = snprintf(reinterpret_cast<char*>(body + n), cap - n, "\r\n--%s--\r\n", kBoundary);
  if (tail < 0 || n + static_cast<size_t>(tail) >= cap) {
    free(body);
    *out = "body overflow";
    return false;
  }
  n += static_cast<size_t>(tail);

  char content_type[80];
  snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", kBoundary);

  const uint32_t t0 = millis();
  String payload;
  const int status = PostBody(g_url, content_type, body, n, &payload);
  free(body);

  if (status != 200) {
    Serial.printf("[vision] http %d after %lums\n", status, static_cast<unsigned long>(millis() - t0));
    *out = "api error " + String(status);
    return false;
  }
  Serial.printf("[vision] ok in %lums, %u bytes\n", static_cast<unsigned long>(millis() - t0),
                static_cast<unsigned>(payload.length()));

  if (!ExtractAnswer(payload, out)) {
    // Unknown shape. Show it rather than swallow it - this is the one place a
    // server-side change would otherwise fail silently and look like a camera
    // fault.
    Serial.printf("[vision] unrecognised reply shape: %s\n", payload.c_str());
    *out = "unparseable reply";
    return false;
  }
  out->trim();
  return true;
}

// The original route: base64 the frame into an OpenAI chat-completions request.
// Kept as a fallback for anyone running this cam without a xiaozhi server, and
// it is what CAM_VISION_ENDPOINT / CAM_VISION_API_KEY in cam_config.h drive.
//
// Note this route ignores `question` and always sends CAM_VISION_PROMPT. The
// prompt is concatenated into the JSON as a string literal, so injecting
// arbitrary text here would need escaping that the xiaozhi route gets for free
// from multipart framing. Not worth the risk on a path that is now secondary.
bool DescribeViaOpenAi(const uint8_t* jpeg, size_t jpeg_len, String* out) {
  const size_t b64_cap = ((jpeg_len + 2) / 3) * 4 + 1;
  const size_t body_cap = b64_cap + sizeof(CAM_VISION_PROMPT) + sizeof(CAM_VISION_MODEL) + 256;

  char* body = static_cast<char*>(ps_malloc(body_cap));
  if (body == nullptr) {
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
    *out = "body overflow";
    return false;
  }

  size_t written = 0;
  const int rc = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(body + n), body_cap - static_cast<size_t>(n), &written, jpeg, jpeg_len);
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

  const uint32_t t0 = millis();
  String payload;
  const int status = PostBody(CAM_VISION_ENDPOINT, "application/json", reinterpret_cast<uint8_t*>(body), body_len, &payload);
  free(body);

  if (status != 200) {
    Serial.printf("[vision] http %d after %lums\n", status, static_cast<unsigned long>(millis() - t0));
    *out = "api error " + String(status);
    return false;
  }
  Serial.printf("[vision] ok in %lums\n", static_cast<unsigned long>(millis() - t0));

  if (!ExtractAnswer(payload, out)) {
    *out = "unparseable reply";
    return false;
  }
  out->trim();
  return true;
}

}  // namespace

void SetEndpoint(const char* url, const char* token, const char* device_id) {
  snprintf(g_url, sizeof(g_url), "%s", url != nullptr ? url : "");
  snprintf(g_token, sizeof(g_token), "%s", token != nullptr ? token : "");
  snprintf(g_device_id, sizeof(g_device_id), "%s", device_id != nullptr ? device_id : "");
  Serial.printf("[vision] endpoint set: %s (token %s, device %s)\n", g_url[0] ? g_url : "(none)",
                g_token[0] ? "yes" : "no", g_device_id[0] ? g_device_id : "(none)");
}

bool HasEndpoint() {
  return g_url[0] != '\0';
}

bool Describe(const char* question, String* out) {
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

  const bool ok = HasEndpoint() ? DescribeViaEndpoint(jpeg, jpeg_len, (question != nullptr && *question != '\0') ? question : CAM_VISION_PROMPT, out)
                                : DescribeViaOpenAi(jpeg, jpeg_len, out);
  free(jpeg);
  return ok;
}

}  // namespace cam_vision
