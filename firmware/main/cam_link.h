#pragma once

#ifndef _CAM_LINK_H_
#define _CAM_LINK_H_

#include <Arduino.h>
#include <stdint.h>

// Link to the ESP32-CAM in the robot's head.
//
// Wiring to the main board's 4-pin I2C PH2.0 socket (left-to-right: SCL SDA 5V G):
//   CAM GPIO 13 (TX) --> MAIN SCL / GPIO 22 (RX)
//   CAM GPIO 14 (RX) <-- MAIN SDA / GPIO 21 (TX)
//   CAM 5V           <-- MAIN 5V (optional if cam is powered via its own USB)
//   CAM GND          <-> MAIN G  (mandatory common ground)
//
// Why a UART and not Wi-Fi. This board has no PSRAM, and while the assistant is
// speaking its largest free heap block is 2,036 bytes - the Wi-Fi driver has
// been logged failing a 2,308 byte allocation. An HTTP server or an ESP-NOW
// peer would want several KB and a task; the codebase already refuses to start
// its debug web server below 25,000 bytes free for exactly this reason.
//
// This class allocates nothing after Init(). The line buffer and the result
// buffer are members, and Init() runs at boot when ~180KB is free.
class CamLink {
 public:
  static constexpr int kPinRx = 22;  // SCL on the 4-pin I2C PH2.0 socket
  static constexpr int kPinTx = 21;  // SDA on the 4-pin I2C PH2.0 socket
  static constexpr uint32_t kBaud = 115200;

  // Video rate. One 240x176 JPEG is ~6KB: 0.52s at 115200, 65ms at 921600.
  // Only used while the viewfinder is on screen - commands stay at kBaud,
  // because there is no reason to run the link hot for a 12 byte line, and
  // every reason not to on unshielded jumper wire.
  // 460800, not 921600. This is a flow-control limit, not a wire-speed one.
  //
  // At 921600 every single frame failed - 13 of 13 - and always the same way:
  // jd_prepare returned JDR_OK but jd_decomp returned JDR_FMT1. A perfect
  // header with broken scan data cannot be line noise; random corruption at a
  // rate that always destroys the ~5400-byte body would also corrupt the
  // ~600-byte header sometimes, and it never did.
  //
  // What it actually is: the decoder blits each MCU to the SPI panel between
  // input reads, so it drains the UART slower than 92,160 bytes/s fills it.
  // The 512-byte driver buffer is 5.5ms of slack, the frame takes ~65ms to
  // arrive and longer than that to decode, and the shortfall lands in the
  // middle of the scan data every time.
  //
  // Halving the rate fixes the cause instead of buffering around it: the buffer
  // only stays empty while the wire is no faster than the decoder.
  //
  // 460800 was a guess made from an estimated 6KB frame and an estimated decode
  // time. Both estimates were wrong, and the instrumentation below now says so:
  //
  //   video: 39 ok, 0 bad, 2090B/frame, decode 79ms vs wire 45ms
  //   video: 36 ok, 2 bad, 2128B/frame, decode 94ms vs wire 46ms
  //
  // Frames are ~2KB, not 6KB, and decode takes roughly twice as long as the
  // wire. A frame therefore lands in the driver buffer faster than it is
  // consumed, and peak occupancy is len * (1 - wire/decode) ~= len * 0.5.
  // Against a 1024-byte buffer that is fine up to ~2050B/frame and overflows
  // above it - which is exactly where the bad frames appeared: none at
  // 1870-1926B, two once the average reached 2089-2128B. Detail in the room
  // raises the frame size, so this was never going to be stable.
  //
  // 230400 puts a 2KB frame on the wire for ~87ms against a 79-98ms decode, so
  // occupancy stays near zero with a whole room of margin.
  //
  // Why 231884 instead of 230400: the cam board builds against Arduino 3.2.0,
  // whose esp32-hal-uart.c switches UART1 to the 1 MHz REF_TICK clock whenever
  // baud <= 250000 (REF_TICK_BAUDRATE_LIMIT). At 1 MHz, 230400 produces a
  // 16x-fractional divisor of 69 (div_int=4, div_frag=5), whose actual bit
  // rate is 16,000,000 / 69 = 231,884.058 Hz (+0.65% above 230,400). This
  // board (Arduino 2.0.11) clocks UART2 from 80 MHz APB, where 231884 gives an
  // exact integer divisor of 345 (80,000,000 / 345 = 231,884.058 Hz, 5 * 69 =
  // 345) - matching the cam board's bit clock to 0.000000%.
  static constexpr uint32_t kFastBaud = 231884;

