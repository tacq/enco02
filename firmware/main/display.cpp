#include <esp_log.h>

#include <algorithm>
#include <map>
#include <vector>

#include "esp_lvgl_port.h"
#include "font_awesome_symbols.h"
#include "font_emoji.h"
#include "display.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_30_4);
LV_FONT_DECLARE(font_awesome_16_4);

// Sci-Fi HUD Jarvis Theme Color Definitions
#define SCI_FI_BG_COLOR lv_color_hex(0x060c14)             // Deep space holographic dark
#define SCI_FI_TEXT_COLOR lv_color_hex(0x7dd3fc)           // Glowing cyan blue text
#define SCI_FI_CHAT_BG_COLOR lv_color_hex(0x0a1526)        // HUD panel background
#define SCI_FI_USER_BUBBLE_COLOR lv_color_hex(0x132a13)    // Tactical targeting green tint
#define SCI_FI_ASSISTANT_BUBBLE_COLOR lv_color_hex(0x0c253d) // Jarvis holographic blue tint
#define SCI_FI_SYSTEM_BUBBLE_COLOR lv_color_hex(0x1a1c29)  // Deep system telemetry slate
#define SCI_FI_SYSTEM_TEXT_COLOR lv_color_hex(0x38bdf8)    // Cyan status text
#define SCI_FI_BORDER_COLOR lv_color_hex(0x0284c7)         // Arc-reactor cyan border
#define SCI_FI_LOW_BATTERY_COLOR lv_color_hex(0xef4444)    // Warning red
#define SCI_FI_JARVIS_CYAN lv_color_hex(0x00f0ff)          // Neon arc cyan
#define SCI_FI_JARVIS_CYAN_DIM lv_color_hex(0x0369a1)      // Dim cyan glow
#define SCI_FI_JARVIS_GOLD lv_color_hex(0xfbbf24)          // Iron Man Stark Gold
#define SCI_FI_USER_TEXT lv_color_hex(0x86efac)            // Bright tactical green
#define SCI_FI_ASSISTANT_TEXT lv_color_hex(0xe0f2fe)       // Crisp hologram white-blue

Display::Display(esp_lcd_panel_io_handle_t panel_io,
                 esp_lcd_panel_handle_t panel,
                 int width,
                 int height,
                 int offset_x,
                 int offset_y,
                 bool mirror_x,
                 bool mirror_y,
                 bool swap_xy)
    : width_(width),
      height_(height),
      current_theme_{
          .background = SCI_FI_BG_COLOR,
          .text = SCI_FI_TEXT_COLOR,
          .chat_background = SCI_FI_CHAT_BG_COLOR,
          .user_bubble = SCI_FI_USER_BUBBLE_COLOR,
          .assistant_bubble = SCI_FI_ASSISTANT_BUBBLE_COLOR,
          .system_bubble = SCI_FI_SYSTEM_BUBBLE_COLOR,
          .system_text = SCI_FI_SYSTEM_TEXT_COLOR,
          .border = SCI_FI_BORDER_COLOR,
          .low_battery = SCI_FI_LOW_BATTERY_COLOR,
          .jarvis_cyan = SCI_FI_JARVIS_CYAN,
          .jarvis_cyan_dim = SCI_FI_JARVIS_CYAN_DIM,
          .jarvis_gold = SCI_FI_JARVIS_GOLD,
          .user_text = SCI_FI_USER_TEXT,
          .assistant_text = SCI_FI_ASSISTANT_TEXT,
      } {
  // Clear screen to deep space black initially
  std::vector<uint16_t> buffer(width_, 0x0821);
  for (int y = 0; y < height_; y++) {
    esp_lcd_panel_draw_bitmap(panel, 0, y, width_, y + 1, buffer.data());
  }

  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
  lv_init();

  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  port_cfg.task_priority = 2;
  port_cfg.timer_period_ms = 20;
  lvgl_port_init(&port_cfg);

  const lvgl_port_display_cfg_t display_cfg = {
      .io_handle = panel_io,
      .panel_handle = panel,
      .control_handle = nullptr,
      .buffer_size = static_cast<uint32_t>(width * 10),
      .double_buffer = false,
      .trans_size = 0,
      .hres = static_cast<uint32_t>(width),
      .vres = static_cast<uint32_t>(height),
      .monochrome = false,
      .rotation =
          {
              .swap_xy = swap_xy,
              .mirror_x = mirror_x,
              .mirror_y = mirror_y,
          },
      .color_format = LV_COLOR_FORMAT_RGB565,
      .flags =
          {
              .buff_dma = 1,
              .buff_spiram = 0,
              .sw_rotate = 0,
              .swap_bytes = 1,
              .full_refresh = 0,
              .direct_mode = 0,
          },
  };

  display_ = lvgl_port_add_disp(&display_cfg);

  assert(display_ != nullptr);
  if (display_ == nullptr) {
    abort();
    return;
  }

  if (offset_x != 0 || offset_y != 0) {
    lv_display_set_offset(display_, offset_x, offset_y);
  }
}

