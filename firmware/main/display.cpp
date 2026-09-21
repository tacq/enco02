#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "esp_lvgl_port.h"
#include "face_assets.h"
#include "font_awesome_symbols.h"
#include "lv_i4_decoder.h"
#include "display.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_30_4);
LV_FONT_DECLARE(font_awesome_16_4);

// Character animation tuning. One timer drives everything; 80ms is fast enough for a convincing
// blink and for mouth movement to track speech, and slow enough to stay out of the audio
// pipeline's way.
static constexpr uint32_t kFaceTickMs = 80;
// open -> half -> shut -> shut -> half -> open
static constexpr uint8_t kBlinkFrameShut = 2;
static constexpr uint8_t kBlinkFrameLast = 5;
static constexpr uint32_t kBlinkMinTicks = 30;   // 2.4s
static constexpr uint32_t kBlinkMaxTicks = 75;   // 6.0s
// Repositioning the portrait dirties the whole screen, so the idle sway steps only this often.
static constexpr uint32_t kSwayPeriodTicks = 30;  // 2.4s


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
  // The character portrait is a 16-colour image; LVGL's own decoder would expand it to 307KB of
  // ARGB8888 in RAM, so register the streaming one before anything can try to draw it.
  enco_i4_decoder_init();

  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  port_cfg.task_priority = 2;
  port_cfg.timer_period_ms = 20;
  // Left at the vendor default of 7168. Trimming this to 6144 was measured on-device to leave only
  // 836 bytes of margin once the chat UI was busy (peak usage 5,308), which is not worth 1KB.
  port_cfg.task_stack = 7168;
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

  // The bitmap character is only a few LVGL objects, so unlike the old vector avatar it can be
  // built up front without eating into what the TLS handshake needs.
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

  volume_label_ = lv_label_create(status_bar_);
  lv_label_set_text(volume_label_, "");
  lv_obj_set_style_text_font(volume_label_, &font_awesome_16_4, 0);
  lv_obj_set_style_text_color(volume_label_, current_theme_.jarvis_cyan, 0);

  network_label_ = lv_label_create(status_bar_);
  lv_label_set_text(network_label_, "");
  lv_obj_set_style_text_font(network_label_, &font_awesome_16_4, 0);
  lv_obj_set_style_text_color(network_label_, current_theme_.jarvis_cyan, 0);
  lv_obj_set_style_margin_left(network_label_, 5, 0);  // 添加左边距，与前面的元素分隔

  lvgl_port_unlock();
}

