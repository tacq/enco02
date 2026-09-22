#pragma once

#ifndef _CAM_LINK_H_
#define _CAM_LINK_H_

#include <Arduino.h>
#include <stdint.h>

// Link to the ESP32-CAM in the robot's head.
//
// Wiring (cross-over; a common ground is mandatory):
//   CAM GPIO 13 (TX) --> MAIN GPIO 18 (RX)
//   CAM GPIO 14 (RX) <-- MAIN GPIO 19 (TX)
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
  static constexpr int kPinRx = 18;  // unreferenced anywhere else in the repo
  static constexpr int kPinTx = 19;
  static constexpr uint32_t kBaud = 115200;

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

  // Pauses tracking for a few seconds so an explicit "向左转头" is not
  // immediately undone by the tracker dragging the head back.
  void NoteManualHeadCommand();

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

 private:
  CamLink() = default;
  CamLink(const CamLink&) = delete;
  CamLink& operator=(const CamLink&) = delete;

  void HandleLine(const char* line);
  void ApplyTracking(int dx, int dy, int conf);
  void SendCommand(const char* cmd);

  // Pushes vision_url_/vision_token_ plus this board's MAC to the cam as a
  // single K line. No-op until both a url and a live cam exist.
  void SendVisionEndpoint();

  bool initialised_ = false;
  bool present_ = false;
  bool tracking_enabled_ = true;
  bool tracking_armed_ = false;  // what the cam has actually been told

  uint32_t last_rx_ms_ = 0;
  uint32_t last_ping_ms_ = 0;
  uint32_t last_move_ms_ = 0;
  uint32_t manual_until_ms_ = 0;

  int64_t look_id_ = 0;
  bool look_pending_ = false;
  uint32_t look_started_ms_ = 0;

  bool result_ready_ = false;
  bool result_ok_ = false;
  int64_t result_id_ = 0;

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
