#include <esp_log.h>
#include <esp_system.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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

  // The 2D avatar is ~45 LVGL objects (>20KB of heap on this no-PSRAM board). Build it only
  // when face mode is actually selected so the TLS handshake has enough RAM at boot.
  if (ui_mode_ == UiMode::kRobotFace) {
    BuildRobotFace();
  }


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

// Builds the whole procedural 2D anime avatar. Split out of Start() and called lazily: on an
// ESP32 without PSRAM these objects are the difference between a successful and a failed TLS
// handshake at boot. Caller must already hold the LVGL lock.
void Display::BuildRobotFace() {
  if (face_built_) {
    return;
  }
  face_built_ = true;

  /* Virtual 2D Anime Avatar Container (Procedural Vector Anime Girl) */
  face_container_ = lv_obj_create(container_);
  lv_obj_set_style_radius(face_container_, 0, 0);
  lv_obj_set_width(face_container_, LV_HOR_RES);
  lv_obj_set_flex_grow(face_container_, 1);
  lv_obj_set_style_pad_all(face_container_, 0, 0);
  lv_obj_set_style_bg_color(face_container_, lv_color_hex(0x0a0c18), 0);
  lv_obj_set_style_bg_grad_color(face_container_, lv_color_hex(0x191630), 0);
  lv_obj_set_style_bg_grad_dir(face_container_, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(face_container_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(face_container_, 1, 0);
  lv_obj_set_style_border_color(face_container_, lv_color_hex(0x38bdf8), 0);
  lv_obj_set_style_border_side(face_container_, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_scrollbar_mode(face_container_, LV_SCROLLBAR_MODE_OFF);

  if (ui_mode_ == UiMode::kChatText) {
    lv_obj_add_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
  }

  // 1. Anime Hairstyle Layer (Back hair / Side strands 姬发)
  side_hair_left_ = lv_obj_create(face_container_);
  lv_obj_set_size(side_hair_left_, 8, 120);
  lv_obj_align(side_hair_left_, LV_ALIGN_TOP_LEFT, 6, 26);
  lv_obj_set_style_radius(side_hair_left_, 4, 0);
  lv_obj_set_style_bg_color(side_hair_left_, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_bg_opa(side_hair_left_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(side_hair_left_, 0, 0);
  lv_obj_set_scrollbar_mode(side_hair_left_, LV_SCROLLBAR_MODE_OFF);

  side_hair_right_ = lv_obj_create(face_container_);
  lv_obj_set_size(side_hair_right_, 8, 120);
  lv_obj_align(side_hair_right_, LV_ALIGN_TOP_RIGHT, -6, 26);
  lv_obj_set_style_radius(side_hair_right_, 4, 0);
  lv_obj_set_style_bg_color(side_hair_right_, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_bg_opa(side_hair_right_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(side_hair_right_, 0, 0);
  lv_obj_set_scrollbar_mode(side_hair_right_, LV_SCROLLBAR_MODE_OFF);

  // 2. Ahoge (呆毛 - 顶端动态自然摇曳)
  ahoge_ = lv_obj_create(face_container_);
  lv_obj_set_size(ahoge_, 12, 28);
  lv_obj_align(ahoge_, LV_ALIGN_TOP_MID, 0, 2);
  lv_obj_set_style_radius(ahoge_, 6, 0);
  lv_obj_set_style_bg_color(ahoge_, lv_color_hex(0x475569), 0);
  lv_obj_set_style_bg_opa(ahoge_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ahoge_, 0, 0);
  lv_obj_set_scrollbar_mode(ahoge_, LV_SCROLLBAR_MODE_OFF);

  // 3. Eyebrows (灵动眉毛 - 轻盈低开销)
  eyebrow_left_ = lv_obj_create(face_container_);
  lv_obj_set_size(eyebrow_left_, 30, 4);
  lv_obj_align(eyebrow_left_, LV_ALIGN_TOP_MID, -46, 46);
  lv_obj_set_style_radius(eyebrow_left_, 2, 0);
  lv_obj_set_style_bg_color(eyebrow_left_, lv_color_hex(0x64748b), 0);
  lv_obj_set_style_bg_opa(eyebrow_left_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(eyebrow_left_, 0, 0);
  lv_obj_set_scrollbar_mode(eyebrow_left_, LV_SCROLLBAR_MODE_OFF);

  eyebrow_right_ = lv_obj_create(face_container_);
  lv_obj_set_size(eyebrow_right_, 30, 4);
  lv_obj_align(eyebrow_right_, LV_ALIGN_TOP_MID, 46, 46);
  lv_obj_set_style_radius(eyebrow_right_, 2, 0);
  lv_obj_set_style_bg_color(eyebrow_right_, lv_color_hex(0x64748b), 0);
  lv_obj_set_style_bg_opa(eyebrow_right_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(eyebrow_right_, 0, 0);
  lv_obj_set_scrollbar_mode(eyebrow_right_, LV_SCROLLBAR_MODE_OFF);

  // 4. Expressive Anime Eyes (二次元精细矢量大眼睛)
  eye_box_ = lv_obj_create(face_container_);
  lv_obj_set_size(eye_box_, 210, 96);
  lv_obj_align(eye_box_, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_style_bg_opa(eye_box_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(eye_box_, 0, 0);
  lv_obj_set_style_pad_all(eye_box_, 0, 0);
  lv_obj_set_scrollbar_mode(eye_box_, LV_SCROLLBAR_MODE_OFF);

  // Left Eye Sclera (眼白)
  eye_left_ = lv_obj_create(eye_box_);
  lv_obj_set_size(eye_left_, current_eye_width_, current_eye_height_);
  lv_obj_align(eye_left_, LV_ALIGN_CENTER, -46, 0);
  lv_obj_set_style_radius(eye_left_, 20, 0);
  lv_obj_set_style_bg_color(eye_left_, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(eye_left_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(eye_left_, 2, 0);
  lv_obj_set_style_border_color(eye_left_, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_pad_all(eye_left_, 0, 0);
  lv_obj_set_scrollbar_mode(eye_left_, LV_SCROLLBAR_MODE_OFF);

  // Left Iris (深度渐变虹膜)
  iris_left_ = lv_obj_create(eye_left_);
  lv_obj_set_size(iris_left_, 40, 58);
  lv_obj_align(iris_left_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(iris_left_, 18, 0);
  lv_obj_set_style_bg_color(iris_left_, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_bg_grad_color(iris_left_, lv_color_hex(0x06b6d4), 0);
  lv_obj_set_style_bg_grad_dir(iris_left_, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(iris_left_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(iris_left_, 0, 0);
  lv_obj_set_style_pad_all(iris_left_, 0, 0);
  lv_obj_set_scrollbar_mode(iris_left_, LV_SCROLLBAR_MODE_OFF);

  // Left Pupil (深色瞳孔)
  pupil_left_ = lv_obj_create(iris_left_);
  lv_obj_set_size(pupil_left_, 16, 22);
  lv_obj_align(pupil_left_, LV_ALIGN_CENTER, 0, -2);
  lv_obj_set_style_radius(pupil_left_, 8, 0);
  lv_obj_set_style_bg_color(pupil_left_, lv_color_hex(0x020617), 0);
  lv_obj_set_style_bg_opa(pupil_left_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(pupil_left_, 0, 0);
  lv_obj_set_scrollbar_mode(pupil_left_, LV_SCROLLBAR_MODE_OFF);

  // Left Sparkle 1 (主高光)
  sparkle1_left_ = lv_obj_create(iris_left_);
  lv_obj_set_size(sparkle1_left_, 10, 10);
  lv_obj_align(sparkle1_left_, LV_ALIGN_TOP_LEFT, 5, 5);
  lv_obj_set_style_radius(sparkle1_left_, 5, 0);
  lv_obj_set_style_bg_color(sparkle1_left_, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(sparkle1_left_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(sparkle1_left_, 0, 0);
  lv_obj_set_scrollbar_mode(sparkle1_left_, LV_SCROLLBAR_MODE_OFF);

  // Left Sparkle 2 (次高光)
  sparkle2_left_ = lv_obj_create(iris_left_);
  lv_obj_set_size(sparkle2_left_, 5, 5);
  lv_obj_align(sparkle2_left_, LV_ALIGN_BOTTOM_RIGHT, -6, -8);
  lv_obj_set_style_radius(sparkle2_left_, 2, 0);
  lv_obj_set_style_bg_color(sparkle2_left_, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(sparkle2_left_, LV_OPA_80, 0);
  lv_obj_set_style_border_width(sparkle2_left_, 0, 0);
  lv_obj_set_scrollbar_mode(sparkle2_left_, LV_SCROLLBAR_MODE_OFF);

  // Left Iris Glow (底部虹膜月牙透光)
  iris_glow_left_ = lv_obj_create(iris_left_);
  lv_obj_set_size(iris_glow_left_, 24, 6);
  lv_obj_align(iris_glow_left_, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_set_style_radius(iris_glow_left_, 3, 0);
  lv_obj_set_style_bg_color(iris_glow_left_, lv_color_hex(0xa5f3fc), 0);
  lv_obj_set_style_bg_opa(iris_glow_left_, LV_OPA_70, 0);
  lv_obj_set_style_border_width(iris_glow_left_, 0, 0);
  lv_obj_set_scrollbar_mode(iris_glow_left_, LV_SCROLLBAR_MODE_OFF);

  // Left Upper Eyelash (上眼睫毛上挑外展)
  eyelash_left_ = lv_obj_create(eye_box_);
  lv_obj_set_size(eyelash_left_, 54, 5);
  lv_obj_align(eyelash_left_, LV_ALIGN_CENTER, -46, -34);
  lv_obj_set_style_radius(eyelash_left_, 2, 0);
  lv_obj_set_style_bg_color(eyelash_left_, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_bg_opa(eyelash_left_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(eyelash_left_, 0, 0);
  lv_obj_set_scrollbar_mode(eyelash_left_, LV_SCROLLBAR_MODE_OFF);

  // Right Eye Sclera (眼白)
  eye_right_ = lv_obj_create(eye_box_);
  lv_obj_set_size(eye_right_, current_eye_width_, current_eye_height_);
  lv_obj_align(eye_right_, LV_ALIGN_CENTER, 46, 0);
  lv_obj_set_style_radius(eye_right_, 20, 0);
  lv_obj_set_style_bg_color(eye_right_, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(eye_right_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(eye_right_, 2, 0);
  lv_obj_set_style_border_color(eye_right_, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_pad_all(eye_right_, 0, 0);
  lv_obj_set_scrollbar_mode(eye_right_, LV_SCROLLBAR_MODE_OFF);

  // Right Iris (深度渐变虹膜)
  iris_right_ = lv_obj_create(eye_right_);
  lv_obj_set_size(iris_right_, 40, 58);
  lv_obj_align(iris_right_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(iris_right_, 18, 0);
  lv_obj_set_style_bg_color(iris_right_, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_bg_grad_color(iris_right_, lv_color_hex(0x06b6d4), 0);
  lv_obj_set_style_bg_grad_dir(iris_right_, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(iris_right_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(iris_right_, 0, 0);
  lv_obj_set_style_pad_all(iris_right_, 0, 0);
  lv_obj_set_scrollbar_mode(iris_right_, LV_SCROLLBAR_MODE_OFF);

  // Right Pupil (深色瞳孔)
  pupil_right_ = lv_obj_create(iris_right_);
  lv_obj_set_size(pupil_right_, 16, 22);
  lv_obj_align(pupil_right_, LV_ALIGN_CENTER, 0, -2);
  lv_obj_set_style_radius(pupil_right_, 8, 0);
  lv_obj_set_style_bg_color(pupil_right_, lv_color_hex(0x020617), 0);
  lv_obj_set_style_bg_opa(pupil_right_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(pupil_right_, 0, 0);
  lv_obj_set_scrollbar_mode(pupil_right_, LV_SCROLLBAR_MODE_OFF);

  // Right Sparkle 1 (主高光)
  sparkle1_right_ = lv_obj_create(iris_right_);
  lv_obj_set_size(sparkle1_right_, 10, 10);
  lv_obj_align(sparkle1_right_, LV_ALIGN_TOP_LEFT, 5, 5);
  lv_obj_set_style_radius(sparkle1_right_, 5, 0);
  lv_obj_set_style_bg_color(sparkle1_right_, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(sparkle1_right_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(sparkle1_right_, 0, 0);
  lv_obj_set_scrollbar_mode(sparkle1_right_, LV_SCROLLBAR_MODE_OFF);

  // Right Sparkle 2 (次高光)
  sparkle2_right_ = lv_obj_create(iris_right_);
  lv_obj_set_size(sparkle2_right_, 5, 5);
  lv_obj_align(sparkle2_right_, LV_ALIGN_BOTTOM_RIGHT, -6, -8);
  lv_obj_set_style_radius(sparkle2_right_, 2, 0);
  lv_obj_set_style_bg_color(sparkle2_right_, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(sparkle2_right_, LV_OPA_80, 0);
  lv_obj_set_style_border_width(sparkle2_right_, 0, 0);
  lv_obj_set_scrollbar_mode(sparkle2_right_, LV_SCROLLBAR_MODE_OFF);

  // Right Iris Glow (底部虹膜月牙透光)
  iris_glow_right_ = lv_obj_create(iris_right_);
  lv_obj_set_size(iris_glow_right_, 24, 6);
  lv_obj_align(iris_glow_right_, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_set_style_radius(iris_glow_right_, 3, 0);
  lv_obj_set_style_bg_color(iris_glow_right_, lv_color_hex(0xa5f3fc), 0);
  lv_obj_set_style_bg_opa(iris_glow_right_, LV_OPA_70, 0);
  lv_obj_set_style_border_width(iris_glow_right_, 0, 0);
  lv_obj_set_scrollbar_mode(iris_glow_right_, LV_SCROLLBAR_MODE_OFF);

  // Right Upper Eyelash (上眼睫毛上挑外展)
  eyelash_right_ = lv_obj_create(eye_box_);
  lv_obj_set_size(eyelash_right_, 54, 5);
  lv_obj_align(eyelash_right_, LV_ALIGN_CENTER, 46, -34);
  lv_obj_set_style_radius(eyelash_right_, 2, 0);
  lv_obj_set_style_bg_color(eyelash_right_, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_bg_opa(eyelash_right_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(eyelash_right_, 0, 0);
  lv_obj_set_scrollbar_mode(eyelash_right_, LV_SCROLLBAR_MODE_OFF);

  // 5. Hair Bangs & Accessories (Overhead Front Layer 前刘海与发夹)
  hair_bang_left_ = lv_obj_create(face_container_);
  lv_obj_set_size(hair_bang_left_, 44, 22);
  lv_obj_align(hair_bang_left_, LV_ALIGN_TOP_MID, -42, 12);
  lv_obj_set_style_radius(hair_bang_left_, 8, 0);
  lv_obj_set_style_bg_color(hair_bang_left_, lv_color_hex(0x283347), 0);
  lv_obj_set_style_bg_opa(hair_bang_left_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hair_bang_left_, 0, 0);
  lv_obj_set_scrollbar_mode(hair_bang_left_, LV_SCROLLBAR_MODE_OFF);

  hair_bang_right_ = lv_obj_create(face_container_);
  lv_obj_set_size(hair_bang_right_, 44, 22);
  lv_obj_align(hair_bang_right_, LV_ALIGN_TOP_MID, 42, 12);
  lv_obj_set_style_radius(hair_bang_right_, 8, 0);
  lv_obj_set_style_bg_color(hair_bang_right_, lv_color_hex(0x283347), 0);
  lv_obj_set_style_bg_opa(hair_bang_right_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hair_bang_right_, 0, 0);
  lv_obj_set_scrollbar_mode(hair_bang_right_, LV_SCROLLBAR_MODE_OFF);

  hair_bang_center_ = lv_obj_create(face_container_);
  lv_obj_set_size(hair_bang_center_, 38, 26);
  lv_obj_align(hair_bang_center_, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_set_style_radius(hair_bang_center_, 10, 0);
  lv_obj_set_style_bg_color(hair_bang_center_, lv_color_hex(0x334155), 0);
  lv_obj_set_style_bg_opa(hair_bang_center_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hair_bang_center_, 0, 0);
  lv_obj_set_scrollbar_mode(hair_bang_center_, LV_SCROLLBAR_MODE_OFF);

  // Hair Gloss Shine (高光发丝光环)
  hair_shine_ = lv_obj_create(face_container_);
  lv_obj_set_size(hair_shine_, 64, 3);
  lv_obj_align(hair_shine_, LV_ALIGN_TOP_MID, 0, 18);
  lv_obj_set_style_radius(hair_shine_, 2, 0);
  lv_obj_set_style_bg_color(hair_shine_, lv_color_hex(0x818cf8), 0);
  lv_obj_set_style_bg_opa(hair_shine_, LV_OPA_70, 0);
  lv_obj_set_style_border_width(hair_shine_, 0, 0);
  lv_obj_set_scrollbar_mode(hair_shine_, LV_SCROLLBAR_MODE_OFF);

  // Hair Clip (红色发夹装饰)
  hair_clip_ = lv_obj_create(face_container_);
  lv_obj_set_size(hair_clip_, 14, 6);
  lv_obj_align(hair_clip_, LV_ALIGN_TOP_MID, 54, 18);
  lv_obj_set_style_radius(hair_clip_, 2, 0);
  lv_obj_set_style_bg_color(hair_clip_, lv_color_hex(0xf43f5e), 0);
  lv_obj_set_style_bg_opa(hair_clip_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hair_clip_, 0, 0);
  lv_obj_set_scrollbar_mode(hair_clip_, LV_SCROLLBAR_MODE_OFF);

  // 6. Cheeks & Blush (软萌粉色腮红与斜线)
  blush_left_ = lv_obj_create(face_container_);
  lv_obj_set_size(blush_left_, 30, 12);
  lv_obj_align(blush_left_, LV_ALIGN_TOP_MID, -48, 134);
  lv_obj_set_style_radius(blush_left_, 6, 0);
  lv_obj_set_style_bg_color(blush_left_, lv_color_hex(0xfb7185), 0);
  lv_obj_set_style_bg_opa(blush_left_, LV_OPA_60, 0);
  lv_obj_set_style_border_width(blush_left_, 0, 0);
  lv_obj_set_scrollbar_mode(blush_left_, LV_SCROLLBAR_MODE_OFF);

  blush_lines_left_ = lv_label_create(blush_left_);
  lv_label_set_text(blush_lines_left_, "///");
  lv_obj_align(blush_lines_left_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_text_color(blush_lines_left_, lv_color_hex(0xe11d48), 0);
  lv_obj_set_style_text_opa(blush_lines_left_, LV_OPA_80, 0);

  blush_right_ = lv_obj_create(face_container_);
  lv_obj_set_size(blush_right_, 30, 12);
  lv_obj_align(blush_right_, LV_ALIGN_TOP_MID, 48, 134);
  lv_obj_set_style_radius(blush_right_, 6, 0);
  lv_obj_set_style_bg_color(blush_right_, lv_color_hex(0xfb7185), 0);
  lv_obj_set_style_bg_opa(blush_right_, LV_OPA_60, 0);
  lv_obj_set_style_border_width(blush_right_, 0, 0);
  lv_obj_set_scrollbar_mode(blush_right_, LV_SCROLLBAR_MODE_OFF);

  blush_lines_right_ = lv_label_create(blush_right_);
  lv_label_set_text(blush_lines_right_, "///");
  lv_obj_align(blush_lines_right_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_text_color(blush_lines_right_, lv_color_hex(0xe11d48), 0);
  lv_obj_set_style_text_opa(blush_lines_right_, LV_OPA_80, 0);

  // 7. Mouth Area (甜美微笑弧度 & 实时张嘴对口型动画)
  mouth_box_ = lv_obj_create(face_container_);
  lv_obj_set_size(mouth_box_, 60, 36);
  lv_obj_align(mouth_box_, LV_ALIGN_TOP_MID, 0, 154);
  lv_obj_set_style_bg_opa(mouth_box_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(mouth_box_, 0, 0);
  lv_obj_set_style_pad_all(mouth_box_, 0, 0);
  lv_obj_set_scrollbar_mode(mouth_box_, LV_SCROLLBAR_MODE_OFF);

  // Idle Smile Arc (Clean, solid rounded pill shape - zero layer overhead, zero mask bugs)
  mouth_smile_ = lv_obj_create(mouth_box_);
  lv_obj_set_size(mouth_smile_, 16, 4);
  lv_obj_align(mouth_smile_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(mouth_smile_, 2, 0);
  lv_obj_set_style_bg_color(mouth_smile_, lv_color_hex(0xf43f5e), 0);
  lv_obj_set_style_bg_opa(mouth_smile_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(mouth_smile_, 0, 0);
  lv_obj_set_scrollbar_mode(mouth_smile_, LV_SCROLLBAR_MODE_OFF);

  // Talking Open Anime Mouth
  anime_mouth_ = lv_obj_create(mouth_box_);
  lv_obj_set_size(anime_mouth_, 22, 16);
  lv_obj_align(anime_mouth_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(anime_mouth_, 8, 0);
  lv_obj_set_style_bg_color(anime_mouth_, lv_color_hex(0x4c0519), 0);
  lv_obj_set_style_bg_opa(anime_mouth_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(anime_mouth_, 0, 0);
  lv_obj_set_style_pad_all(anime_mouth_, 0, 0);
  lv_obj_set_scrollbar_mode(anime_mouth_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(anime_mouth_, LV_OBJ_FLAG_HIDDEN);

  // Upper tooth inside open mouth
  mouth_tooth_ = lv_obj_create(anime_mouth_);
  lv_obj_set_size(mouth_tooth_, 10, 3);
  lv_obj_align(mouth_tooth_, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_radius(mouth_tooth_, 2, 0);
  lv_obj_set_style_bg_color(mouth_tooth_, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(mouth_tooth_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(mouth_tooth_, 0, 0);
  lv_obj_set_scrollbar_mode(mouth_tooth_, LV_SCROLLBAR_MODE_OFF);

  // Pink tongue inside open mouth
  mouth_tongue_ = lv_obj_create(anime_mouth_);
  lv_obj_set_size(mouth_tongue_, 14, 6);
  lv_obj_align(mouth_tongue_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_radius(mouth_tongue_, 3, 0);
  lv_obj_set_style_bg_color(mouth_tongue_, lv_color_hex(0xfb7185), 0);
  lv_obj_set_style_bg_opa(mouth_tongue_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(mouth_tongue_, 0, 0);
  lv_obj_set_scrollbar_mode(mouth_tongue_, LV_SCROLLBAR_MODE_OFF);

  // 8. Floating Emote Badge (悬浮情绪小气泡)
  emote_badge_ = lv_label_create(face_container_);
  lv_obj_set_style_text_font(emote_badge_, font_emoji_32_init(), 0);
  lv_obj_align(emote_badge_, LV_ALIGN_TOP_RIGHT, -18, 38);
  lv_obj_add_flag(emote_badge_, LV_OBJ_FLAG_HIDDEN);

  // 9. Frosted Glass Subtitle Banner (半透明磨砂对话框)
  subtitle_box_ = lv_obj_create(face_container_);
  lv_obj_set_size(subtitle_box_, 224, 56);
  lv_obj_align(subtitle_box_, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_radius(subtitle_box_, 10, 0);
  lv_obj_set_style_bg_color(subtitle_box_, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_bg_opa(subtitle_box_, LV_OPA_80, 0);
  lv_obj_set_style_border_width(subtitle_box_, 1, 0);
  lv_obj_set_style_border_color(subtitle_box_, lv_color_hex(0x38bdf8), 0);
  lv_obj_set_style_pad_all(subtitle_box_, 6, 0);
  lv_obj_set_scrollbar_mode(subtitle_box_, LV_SCROLLBAR_MODE_OFF);

  subtitle_label_ = lv_label_create(subtitle_box_);
  lv_obj_set_width(subtitle_label_, 210);
  lv_obj_set_style_text_font(subtitle_label_, &font_puhui_16_4, 0);
  lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xf1f5f9), 0);
  lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(subtitle_label_, "🌸 Enco 正在待命...");

  // Natural Blinking, Voice Lipsync & Ahoge Swaying Timers (Efficient rates)
  blink_timer_ = lv_timer_create(OnBlinkTimer, 3500, this);
  voice_anim_timer_ = lv_timer_create(OnVoiceAnimTimer, 120, this);
  ahoge_timer_ = lv_timer_create(OnAhogeTimer, 120, this);
}

// Each retained message is an LVGL container plus a wrapped UTF-8 label. With the TLS session and
// the audio pipeline live there is very little heap left, so keep the visible history short.
#define MAX_MESSAGES (3)
void Display::SetChatMessage(const Role role, const std::string& content) {
  if (content.empty()) {
    return;
  }

  // Never let the transcript be the thing that pushes the device over the edge: the conversation
  // itself (audio + WebSocket) matters more than the on-screen history.
  if (esp_get_free_heap_size() < 14000) {
    printf("[display] skipping chat message, free heap: %u\n", static_cast<unsigned>(esp_get_free_heap_size()));
    return;
  }

  lvgl_port_lock(0);

  // 检查消息数量是否超过限制
  uint32_t child_count = lv_obj_get_child_cnt(content_);
  while (child_count >= MAX_MESSAGES) {
    // 删除最早的消息（第一个子对象）
    lv_obj_t* first_child = lv_obj_get_child(content_, 0);
    if (first_child == nullptr) {
      break;
    }
    lv_obj_del(first_child);
    child_count = lv_obj_get_child_cnt(content_);
  }
  if (child_count > 0) {
    // Scroll to the last message immediately
    lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
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

  // Update virtual anime avatar subtitle if active
  if (subtitle_label_ != nullptr) {
    if (role == Role::kUser) {
      lv_label_set_text(subtitle_label_, ("▲ 你: " + content).c_str());
    } else if (role == Role::kAssistant) {
      lv_label_set_text(subtitle_label_, ("🌸 Enco: " + content).c_str());
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
      lv_label_set_text(subtitle_label_, "🌸 Enco 正在待命...");
      UpdateRobotFaceEmotion("neutral");
    } else if (s == "连接中...") {
      lv_label_set_text(subtitle_label_, "⚡ 正在连接小智云端...");
    } else if (s == "网络已连接") {
      lv_label_set_text(subtitle_label_, "🌐 网络已连接");
    } else if (s == "网络配置中" || s == "热点配网模式") {
      lv_label_set_text(subtitle_label_, "📶 请使用手机进行配网");
    } else if (s == "抬头中...") {
      lv_label_set_text(subtitle_label_, "👀 正在抬头看上面...");
    } else if (s == "低头中...") {
      lv_label_set_text(subtitle_label_, "👀 正在低头看地面...");
    } else if (s == "向左歪头...") {
      lv_label_set_text(subtitle_label_, "🙃 向左歪头倾听...");
    } else if (s == "向右歪头...") {
      lv_label_set_text(subtitle_label_, "🙃 向右歪头倾听...");
    } else if (s == "向左转头...") {
      lv_label_set_text(subtitle_label_, "👀 向左转头看看...");
    } else if (s == "向右转头...") {
      lv_label_set_text(subtitle_label_, "👀 向右转头看看...");
    } else if (s == "摇头晃脑...") {
      lv_label_set_text(subtitle_label_, "🤪 萌动摇头晃脑中~");
    } else if (s == "头已正视") {
      lv_label_set_text(subtitle_label_, "🌸 头已摆正正视前方");
    }
  }

  lvgl_port_unlock();
}

void Display::SetEmotion(const std::string& emotion) {
  // This used to build a 21-entry std::map<std::string, const char*> on every call - roughly 1.5KB
  // of allocate-and-free churn each time the assistant changes expression. A static table costs
  // nothing at runtime and keeps the (very small) remaining heap unfragmented.
  struct EmotionIcon {
    const char* name;
    const char* icon;
  };
  static constexpr EmotionIcon kEmotions[] = {
      {"neutral", "😶"},  {"happy", "🙂"},     {"laughing", "😆"},   {"funny", "😂"},     {"sad", "😔"},
      {"angry", "😠"},    {"crying", "😭"},    {"loving", "😍"},     {"embarrassed", "😳"}, {"surprised", "😯"},
      {"shocked", "😱"},  {"thinking", "🤔"},  {"winking", "😉"},    {"cool", "😎"},      {"relaxed", "😌"},
      {"delicious", "🤤"}, {"kissy", "😘"},    {"confident", "😏"},  {"sleepy", "😴"},    {"silly", "😜"},
      {"confused", "🙄"},
  };

  const char* icon = "😶";
  for (const auto& entry : kEmotions) {
    if (emotion == entry.name) {
      icon = entry.icon;
      break;
    }
  }

  lvgl_port_lock(0);
  if (emotion_label_ != nullptr) {
    lv_obj_set_style_text_font(emotion_label_, font_emoji_32_init(), 0);
    lv_label_set_text(emotion_label_, icon);
  }

  UpdateRobotFaceEmotion(emotion);
  lvgl_port_unlock();
}

void Display::SetUiMode(UiMode mode) {
  lvgl_port_lock(0);
  if (mode == UiMode::kRobotFace && !face_built_) {
    // Building the avatar needs roughly 20KB. Refuse (and stay in text mode) rather than let LVGL
    // hand out nullptrs halfway through and take the whole device down.
    if (esp_get_free_heap_size() < 45000) {
      printf("[display] not enough heap for face mode (free: %u), staying in text mode\n", static_cast<unsigned>(esp_get_free_heap_size()));
      lvgl_port_unlock();
      return;
    }
    BuildRobotFace();
    printf("[display] face built, free heap: %u\n", static_cast<unsigned>(esp_get_free_heap_size()));
  }

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
  if (!eye_left_ || !eye_right_ || !iris_left_ || !iris_right_) return;

  // Default geometry & styles for 2D Anime Avatar
  current_eye_width_ = 50;
  current_eye_height_ = 68;
  lv_color_t iris_top = lv_color_hex(0x0f172a);
  lv_color_t iris_bot = lv_color_hex(0x06b6d4);
  int eyebrow_y = 46;
  bool show_blush = false;
  bool show_emote = false;
  const char* emote_icon = "";

  if (emotion == "happy" || emotion == "laughing" || emotion == "funny") {
    // Happy sparkling eyes + rosy blush
    current_eye_height_ = 60;
    iris_top = lv_color_hex(0x0369a1);
    iris_bot = lv_color_hex(0x38bdf8);
    eyebrow_y = 42;
    show_blush = true;
    show_emote = true;
    emote_icon = "🙂";
  } else if (emotion == "loving" || emotion == "kissy") {
    // Warm romantic magenta-rose eyes + deep blush
    current_eye_height_ = 66;
    iris_top = lv_color_hex(0x831843);
    iris_bot = lv_color_hex(0xfb7185);
    eyebrow_y = 44;
    show_blush = true;
    show_emote = true;
    emote_icon = (emotion == "kissy") ? "😘" : "😍";
  } else if (emotion == "sad" || emotion == "crying") {
    // Droopy sad eyes, blue-grey
    current_eye_height_ = 54;
    iris_top = lv_color_hex(0x1e293b);
    iris_bot = lv_color_hex(0x0284c7);
    eyebrow_y = 50;
    show_emote = true;
    emote_icon = (emotion == "crying") ? "😭" : "😔";
  } else if (emotion == "angry") {
    // Fierce sharp glowing crimson eyes + lowered eyebrows
    current_eye_height_ = 50;
    iris_top = lv_color_hex(0x450a0a);
    iris_bot = lv_color_hex(0xef4444);
    eyebrow_y = 52;
    show_emote = true;
    emote_icon = "😠";
  } else if (emotion == "surprised" || emotion == "shocked") {
    // Wide open anime eyes (O O) + raised eyebrows
    current_eye_height_ = 72;
    current_eye_width_ = 54;
    iris_top = lv_color_hex(0x082f49);
    iris_bot = lv_color_hex(0x67e8f9);
    eyebrow_y = 36;
    show_emote = true;
    emote_icon = (emotion == "shocked") ? "😱" : "😯";
  } else if (emotion == "thinking" || emotion == "confused") {
    // Inquisitive look with golden amber glow
    current_eye_height_ = 62;
    iris_top = lv_color_hex(0x1c1917);
    iris_bot = lv_color_hex(0xf59e0b);
    eyebrow_y = 42;
    show_emote = true;
    emote_icon = (emotion == "thinking") ? "🤔" : "🙄";
  } else if (emotion == "cool" || emotion == "confident") {
    // Narrow confident eyes
    current_eye_height_ = 48;
    current_eye_width_ = 52;
    iris_top = lv_color_hex(0x0f172a);
    iris_bot = lv_color_hex(0x06b6d4);
    eyebrow_y = 44;
    show_emote = true;
    emote_icon = (emotion == "cool") ? "😎" : "😏";
  } else if (emotion == "sleepy") {
    // Closed peaceful anime eye lines
    current_eye_height_ = 4;
    current_eye_width_ = 48;
    eyebrow_y = 48;
    show_emote = true;
    emote_icon = "😴";
  } else if (emotion == "winking") {
    // Wink: Left eye wide open with sparkle, right eye closed cute slit
    lv_obj_set_size(eye_left_, 52, 68);
    lv_obj_set_size(eye_right_, 50, 4);
    lv_obj_clear_flag(iris_left_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(iris_right_, LV_OBJ_FLAG_HIDDEN);
    if (blush_left_) lv_obj_clear_flag(blush_left_, LV_OBJ_FLAG_HIDDEN);
    if (blush_right_) lv_obj_clear_flag(blush_right_, LV_OBJ_FLAG_HIDDEN);
    if (eyebrow_left_) lv_obj_set_y(eyebrow_left_, 42);
    if (eyebrow_right_) lv_obj_set_y(eyebrow_right_, 46);
    if (emote_badge_) {
      lv_label_set_text(emote_badge_, "😉");
      lv_obj_clear_flag(emote_badge_, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }

  // Apply eye sclera size
  lv_obj_set_size(eye_left_, current_eye_width_, current_eye_height_);
  lv_obj_set_size(eye_right_, current_eye_width_, current_eye_height_);

  // Apply iris gradient
  lv_obj_set_style_bg_color(iris_left_, iris_top, 0);
  lv_obj_set_style_bg_grad_color(iris_left_, iris_bot, 0);
  lv_obj_set_style_bg_color(iris_right_, iris_top, 0);
  lv_obj_set_style_bg_grad_color(iris_right_, iris_bot, 0);

  if (current_eye_height_ <= 10) {
    lv_obj_add_flag(iris_left_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(iris_right_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(iris_left_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(iris_right_, LV_OBJ_FLAG_HIDDEN);
  }

  // Eyebrows position (zero layer allocation overhead)
  if (eyebrow_left_) {
    lv_obj_set_y(eyebrow_left_, eyebrow_y);
  }
  if (eyebrow_right_) {
    lv_obj_set_y(eyebrow_right_, eyebrow_y);
  }

  // Blush
  if (blush_left_) {
    if (show_blush) lv_obj_clear_flag(blush_left_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(blush_left_, LV_OBJ_FLAG_HIDDEN);
  }
  if (blush_right_) {
    if (show_blush) lv_obj_clear_flag(blush_right_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(blush_right_, LV_OBJ_FLAG_HIDDEN);
  }

  // Floating Emote Badge
  if (emote_badge_) {
    if (show_emote && emote_icon[0] != '\0') {
      lv_label_set_text(emote_badge_, emote_icon);
      lv_obj_clear_flag(emote_badge_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(emote_badge_, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void Display::LookDirection(const char* dir) {
  if (!iris_left_ || !iris_right_) return;
  lvgl_port_lock(0);
  int x_off = 0;
  int y_off = 0;
  if (strcmp(dir, "up") == 0) {
    y_off = -6;
  } else if (strcmp(dir, "down") == 0) {
    y_off = 6;
  } else if (strcmp(dir, "left") == 0) {
    x_off = -8;
  } else if (strcmp(dir, "right") == 0) {
    x_off = 8;
  }
  lv_obj_align(iris_left_, LV_ALIGN_CENTER, x_off, y_off);
  lv_obj_align(iris_right_, LV_ALIGN_CENTER, x_off, y_off);
  lvgl_port_unlock();
}

void Display::OnBlinkTimer(lv_timer_t* timer) {
  auto* self = static_cast<Display*>(lv_timer_get_user_data(timer));
  if (!self || self->ui_mode_ != UiMode::kRobotFace) return;
  if (esp_get_free_heap_size() < 12000) return;
  if (self->current_emotion_ == "sleepy" || self->current_emotion_ == "winking") return;
  if (!self->eye_left_ || !self->eye_right_) return;

  // Quick cute anime blink
  lv_obj_set_height(self->eye_left_, 4);
  lv_obj_set_height(self->eye_right_, 4);
  if (self->iris_left_) lv_obj_add_flag(self->iris_left_, LV_OBJ_FLAG_HIDDEN);
  if (self->iris_right_) lv_obj_add_flag(self->iris_right_, LV_OBJ_FLAG_HIDDEN);

  lv_timer_t* restore_timer = lv_timer_create(
      [](lv_timer_t* t) {
        auto* d = static_cast<Display*>(lv_timer_get_user_data(t));
        if (d && d->ui_mode_ == UiMode::kRobotFace && d->eye_left_ && d->eye_right_) {
          lv_obj_set_height(d->eye_left_, d->current_eye_height_);
          lv_obj_set_height(d->eye_right_, d->current_eye_height_);
          if (d->iris_left_) lv_obj_clear_flag(d->iris_left_, LV_OBJ_FLAG_HIDDEN);
          if (d->iris_right_) lv_obj_clear_flag(d->iris_right_, LV_OBJ_FLAG_HIDDEN);
        }
      },
      110, self);
  lv_timer_set_repeat_count(restore_timer, 1);
  lv_timer_set_auto_delete(restore_timer, true);
}

void Display::OnVoiceAnimTimer(lv_timer_t* timer) {
  auto* self = static_cast<Display*>(lv_timer_get_user_data(timer));
  if (!self || self->ui_mode_ != UiMode::kRobotFace) return;
  if (esp_get_free_heap_size() < 12000) return;

  static uint8_t anim_step = 0;
  anim_step = (anim_step + 1) % 6;

  if (self->is_speaking_) {
    // Talking: show open anime mouth with lively opening/closing lipsync
    if (self->mouth_smile_) lv_obj_add_flag(self->mouth_smile_, LV_OBJ_FLAG_HIDDEN);
    if (self->anime_mouth_) {
      lv_obj_clear_flag(self->anime_mouth_, LV_OBJ_FLAG_HIDDEN);
      const int mouth_h[6] = {10, 16, 12, 18, 12, 14};
      const int mouth_w[6] = {18, 22, 20, 24, 18, 20};
      lv_obj_set_size(self->anime_mouth_, mouth_w[anim_step], mouth_h[anim_step]);
    }
  } else {
    // Idle: show gentle smile arc
    if (self->anime_mouth_) lv_obj_add_flag(self->anime_mouth_, LV_OBJ_FLAG_HIDDEN);
    if (self->mouth_smile_) lv_obj_clear_flag(self->mouth_smile_, LV_OBJ_FLAG_HIDDEN);
  }
}

void Display::OnAhogeTimer(lv_timer_t* timer) {
  auto* self = static_cast<Display*>(lv_timer_get_user_data(timer));
  if (!self || self->ui_mode_ != UiMode::kRobotFace || !self->ahoge_) return;
  if (esp_get_free_heap_size() < 12000) return;

  static int tick = 0;
  tick++;

  // Smooth sinusoidal sway by shifting X offset - zero transform layer overhead!
  int amplitude = (self->current_emotion_ == "happy" || self->is_speaking_) ? 5 : 3;
  float phase = (tick % 24) * (3.14159265f * 2.0f / 24.0f);
  int x_off = static_cast<int>(sinf(phase) * amplitude);

  lv_obj_align(self->ahoge_, LV_ALIGN_TOP_MID, x_off, 2);
}
