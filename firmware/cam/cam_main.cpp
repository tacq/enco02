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
#include <hal/uart_ll.h>
#include <img_converters.h>

#include "cam_config.h"
#include "cam_pins.h"
#include "cam_tracker.h"
#include "cam_vision.h"
#include "cam_web.h"

namespace {

// Defined below, next to the tracking loop. SendVideoFrame() has to emit the
// tracking line before it starts a binary payload, so it needs this early.
void ReportSample(const TrackSample& s);

// QVGA, halved by the tracker's 2px stride to an effective 160x120. Bigger buys
// nothing: we are computing a centroid, not reading a licence plate.
constexpr framesize_t kTrackSize = FRAMESIZE_QVGA;
constexpr int kTrackWidth = 320;
constexpr int kTrackHeight = 240;

// Viewfinder mode. HQVGA is 240x176 - and the robot's screen is 240 wide, so
// this is the one sensor size that needs no scaling at either end. That matters
// more than it sounds: the main board's JPEG decoder is built with JD_USE_SCALE
// off, so it can only decode 1:1, and it has nowhere to put a scratch frame to
// resample into. Sending exactly the pixels the panel wants sidesteps both.
constexpr framesize_t kVideoSize = FRAMESIZE_HQVGA;
constexpr int kVideoWidth = 240;
constexpr int kVideoHeight = 176;

// JPEG quality for the viewfinder. 10-63, lower is better and bigger. 14 lands
// around 5-7KB for a typical indoor scene, which is ~65ms on the fast link.
constexpr int kVideoQuality = 14;

// Ceiling on the frame rate. The transfer itself is the real limit; this just
// stops the cam spinning flat out when the scene is simple and the JPEG small.
constexpr uint32_t kVideoIntervalMs = 90;

// 12 samples/sec. The servos are rate-limited to 2 degrees per 60ms on the
// other side, so sending faster would only fill a queue nobody drains.
constexpr uint32_t kTrackIntervalMs = 80;

// While tracking is disarmed the tracker still runs, but at 3 fps: the only
// thing it is looking for is the one-finger gesture that arms it.
constexpr uint32_t kIdleScanIntervalMs = 320;

// Whether a one-finger gesture may arm tracking. OFF, on measured evidence.
//
// The detector does not work, and the debounce below cannot rescue it. Polled
// against a real scene with nobody gesturing, /status reported "one finger" on
// 43 of 90 consecutive frames - 48% - with a longest unbroken run of 7. An
// earlier 67-sample capture minutes before, same room, put it at 9%. A signal
// whose false-positive rate swings from 9% to 48% depending on what happens to
// be in shot is not measuring fingers; it is measuring the scene.
//
// The reason is structural. The test runs on head_row_n[], which is the
// tracker's locked *topmost skin cluster* - the user's head. So it is really
// asking "is the face blob tall and narrow", and a face turned side-on or
// clipped by the frame edge answers yes. Tightening the geometry (bounded
// aspect, bounded height, minimum width) cut the rate but could not separate
// two things being computed from the same pixels.
//
// Raising kGestureFrames past the observed run of 7 would be fitting to one
// 90-sample capture, not fixing the mechanism - and the cost of being wrong is
// precisely the bug this was meant to address: the head starting to move on
// its own. Voice ("开启跟踪") is deterministic and already does the job, so it
// is the only trigger until the detector earns its place back.
//
// Re-enabling needs real connected-component labelling to find a finger blob
// *separate* from the face, not a better threshold on this one.
constexpr bool kGestureArmingEnabled = false;

// Consecutive frames of "one finger" before it counts, and how long to ignore
// the gesture afterwards so a held-up finger arms tracking once. Retained for
// when the detector above is fixed; inert while kGestureArmingEnabled is false.
constexpr uint8_t kGestureFrames = 8;
// ...preceded by this many frames with no gesture at all. See ReportSample():
// this is what stops something permanently in the frame from ever arming
// tracking, which no amount of extra hold time can do on its own.
constexpr uint8_t kGestureClearFrames = 5;
constexpr uint32_t kGestureCooldownMs = 3000;

// Below this the reading is noise and is not worth a line on the wire.
constexpr uint8_t kMinReportConf = 25;

// Tracking starts DISARMED. The head moving on its own the instant the robot
// powers up is startling, and the user asked for it to be something they turn
// on deliberately - by voice ("开启跟踪") or by holding up one finger.
bool g_tracking = false;

// True while Describe() owns the sensor. Track() must not grab a frame then:
// the sensor is in JPEG mode and the RGB565 tracker would read garbage.
bool g_vision_busy = false;

uint8_t g_gesture_frames = 0;
uint8_t g_gesture_clear_frames = 0;
uint32_t g_last_gesture_ms = 0;

// Viewfinder streaming to the robot's screen. Off unless the main board asks.
bool g_video = false;
uint32_t g_last_video_ms = 0;
framesize_t g_sensor_size = kTrackSize;

// Viewfinder diagnostics. When the screen shows nothing there are three very
// different reasons - we never got a frame from the sensor, we got one but
// could not compress it, or we sent bytes that the main board did not like -
// and from the cam's side they look identical unless we count them separately.
// Printed every 2s while video is on; silent otherwise.
uint32_t g_video_sent = 0;
uint16_t g_video_last_len = 0;
uint16_t g_video_last_w = 0;
uint16_t g_video_last_h = 0;
uint32_t g_video_fb_fail = 0;
uint32_t g_video_jpg_fail = 0;
uint32_t g_video_stat_ms = 0;

// Fast link for video. MUST match CamLink::kFastBaud on the main board.
//
// Was 921600, then 460800. Now 230400, because the main board's JPEG decoder
// blits to its SPI panel between reads and drains the UART at roughly half the
// speed 460800 fills it - measured on the device at 2090B/frame, decode 79ms
// against 45ms of wire time. Frames larger than ~2050B overflowed its 1024-byte
// driver buffer and arrived with a good header and broken scan data. 230400
// matches the wire to the decoder and costs no frame rate, because the decoder
// is the bottleneck. See the long note on kFastBaud in the main board's
// cam_link.h.
constexpr uint32_t kLinkFastBaud = 230400;
bool g_link_fast = false;
uint32_t g_last_cmd_ms = 0;

// Link activity, reported by the web /status page. See cam_web::SetLinkStats.
uint32_t g_link_rx_bytes = 0;
uint32_t g_link_cmds = 0;
uint32_t g_link_last_rx_ms = 0;

// Re-announce ourselves if the main board has not said anything for a while.
//
// 'R' used to be sent once, at boot, and nowhere else. That made presence a
// one-shot: if the main board missed it - because it was still booting, or
// being reflashed, or the cable was out at that instant - it would never learn
// we exist, and nothing short of power-cycling this board could fix it. Since
// the main board pings every 3s, silence for 10s means it is not hearing us or
// we are not hearing it; re-announcing every 5s after that costs two bytes and
// makes the link heal itself the moment the fault clears.
constexpr uint32_t kReannounceAfterMs = 10000;
constexpr uint32_t kReannounceEveryMs = 5000;
uint32_t g_last_announce_ms = 0;

// If the main board stops talking while we are at the fast rate, fall back.
// Anything that silences it - a reset, a reflash, a wire pulled out - would
// otherwise leave the two boards at different baud rates and unable to say so.
// The main board pings every 3s, so 7s is quiet enough to be real.
constexpr uint32_t kFastLinkIdleMs = 7000;

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

bool InitTrackingCamera(framesize_t size = kTrackSize) {
  // Preserve any Flip V / Flip H toggled in the web UI across the JPEG mode
  // switch, falling back to the cam_config.h defaults on first boot.
  int vflip = CAM_VFLIP;
  int hmirror = CAM_HMIRROR;
  if (sensor_t* prev = esp_camera_sensor_get()) {
    vflip = prev->status.vflip;
    hmirror = prev->status.hmirror;
  }

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
  cfg.frame_size = size;
  cfg.fb_count = 1;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;
  // LATEST rather than WHEN_EMPTY: a stale frame makes the head chase history.
  cfg.grab_mode = CAMERA_GRAB_LATEST;

  const esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed: 0x%x\n", err);
    return false;
  }
  g_sensor_size = size;

  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    // Orientation is not cosmetic here: hmirror swaps left and right, and the
    // main board turns the head towards whichever side the subject appears on.
    // Get it wrong and the head runs away from the user.
    s->set_vflip(s, vflip);
    s->set_hmirror(s, hmirror);
    // Auto white balance left on - the skin-tone test depends on colour being
    // roughly right, and a fixed gain indoors is reliably wrong.
  }

  cam_tracker::Reset();
  return true;
}

