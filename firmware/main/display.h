#pragma once

#ifndef _DISPLAY_H_
#define _DISPLAY_H_

#include <esp_lcd_panel_ops.h>

#include <memory>
#include <string>

#include "lvgl.h"

class Display {
 public:
  enum class Role : uint8_t {
    kSystem,
    kAssistant,
    kUser,
  };
  enum class UiMode : uint8_t {
    kRobotFace = 0,   // Virtual cute robot face with reactions (Default)
    kChatText = 1,    // Full conversation history HUD
    kCameraView = 2,  // Camera viewfinder: live picture from the head, plus HUD chrome
  };

  // Viewfinder geometry, in panel pixels on a 240x320 screen.
  //
  // These are public because the JPEG lands on the panel by a different route
  // to everything else on screen. There is no framebuffer on this board, so a
  // frame cannot be handed to LVGL as an image; it is decoded a 16x16 block at
  // a time straight into the ST7789. That means the exact rectangle has to be
  // agreed between the HUD (which must not draw inside it) and the decoder.
  //
  // The camera sends 240x176 (HQVGA), which is exactly as wide as the screen.
  // kCamCrop trims kCamInset pixels off each edge so there is a margin the
  // video never touches and the corner brackets have somewhere to live.
  static constexpr int kCamInset = 8;
  static constexpr int kCamFrameX = 0;
  static constexpr int kCamFrameY = 24;
  static constexpr int kCamFrameW = 240;
  static constexpr int kCamFrameH = 176;
  static constexpr int kCamCropW = kCamFrameW - 2 * kCamInset;  // 224
  static constexpr int kCamCropH = kCamFrameH - 2 * kCamInset;  // 160
  static constexpr int kCamVideoX = kCamFrameX + kCamInset;
  static constexpr int kCamVideoY = kCamFrameY + kCamInset;

  Display(esp_lcd_panel_io_handle_t panel_io,
          esp_lcd_panel_handle_t panel,
          int width,
          int height,
          int offset_x,
          int offset_y,
          bool mirror_x,
          bool mirror_y,
          bool swap_xy);
  ~Display();
  void Start();
  void SetChatMessage(const Role role, const std::string& content);
  void ShowStatus(const char* status);
  // Transient "音量 70%" readout plus a speaker icon in the status bar. Deliberately not routed
  // through ShowStatus(): that derives is_speaking_ from the string it is given, and the volume
  // almost always changes while the assistant is mid-sentence.
  void ShowVolume(uint16_t volume);
  // Countdown chip in the middle of the status bar. While it is up it owns the centre of the bar:
  // the chat status and the volume toast both step aside, so the digits land on the optical middle
  // of the screen rather than being pushed off to one side.
  void ShowTimer(uint32_t remaining_seconds);
  void ShowTimerFinished();
  void HideTimer();
  void SetEmotion(const std::string& emotion);

  // Wi-Fi strength indicator at the right-hand end of the status bar.
  //
  // The status bar has had a network_label_ since the beginning, but nothing ever set its text, so
  // the one piece of information the user most often wants - "is it on the network?" - was the one
  // thing the screen never showed. `rssi_dbm` is ignored when `connected` is false.
  void ShowNetwork(bool connected, int rssi_dbm);

  // Event notification card, over the character's right-hand side.
  //
  // `title` is one short line, `body` up to about three. Both are UTF-8 and may be Chinese - the
  // card uses the 16px CJK font, not the Latin font the countdown digits use.
  //
  // Like the countdown panel, the card is created when it is needed and destroyed when it clears,
  // so neither costs anything while the robot is just sitting there. Calling ShowAlert() twice
  // rewrites the existing card rather than stacking a second one.
  void ShowAlert(const char* title, const char* body, uint32_t duration_ms = 5000);
  void HideAlert();


  void SetUiMode(UiMode mode);
  UiMode GetUiMode() const { return ui_mode_; }
  void ToggleUiMode();
  void UpdateRobotFaceEmotion(const std::string& emotion);
  void LookDirection(const char* dir);
  // Creates the handful of LVGL objects that make up the bitmap character. Safe to call more than
  // once; the pixels themselves live in flash, so this costs well under 2KB of heap.
  void BuildRobotFace();