// Builds the bitmap character. The picture itself is a 240x320 16-colour image that is streamed
// straight out of flash by the decoder in lv_i4_decoder.c, so the only heap cost here is the
// handful of LVGL widget structs. Caller must already hold the LVGL lock.
//
// Animation is deliberately done by swapping small sprites over the eyes and mouth rather than by
// redrawing the whole portrait: a blink only dirties a 120x59 rectangle, which is about 3% of the
// pixels a full refresh would push over the SPI bus.
void Display::BuildRobotFace() {
  if (face_built_) {
    return;
  }
  face_built_ = true;

  face_container_ = lv_obj_create(container_);
  lv_obj_set_style_radius(face_container_, 0, 0);
  lv_obj_set_width(face_container_, LV_HOR_RES);
  lv_obj_set_flex_grow(face_container_, 1);
  lv_obj_set_style_pad_all(face_container_, 0, 0);
  lv_obj_set_style_border_width(face_container_, 0, 0);
  // Matches palette entry 0 of the portrait, so the strip below the image is seamless with it.
  lv_obj_set_style_bg_color(face_container_, lv_color_hex(0x0c1121), 0);
  lv_obj_set_style_bg_opa(face_container_, LV_OPA_COVER, 0);
  lv_obj_set_scrollbar_mode(face_container_, LV_SCROLLBAR_MODE_OFF);
  // The portrait is taller than the area left under the status bar; without this LVGL would make
  // the container scrollable and the image would drift when children are repositioned.
  lv_obj_clear_flag(face_container_, LV_OBJ_FLAG_SCROLLABLE);

  if (ui_mode_ == UiMode::kChatText) {
    lv_obj_add_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
  }

  face_image_ = lv_image_create(face_container_);
  lv_image_set_src(face_image_, &enco_face_base);
  lv_obj_set_pos(face_image_, 0, 0);

  // All overlays are opaque crops sharing the base portrait's 16-colour palette, so they composite
  // over the base with no seam. Hidden means "use whatever the base already shows there". Hair is
  // created before the eyes and mouth so blinking and speaking always sit above it in z-order.
  bangs_overlay_ = lv_image_create(face_container_);
  lv_image_set_src(bangs_overlay_, &enco_face_bangs_left);
  lv_obj_set_pos(bangs_overlay_, ENCO_FACE_BANGS_X, ENCO_FACE_BANGS_Y);
  lv_obj_add_flag(bangs_overlay_, LV_OBJ_FLAG_HIDDEN);

  locks_l_overlay_ = lv_image_create(face_container_);
  lv_image_set_src(locks_l_overlay_, &enco_face_locks_l_left);
  lv_obj_set_pos(locks_l_overlay_, ENCO_FACE_LOCKS_L_X, ENCO_FACE_LOCKS_L_Y);
  lv_obj_add_flag(locks_l_overlay_, LV_OBJ_FLAG_HIDDEN);

  locks_r_overlay_ = lv_image_create(face_container_);
  lv_image_set_src(locks_r_overlay_, &enco_face_locks_r_left);
  lv_obj_set_pos(locks_r_overlay_, ENCO_FACE_LOCKS_R_X, ENCO_FACE_LOCKS_R_Y);
  lv_obj_add_flag(locks_r_overlay_, LV_OBJ_FLAG_HIDDEN);

  eyes_overlay_ = lv_image_create(face_container_);
  lv_image_set_src(eyes_overlay_, &enco_face_eyes_shut);
  lv_obj_set_pos(eyes_overlay_, ENCO_FACE_EYES_X, ENCO_FACE_EYES_Y);
  lv_obj_add_flag(eyes_overlay_, LV_OBJ_FLAG_HIDDEN);

  mouth_overlay_ = lv_image_create(face_container_);
  lv_image_set_src(mouth_overlay_, &enco_face_mouth_small);
  lv_obj_set_pos(mouth_overlay_, ENCO_FACE_MOUTH_X, ENCO_FACE_MOUTH_Y);
  lv_obj_add_flag(mouth_overlay_, LV_OBJ_FLAG_HIDDEN);

  subtitle_box_ = lv_obj_create(face_container_);
  lv_obj_set_size(subtitle_box_, 232, 52);
  lv_obj_align(subtitle_box_, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_set_style_radius(subtitle_box_, 8, 0);
  lv_obj_set_style_bg_color(subtitle_box_, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_bg_opa(subtitle_box_, LV_OPA_80, 0);
  lv_obj_set_style_border_width(subtitle_box_, 1, 0);
  lv_obj_set_style_border_color(subtitle_box_, lv_color_hex(0x38bdf8), 0);
  lv_obj_set_style_pad_all(subtitle_box_, 5, 0);
  lv_obj_set_scrollbar_mode(subtitle_box_, LV_SCROLLBAR_MODE_OFF);

  subtitle_label_ = lv_label_create(subtitle_box_);
  lv_obj_set_width(subtitle_label_, 218);
  lv_obj_set_style_text_font(subtitle_label_, &font_puhui_16_4, 0);
  lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xf1f5f9), 0);
  lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(subtitle_label_, "Enco 正在待命...");

  next_blink_tick_ = kBlinkMinTicks;
  next_hair_tick_ = 14;
  face_timer_ = lv_timer_create(OnFaceTimer, kFaceTickMs, this);
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
      lv_label_set_text(subtitle_label_, ("你: " + content).c_str());
    } else if (role == Role::kAssistant) {
      lv_label_set_text(subtitle_label_, ("Enco: " + content).c_str());
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
  const bool was_speaking = is_speaking_;
  is_speaking_ = (s == "说话中");
  if (is_speaking_ != was_speaking) {
    // Snap the mouth immediately rather than waiting up to a tick, so the picture and the audio
    // start and stop together.
    ApplyMouthFrame();
  }

  if (subtitle_label_ != nullptr) {
    if (s == "聆听中") {
      lv_label_set_text(subtitle_label_, "正在聆听你的指令...");
      UpdateRobotFaceEmotion("neutral");
    } else if (s == "待命") {
      lv_label_set_text(subtitle_label_, "Enco 正在待命...");
      UpdateRobotFaceEmotion("neutral");
    } else if (s == "连接中...") {
      lv_label_set_text(subtitle_label_, "正在连接小智云端...");
    } else if (s == "网络已连接") {
      lv_label_set_text(subtitle_label_, "网络已连接");
    } else if (s == "网络配置中" || s == "热点配网模式") {
      lv_label_set_text(subtitle_label_, "请使用手机进行配网");
    } else if (s == "抬头中...") {
      lv_label_set_text(subtitle_label_, "正在抬头看上面...");
    } else if (s == "低头中...") {
      lv_label_set_text(subtitle_label_, "正在低头看地面...");
    } else if (s == "向左歪头...") {
      lv_label_set_text(subtitle_label_, "向左歪头倾听...");
    } else if (s == "向右歪头...") {
      lv_label_set_text(subtitle_label_, "向右歪头倾听...");
    } else if (s == "向左转头...") {
      lv_label_set_text(subtitle_label_, "向左转头看看...");
    } else if (s == "向右转头...") {
      lv_label_set_text(subtitle_label_, "向右转头看看...");
    } else if (s == "摇头晃脑...") {
      lv_label_set_text(subtitle_label_, "萌动摇头晃脑中~");
    } else if (s == "头已正视") {
      lv_label_set_text(subtitle_label_, "头已摆正正视前方");
    }
  }

  lvgl_port_unlock();
}

