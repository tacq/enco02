// ENCO-02 vision module - ESP32-CAM (AI-Thinker, OV2640, 4MB PSRAM)
//
// Flashed separately from the main board:
//   pio run -e enco02_cam -t upload
//
// Talks to the main board over a UART on GPIO 13/14. One line per message, so
// the whole link can be read by clipping a USB-serial adapter onto the wire.
//
//   cam  -> main   R                    booted, ready
//                  T <dx> <dy> <conf>   subject offset, -100..100, conf 0..100
//                  L <text>             vision result
//                  E <msg>              something went wrong
//
//   main -> cam    A 1 | A 0            arm / disarm tracking
//                  V                    capture and describe
//                  P                    ping (answered with R)
//
// The cam never commands a servo. It reports where the person is and the main
// board decides what to do about it - only the main board knows the safe angle
// limits, whether a gesture animation is already running, and whether the user
// just asked for the head to be somewhere specific.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>

#include "cam_config.h"
#include "cam_pins.h"
#include "cam_tracker.h"
#include "cam_vision.h"

namespace {

// QVGA, halved by the tracker's 2px stride to an effective 160x120. Bigger buys
// nothing: we are computing a centroid, not reading a licence plate.
constexpr framesize_t kTrackSize = FRAMESIZE_QVGA;
constexpr int kTrackWidth = 320;
constexpr int kTrackHeight = 240;

// 12 samples/sec. The servos are rate-limited to 2 degrees per 60ms on the
// other side, so sending faster would only fill a queue nobody drains.
constexpr uint32_t kTrackIntervalMs = 80;

// Below this the reading is noise and is not worth a line on the wire.
constexpr uint8_t kMinReportConf = 25;

bool g_tracking = false;
uint32_t g_last_track_ms = 0;
int16_t g_last_dx = 0;
int16_t g_last_dy = 0;

char g_line[96];
uint8_t g_line_len = 0;

void Send(const char* s) {
  Serial1.print(s);
  Serial1.print('\n');
}

bool InitTrackingCamera() {
  // Harmless if the driver is not up; required if it is, because Describe()
  // leaves the sensor configured for JPEG.
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
  cfg.pixel_format = PIXFORMAT_RGB565;
  cfg.frame_size = kTrackSize;
  cfg.fb_count = 1;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;
  // LATEST rather than WHEN_EMPTY: a stale frame makes the head chase history.
  cfg.grab_mode = CAMERA_GRAB_LATEST;

  const esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    // Orientation is not cosmetic here: hmirror swaps left and right, and the
    // main board turns the head towards whichever side the subject appears on.
    // Get it wrong and the head runs away from the user.
    s->set_vflip(s, CAM_VFLIP);
    s->set_hmirror(s, CAM_HMIRROR);
    // Auto white balance left on - the skin-tone test depends on colour being
    // roughly right, and a fixed gain indoors is reliably wrong.
  }

  cam_tracker::Reset();
  return true;
}

void HandleCommand(const char* line) {
  switch (line[0]) {
    case 'P': {
      Send("R");
      break;
    }
    case 'A': {
      g_tracking = (line[1] == ' ' && line[2] == '1');
      if (!g_tracking) {
        cam_tracker::Reset();
      }
      Serial.printf("[cam] tracking %s\n", g_tracking ? "on" : "off");
      break;
    }
    case 'V': {
      // Blocks for a few seconds. Deliberate: the main board's MCP call is
      // asynchronous and is not waiting on this task.
      const bool was_tracking = g_tracking;
      g_tracking = false;

      String text;
      const bool ok = cam_vision::Describe(&text);

      // Describe() reconfigured the sensor for JPEG; put it back.
      InitTrackingCamera();
      g_tracking = was_tracking;

      if (ok) {
        // Newline terminates a message, so a stray one would split the reply.
        text.replace('\n', ' ');
        text.replace('\r', ' ');
        Serial1.print("L ");
        Serial1.print(text);
        Serial1.print('\n');
        Serial.printf("[cam] look -> %s\n", text.c_str());
      } else {
        Serial1.print("E ");
        Serial1.print(text);
        Serial1.print('\n');
        Serial.printf("[cam] look failed: %s\n", text.c_str());
      }
      break;
    }
    default:
      break;
  }
}

void PollLink() {
  while (Serial1.available() > 0) {
    const int c = Serial1.read();
    if (c < 0) {
      return;
    }
    if (c == '\n' || c == '\r') {
      if (g_line_len > 0) {
        g_line[g_line_len] = '\0';
        HandleCommand(g_line);
        g_line_len = 0;
      }
      continue;
    }
    if (g_line_len < sizeof(g_line) - 1) {
      g_line[g_line_len++] = static_cast<char>(c);
    } else {
      g_line_len = 0;  // overlong: drop it rather than act on a fragment
    }
  }
}

void Track() {
  if (!g_tracking) {
    return;
  }
  if (millis() - g_last_track_ms < kTrackIntervalMs) {
    return;
  }
  g_last_track_ms = millis();

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    return;
  }
  const TrackSample s = cam_tracker::Analyse(fb->buf, fb->width, fb->height);
  esp_camera_fb_return(fb);

  if (s.conf < kMinReportConf) {
    return;
  }
  // The main board applies its own deadband, but there is no point spending a
  // line on a reading it will certainly discard.
  if (abs(s.dx - g_last_dx) < 3 && abs(s.dy - g_last_dy) < 3) {
    return;
  }
  g_last_dx = s.dx;
  g_last_dy = s.dy;

  char buf[32];
  snprintf(buf, sizeof(buf), "T %d %d %u", s.dx, s.dy, static_cast<unsigned>(s.conf));
  Send(buf);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("\n[cam] boot");

  // UART1. Its default pins sit on the flash bus, so they must be remapped -
  // the GPIO matrix makes that free.
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_GPIO, LINK_TX_GPIO);

  if (!psramFound()) {
    // Without PSRAM neither a QVGA RGB565 frame (150KB) nor a base64 VGA JPEG
    // (~45KB) fits, and this firmware needs both. Say so instead of crashing
    // somewhere less obvious ten seconds later.
    Serial.println("[cam] FATAL: no PSRAM - wrong board variant?");
    Send("E no psram");
  }

  if (!InitTrackingCamera()) {
    Send("E camera init failed");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(CAM_WIFI_SSID, CAM_WIFI_PASSWORD);
  Serial.print("[cam] wifi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[cam] wifi ok: %s\n", WiFi.localIP().toString().c_str());
  } else {
    // Not fatal. Tracking is entirely local; only the vision round trip needs
    // the network, and by then WiFi may well have come up on its own.
    Serial.println("[cam] wifi failed - tracking still works");
  }

  Serial.printf("[cam] free heap %u, free psram %u\n", static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getFreePsram()));
  Send("R");
}

void loop() {
  PollLink();
  Track();
  delay(2);
}
