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
  lv_obj_t* mute_label_ = nullptr;

  // Character face mode: a full-screen portrait plus two small sprites that are swapped over the
  // eyes and mouth to animate it. Everything else about the picture stays put.
  lv_obj_t* face_container_ = nullptr;
  lv_obj_t* face_image_ = nullptr;
  lv_obj_t* eyes_overlay_ = nullptr;
  lv_obj_t* mouth_overlay_ = nullptr;
  lv_obj_t* subtitle_box_ = nullptr;
  lv_obj_t* subtitle_label_ = nullptr;

  lv_timer_t* face_timer_ = nullptr;

  std::string current_emotion_ = "neutral";
  bool is_speaking_ = false;

  // Animation state, all driven from the single face timer.
  uint32_t face_tick_ = 0;        // increments once per timer period
  uint32_t next_blink_tick_ = 0;  // when the next blink starts
  uint8_t blink_frame_ = 0;       // 0 = eyes open, otherwise a step in the blink sequence
  int8_t head_offset_x_ = 0;      // current idle sway / look-direction offset
  int8_t head_offset_y_ = 0;

  static void OnFaceTimer(lv_timer_t* timer);
  void ApplyBlinkFrame();
  void ApplyMouthFrame();
  void ApplyHeadOffset(int dx, int dy);

  ThemeColors current_theme_;
};

extern std::unique_ptr<Display> g_display;

#endif