  // How long to wait for a vision answer before giving up on the tool call.
  // Capture plus a TLS round trip to a vision API runs 3-8s; 15s is generous
  // without leaving the assistant hanging.
  static constexpr uint32_t kLookTimeoutMs = 15000;

  static CamLink& GetInstance();

  // Opens UART2. Safe to call when no camera is attached: without one the link
  // simply never reports present and every feature degrades to a no-op.
  void Init();

  // Drains the RX buffer and steers the servos. Non-blocking; call from loop().
  void Poll();

  bool IsPresent() const { return present_; }

  void SetTrackingEnabled(bool enabled);
  bool tracking_enabled() const { return tracking_enabled_; }

  // True once per one-finger gesture seen by the cam, which arms tracking on
  // its own. Lets the caller announce it on screen.
  bool TakeGestureArmed();

  // Pauses tracking for a few seconds so an explicit "向左转头" is not
  // immediately undone by the tracker dragging the head back.
  void NoteManualHeadCommand();

  // Self-calibrating tracking direction, per axis (+1 / -1). The tracker watches whether moving the
  // head actually shrinks the subject's offset from frame centre; if the offset keeps GROWING while
  // the head moves, it was turning the wrong way, so it flips that axis and saves the result in NVS.
  int yaw_dir() const { return yaw_dir_; }
  int pitch_dir() const { return pitch_dir_; }
  // Which way the head tilts to mirror the finger's lean. Not learned (the cam does not roll with the
  // head, so there is nothing to learn from); only flipped by hand.
  int roll_dir() const { return roll_dir_; }
  void FlipTrackDir(char axis);  // 'y', 'p' or 'r': manual override from the web page (also saved)
  void ResetTrackDirs();         // back to the defaults (also saved)

  // Hand the cam a vision service to use.
  //
  // The xiaozhi server advertises one in the params of the MCP `initialize`
  // call. Forwarding it means the camera board needs no API key of its own -
  // the account already paying for the assistant pays for the picture.
  //
  // Stored, because the cam may not be listening yet (or may reboot): the
  // endpoint is re-sent every time the cam announces itself with R.
  void SetVisionEndpoint(const char* url, const char* token);

  // Asks the cam for a description. Returns false if a request is already in
  // flight or no camera has ever answered. The reply arrives asynchronously via
  // TakeLookResult().
  //
  // `question` is forwarded to the vision service so the assistant can ask
  // something specific rather than always getting a generic description. Pass
  // nullptr for the cam's default prompt.
  bool RequestLook(int64_t mcp_id, const char* question);

  // Non-blocking. Returns true once per completed (or timed out) request.
  // `text` points at an internal buffer valid until the next Poll().
  bool TakeLookResult(int64_t* id, bool* ok, const char** text);

  // Viewfinder. BeginVideo() speeds the link up and asks the cam to start
  // sending JPEGs, which Poll() then decodes straight onto the panel.
  //
  // The destination is whatever Display::EnterCameraView() opened on the sink,
  // so that must be called first: the geometry belongs to the HUD, which is the
  // thing that has to avoid drawing over the picture. Returns false if the cam
  // is not there or the sink is not open.
  bool BeginVideo();
  void EndVideo();
  bool video_active() const { return video_active_; }
  uint32_t active_baud() const { return link_fast_ ? kFastBaud : kBaud; }