  // Camera viewfinder.
  //
  // EnterCameraView() builds the HUD, remembers which mode to go back to, and
  // opens the JPEG decoder onto the panel. It returns false if either the
  // widgets or the ~4KB decoder workspace could not be had, in which case
  // nothing has changed and the caller must not start the video link.
  bool EnterCameraView();
  void ExitCameraView();
  void SuspendCameraVideo();
  bool ResumeCameraVideo();
  bool InCameraView() const { return ui_mode_ == UiMode::kCameraView; }

  // Big caption over the picture - "识别中…", the answer, an error. Pass nullptr
  // or "" to clear it and show the live picture unobstructed.
  void SetCameraHint(const char* text);

  // Right-hand end of the telemetry strip, e.g. "12 FPS · 921K". Short: the
  // strip is 18px tall and shares its width with a fixed label.
  void SetCameraTelemetry(const char* text);

  // Swaps the blinking "REC ●" for a steady "SHOT ■" while a capture is being
  // recognised, so a frozen picture reads as deliberate rather than as a stall.
  void SetCameraCapturing(bool capturing);

 private:
  struct ThemeColors {
    lv_color_t background;
    lv_color_t text;
    lv_color_t chat_background;
    lv_color_t user_bubble;
    lv_color_t assistant_bubble;
    lv_color_t system_bubble;
    lv_color_t system_text;
    lv_color_t border;
    lv_color_t low_battery;
    lv_color_t jarvis_cyan;
    lv_color_t jarvis_cyan_dim;
    lv_color_t jarvis_gold;
    lv_color_t user_text;
    lv_color_t assistant_text;
  };
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  // Kept because the camera viewfinder writes to the panel directly, bypassing
  // LVGL entirely - there is no heap for a framebuffer to hand it instead.
  esp_lcd_panel_handle_t panel_ = nullptr;
  lv_display_t* display_ = nullptr;
  lv_obj_t* container_ = nullptr;
  lv_obj_t* status_bar_ = nullptr;

  // Boot straight into the character view. The bitmap avatar is four LVGL objects reading pixels
  // straight out of flash, so unlike the old vector face it costs almost no heap and no longer has
  // to be traded off against the TLS handshake.
  UiMode ui_mode_ = UiMode::kRobotFace;
  bool face_built_ = false;

  // Chat Text Mode elements (preserved!)
  lv_obj_t* content_ = nullptr;
  lv_obj_t* content_left_ = nullptr;
  lv_obj_t* content_right_ = nullptr;
  lv_obj_t* emotion_label_ = nullptr;
  lv_obj_t* chat_message_label_ = nullptr;
  lv_obj_t* network_label_ = nullptr;
  // Latches once WiFi has associated at least once. Before that, "down" means "still booting" and
  // must not be painted as a fault - association takes ~12s from reset on this board.
  bool net_ever_connected_ = false;
  lv_obj_t* notification_label_ = nullptr;
  lv_obj_t* status_label_ = nullptr;
  // Speaker icon reflecting the current output volume. (Was an unused mute indicator.)
  lv_obj_t* volume_label_ = nullptr;
  // Countdown readout. Shares the centre of the status bar with status_label_/notification_label_,
  // and wins while a timer is running - see timer_visible_.
  lv_obj_t* timer_label_ = nullptr;
  // Latch so ShowStatus()/ShowVolume() know to leave the centre of the bar alone. Without it the
  // next chat state change would unhide status_label_ underneath the countdown and the two would
  // sit side by side, each with flex_grow 1, pushing the digits off centre.
  bool timer_visible_ = false;

  // Character face mode: a full-screen portrait plus small sprites that are swapped over the
  // hair, eyes and mouth to animate it. Everything else about the picture stays put.
  lv_obj_t* face_container_ = nullptr;
  lv_obj_t* face_image_ = nullptr;
  lv_obj_t* bangs_overlay_ = nullptr;
  lv_obj_t* locks_l_overlay_ = nullptr;
  lv_obj_t* locks_r_overlay_ = nullptr;
  lv_obj_t* eyes_overlay_ = nullptr;
  lv_obj_t* mouth_overlay_ = nullptr;
  lv_obj_t* subtitle_box_ = nullptr;
  lv_obj_t* subtitle_label_ = nullptr;