// One place for the JPEG mode switch, the brevity hint, and putting the sensor
// back afterwards. Called by the UART V command and by the browser's /vision
// endpoint so the two cannot drift apart.
bool RunVision(const char* question, String* text) {
  // Recognition mode. Tracking is disarmed for the whole capture and is NOT
  // restored here - the main board owns that state and re-arms with `A 1` once
  // it has delivered the answer.
  //
  // This is the fix for "it cannot recognise anything while it is tracking":
  // the head was still being stepped 3 degrees every 60ms when the shutter
  // fired, so the JPEG was motion-blurred, and worse, the tracker had already
  // pointed the lens at the user's *face* rather than at the object in their
  // hand. Stopping first, then settling, then capturing gets a sharp frame of
  // whatever is actually in front of the robot.
  g_tracking = false;
  g_vision_busy = true;
  g_gesture_frames = 0;
  // Reset the clear run as well, not just the count. The user is holding
  // something up to the camera, which is the one situation guaranteed to put
  // a hand-shaped blob in frame; without this the capture itself could satisfy
  // the gesture and re-arm the tracking we just deliberately switched off.
  g_gesture_clear_frames = 0;

  // Let the servos coast to a stop and the OV2640's auto-exposure and auto
  // white balance re-converge on the new scene. Without this the first frame
  // after a pan is both smeared and badly exposed.
  delay(350);

  // Ask for a short answer, in the one phrasing that actually works.
  //
  // Measured against this endpoint, same scene, three questions:
  //
  //   "这是什么"                                     -> 432 bytes
  //   "一句话，最多20个字：画面主体是什么"           -> 212 bytes
  //   "只说画面主体是什么，20字以内，不要描述细节"   ->  87 bytes
  //
  // A word limit on its own is ignored - the model happily writes a paragraph
  // and then stops mid-word when we truncate it. What works is the negative
  // instruction: tell it not to describe details. Hence the suffix below
  // rather than the obvious "不超过30个字".
  char q[224];
  if (question != nullptr && *question != '\0') {
    snprintf(q, sizeof(q), "%s（20字以内，直接回答，不要描述细节）", question);
  } else {
    q[0] = '\0';
  }

  const bool ok = cam_vision::Describe(q, text);

  // Describe() reconfigured the sensor for JPEG; put it back - at whichever
  // size we were using, which is the viewfinder size if the robot's screen is
  // showing the feed.
  InitTrackingCamera(g_video ? kVideoSize : kTrackSize);
  g_vision_busy = false;
  g_last_track_ms = millis();

  // Newline terminates a message, so a stray one would split the reply.
  text->replace('\n', ' ');
  text->replace('\r', ' ');
  return ok;
}