  // Frames actually drawn since BeginVideo(). Lets the UI show a real rate
  // rather than claiming one.
  uint16_t video_frames() const { return video_frames_; }

 private:
  CamLink() = default;
  CamLink(const CamLink&) = delete;
  CamLink& operator=(const CamLink&) = delete;

  void HandleLine(const char* line);
  void ApplyTracking(int dx, int dy, int conf, int lean = 0);

  // Aims the roll servo so the head tilts with the finger's lean (degrees, + = tip to the right).
  void MirrorLean(int lean);

  // Called every Poll(): recentres the head once the finger has been out of sight for a few
  // seconds, and switches a gesture-armed session off after longer.
  void SuperviseTracking(uint32_t now);

  // Eases the head to a stop after the cam stops sending updates. Called every
  // Poll(); does nothing unless a movement is actually in progress.
  void CoastTracking();
  void SendCommand(const char* cmd);

  // Pushes vision_url_/vision_token_ plus this board's MAC to the cam as a
  // single K line. No-op until both a url and a live cam exist.
  void SendVisionEndpoint();

  // Changes the UART speed on both ends and waits up to `ack_timeout_ms` for
  // the cam to confirm at the new rate. Returns false if it never did, meaning
  // the two boards are no longer agreed and nothing sent now will be heard.
  bool SetLinkFast(bool fast, uint32_t ack_timeout_ms, bool force = false);

  // Downshifts an active viewfinder stream to 115200 baud (B 0 + F 1) when the
  // fast rate produces corrupted frames, keeping the camera view live rather
  // than closing it.
  bool FallbackVideoToSlow();

  // Reads one binary video frame, header already consumed, and feeds it to the
  // decoder. Blocks for as long as the frame takes (~65ms at 921600), which is
  // affordable here: audio, networking and LVGL all run on their own tasks.
  void ReceiveVideoFrame();

  // Pulls exactly `len` bytes off the link with a deadline, CRCing them on the
  // way past. `dst == nullptr` discards. Used as the decoder's input callback.
  static size_t VideoRead(void* ctx, uint8_t* dst, size_t len);

  bool initialised_ = false;
  bool present_ = false;
  // Disarmed at boot. The head must not start following people the moment the
  // robot powers on - the user arms it with "开启跟踪" or by holding up one
  // finger, and disarms it with "关闭跟踪".
  bool tracking_enabled_ = false;
  bool tracking_armed_ = false;  // what the cam has actually been told

  // Set when the cam reports a one-finger gesture, cleared by
  // TakeGestureArmed(). Lets main.cpp say so on screen.
  bool gesture_armed_ = false;

  // Tracking was switched on by the gesture rather than by voice or the web page. Such a session
  // turns itself off once the finger has been gone for a while; an explicit one stays on.
  bool armed_by_gesture_ = false;
  bool look_resume_gesture_ = false;  // armed_by_gesture_, saved across a look

  // When a finger report last arrived, and whether the head has already been sent back to centre
  // since then. See SuperviseTracking().
  uint32_t last_finger_ms_ = 0;
  bool recentred_ = false;

  // Where the roll glide was last aimed (ServoController::kDefaultAngle = upright), and which way a
  // lean maps onto it. +1: finger tip to the right of the picture -> roll angle up.
  float roll_target_ = 90.0f;
  int8_t roll_dir_ = 1;

  // The old face-tracking cam firmware sends 4-field T lines; say so once rather than per line.
  bool legacy_warned_ = false;

  // Tracking is suspended for the duration of a look so the head holds still
  // for the photo; this remembers whether to switch it back on afterwards.
  bool look_resume_tracking_ = false;

  uint32_t last_rx_ms_ = 0;
  uint32_t last_ping_ms_ = 0;
  uint32_t last_move_ms_ = 0;
  uint32_t manual_until_ms_ = 0;

  // Current head speed in degrees per update, carried between updates so the
  // acceleration limiter has something to ramp. These are a velocity, not a
  // position: the servo angle itself lives in ServoController.
  float yaw_speed_ = 0.0f;
  float pitch_speed_ = 0.0f;

