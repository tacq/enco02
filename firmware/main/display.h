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
    kRobotFace = 0,  // Virtual cute robot face with reactions (Default)
    kChatText = 1,   // Full conversation history HUD
  };

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

  void SetUiMode(UiMode mode);
  UiMode GetUiMode() const { return ui_mode_; }
  void ToggleUiMode();
  void UpdateRobotFaceEmotion(const std::string& emotion);
  void LookDirection(const char* dir);
  // Creates the handful of LVGL objects that make up the bitmap character. Safe to call more than
  // once; the pixels themselves live in flash, so this costs well under 2KB of heap.
  void BuildRobotFace();

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

  static void OnFaceTimer(lv_timer_t* timer);
  void ApplyBlinkFrame();
  void ApplyMouthFrame();
  void ApplyHairFrame();
  void ApplyHeadOffset(int dx, int dy);

  ThemeColors current_theme_;
};

extern std::unique_ptr<Display> g_display;

#endif