// Feedback for a volume change.
//
// This deliberately does not go through ShowStatus(). ShowStatus() infers is_speaking_ from the
// string it is handed, and the volume is nearly always changed while the assistant is talking
// ("好的，已经调大了") - borrowing it here would latch the mouth shut for the rest of the reply.
//
// The percentage is shown in the notification label, which is a toast by construction: the next
// ShowStatus() call hides it again, and one always follows within a second or two as the chat
// state moves on. The speaker icon next to it is persistent, so the current level stays readable.
void Display::ShowVolume(const uint16_t volume) {
  char text[24];
  snprintf(text, sizeof(text), "音量 %u%%", static_cast<unsigned>(volume));

  lvgl_port_lock(0);

  if (volume_label_ != nullptr) {
    const char* icon = FONT_AWESOME_VOLUME_HIGH;
    if (volume == 0) {
      icon = FONT_AWESOME_VOLUME_MUTE;
    } else if (volume < 34) {
      icon = FONT_AWESOME_VOLUME_LOW;
    } else if (volume < 67) {
      icon = FONT_AWESOME_VOLUME_MEDIUM;
    }
    lv_label_set_text(volume_label_, icon);
  }

  if (notification_label_ != nullptr && status_label_ != nullptr) {
    lv_label_set_text(notification_label_, text);
    lv_obj_clear_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
  }

  if (subtitle_label_ != nullptr) {
    lv_label_set_text(subtitle_label_, text);
  }

  lvgl_port_unlock();
}