  // When the cam last sent a usable tracking update. CoastTracking() uses this
  // to tell "the subject is centred and the cam has gone quiet" apart from
  // "updates are still arriving".
  uint32_t last_track_msg_ms_ = 0;

  // ---- Direction learning (see yaw_dir()) ----
  // One observation window per axis: remember the offset when the head starts moving, and once it
  // has actually moved kLearnWindowDeg, compare. Grown by a clear margin = a "wrong way" vote;
  // shrunk = "right way" (clears the votes). kLearnVotes wrong windows in a row flips the axis.
  struct AxisLearn {
    bool active = false;
    int ref_offset = 0;
    float moved_deg = 0.0f;
    uint8_t wrong_votes = 0;
    uint8_t confirmed = 0;        // right-way windows since boot / last flip
    uint32_t stuck_since_ms = 0;  // pinned on an end stop with the subject still off to that side
  };
  void LearnAxis(bool yaw, int offset, float applied_deg, bool at_limit, uint32_t now);
  void LoadTrackDirs();
  void SaveTrackDirs();
  int8_t yaw_dir_ = -1;   // yaw angle up = head turns LEFT, so a subject on the right (dx > 0) needs yaw DOWN
  int8_t pitch_dir_ = 1;  // pitch angle up = look down, subject below (dy > 0) needs pitch UP
  AxisLearn yaw_learn_;
  AxisLearn pitch_learn_;

  // Accumulators for the periodic decode-vs-wire report in ReceiveVideoFrame().
  uint32_t video_decode_ms_ = 0;
  uint32_t video_timed_ = 0;
  uint32_t video_stat_ms_ = 0;

  int64_t look_id_ = 0;
  bool look_pending_ = false;
  uint32_t look_started_ms_ = 0;
  uint32_t look_last_tx_ms_ = 0;
  uint8_t look_retries_ = 0;
  char look_cmd_[192] = {0};

  bool result_ready_ = false;
  bool result_ok_ = false;
  int64_t result_id_ = 0;

  // Viewfinder state.
  bool video_active_ = false;
  bool link_fast_ = false;
  bool video_slow_fallback_ = false;
  uint32_t video_stopped_ms_ = 0;
  uint16_t video_frames_ = 0;
  // Frames whose CRC did not match, or that the decoder rejected. A handful is
  // normal on jumper wire at 921600; a flood means the wiring cannot take it.
  uint16_t video_bad_ = 0;
  // How many times `F 1` has been repeated while waiting for the first frame.
  uint8_t video_retry_ = 0;
  uint32_t last_frame_ms_ = 0;
  // Running CRC of the frame currently being pulled through the decoder, and
  // how much of its payload is still on the wire.
  uint16_t frame_crc_ = 0;
  uint32_t frame_left_ = 0;
  uint8_t frame_head_[8] = {0};
  uint8_t frame_head_len_ = 0;

  // 320, and the cam's send buffer is the same.
  //
  // This was 192, which was wrong: asked "这是什么" the vision service returned a
  // 223 byte answer, and Poll() discards an over-long line whole rather than
  // act on a fragment - so the assistant heard nothing at all. 320 bytes is
  // ~105 Chinese characters, against the 30 the cam now asks the model for.
  //
  // line_len_ is uint16_t because a uint8_t wraps at 255 and would have turned
  // the bounds check into an infinite write.
  uint16_t line_len_ = 0;
  bool line_corrupt_ = false;
  char line_[320];
  char result_[320];

  // Sized from the real values: "http://api.xiaozhi.me/vision/explain" is 36
  // chars and the token is a 36-char UUID. Fixed buffers rather than String,
  // for the same reason as everything else in this class - the heap here has
  // been logged down to a 2,036 byte largest free block.
  char vision_url_[128] = {0};
  char vision_token_[80] = {0};
};

#endif  // _CAM_LINK_H_
