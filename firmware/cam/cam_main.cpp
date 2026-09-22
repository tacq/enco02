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
//                  V [question]         capture and describe
//                  K <url> <token> <mac>  use this vision endpoint
//                  P                    ping (answered with R)
//
// K is how this board gets a vision service without an API key of its own. The
// xiaozhi server hands the main board a url and a token when it connects; the
// main board passes them here, along with its own MAC, because the token is
// issued against that MAC and the server checks the two agree.
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

// 240, not 96. A K command is "K " + url + token + a 17 char MAC; the main
// board sizes its side at 240 from the worst case its buffers allow (227
// chars), and if these two disagree the longer line is silently dropped here
// as a framing error. The real message is ~95 bytes. V can also now carry a
// question, which the assistant writes and may be a full sentence of UTF-8.
//
// Stays under 255 because PumpStream's `cap` is a uint8_t.
char g_line[240];
uint8_t g_line_len = 0;
char g_dbg_line[240];
uint8_t g_dbg_line_len = 0;

// Drop a trailing incomplete UTF-8 sequence, in place.
//
// snprintf truncates at a byte boundary. Chinese is three bytes per character
// here, so a cut lands mid-character two times in three and the tail becomes a
// replacement glyph on the robot's screen and gibberish to the TTS.
void TruncateUtf8(char* s) {
  size_t len = strlen(s);
  size_t i = len;
  // Walk back over continuation bytes (10xxxxxx) to the lead byte.
  while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
    --i;
  }
  if (i == 0) {
    return;
  }
  const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
  size_t need = 1;
  if ((lead & 0xE0) == 0xC0) {
    need = 2;
  } else if ((lead & 0xF0) == 0xE0) {
    need = 3;
  } else if ((lead & 0xF8) == 0xF0) {
    need = 4;
  }
  // Only cut if the sequence really is short of its full width - otherwise the
  // string already ends on a clean boundary.
  if ((i - 1) + need > len) {
    s[i - 1] = '\0';
  }
}

// Everything sent on the link is echoed to the USB console with a "->" prefix,
// and anything typed into the USB console is accepted as if it had arrived on
// the link. UART0 (USB) and UART1 (the link) are separate peripherals, so this
// works even while the main board is attached - you can sit on the cam's log
// and watch the conversation. It is also how you bring the board up before any
// wiring exists.
void Send(const char* s) {
  Serial1.print(s);
  Serial1.print('\n');
  Serial.print("-> ");
  Serial.println(s);
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

void HandleCommand(char* line) {
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
    case 'S': {
      // Bring-up aid: everything you want to know before blaming the wiring.
      Serial.printf("[cam] psram:%s heap:%u psram_free:%u wifi:%s ip:%s tracking:%d vision:%s\n", psramFound() ? "yes" : "NO",
                    static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getFreePsram()),
                    WiFi.status() == WL_CONNECTED ? "up" : "DOWN", WiFi.localIP().toString().c_str(),
                    static_cast<int>(g_tracking), cam_vision::HasEndpoint() ? "server" : "config-key");
      break;
    }
    case 'K': {
      // "K <url> <token> <device-id>". Split in place: the caller's buffer is
      // ours until the next line arrives, and this avoids three String copies
      // on a path that runs while a camera frame buffer is live.
      char* p = line + 1;
      auto next = [&p]() -> const char* {
        while (*p == ' ') {
          ++p;
        }
        if (*p == '\0') {
          return "";
        }
        const char* start = p;
        while (*p != '\0' && *p != ' ') {
          ++p;
        }
        if (*p == ' ') {
          *p++ = '\0';
        }
        return start;
      };
      const char* url = next();
      const char* token = next();
      const char* device_id = next();
      cam_vision::SetEndpoint(url, token, device_id);
      break;
    }
    case 'V': {
      // Blocks for a few seconds. Deliberate: the main board's MCP call is
      // asynchronous and is not waiting on this task.
      const bool was_tracking = g_tracking;
      g_tracking = false;

      // Everything after "V " is the question, which may contain spaces.
      const char* question = (line[1] == ' ') ? line + 2 : "";

      // Ask for a short answer, in the one phrasing that actually works.
      //
      // Measured against this endpoint, same scene, three questions:
      //
      //   "这是什么"                                     -> 432 bytes
      //   "一句话，最多20个字：画面主体是什么"           -> 212 bytes
      //   "只说画面主体是什么，20字以内，不要描述细节"   ->  87 bytes
      //
      // A word limit on its own is ignored - the model happily writes a
      // paragraph and then stops mid-word when we truncate it. What works is
      // the negative instruction: tell it not to describe details. Hence the
      // suffix below rather than the obvious "不超过30个字".
      char q[224];
      if (*question != '\0') {
        snprintf(q, sizeof(q), "%s（20字以内，直接回答，不要描述细节）", question);
      } else {
        q[0] = '\0';
      }

      String text;
      const bool ok = cam_vision::Describe(q, &text);

      // Describe() reconfigured the sensor for JPEG; put it back.
      InitTrackingCamera();
      g_tracking = was_tracking;

      // Newline terminates a message, so a stray one would split the reply.
      text.replace('\n', ' ');
      text.replace('\r', ' ');

      // 320 bytes, matching the main board's receive buffer exactly. At 3 bytes
      // per Chinese character that is ~105 characters, comfortably more than
      // the 30 we asked for. If the two sizes ever diverge, the main board
      // discards the whole line as a framing error and the assistant hears
      // nothing at all - which is how this size was found.
      char out[320];
      const int n = snprintf(out, sizeof(out), "%c %s", ok ? 'L' : 'E', text.c_str());
      if (n >= static_cast<int>(sizeof(out))) {
        TruncateUtf8(out);
      }
      Send(out);
      break;
    }
    default:
      break;
  }
}

// One reader, two sources. `buf`/`len` are per-source so a half-typed console
// command cannot interleave with a link message and corrupt both.
void PumpStream(Stream& in, char* buf, uint8_t& len, uint8_t cap) {
  while (in.available() > 0) {
    const int c = in.read();
    if (c < 0) {
      return;
    }
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buf[len] = '\0';
        HandleCommand(buf);
        len = 0;
      }
      continue;
    }
    if (len < cap - 1) {
      buf[len++] = static_cast<char>(c);
    } else {
      len = 0;  // overlong: drop it rather than act on a fragment
    }
  }
}

void PollLink() {
  PumpStream(Serial1, g_line, g_line_len, sizeof(g_line));
}

// The USB console accepts the same commands as the link. This is the whole
// bring-up story for this board: flash it, open a serial monitor, type V, and
// you know whether the camera and the vision API work before a single wire has
// been soldered to the main board.
void PollDebugConsole() {
  PumpStream(Serial, g_dbg_line, g_dbg_line_len, sizeof(g_dbg_line));
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
  Serial.println("[cam] console ready - type a command and press enter:");
  Serial.println("        S = status   A 1 / A 0 = tracking on/off");
  Serial.println("        V [question] = capture and describe   P = ping");
  Serial.println("        K <url> <token> <mac> = point vision at a server");
  if (!cam_vision::HasEndpoint()) {
    // Worth saying plainly. Without either of these the V command returns a
    // 401 and it looks like the camera is broken when it is not.
    Serial.println("[cam] vision route: compiled-in key from cam_config.h.");
    Serial.println("      The main board will send K automatically once it");
    Serial.println("      connects and the server advertises a vision url.");
  }
}

void loop() {
  PollLink();
  PollDebugConsole();
  Track();
  delay(2);
}