void Display::SetEmotion(const std::string& emotion) {
  // This used to build a 21-entry std::map<std::string, const char*> on every call - roughly 1.5KB
  // of allocate-and-free churn each time the assistant changes expression. A static table costs
  // nothing at runtime and keeps the (very small) remaining heap unfragmented.
  // Monochrome Font Awesome glyphs, not colour emoji: the status bar is a cyan HUD and a full
  // colour yellow smiley in the corner of it looked like it belonged to a different program. These
  // tint with the rest of the bar and are already in the font_awesome_30_4 subset.
  struct EmotionIcon {
    const char* name;
    const char* icon;
  };
  static constexpr EmotionIcon kEmotions[] = {
      {"neutral", FONT_AWESOME_EMOJI_NEUTRAL},         {"happy", FONT_AWESOME_EMOJI_HAPPY},
      {"laughing", FONT_AWESOME_EMOJI_LAUGHING},       {"funny", FONT_AWESOME_EMOJI_FUNNY},
      {"sad", FONT_AWESOME_EMOJI_SAD},                 {"angry", FONT_AWESOME_EMOJI_ANGRY},
      {"crying", FONT_AWESOME_EMOJI_CRYING},           {"loving", FONT_AWESOME_EMOJI_LOVING},
      {"embarrassed", FONT_AWESOME_EMOJI_EMBARRASSED}, {"surprised", FONT_AWESOME_EMOJI_SURPRISED},
      {"shocked", FONT_AWESOME_EMOJI_SHOCKED},         {"thinking", FONT_AWESOME_EMOJI_THINKING},
      {"winking", FONT_AWESOME_EMOJI_WINKING},         {"cool", FONT_AWESOME_EMOJI_COOL},
      {"relaxed", FONT_AWESOME_EMOJI_RELAXED},         {"delicious", FONT_AWESOME_EMOJI_DELICIOUS},
      {"kissy", FONT_AWESOME_EMOJI_KISSY},             {"confident", FONT_AWESOME_EMOJI_CONFIDENT},
      {"sleepy", FONT_AWESOME_EMOJI_SLEEPY},           {"silly", FONT_AWESOME_EMOJI_SILLY},
      {"confused", FONT_AWESOME_EMOJI_CONFUSED},
  };

  const char* icon = FONT_AWESOME_EMOJI_NEUTRAL;
  for (const auto& entry : kEmotions) {
    if (emotion == entry.name) {
      icon = entry.icon;
      break;
    }
  }

  lvgl_port_lock(0);
  if (emotion_label_ != nullptr) {
    lv_label_set_text(emotion_label_, icon);
  }

  UpdateRobotFaceEmotion(emotion);
  lvgl_port_unlock();
}

