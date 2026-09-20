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

  UiMode ui_mode_ = UiMode::kRobotFace;  // Default to Virtual Robot Face mode

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

  // Virtual Robot Face Mode elements
  lv_obj_t* face_container_ = nullptr;
  lv_obj_t* eye_box_ = nullptr;
  lv_obj_t* eye_left_ = nullptr;
  lv_obj_t* eye_right_ = nullptr;
  lv_obj_t* iris_left_ = nullptr;
  lv_obj_t* iris_right_ = nullptr;
  lv_obj_t* pupil_left_ = nullptr;
  lv_obj_t* pupil_right_ = nullptr;
  lv_obj_t* sparkle1_left_ = nullptr;
  lv_obj_t* sparkle1_right_ = nullptr;
  lv_obj_t* sparkle2_left_ = nullptr;
  lv_obj_t* sparkle2_right_ = nullptr;
  lv_obj_t* iris_glow_left_ = nullptr;
  lv_obj_t* iris_glow_right_ = nullptr;
  lv_obj_t* eyelash_left_ = nullptr;
  lv_obj_t* eyelash_right_ = nullptr;
  lv_obj_t* eyebrow_left_ = nullptr;
  lv_obj_t* eyebrow_right_ = nullptr;

  lv_obj_t* ahoge_ = nullptr;
  lv_obj_t* hair_bang_center_ = nullptr;
  lv_obj_t* hair_bang_left_ = nullptr;
  lv_obj_t* hair_bang_right_ = nullptr;
  lv_obj_t* hair_shine_ = nullptr;
  lv_obj_t* hair_clip_ = nullptr;
  lv_obj_t* side_hair_left_ = nullptr;
  lv_obj_t* side_hair_right_ = nullptr;

  lv_obj_t* blush_left_ = nullptr;
  lv_obj_t* blush_right_ = nullptr;
  lv_obj_t* blush_lines_left_ = nullptr;
  lv_obj_t* blush_lines_right_ = nullptr;

  lv_obj_t* mouth_box_ = nullptr;
  lv_obj_t* mouth_smile_ = nullptr;
  lv_obj_t* anime_mouth_ = nullptr;
  lv_obj_t* mouth_tooth_ = nullptr;
  lv_obj_t* mouth_tongue_ = nullptr;

  lv_obj_t* emote_badge_ = nullptr;

  lv_obj_t* subtitle_box_ = nullptr;
  lv_obj_t* subtitle_label_ = nullptr;

  lv_timer_t* blink_timer_ = nullptr;
  lv_timer_t* voice_anim_timer_ = nullptr;
  lv_timer_t* ahoge_timer_ = nullptr;

  std::string current_emotion_ = "neutral";
  bool is_speaking_ = false;
  int current_eye_height_ = 68;
  int current_eye_width_ = 50;
  lv_color_t current_eye_color_;

  static void OnBlinkTimer(lv_timer_t* timer);
  static void OnVoiceAnimTimer(lv_timer_t* timer);
  static void OnAhogeTimer(lv_timer_t* timer);

  ThemeColors current_theme_;
};

extern std::unique_ptr<Display> g_display;

#endif