Display::~Display() {
  // TODO:
}

void Display::Start() {
  lvgl_port_lock(0);

  auto screen = lv_screen_active();
  lv_obj_set_style_text_font(screen, &font_puhui_16_4, 0);
  lv_obj_set_style_text_color(screen, current_theme_.text, 0);
  lv_obj_set_style_bg_color(screen, current_theme_.background, 0);

  /* Container */
  container_ = lv_obj_create(screen);
  lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(container_, 0, 0);
  lv_obj_set_style_border_width(container_, 0, 0);
  lv_obj_set_style_pad_row(container_, 0, 0);
  lv_obj_set_style_bg_color(container_, current_theme_.background, 0);
  lv_obj_set_style_border_color(container_, current_theme_.border, 0);

  /* Status bar - Sci-Fi Top HUD Bar */
  status_bar_ = lv_obj_create(container_);
  lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(status_bar_, 0, 0);
  lv_obj_set_style_bg_color(status_bar_, lv_color_hex(0x040910), 0);
  lv_obj_set_style_bg_opa(status_bar_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(status_bar_, 0, 0);
  lv_obj_set_style_border_color(status_bar_, current_theme_.border, 0);
  lv_obj_set_style_border_side(status_bar_, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_text_color(status_bar_, current_theme_.jarvis_cyan, 0);

  /* Content - Sci-Fi Chat area */
  content_ = lv_obj_create(container_);
  lv_obj_set_style_radius(content_, 0, 0);
  lv_obj_set_width(content_, LV_HOR_RES);
  lv_obj_set_flex_grow(content_, 1);
  lv_obj_set_style_pad_all(content_, 8, 0);
  lv_obj_set_style_bg_color(content_, current_theme_.chat_background, 0);
  lv_obj_set_style_border_width(content_, 1, 0);
  lv_obj_set_style_border_color(content_, current_theme_.border, 0);
  lv_obj_set_style_border_side(content_, LV_BORDER_SIDE_TOP, 0);

  // Enable scrolling for chat content
  lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_dir(content_, LV_DIR_VER);

  // Create a flex container for chat messages
  lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(content_, 10, 0);  // Space between messages

  // We'll create chat messages dynamically in SetChatMessage
  chat_message_label_ = nullptr;

  // By default, hide content_ if in kRobotFace mode
  if (ui_mode_ == UiMode::kRobotFace) {
    lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
  }

  /* Virtual Robot Face Container (Default New Interaction Way) */
  face_container_ = lv_obj_create(container_);
  lv_obj_set_style_radius(face_container_, 0, 0);
  lv_obj_set_width(face_container_, LV_HOR_RES);
  lv_obj_set_flex_grow(face_container_, 1);
  lv_obj_set_style_pad_all(face_container_, 0, 0);
  lv_obj_set_style_bg_color(face_container_, lv_color_hex(0x050912), 0);
  lv_obj_set_style_bg_opa(face_container_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(face_container_, 1, 0);
  lv_obj_set_style_border_color(face_container_, current_theme_.border, 0);
  lv_obj_set_style_border_side(face_container_, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_scrollbar_mode(face_container_, LV_SCROLLBAR_MODE_OFF);

  if (ui_mode_ == UiMode::kChatText) {
    lv_obj_add_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
  }

  // 1. Eye Area Container
  eye_box_ = lv_obj_create(face_container_);
  lv_obj_set_size(eye_box_, 200, 110);
  lv_obj_align(eye_box_, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_set_style_bg_opa(eye_box_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(eye_box_, 0, 0);
  lv_obj_set_style_pad_all(eye_box_, 0, 0);
  lv_obj_set_scrollbar_mode(eye_box_, LV_SCROLLBAR_MODE_OFF);

  // Left Eye (Capsule)
  eye_left_ = lv_obj_create(eye_box_);
  lv_obj_set_size(eye_left_, current_eye_width_, current_eye_height_);
  lv_obj_align(eye_left_, LV_ALIGN_CENTER, -44, 0);
  lv_obj_set_style_radius(eye_left_, 24, 0);
  lv_obj_set_style_bg_color(eye_left_, current_theme_.jarvis_cyan, 0);
  lv_obj_set_style_bg_opa(eye_left_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(eye_left_, 2, 0);
  lv_obj_set_style_border_color(eye_left_, lv_color_hex(0x7dd3fc), 0);
  lv_obj_set_style_pad_all(eye_left_, 0, 0);
  lv_obj_set_scrollbar_mode(eye_left_, LV_SCROLLBAR_MODE_OFF);

  // Specular Highlight Left
  pupil_left_ = lv_obj_create(eye_left_);
  lv_obj_set_size(pupil_left_, 14, 14);
  lv_obj_align(pupil_left_, LV_ALIGN_TOP_RIGHT, -4, 6);
  lv_obj_set_style_radius(pupil_left_, 7, 0);
  lv_obj_set_style_bg_color(pupil_left_, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(pupil_left_, LV_OPA_80, 0);
  lv_obj_set_style_border_width(pupil_left_, 0, 0);
  lv_obj_set_scrollbar_mode(pupil_left_, LV_SCROLLBAR_MODE_OFF);

  // Right Eye (Capsule)
  eye_right_ = lv_obj_create(eye_box_);
  lv_obj_set_size(eye_right_, current_eye_width_, current_eye_height_);
  lv_obj_align(eye_right_, LV_ALIGN_CENTER, 44, 0);
  lv_obj_set_style_radius(eye_right_, 24, 0);
  lv_obj_set_style_bg_color(eye_right_, current_theme_.jarvis_cyan, 0);
  lv_obj_set_style_bg_opa(eye_right_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(eye_right_, 2, 0);
  lv_obj_set_style_border_color(eye_right_, lv_color_hex(0x7dd3fc), 0);
  lv_obj_set_style_pad_all(eye_right_, 0, 0);
  lv_obj_set_scrollbar_mode(eye_right_, LV_SCROLLBAR_MODE_OFF);

  // Specular Highlight Right
  pupil_right_ = lv_obj_create(eye_right_);
  lv_obj_set_size(pupil_right_, 14, 14);
  lv_obj_align(pupil_right_, LV_ALIGN_TOP_RIGHT, -4, 6);
  lv_obj_set_style_radius(pupil_right_, 7, 0);
  lv_obj_set_style_bg_color(pupil_right_, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(pupil_right_, LV_OPA_80, 0);
  lv_obj_set_style_border_width(pupil_right_, 0, 0);
  lv_obj_set_scrollbar_mode(pupil_right_, LV_SCROLLBAR_MODE_OFF);

  // Left Blush
  blush_left_ = lv_obj_create(eye_box_);
  lv_obj_set_size(blush_left_, 26, 10);
  lv_obj_align(blush_left_, LV_ALIGN_CENTER, -44, 48);
  lv_obj_set_style_radius(blush_left_, 5, 0);
  lv_obj_set_style_bg_color(blush_left_, lv_color_hex(0xf43f5e), 0);
  lv_obj_set_style_bg_opa(blush_left_, LV_OPA_70, 0);
  lv_obj_set_style_border_width(blush_left_, 0, 0);
  lv_obj_add_flag(blush_left_, LV_OBJ_FLAG_HIDDEN);

  // Right Blush
  blush_right_ = lv_obj_create(eye_box_);
  lv_obj_set_size(blush_right_, 26, 10);
  lv_obj_align(blush_right_, LV_ALIGN_CENTER, 44, 48);
  lv_obj_set_style_radius(blush_right_, 5, 0);
  lv_obj_set_style_bg_color(blush_right_, lv_color_hex(0xf43f5e), 0);
  lv_obj_set_style_bg_opa(blush_right_, LV_OPA_70, 0);
  lv_obj_set_style_border_width(blush_right_, 0, 0);
  lv_obj_add_flag(blush_right_, LV_OBJ_FLAG_HIDDEN);

  // 2. Mouth / Voice Waveform Area
  mouth_box_ = lv_obj_create(face_container_);
  lv_obj_set_size(mouth_box_, 100, 32);
  lv_obj_align(mouth_box_, LV_ALIGN_TOP_MID, 0, 150);
  lv_obj_set_style_bg_opa(mouth_box_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(mouth_box_, 0, 0);
  lv_obj_set_style_pad_all(mouth_box_, 0, 0);
  lv_obj_set_scrollbar_mode(mouth_box_, LV_SCROLLBAR_MODE_OFF);

  const int bar_x_offsets[5] = {-24, -12, 0, 12, 24};
  for (int i = 0; i < 5; ++i) {
    wave_bars_[i] = lv_obj_create(mouth_box_);
    lv_obj_set_size(wave_bars_[i], 6, 6);
    lv_obj_align(wave_bars_[i], LV_ALIGN_CENTER, bar_x_offsets[i], 0);
    lv_obj_set_style_radius(wave_bars_[i], 3, 0);
    lv_obj_set_style_bg_color(wave_bars_[i], current_theme_.jarvis_cyan, 0);
    lv_obj_set_style_bg_opa(wave_bars_[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wave_bars_[i], 0, 0);
    lv_obj_set_scrollbar_mode(wave_bars_[i], LV_SCROLLBAR_MODE_OFF);
  }

  // 3. Bottom Subtitle Banner
  subtitle_box_ = lv_obj_create(face_container_);
  lv_obj_set_size(subtitle_box_, 224, 56);
  lv_obj_align(subtitle_box_, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_radius(subtitle_box_, 10, 0);
  lv_obj_set_style_bg_color(subtitle_box_, lv_color_hex(0x0c1b2c), 0);
  lv_obj_set_style_bg_opa(subtitle_box_, LV_OPA_90, 0);
  lv_obj_set_style_border_width(subtitle_box_, 1, 0);
  lv_obj_set_style_border_color(subtitle_box_, lv_color_hex(0x0284c7), 0);
  lv_obj_set_style_pad_all(subtitle_box_, 6, 0);
  lv_obj_set_scrollbar_mode(subtitle_box_, LV_SCROLLBAR_MODE_OFF);

  subtitle_label_ = lv_label_create(subtitle_box_);
  lv_obj_set_width(subtitle_label_, 210);
  lv_obj_set_style_text_font(subtitle_label_, &font_puhui_16_4, 0);
  lv_obj_set_style_text_color(subtitle_label_, current_theme_.assistant_text, 0);
  lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(subtitle_label_, "🤖 Enco 正在待命...");

  // Natural Blinking & Voice Animation Timers
  blink_timer_ = lv_timer_create(OnBlinkTimer, 3500, this);
  voice_anim_timer_ = lv_timer_create(OnVoiceAnimTimer, 120, this);

  /* Status bar */
  lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all(status_bar_, 0, 0);
  lv_obj_set_style_border_width(status_bar_, 0, 0);
  lv_obj_set_style_pad_column(status_bar_, 0, 0);
  lv_obj_set_style_pad_left(status_bar_, 10, 0);
  lv_obj_set_style_pad_right(status_bar_, 10, 0);
  lv_obj_set_style_pad_top(status_bar_, 2, 0);
  lv_obj_set_style_pad_bottom(status_bar_, 2, 0);
  lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
  // 设置状态栏的内容垂直居中
  lv_obj_set_flex_align(status_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // 创建emotion_label_在状态栏最左侧
  emotion_label_ = lv_label_create(status_bar_);
  lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
  lv_obj_set_style_text_color(emotion_label_, current_theme_.jarvis_cyan, 0);
  lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);
  lv_obj_set_style_margin_right(emotion_label_, 5, 0);  // 添加右边距，与后面的元素分隔

  notification_label_ = lv_label_create(status_bar_);
  lv_obj_set_flex_grow(notification_label_, 1);
  lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(notification_label_, current_theme_.jarvis_gold, 0);
  lv_label_set_text(notification_label_, "");
  lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

  status_label_ = lv_label_create(status_bar_);
  lv_obj_set_flex_grow(status_label_, 1);
  lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(status_label_, current_theme_.jarvis_gold, 0);
  lv_label_set_text(status_label_, "ENCO ONLINE");

  mute_label_ = lv_label_create(status_bar_);
  lv_label_set_text(mute_label_, "");
  lv_obj_set_style_text_font(mute_label_, &font_awesome_16_4, 0);
  lv_obj_set_style_text_color(mute_label_, current_theme_.jarvis_cyan, 0);

  network_label_ = lv_label_create(status_bar_);
  lv_label_set_text(network_label_, "");
  lv_obj_set_style_text_font(network_label_, &font_awesome_16_4, 0);
  lv_obj_set_style_text_color(network_label_, current_theme_.jarvis_cyan, 0);
  lv_obj_set_style_margin_left(network_label_, 5, 0);  // 添加左边距，与前面的元素分隔

  lvgl_port_unlock();
}

#define MAX_MESSAGES (5)
void Display::SetChatMessage(const Role role, const std::string& content) {
  if (content.empty()) {
    return;
  }

  lvgl_port_lock(0);

  // 检查消息数量是否超过限制
  uint32_t child_count = lv_obj_get_child_cnt(content_);
  if (child_count >= MAX_MESSAGES) {
    // 删除最早的消息（第一个子对象）
    lv_obj_t* first_child = lv_obj_get_child(content_, 0);
    lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
    if (first_child != nullptr) {
      lv_obj_del(first_child);
    }
    // Scroll to the last message immediately
    if (last_child != nullptr) {
      lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
    }
  }

  // Create a Sci-Fi message bubble
  lv_obj_t* msg_bubble = lv_obj_create(content_);
  lv_obj_set_style_radius(msg_bubble, 4, 0);  // High-tech angular bevels
  lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_border_width(msg_bubble, 1, 0);
  lv_obj_set_style_pad_all(msg_bubble, 7, 0);

  // Format Sci-Fi content with Enco / HUD prefixes
  std::string formatted_content;
  if (role == Role::kAssistant) {
    formatted_content = "◈ Enco:\n" + content;
  } else if (role == Role::kUser) {
    formatted_content = "▲ PILOT:\n" + content;
  } else {
    formatted_content = "SYS // " + content;
  }

  // Create the message text
  lv_obj_t* msg_text = lv_label_create(msg_bubble);
  lv_label_set_text(msg_text, formatted_content.c_str());

  // 计算文本实际宽度
  lv_coord_t text_width = lv_txt_get_width(formatted_content.c_str(), formatted_content.size(), &font_puhui_16_4, 0);

  // 计算气泡宽度
  lv_coord_t max_width = LV_HOR_RES * 88 / 100 - 16;  // 屏幕宽度的88%
  lv_coord_t min_width = 30;
  lv_coord_t bubble_width;

  // 确保文本宽度不小于最小宽度
  if (text_width < min_width) {
    text_width = min_width;
  }

  if (text_width < max_width) {
    bubble_width = text_width;
  } else {
    bubble_width = max_width;
  }

  // 设置消息文本的宽度
  lv_obj_set_width(msg_text, bubble_width);
  lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(msg_text, &font_puhui_16_4, 0);

  // 设置气泡宽度与高度
  lv_obj_set_width(msg_bubble, bubble_width);
  lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

  // Set Sci-Fi HUD alignment and style based on message role
  if (role == Role::kUser) {
    // User / Pilot message: Tactical dark green tinted panel with bright green HUD border
    lv_obj_set_style_bg_color(msg_bubble, current_theme_.user_bubble, 0);
    lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(msg_bubble, lv_color_hex(0x22c55e), 0);  // Tactical HUD green border
    lv_obj_set_style_text_color(msg_text, current_theme_.user_text, 0);

    lv_obj_set_user_data(msg_bubble, (void*)"user");
    lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
  } else if (role == Role::kAssistant) {
    // Jarvis AI message: Deep cyan holographic glass with neon arc cyan border
    lv_obj_set_style_bg_color(msg_bubble, current_theme_.assistant_bubble, 0);
    lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(msg_bubble, current_theme_.jarvis_cyan, 0);  // Glowing Arc Cyan border
    lv_obj_set_style_text_color(msg_text, current_theme_.assistant_text, 0);

    lv_obj_set_user_data(msg_bubble, (void*)"assistant");
    lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
  } else if (role == Role::kSystem) {
    // System message: Arc reactor gold / dark telemetry
    lv_obj_set_style_bg_color(msg_bubble, current_theme_.system_bubble, 0);
    lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(msg_bubble, current_theme_.jarvis_gold, 0);  // Stark Gold border
    lv_obj_set_style_text_color(msg_text, current_theme_.system_text, 0);

    lv_obj_set_user_data(msg_bubble, (void*)"system");
    lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
  }

  // Create a full-width container for user messages to ensure right alignment
  if (role == Role::kUser) {
    lv_obj_t* container = lv_obj_create(content_);
    lv_obj_set_width(container, LV_HOR_RES);
    lv_obj_set_height(container, LV_SIZE_CONTENT);

    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);

    lv_obj_set_parent(msg_bubble, container);
    lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
  } else if (role == Role::kSystem) {
    lv_obj_t* container = lv_obj_create(content_);
    lv_obj_set_width(container, LV_HOR_RES);
    lv_obj_set_height(container, LV_SIZE_CONTENT);

    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);

    lv_obj_set_parent(msg_bubble, container);
    lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
    lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);

    // 自动滚动底部
    lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
  } else {
    // For assistant messages
    // Left align assistant messages
    lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the message bubble
    lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
  }

  // Store reference to the latest message label
  chat_message_label_ = msg_text;

  // Update virtual robot face subtitle if active
  if (subtitle_label_ != nullptr) {
    if (role == Role::kUser) {
      lv_label_set_text(subtitle_label_, ("▲ 你: " + content).c_str());
    } else if (role == Role::kAssistant) {
      lv_label_set_text(subtitle_label_, ("◈ Enco: " + content).c_str());
    } else {
      lv_label_set_text(subtitle_label_, content.c_str());
    }
  }

  lvgl_port_unlock();
}

void Display::ShowStatus(const char* status) {
  lvgl_port_lock(0);
  lv_label_set_text(status_label_, status);
  lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

  std::string s(status);
  if (s == "说话中") {
    is_speaking_ = true;
  } else {
    is_speaking_ = false;
  }

  if (subtitle_label_ != nullptr) {
    if (s == "聆听中") {
      lv_label_set_text(subtitle_label_, "👂 正在聆听你的指令...");
      UpdateRobotFaceEmotion("neutral");
      if (eye_left_ && eye_right_) {
        lv_obj_set_size(eye_left_, 54, 72);
        lv_obj_set_size(eye_right_, 54, 72);
      }
    } else if (s == "待命") {
      lv_label_set_text(subtitle_label_, "🤖 Enco 正在待命...");
      UpdateRobotFaceEmotion("neutral");
    } else if (s == "连接中...") {
      lv_label_set_text(subtitle_label_, "⚡ 正在连接小智云端...");
    } else if (s == "网络已连接") {
      lv_label_set_text(subtitle_label_, "🌐 网络已连接");
    } else if (s == "网络配置中" || s == "热点配网模式") {
      lv_label_set_text(subtitle_label_, "📶 请使用手机进行配网");
    }
  }

  lvgl_port_unlock();
}

void Display::SetEmotion(const std::string& emotion) {
  std::map<std::string, const char*> emotion_map = {
      {"neutral", "😶"}, {"happy", "🙂"},       {"laughing", "😆"},  {"funny", "😂"},     {"sad", "😔"},      {"angry", "😠"},   {"crying", "😭"},
      {"loving", "😍"},  {"embarrassed", "😳"}, {"surprised", "😯"}, {"shocked", "😱"},   {"thinking", "🤔"}, {"winking", "😉"}, {"cool", "😎"},
      {"relaxed", "😌"}, {"delicious", "🤤"},   {"kissy", "😘"},     {"confident", "😏"}, {"sleepy", "😴"},   {"silly", "😜"},   {"confused", "🙄"},
  };

  auto it = emotion_map.find(emotion);

  lvgl_port_lock(0);
  if (emotion_label_ != nullptr) {
    lv_obj_set_style_text_font(emotion_label_, font_emoji_32_init(), 0);
    if (it != emotion_map.end()) {
      lv_label_set_text(emotion_label_, it->second);
    } else {
      lv_label_set_text(emotion_label_, "😶");
    }
  }

  UpdateRobotFaceEmotion(emotion);
  lvgl_port_unlock();
}

void Display::SetUiMode(UiMode mode) {
  lvgl_port_lock(0);
  ui_mode_ = mode;
  if (ui_mode_ == UiMode::kRobotFace) {
    if (content_) lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
    if (face_container_) lv_obj_clear_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
  } else {
    if (face_container_) lv_obj_add_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
    if (content_) lv_obj_clear_flag(content_, LV_OBJ_FLAG_HIDDEN);
  }
  lvgl_port_unlock();
}

void Display::ToggleUiMode() {
  if (ui_mode_ == UiMode::kRobotFace) {
    SetUiMode(UiMode::kChatText);
  } else {
    SetUiMode(UiMode::kRobotFace);
  }
}

void Display::UpdateRobotFaceEmotion(const std::string& emotion) {
  current_emotion_ = emotion;
  if (!eye_left_ || !eye_right_) return;

  // Reset default geometry
  current_eye_width_ = 50;
  current_eye_height_ = 68;
  current_eye_color_ = current_theme_.jarvis_cyan; // #00f0ff
  bool show_blush = false;
  bool show_pupil = true;

  if (emotion == "happy" || emotion == "laughing" || emotion == "funny") {
    // Happy squint: wide crescent slits with glowing smile
    current_eye_height_ = 20;
    current_eye_width_ = 52;
    current_eye_color_ = lv_color_hex(0x38bdf8);
    show_blush = true;
    show_pupil = false;
  } else if (emotion == "loving" || emotion == "kissy") {
    // Warm glowing rose eyes + blush
    current_eye_height_ = 48;
    current_eye_width_ = 52;
    current_eye_color_ = lv_color_hex(0xf43f5e); // Rose Pink
    show_blush = true;
    show_pupil = true;
  } else if (emotion == "sad" || emotion == "crying") {
    // Droopy sad eyes
    current_eye_height_ = 36;
    current_eye_width_ = 48;
    current_eye_color_ = lv_color_hex(0x0284c7); // Deep tear blue
    show_blush = false;
  } else if (emotion == "angry") {
    // Fierce sharp glowing orange-red
    current_eye_height_ = 32;
    current_eye_width_ = 52;
    current_eye_color_ = lv_color_hex(0xef4444); // Crimson red
    show_blush = false;
  } else if (emotion == "surprised" || emotion == "shocked") {
    // Big dilated circular eyes (O O)
    current_eye_height_ = 64;
    current_eye_width_ = 64;
    current_eye_color_ = lv_color_hex(0xa5f3fc); // Intense bright cyan
    show_blush = false;
  } else if (emotion == "thinking" || emotion == "confused") {
    // Curious inquisitive expression
    current_eye_height_ = 50;
    current_eye_color_ = current_theme_.jarvis_gold; // Stark Gold
    show_blush = false;
  } else if (emotion == "cool" || emotion == "confident") {
    // Narrow confident visor eyes
    current_eye_height_ = 24;
    current_eye_width_ = 56;
    current_eye_color_ = current_theme_.jarvis_cyan;
    show_blush = false;
    show_pupil = false;
  } else if (emotion == "sleepy") {
    // Closed peaceful lines
    current_eye_height_ = 4;
    current_eye_width_ = 46;
    current_eye_color_ = current_theme_.jarvis_cyan_dim;
    show_blush = false;
    show_pupil = false;
  } else if (emotion == "winking") {
    // Wink: Left eye open, right eye closed line
    lv_obj_set_size(eye_left_, 50, 68);
    lv_obj_set_size(eye_right_, 50, 4);
    lv_obj_set_style_bg_color(eye_left_, current_theme_.jarvis_cyan, 0);
    lv_obj_set_style_bg_color(eye_right_, current_theme_.jarvis_cyan, 0);
    if (pupil_left_) lv_obj_clear_flag(pupil_left_, LV_OBJ_FLAG_HIDDEN);
    if (pupil_right_) lv_obj_add_flag(pupil_right_, LV_OBJ_FLAG_HIDDEN);
    if (blush_left_) lv_obj_clear_flag(blush_left_, LV_OBJ_FLAG_HIDDEN);
    if (blush_right_) lv_obj_clear_flag(blush_right_, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  // Apply geometry
  lv_obj_set_size(eye_left_, current_eye_width_, current_eye_height_);
  lv_obj_set_size(eye_right_, current_eye_width_, current_eye_height_);
  lv_obj_set_style_bg_color(eye_left_, current_eye_color_, 0);
  lv_obj_set_style_bg_color(eye_right_, current_eye_color_, 0);

  if (pupil_left_) {
    if (show_pupil) lv_obj_clear_flag(pupil_left_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(pupil_left_, LV_OBJ_FLAG_HIDDEN);
  }
  if (pupil_right_) {
    if (show_pupil) lv_obj_clear_flag(pupil_right_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(pupil_right_, LV_OBJ_FLAG_HIDDEN);
  }

  if (blush_left_) {
    if (show_blush) lv_obj_clear_flag(blush_left_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(blush_left_, LV_OBJ_FLAG_HIDDEN);
  }
  if (blush_right_) {
    if (show_blush) lv_obj_clear_flag(blush_right_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(blush_right_, LV_OBJ_FLAG_HIDDEN);
  }
}

void Display::OnBlinkTimer(lv_timer_t* timer) {
  auto* self = static_cast<Display*>(lv_timer_get_user_data(timer));
  if (!self || self->ui_mode_ != UiMode::kRobotFace) return;
  if (self->current_emotion_ == "sleepy" || self->current_emotion_ == "winking") return;
  if (!self->eye_left_ || !self->eye_right_) return;

  // Quick cute blink
  lv_obj_set_height(self->eye_left_, 4);
  lv_obj_set_height(self->eye_right_, 4);
  if (self->pupil_left_) lv_obj_add_flag(self->pupil_left_, LV_OBJ_FLAG_HIDDEN);
  if (self->pupil_right_) lv_obj_add_flag(self->pupil_right_, LV_OBJ_FLAG_HIDDEN);

  lv_timer_t* restore_timer = lv_timer_create(
      [](lv_timer_t* t) {
        auto* d = static_cast<Display*>(lv_timer_get_user_data(t));
        if (d && d->ui_mode_ == UiMode::kRobotFace && d->eye_left_ && d->eye_right_) {
          lv_obj_set_height(d->eye_left_, d->current_eye_height_);
          lv_obj_set_height(d->eye_right_, d->current_eye_height_);
          if (d->pupil_left_) lv_obj_clear_flag(d->pupil_left_, LV_OBJ_FLAG_HIDDEN);
          if (d->pupil_right_) lv_obj_clear_flag(d->pupil_right_, LV_OBJ_FLAG_HIDDEN);
        }
      },
      120, self);
  lv_timer_set_repeat_count(restore_timer, 1);
  lv_timer_set_auto_delete(restore_timer, true);
}

void Display::OnVoiceAnimTimer(lv_timer_t* timer) {
  auto* self = static_cast<Display*>(lv_timer_get_user_data(timer));
  if (!self || self->ui_mode_ != UiMode::kRobotFace) return;

  static uint8_t anim_step = 0;
  anim_step = (anim_step + 1) % 6;

  if (self->is_speaking_) {
    const int heights[6][5] = {
        {6, 16, 24, 14, 8},
        {12, 22, 10, 20, 14},
        {18, 12, 26, 16, 6},
        {8, 24, 14, 22, 12},
        {14, 18, 20, 10, 18},
        {20, 10, 16, 24, 8},
    };
    for (int i = 0; i < 5; ++i) {
      if (self->wave_bars_[i]) {
        lv_obj_set_height(self->wave_bars_[i], heights[anim_step][i]);
      }
    }
  } else {
    for (int i = 0; i < 5; ++i) {
      if (self->wave_bars_[i]) {
        lv_obj_set_height(self->wave_bars_[i], 6);
      }
    }
  }
}