void Display::SetUiMode(UiMode mode) {
  lvgl_port_lock(0);
  if (mode == UiMode::kRobotFace && !face_built_) {
    // The bitmap face needs well under 2KB, but LVGL aborts on a failed allocation, so still
    // refuse (and stay in text mode) if the heap is already on the floor.
    if (esp_get_free_heap_size() < 10000) {
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
  // There is one portrait, so emotion is expressed through timing rather than through geometry:
  // a cheerful Enco blinks more often, a sleepy one keeps her eyes shut.
  current_emotion_ = emotion;
  if (eyes_overlay_ == nullptr) {
    return;
  }
  if (emotion == "sleepy") {
    blink_frame_ = kBlinkFrameShut;
  } else if (blink_frame_ == kBlinkFrameShut) {
    blink_frame_ = 0;
  }
  ApplyBlinkFrame();
}

void Display::LookDirection(const char* dir) {
  // With a single portrait there is no separate eye sprite to slide around, so the whole head
  // shifts a couple of pixels instead. These come from MCP tool calls, so they are rare enough
  // that the resulting full-screen repaint does not matter.
  int dx = 0;
  int dy = 0;
  if (strcmp(dir, "up") == 0) {
    dy = -3;
  } else if (strcmp(dir, "down") == 0) {
    dy = 3;
  } else if (strcmp(dir, "left") == 0) {
    dx = -4;
  } else if (strcmp(dir, "right") == 0) {
    dx = 4;
  }
  lvgl_port_lock(0);
  ApplyHeadOffset(dx, dy);
  lvgl_port_unlock();
}

// Moves the portrait and all overlay sprites together so they stay registered with each other.
void Display::ApplyHeadOffset(int dx, int dy) {
  if (face_image_ == nullptr) {
    return;
  }
  if (dx == head_offset_x_ && dy == head_offset_y_) {
    return;  // Nothing to do, and repositioning would needlessly invalidate the whole screen.
  }
  head_offset_x_ = static_cast<int8_t>(dx);
  head_offset_y_ = static_cast<int8_t>(dy);
  lv_obj_set_pos(face_image_, dx, dy);
  if (bangs_overlay_) {
    lv_obj_set_pos(bangs_overlay_, ENCO_FACE_BANGS_X + dx, ENCO_FACE_BANGS_Y + dy);
  }
  if (locks_l_overlay_) {
    lv_obj_set_pos(locks_l_overlay_, ENCO_FACE_LOCKS_L_X + dx, ENCO_FACE_LOCKS_L_Y + dy);
  }
  if (locks_r_overlay_) {
    lv_obj_set_pos(locks_r_overlay_, ENCO_FACE_LOCKS_R_X + dx, ENCO_FACE_LOCKS_R_Y + dy);
  }
  if (eyes_overlay_) {
    lv_obj_set_pos(eyes_overlay_, ENCO_FACE_EYES_X + dx, ENCO_FACE_EYES_Y + dy);
  }
  if (mouth_overlay_) {
    lv_obj_set_pos(mouth_overlay_, ENCO_FACE_MOUTH_X + dx, ENCO_FACE_MOUTH_Y + dy);
  }
}

void Display::ApplyBlinkFrame() {
  if (eyes_overlay_ == nullptr) {
    return;
  }
  if (blink_frame_ == 0) {
    lv_obj_add_flag(eyes_overlay_, LV_OBJ_FLAG_HIDDEN);  // Base portrait already has open eyes.
    return;
  }
  lv_image_set_src(eyes_overlay_,
                   blink_frame_ == kBlinkFrameShut ? &enco_face_eyes_shut : &enco_face_eyes_half);
  lv_obj_clear_flag(eyes_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void Display::ApplyMouthFrame() {
  if (mouth_overlay_ == nullptr) {
    return;
  }
  if (!is_speaking_) {
    lv_obj_add_flag(mouth_overlay_, LV_OBJ_FLAG_HIDDEN);  // Back to the base portrait's soft smile.
    return;
  }
  // A closed / small / wide cycle reads as speech without needing to know anything about the audio.
  // The irregular pattern stops it looking like a metronome.
  static const uint8_t kMouthCycle[] = {1, 2, 1, 0, 2, 1, 2, 0};
  const uint8_t frame = kMouthCycle[(face_tick_ / 2) % (sizeof(kMouthCycle) / sizeof(kMouthCycle[0]))];
  if (frame == 0) {
    lv_obj_add_flag(mouth_overlay_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_image_set_src(mouth_overlay_, frame == 2 ? &enco_face_mouth_wide : &enco_face_mouth_small);
  lv_obj_clear_flag(mouth_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void Display::ApplyHairFrame() {
  if (bangs_overlay_ == nullptr || locks_l_overlay_ == nullptr || locks_r_overlay_ == nullptr) {
    return;
  }

  auto set_part = [](lv_obj_t* obj, int8_t dir, const lv_image_dsc_t* left_dsc,
                     const lv_image_dsc_t* right_dsc) {
    if (dir == 0) {
      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_image_set_src(obj, dir < 0 ? left_dsc : right_dsc);
      lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
  };

  if (hair_step_ == 0) {
    set_part(bangs_overlay_, 0, &enco_face_bangs_left, &enco_face_bangs_right);
    set_part(locks_l_overlay_, 0, &enco_face_locks_l_left, &enco_face_locks_l_right);
    set_part(locks_r_overlay_, 0, &enco_face_locks_r_left, &enco_face_locks_r_right);
    return;
  }

  // Each step in a breeze sets (bangs_dir, side_locks_dir) in {-1, 0, +1}. Letting the lighter
  // bangs lead or flutter on their own keeps the motion organic rather than rigid.
  struct HairPose {
    int8_t bangs;
    int8_t locks;
  };
  static constexpr HairPose kPatterns[4][6] = {
      // 0: Leftward breeze with gentle rebound; bangs lead the heavier side locks by a beat.
      {{-1, 0}, {-1, -1}, {-1, -1}, {1, -1}, {0, 1}, {0, 0}},
      // 1: Rightward breeze with gentle rebound.
      {{1, 0}, {1, 1}, {1, 1}, {-1, 1}, {0, -1}, {0, 0}},
      // 2: Light rustle of the front bangs only.
      {{-1, 0}, {-1, 0}, {1, 0}, {1, 0}, {0, 0}, {0, 0}},
      // 3: Playful two-way sway across both bangs and side locks.
      {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}, {-1, 0}, {0, 0}},
  };

  const uint8_t idx = (hair_step_ - 1) / 2;
  if (idx >= 6) {
    hair_step_ = 0;
    set_part(bangs_overlay_, 0, &enco_face_bangs_left, &enco_face_bangs_right);
    set_part(locks_l_overlay_, 0, &enco_face_locks_l_left, &enco_face_locks_l_right);
    set_part(locks_r_overlay_, 0, &enco_face_locks_r_left, &enco_face_locks_r_right);
    return;
  }

  const HairPose pose = kPatterns[hair_pattern_ & 3][idx];
  set_part(bangs_overlay_, pose.bangs, &enco_face_bangs_left, &enco_face_bangs_right);
  set_part(locks_l_overlay_, pose.locks, &enco_face_locks_l_left, &enco_face_locks_l_right);
  set_part(locks_r_overlay_, pose.locks, &enco_face_locks_r_left, &enco_face_locks_r_right);
}

void Display::OnFaceTimer(lv_timer_t* timer) {
  auto* self = static_cast<Display*>(lv_timer_get_user_data(timer));
  if (self == nullptr || self->ui_mode_ != UiMode::kRobotFace || self->face_image_ == nullptr) {
    return;
  }
  // Redrawing is not worth crowding out the audio pipeline when memory is already scarce.
  if (esp_get_free_heap_size() < 12000) {
    return;
  }
  self->face_tick_++;

  // --- Blink -------------------------------------------------------------------------------
  // Runs open -> half -> shut -> shut -> half -> open, i.e. about 240ms lid-down, then waits a
  // randomised few seconds so it never looks mechanical.
  if (self->current_emotion_ != "sleepy") {
    if (self->blink_frame_ != 0) {
      const uint8_t next = self->blink_frame_ + 1;
      self->blink_frame_ = next > kBlinkFrameLast ? 0 : next;
      self->ApplyBlinkFrame();
      if (self->blink_frame_ == 0) {
        const uint32_t spread = kBlinkMaxTicks - kBlinkMinTicks;
        self->next_blink_tick_ = self->face_tick_ + kBlinkMinTicks + (esp_random() % spread);
      }
    } else if (self->face_tick_ >= self->next_blink_tick_) {
      self->blink_frame_ = 1;
      self->ApplyBlinkFrame();
    }
  }

  // --- Random hair breeze ------------------------------------------------------------------
  if (self->hair_step_ != 0) {
    self->hair_step_++;
    if (self->hair_step_ > 12) {
      self->hair_step_ = 0;
      self->next_hair_tick_ = self->face_tick_ + 16 + (esp_random() % 32);
    }
    self->ApplyHairFrame();
  } else if (self->face_tick_ >= self->next_hair_tick_) {
    self->hair_pattern_ = static_cast<uint8_t>(esp_random() & 3);
    self->hair_step_ = 1;
    self->ApplyHairFrame();
  }

  // --- Mouth -------------------------------------------------------------------------------
  self->ApplyMouthFrame();

  // --- Idle sway ---------------------------------------------------------------------------
  // Shifting the portrait invalidates the entire screen, so this only happens while she is quiet
  // (the mouth is providing the movement otherwise) and only every few seconds.
  if (!self->is_speaking_ && self->blink_frame_ == 0 && self->hair_step_ == 0 &&
      (self->face_tick_ % kSwayPeriodTicks) == 0) {
    static const int8_t kSway[] = {0, 1, 2, 1, 0, -1, -2, -1};
    const uint32_t step = (self->face_tick_ / kSwayPeriodTicks) % (sizeof(kSway) / sizeof(kSway[0]));
    self->ApplyHeadOffset(kSway[step], 0);
  }
}