// CRC16/CCITT-FALSE. Not for security, for noise: the fast link runs at 921600
// over unshielded Dupont jumpers, and a flipped bit in a JPEG is the difference
// between a picture and a screen of coloured hash.
uint16_t Crc16(const uint8_t* p, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n-- > 0) {
    crc ^= static_cast<uint16_t>(*p++) << 8;
    for (int i = 0; i < 8; ++i) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

// One viewfinder frame, as a length-prefixed binary record.
//
// The command protocol is newline-delimited ASCII and a JPEG is full of 0x0A,
// so video cannot travel as a line. Instead it is framed:
//
//   A5 5A | len:u16 | w:u16 | h:u16 | crc:u16 | \<len bytes of JPEG\>
//
// 0xA5 never appears in the ASCII protocol, so the receiver can sit in its
// normal line parser and drop into binary mode the moment it sees the magic.
void SendVideoFrame() {
  if (!g_video || g_vision_busy) {
    return;
  }
  const uint32_t now = millis();
  if (now - g_last_video_ms < kVideoIntervalMs) {
    return;
  }
  g_last_video_ms = now;

  // A browser on /stream and the robot's own screen cannot both have the
  // sensor: there is one framebuffer (fb_count=1) and two consumers of it just
  // starve each other.
  //
  // The screen wins. This used to return here and leave the viewfinder black,
  // which is the wrong way round - the web stream is a bring-up aid, the screen
  // is the product. Dropping the HTTP client is also the only way the user
  // finds out; a silently blank viewfinder looks like the link is broken.
  if (cam_web::IsStreaming()) {
    Serial.println("[cam] viewfinder active - dropping web stream client");
    cam_web::StopStream();
  }

  // Say what is actually going out, on the USB console, every 2s.
  //
  // This sits ABOVE the frame grab on purpose. Every failure below returns
  // early, so a run where the sensor never hands over a frame - the single
  // most likely cause of a black viewfinder - would print nothing at all if
  // this lived at the end. Reporting the previous period's numbers one frame
  // late is a small price for having the counters appear at all.
  if (now - g_video_stat_ms >= 2000) {
    g_video_stat_ms = now;
    Serial.printf("[cam] video: %u sent, last %ux%u %uB, fb_fail %u, jpg_fail %u\n",
                  static_cast<unsigned>(g_video_sent), static_cast<unsigned>(g_video_last_w),
                  static_cast<unsigned>(g_video_last_h), static_cast<unsigned>(g_video_last_len),
                  static_cast<unsigned>(g_video_fb_fail), static_cast<unsigned>(g_video_jpg_fail));
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    ++g_video_fb_fail;
    return;
  }

  // Track off the very frame we are about to send, so the crosshair the main
  // board draws lines up with the picture underneath it rather than with where
  // the subject was two frames ago.
  const TrackSample s = cam_tracker::Analyse(fb->buf, fb->width, fb->height);
  cam_web::SetLastSample(s);

  const uint16_t w = static_cast<uint16_t>(fb->width);
  const uint16_t h = static_cast<uint16_t>(fb->height);
  g_video_last_w = w;
  g_video_last_h = h;

  uint8_t* jpg = nullptr;
  size_t jpg_len = 0;
  const bool ok = frame2jpg(fb, kVideoQuality, &jpg, &jpg_len);
  esp_camera_fb_return(fb);

  // The T line goes out FIRST, while the wire is still in ASCII. Interleaving a
  // text line into the middle of a binary payload would corrupt both.
  ReportSample(s);

  if (!ok || jpg == nullptr) {
    ++g_video_jpg_fail;
    return;
  }
  if (jpg_len > 0 && jpg_len <= 0xFFFF) {
    const uint16_t crc = Crc16(jpg, jpg_len);
    uint8_t hdr[10] = {
        0xA5,
        0x5A,
        static_cast<uint8_t>(jpg_len & 0xFF),
        static_cast<uint8_t>((jpg_len >> 8) & 0xFF),
        static_cast<uint8_t>(w & 0xFF),
        static_cast<uint8_t>((w >> 8) & 0xFF),
        static_cast<uint8_t>(h & 0xFF),
        static_cast<uint8_t>((h >> 8) & 0xFF),
        static_cast<uint8_t>(crc & 0xFF),
        static_cast<uint8_t>((crc >> 8) & 0xFF),
    };
    Serial1.write(hdr, sizeof(hdr));
    Serial1.write(jpg, jpg_len);
    ++g_video_sent;
    g_video_last_len = static_cast<uint16_t>(jpg_len);
  } else {
    ++g_video_jpg_fail;
  }
  free(jpg);
}

// Enter or leave viewfinder mode. The sensor has to be reconfigured either way
// because the tracking size (320x240) and the screen size (240x176) differ.
void SetVideoMode(bool on) {
  if (on == g_video) {
    return;
  }
  g_video = on;
  InitTrackingCamera(on ? kVideoSize : kTrackSize);
  g_last_video_ms = 0;
  // Per-session counters, not lifetime ones. "12 sent, 40 fb_fail" is only
  // readable if it describes this viewfinder session; totals since boot would
  // hide a session that sent nothing behind a healthy earlier one.
  if (on) {
    g_video_sent = 0;
    g_video_fb_fail = 0;
    g_video_jpg_fail = 0;
    g_video_stat_ms = millis();
  }
  Serial.printf("[cam] video %s (%dx%d)\n", on ? "on" : "off", on ? kVideoWidth : kTrackWidth, on ? kVideoHeight : kTrackHeight);
}

// Returns true if the line was a command we understand.
//
// The caller uses that to decide whether to feed the fast-link watchdog. This
// used to stamp g_last_cmd_ms unconditionally right here, which defeated the
// watchdog in the exact situation it exists for: if the two boards end up at
// different baud rates, what arrives is noise, and noise contains 0x0A often
// enough to look like a steady stream of lines. The watchdog would be fed by
// the very garbage that proves the link is broken, and the boards would stay
// desynchronised forever instead of healing after 7s. (It was also fed by
// anything typed on the USB console, which has nothing to do with the link.)
bool HandleCommand(char* line) {
  bool known = true;
  switch (line[0]) {
    case 'P': {
      Send("R");
      break;
    }
    case 'F': {
      // Viewfinder on the robot's screen.
      SetVideoMode(line[1] == ' ' && line[2] == '1');
      break;
    }
    case 'B': {
      // Link speed. The command protocol is perfectly happy at 115200, but one
      // JPEG at that rate takes 0.7s - a slideshow. Video mode asks for 921600,
      // which turns the same frame into 65ms.
      //
      // flush() first: the reply to whatever we were doing must leave the FIFO
      // at the OLD rate, or the main board reads it as noise. And if the main
      // board then goes quiet - crash, reset, a wire falling out - the watchdog
      // in loop() drops us back to 115200 so the link heals itself.
      const bool fast = (line[1] == ' ' && line[2] == '1');
      Serial1.flush();
      delay(5);
      const uint32_t target_baud = fast ? kLinkFastBaud : LINK_BAUD;
      Serial1.updateBaudRate(target_baud);
      uart_ll_set_sclk(UART_LL_GET_HW(1), SOC_MOD_CLK_APB);
      uart_ll_set_baudrate(UART_LL_GET_HW(1), target_baud, 80000000);
      g_link_fast = fast;

      // Acknowledge, at the NEW rate. This is what makes the switch reliable
      // rather than a race.
      //
      // The main board cannot know when this line was read: commands are only
      // polled between frames, and a 6KB JPEG occupies the wire for 65ms at the
      // fast rate and 520ms at the slow one - on top of which `F 0` makes us
      // re-initialise the sensor for a few hundred milliseconds. A fixed delay
      // on the other side was guesswork, and it lost: the main board dropped to
      // 115200 while this board was still transmitting at 921600, and every
      // command after it - including the `V` for a "这是什么" - went into a
      // camera that was not listening at that speed.
      Send("B");
      Serial.printf("[cam] link %u baud\n", static_cast<unsigned>(fast ? kLinkFastBaud : LINK_BAUD));
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
      // Everything after "V " is the question, which may contain spaces.
      const char* question = (line[1] == ' ') ? line + 2 : "";
      String text;
      const bool ok = RunVision(question, &text);

      // 320 bytes, matching the main board's receive buffer exactly. At 3 bytes
      // per Chinese character that is ~105 characters, comfortably more than
      // the 20 we asked for. If the two sizes ever diverge, the main board
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
      known = false;
      break;
  }
  return known;
}

// One reader, two sources. `buf`/`len` are per-source so a half-typed console
// command cannot interleave with a link message and corrupt both.
//
// `from_link` separates the two for the watchdog's benefit: only a command we
// recognised, arriving on the wire from the main board, is evidence that the
// link is healthy.
void PumpStream(Stream& in, char* buf, uint8_t& len, uint8_t cap, bool from_link) {
  while (in.available() > 0) {
    const int c = in.read();
    if (c < 0) {
      return;
    }
    if (from_link) {
      ++g_link_rx_bytes;
      g_link_last_rx_ms = millis();
    }
    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buf[len] = '\0';
        const bool known = HandleCommand(buf);
        if (known && from_link) {
          ++g_link_cmds;
          g_last_cmd_ms = millis();
        }
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
  PumpStream(Serial1, g_line, g_line_len, sizeof(g_line), true);
}

// The USB console accepts the same commands as the link. This is the whole
// bring-up story for this board: flash it, open a serial monitor, type V, and
// you know whether the camera and the vision API work before a single wire has
// been soldered to the main board.
void PollDebugConsole() {
  PumpStream(Serial, g_dbg_line, g_dbg_line_len, sizeof(g_dbg_line), false);
}

// Shared between the tracking loop and the web stream, so a browser watching
// /stream does not silence the servo updates going to the main board.
void ReportSample(const TrackSample& s) {
  // The gesture is evaluated whether or not tracking is armed - that is the
  // whole point of it, since it is how the user arms tracking in the first
  // place.
  //
  // Debounced over kGestureFrames consecutive frames because a single frame of
  // "tall narrow skin blob" is also what you get from a forearm crossing the
  // lens, and a cooldown afterwards so one held-up finger arms tracking once
  // rather than several times a second.
  //
  // The clear-run requirement is the important half. A raised finger is an
  // *event*: it was not there, then it is. Anything permanently in the frame -
  // the skin-toned edge of a bookshelf, a chair back - is a standing condition,
  // and a pure consecutive-frames counter cannot tell the two apart, so it
  // armed tracking the moment the board booted. Demanding kGestureClearFrames
  // of no-gesture before the count is allowed to arm anything makes furniture
  // permanently ineligible: it never produces the clear run, so its counter
  // never becomes valid, no matter how long it sits there.
  const uint32_t now = millis();
  if (s.gesture == 1) {
    if (g_gesture_frames < 255) {
      ++g_gesture_frames;
    }
    if (kGestureArmingEnabled && g_gesture_frames == kGestureFrames && g_gesture_clear_frames >= kGestureClearFrames &&
        (now - g_last_gesture_ms) > kGestureCooldownMs) {
      g_last_gesture_ms = now;
      g_gesture_clear_frames = 0;
      Serial.println("[cam] gesture: one finger -> arm tracking");
      Send("G 1");
      g_tracking = true;
    }
  } else {
    g_gesture_frames = 0;
    if (g_gesture_clear_frames < 255) {
      ++g_gesture_clear_frames;
    }
  }

  if (!g_tracking || s.conf < kMinReportConf) {
    return;
  }
  // Only suppress unchanged readings when the subject is ALREADY inside the
  // main board's deadband (and head roll is inside +/-15%). When outside it the
  // main board eases toward the subject over several updates, so we must keep
  // emitting 'T' every tick until the head arrives - it decelerates using these
  // messages, and starving it mid-move would leave it stopped off-centre.
  //
  // 15 must match kDeadband in the main board's cam_link.cpp. If this number is
  // smaller the head hunts (we keep talking about an offset it has decided to
  // ignore); if it is larger the head stops short and never gets the updates it
  // needs to finish centring.
  const bool inside_deadband = (abs(s.dx) < 15 && abs(s.dy) < 15 && abs(s.roll) < 15);
  if (inside_deadband && abs(s.dx - g_last_dx) < 3 && abs(s.dy - g_last_dy) < 3) {
    return;
  }
  g_last_dx = s.dx;
  g_last_dy = s.dy;

  char buf[40];
  snprintf(buf, sizeof(buf), "T %d %d %u %d", s.dx, s.dy, static_cast<unsigned>(s.conf), s.roll);
  Send(buf);
}

void Track() {
  // Runs even when tracking is disarmed, but four times slower: the frame is
  // still needed to spot the one-finger gesture that arms it. Analyse() costs
  // ~8ms, so the idle duty cycle is ~2.5%.
  //
  // Not while a vision capture is in flight: Describe() reconfigures the sensor
  // for JPEG, and grabbing an RGB565 frame underneath it returns garbage.
  if (g_vision_busy) {
    return;
  }
  // In viewfinder mode SendVideoFrame() already grabs, analyses and reports
  // every frame it transmits. Running this too would just fight it for the
  // single framebuffer.
  if (g_video) {
    return;
  }
  // When a browser is streaming /stream, PumpStreamFrame() in cam_web is
  // already grabbing frames, running Analyse(), and calling ReportSample() at
  // 10 fps. Do not contend for the single framebuffer here.
  if (cam_web::IsStreaming()) {
    return;
  }
  const uint32_t interval = g_tracking ? kTrackIntervalMs : kIdleScanIntervalMs;
  if (millis() - g_last_track_ms < interval) {
    return;
  }
  g_last_track_ms = millis();

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    return;
  }
  const TrackSample s = cam_tracker::Analyse(fb->buf, fb->width, fb->height);
  esp_camera_fb_return(fb);
  cam_web::SetLastSample(s);
  ReportSample(s);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("\n[cam] boot");

  // UART1. Its default pins sit on the flash bus, so they must be remapped -
  // the GPIO matrix makes that free.
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_GPIO, LINK_TX_GPIO);
  uart_ll_set_sclk(UART_LL_GET_HW(1), SOC_MOD_CLK_APB);
  uart_ll_set_baudrate(UART_LL_GET_HW(1), LINK_BAUD, 80000000);

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

  cam_web::SetSampleSink(&ReportSample);
  cam_web::SetVisionHandler(&RunVision);
  cam_web::Begin();

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

  // Make the link's state visible over WiFi, and keep saying hello if the main
  // board has gone quiet. Both are cheap and only matter when something is
  // wrong - which is exactly when there is no serial adapter attached to this
  // board to tell you what is happening.
  cam_web::SetLinkStats(g_link_rx_bytes, g_link_cmds, g_link_last_rx_ms);
  {
    const uint32_t now = millis();
    const uint32_t quiet_for = (g_link_last_rx_ms == 0) ? now : (now - g_link_last_rx_ms);
    if (quiet_for > kReannounceAfterMs && (now - g_last_announce_ms) > kReannounceEveryMs) {
      g_last_announce_ms = now;
      Send("R");
    }
  }

  cam_web::Poll();
  Track();
  SendVideoFrame();

  // Fast-link watchdog. If we switched to 921600 and the main board then went
  // quiet, we are almost certainly the only one still there - it reset, or was
  // reflashed. Drop back to the rate it will be listening at, otherwise the two
  // boards sit at different speeds and neither can tell the other.
  if (g_link_fast && g_last_cmd_ms != 0 && (millis() - g_last_cmd_ms) > kFastLinkIdleMs) {
    Serial.println("[cam] fast link idle - falling back to 115200");
    Serial1.flush();
    Serial1.updateBaudRate(LINK_BAUD);
    g_link_fast = false;
    g_last_cmd_ms = millis();
    SetVideoMode(false);
  }

  delay(2);
}