  lv_timer_t* face_timer_ = nullptr;

  std::string current_emotion_ = "neutral";
  // What the chat state machine says. Used for the status text only: the mouth follows the
  // amplifier (see mouth_open_ below), not the protocol.
  bool is_speaking_ = false;

  // Animation state, all driven from the single face timer.
  uint32_t face_tick_ = 0;        // increments once per timer period
  uint32_t next_blink_tick_ = 0;  // when the next blink starts
  uint8_t blink_frame_ = 0;       // 0 = eyes open, otherwise a step in the blink sequence
  bool mouth_open_ = false;       // true while PCM is actually reaching the speaker
  uint32_t next_hair_tick_ = 0;   // when the next random hair breeze starts
  uint8_t hair_step_ = 0;         // 0 = hair at rest, otherwise 1-based step in the breeze sequence
  uint8_t hair_pattern_ = 0;      // which breeze pattern is playing
  // Last level applied to each hair sprite, in [-2, +2]. Cached because LVGL invalidates an image
  // whenever its source is set, even to the value it already held - and a redundant lock redraw is
  // 8K pixels over the SPI bus.
  int8_t hair_level_bangs_ = 0;
  int8_t hair_level_locks_ = 0;
  // Current explicit look-direction offset. Only ever changed by LookDirection(); there is no
  // automatic sway, because shifting the portrait invalidates the whole screen and read as a flash.
  int8_t head_offset_x_ = 0;
  int8_t head_offset_y_ = 0;

  // Camera viewfinder. A sibling of container_ rather than a child: the picture
  // is blitted at fixed panel coordinates, so the HUD around it cannot be
  // allowed to shift with the height of the status bar.
  lv_obj_t* cam_container_ = nullptr;
  lv_obj_t* cam_rec_label_ = nullptr;
  lv_obj_t* cam_hint_box_ = nullptr;
  lv_obj_t* cam_hint_label_ = nullptr;
  lv_obj_t* cam_telemetry_label_ = nullptr;
  bool cam_built_ = false;
  bool cam_capturing_ = false;
  bool cam_rec_on_ = true;         // phase of the blinking REC dot
  uint32_t cam_rec_next_tick_ = 0; // next face_tick_ at which it flips
  // Where to go back to when the viewfinder closes. Camera mode is always a
  // detour - the robot returns to whatever it was showing before.
  UiMode cam_return_mode_ = UiMode::kRobotFace;

  void BuildCameraView();

  // Countdown + event alert overlay.
  //
  // Siblings of container_, for the same reason cam_container_ is: they are positioned in absolute
  // panel coordinates and must not move when the status bar changes height.
  //
  // Both are built on demand and deleted again, so the steady-state cost is zero objects. That is
  // the only way they fit: an LVGL widget costs ~430 bytes here and this board has been logged at
  // 6,336 bytes free during TTS. A permanently-allocated countdown panel would be ~1.3KB held for
  // the 99.9% of the time no timer is running.
  lv_obj_t* timer_panel_ = nullptr;
  lv_obj_t* timer_digits_ = nullptr;
  lv_obj_t* alert_card_ = nullptr;
  lv_obj_t* alert_title_ = nullptr;
  lv_obj_t* alert_body_ = nullptr;
  uint32_t alert_hide_at_ms_ = 0;

  // Returns false if the heap could not take it, in which case the caller falls back to the
  // status-bar countdown, which costs nothing because those widgets already exist.
  bool EnsureTimerPanel();
  void DestroyTimerPanel();
  // Keeps the caption pill and the countdown panel from fighting over the bottom of the screen.
  void SetSubtitleHidden(bool hidden);


  static void OnFaceTimer(lv_timer_t* timer);
  void ApplyBlinkFrame();
  void ApplyMouthFrame();
  void ApplyHairFrame();
  void ApplyHeadOffset(int dx, int dy);

  ThemeColors current_theme_;
};

extern std::unique_ptr<Display> g_display;

#endif