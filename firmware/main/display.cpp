#include <esp_heap_caps.h>
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
#include "core/audio_playback_signal.h"
#include "display.h"
#include "video_sink.h"

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
// How long after the last PCM buffer the mouth keeps moving. Audio frames arrive every 20-60ms, so
// this has to absorb an ordinary frame gap without letting the mouth flap on after a reply ends.
static constexpr uint32_t kMouthHoldMs = 200;
// A breeze advances one pose per tick. With the half-strength hair frames that is a ~1px step every
// 80ms, which is what makes the motion read as a sway rather than a flip-book.
static constexpr uint8_t kHairPoseCount = 14;
static constexpr uint32_t kHairMinGapTicks = 20;   // 1.6s
static constexpr uint32_t kHairGapSpreadTicks = 40;  // up to a further 3.2s

// What the caption pill says when nothing is going on.
//
// It used to read "Enco 正在待命..." - true, but it told the user nothing they could act on. This
// device has exactly one control and no labels on it, so the idle state is the only moment there is
// room to explain it. Both routes named here are real: WakeNet runs continuously (see
// EngineImpl::OnWakeUp), and the boot button is wired to Engine::Advance(), which starts a session
// from standby and interrupts her while she is talking.
static constexpr const char* kIdleCaption = "待命中\n按键或唤醒词开始对话";

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
      panel_(panel),
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
  // A hairline rule under the bar, in the same gold as the status text. This is the one piece of
  // chrome the reference HUD leans on hardest, and it costs no object: it separates the bar from
  // the portrait, which shares its near-black background and otherwise ran straight into it.
  lv_obj_set_style_border_width(status_bar_, 1, 0);
  lv_obj_set_style_border_side(status_bar_, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(status_bar_, current_theme_.jarvis_gold, 0);
  lv_obj_set_style_border_opa(status_bar_, LV_OPA_60, 0);
  lv_obj_set_style_pad_column(status_bar_, 0, 0);
  lv_obj_set_style_pad_left(status_bar_, 8, 0);
  lv_obj_set_style_pad_right(status_bar_, 8, 0);
  lv_obj_set_style_pad_top(status_bar_, 3, 0);
  lv_obj_set_style_pad_bottom(status_bar_, 3, 0);
  lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
  // 设置状态栏的内容垂直居中
  lv_obj_set_flex_align(status_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // 创建emotion_label_在状态栏最左侧
  //
  // 16px, not 30px. The bar is LV_SIZE_CONTENT, so the emotion glyph alone used to set its height
  // at ~36px - a ninth of the screen spent on one icon, and the portrait pushed down by the same
  // amount. font_awesome_16_4 is generated from the identical glyph subset as font_awesome_30_4
  // (both cover range 57419 + 6008), so every emotion still has an icon; it just sits level with
  // the volume and network indicators now, which is what makes the bar read as one strip.
  //
  // This was briefly put back to 30px on the theory that a bigger network icon needed matching
  // siblings. On the actual panel the whole row came out oversized; 16px is the right size here.
  emotion_label_ = lv_label_create(status_bar_);
  lv_obj_set_style_text_font(emotion_label_, &font_awesome_16_4, 0);
  lv_obj_set_style_text_color(emotion_label_, current_theme_.jarvis_cyan, 0);
  lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);
  lv_obj_set_style_margin_right(emotion_label_, 6, 0);  // 添加右边距，与后面的元素分隔

  // 倒计时标签。Flex order is creation order, so this has to be built before the notification and
  // status labels for the countdown to end up in the middle of the bar. It stays hidden (and
  // without flex grow) until a timer is actually running, so it costs no space the rest of the time.
  timer_label_ = lv_label_create(status_bar_);
  lv_obj_set_style_text_align(timer_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(timer_label_, current_theme_.jarvis_cyan, 0);
  lv_label_set_text(timer_label_, "");
  lv_obj_add_flag(timer_label_, LV_OBJ_FLAG_HIDDEN);

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
  // Starts as "no link" rather than blank. An empty label here reads as "everything is fine" when
  // it actually means "nobody has told the screen anything yet", which is the opposite.
  lv_label_set_text(network_label_, FONT_AWESOME_WIFI_OFF);
  lv_obj_set_style_text_font(network_label_, &font_awesome_16_4, 0);
  lv_obj_set_style_text_color(network_label_, current_theme_.jarvis_cyan_dim, 0);
  lv_obj_set_style_margin_left(network_label_, 6, 0);  // 添加左边距，与前面的元素分隔

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
  lv_image_set_src(bangs_overlay_, &enco_face_bangs_lhalf);
  lv_obj_set_pos(bangs_overlay_, ENCO_FACE_BANGS_X, ENCO_FACE_BANGS_Y);
  lv_obj_add_flag(bangs_overlay_, LV_OBJ_FLAG_HIDDEN);

  locks_l_overlay_ = lv_image_create(face_container_);
  lv_image_set_src(locks_l_overlay_, &enco_face_locks_l_lhalf);
  lv_obj_set_pos(locks_l_overlay_, ENCO_FACE_LOCKS_L_X, ENCO_FACE_LOCKS_L_Y);
  lv_obj_add_flag(locks_l_overlay_, LV_OBJ_FLAG_HIDDEN);

  locks_r_overlay_ = lv_image_create(face_container_);
  lv_image_set_src(locks_r_overlay_, &enco_face_locks_r_lhalf);
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

  // The caption pill. This is the bottom half of the reference HUD, reduced to the part that
  // carries information: what she is saying, or - when nothing is happening - how to talk to her.
  //
  // The reference also has an oscilloscope trace, a spectrum bar graph and a second copy of the
  // connection banner. The banner is a duplicate of the top bar and is simply dropped. The two
  // visualisers are not built: at ~430 bytes of heap per LVGL object they would cost several KB of
  // a board that has ~6KB free during TTS, and neither is driven by anything real - there is a
  // playback beacon (audio_playback_signal) but no amplitude, so they would animate to nothing.
  subtitle_box_ = lv_obj_create(face_container_);
  lv_obj_set_size(subtitle_box_, 232, 52);
  lv_obj_align(subtitle_box_, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_set_style_radius(subtitle_box_, 12, 0);
  lv_obj_set_style_bg_color(subtitle_box_, lv_color_hex(0x0b1220), 0);
  // 80 -> 90: her hair is near-white and sits directly behind this, and at 80 the descenders of the
  // caption were competing with it.
  lv_obj_set_style_bg_opa(subtitle_box_, LV_OPA_90, 0);
  lv_obj_set_style_border_width(subtitle_box_, 1, 0);
  lv_obj_set_style_border_color(subtitle_box_, lv_color_hex(0x38bdf8), 0);
  // The faint second ring the reference draws around its pill. An outline is a style, not an
  // object, so the effect is free.
  lv_obj_set_style_outline_width(subtitle_box_, 1, 0);
  lv_obj_set_style_outline_color(subtitle_box_, lv_color_hex(0x38bdf8), 0);
  lv_obj_set_style_outline_opa(subtitle_box_, LV_OPA_30, 0);
  lv_obj_set_style_outline_pad(subtitle_box_, 2, 0);
  lv_obj_set_style_pad_all(subtitle_box_, 5, 0);
  lv_obj_set_scrollbar_mode(subtitle_box_, LV_SCROLLBAR_MODE_OFF);

  subtitle_label_ = lv_label_create(subtitle_box_);
  lv_obj_set_width(subtitle_label_, 218);
  lv_obj_set_style_text_font(subtitle_label_, &font_puhui_16_4, 0);
  lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xf1f5f9), 0);
  lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(subtitle_label_, kIdleCaption);

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

  // The caption under the character comes first, and is deliberately outside both guards below.
  //
  // It used to be the last thing this function did, which meant the low-heap bail-out took it down
  // with the transcript - so in exactly the conditions where the screen is the only feedback the
  // user has, the character went silent and kept smiling. Writing an existing label is a realloc of
  // one small buffer, not two new widgets; it is not what puts the board under pressure.
  if (subtitle_label_ != nullptr) {
    lvgl_port_lock(0);
    if (role == Role::kUser) {
      lv_label_set_text(subtitle_label_, ("你: " + content).c_str());
    } else if (role == Role::kAssistant) {
      lv_label_set_text(subtitle_label_, ("Enco: " + content).c_str());
    } else {
      lv_label_set_text(subtitle_label_, content.c_str());
    }
    lvgl_port_unlock();
  }

  // Everything below builds the scrolling transcript, which only kChatText ever shows.
  //
  // It used to be built in every mode. The device boots into the character view and mostly stays
  // there, so up to MAX_MESSAGES bubbles - each a container plus a wrapped label, ~430 bytes of
  // heap apiece - were being created, laid out and retained behind a hidden parent that the user
  // was not looking at. That is on the order of 2.5KB permanently unavailable, on a board that
  // spends TTS with ~6KB free and where the Wi-Fi stack is already failing 2,308-byte allocations.
  //
  // The cost of this: switching to chat mode starts from an empty transcript rather than showing
  // the last three exchanges. Nothing is actually lost - that history was never on screen - and
  // the caption above has carried the current exchange the whole time.
  if (ui_mode_ != UiMode::kChatText) {
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

  // (The character's caption was already updated at the top of this function, before the mode and
  // heap guards, so that it keeps working when the transcript is skipped.)

  lvgl_port_unlock();
}

void Display::ShowStatus(const char* status) {
  lvgl_port_lock(0);
  lv_label_set_text(status_label_, status);
  // A running countdown owns the middle of the bar; leave the status text parked until it is done.
  // The same information is spelled out in the subtitle box below, so nothing becomes unreadable.
  if (!timer_visible_) {
    lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

  const std::string s(status);

  // Kept only for the status text and for callers that ask whether the assistant is mid-reply.
  // The mouth is deliberately NOT driven from here: this string flips to 说话中 on the server's
  // tts/start frame, a network round trip and a decode queue before the first sample reaches the
  // amplifier, and it gets overwritten mid-reply by servo commands like 抬头中... - which used to
  // start the lips early and then freeze them. OnFaceTimer() follows the speaker instead.
  is_speaking_ = (s == "说话中");

  if (subtitle_label_ != nullptr) {
    if (s == "聆听中") {
      lv_label_set_text(subtitle_label_, "正在聆听你的指令...");
      UpdateRobotFaceEmotion("neutral");
    } else if (s == "待命") {
      lv_label_set_text(subtitle_label_, kIdleCaption);
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

// Wi-Fi arc + signal bars at the right-hand end of the bar, the way the reference HUD shows them.
//
// Two glyphs in one label rather than two labels: the pair is always written together, and an
// LVGL object costs ~430 bytes of heap here (measured - the camera view's 15 widgets cost 6,480).
// On a board that spends TTS with 6KB free and a 2.1KB largest block, a second label for something
// that never changes independently is not worth it.
//
// Only the colours have been touched. Two things were wrong, and neither was the glyph choice:
//
//  1. Amber started at -77dBm, which is an ordinary reading for a device one room from the access
//     point. Measured on this board: -49 to -57dBm, so that threshold was not the reported problem,
//     but it would have cried wolf on any weaker install. Amber now means close to dropping.
//  2. A disconnected link painted red, including the ~12s WiFi takes to associate from reset. Every
//     single boot therefore put a red icon in the corner - "not up yet" shown as a fault.
void Display::ShowNetwork(const bool connected, const int rssi_dbm) {
  if (network_label_ == nullptr) {
    return;
  }

  const char* glyphs;
  lv_color_t colour = current_theme_.jarvis_cyan;
  if (!connected) {
    glyphs = FONT_AWESOME_WIFI_OFF;
    colour = net_ever_connected_ ? current_theme_.low_battery : current_theme_.jarvis_cyan_dim;
  } else {
    net_ever_connected_ = true;
    if (rssi_dbm >= -70) {
      glyphs = FONT_AWESOME_WIFI FONT_AWESOME_SIGNAL_FULL;
    } else if (rssi_dbm >= -80) {
      glyphs = FONT_AWESOME_WIFI FONT_AWESOME_SIGNAL_4;
    } else if (rssi_dbm >= -88) {
      glyphs = FONT_AWESOME_WIFI_FAIR FONT_AWESOME_SIGNAL_3;
      colour = current_theme_.jarvis_cyan_dim;  // marginal, but still carrying audio
    } else {
      glyphs = FONT_AWESOME_WIFI_WEAK FONT_AWESOME_SIGNAL_2;
      colour = current_theme_.jarvis_gold;  // amber: this is where dropouts actually start
    }
  }

  lvgl_port_lock(0);
  lv_label_set_text(network_label_, glyphs);
  lv_obj_set_style_text_color(network_label_, colour, 0);
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

  // The toast and the countdown both want the middle of the bar. The countdown is the one the user
  // is actively watching, so it keeps it; the speaker icon above still reflects the new level, and
  // the percentage is repeated in the subtitle box below.
  if (!timer_visible_ && notification_label_ != nullptr && status_label_ != nullptr) {
    lv_label_set_text(notification_label_, text);
    lv_obj_clear_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
  }

  if (subtitle_label_ != nullptr) {
    lv_label_set_text(subtitle_label_, text);
  }

  lvgl_port_unlock();
}

// ---------------------------------------------------------------- countdown + alert overlay

// The caption pill and the countdown panel both live at the bottom of the screen. Whichever is up
// owns it; there is no room to stack them, and a timer the user asked for outranks "待命中".
void Display::SetSubtitleHidden(const bool hidden) {
  if (subtitle_box_ == nullptr) {
    return;
  }
  if (hidden) {
    lv_obj_add_flag(subtitle_box_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(subtitle_box_, LV_OBJ_FLAG_HIDDEN);
  }
}

// Three widgets: panel, "T-MINUS" caption, digits. Caller must hold the LVGL lock.
bool Display::EnsureTimerPanel() {
  if (timer_panel_ != nullptr) {
    return true;
  }
  // The viewfinder paints straight to the panel, bypassing LVGL, so anything drawn over it is
  // erased by the next frame. Refusing here keeps the countdown in the status bar, where it stays
  // visible, instead of flickering underneath the video.
  if (ui_mode_ == UiMode::kCameraView) {
    return false;
  }
  // Same reasoning as the camera view's guard, scaled to three widgets rather than fifteen.
  const size_t free_heap = esp_get_free_heap_size();
  if (free_heap < 12000) {
    printf("[display] countdown panel refused (free %u)\n", static_cast<unsigned>(free_heap));
    return false;
  }

  timer_panel_ = lv_obj_create(lv_screen_active());
  lv_obj_set_pos(timer_panel_, 27, 278);
  lv_obj_set_size(timer_panel_, 186, 38);
  lv_obj_set_style_radius(timer_panel_, 5, 0);
  lv_obj_set_style_pad_all(timer_panel_, 0, 0);
  lv_obj_set_style_bg_color(timer_panel_, lv_color_hex(0x0a0f1a), 0);
  lv_obj_set_style_bg_opa(timer_panel_, LV_OPA_50, 0);
  lv_obj_set_style_border_width(timer_panel_, 1, 0);
  lv_obj_set_style_border_color(timer_panel_, current_theme_.jarvis_gold, 0);
  lv_obj_set_style_outline_width(timer_panel_, 1, 0);
  lv_obj_set_style_outline_color(timer_panel_, current_theme_.jarvis_gold, 0);
  lv_obj_set_style_outline_opa(timer_panel_, LV_OPA_30, 0);
  lv_obj_set_style_outline_pad(timer_panel_, 2, 0);
  lv_obj_clear_flag(timer_panel_, LV_OBJ_FLAG_SCROLLABLE);

  auto* caption = lv_label_create(timer_panel_);
  lv_label_set_text(caption, "T-MINUS");
  lv_obj_set_style_text_font(caption, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(caption, current_theme_.jarvis_gold, 0);
  lv_obj_align(caption, LV_ALIGN_LEFT_MID, 8, 0);

  timer_digits_ = lv_label_create(timer_panel_);
  // Montserrat 32px (80% of 40px) so the digits scale down proportionally with the 186x38 panel.
  lv_obj_set_style_text_font(timer_digits_, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(timer_digits_, current_theme_.jarvis_gold, 0);
  lv_label_set_text(timer_digits_, "00:00");
  lv_obj_align(timer_digits_, LV_ALIGN_RIGHT_MID, -8, 0);

  lv_obj_move_foreground(timer_panel_);
  SetSubtitleHidden(true);
  return true;
}

void Display::DestroyTimerPanel() {
  if (timer_panel_ == nullptr) {
    return;
  }
  lv_obj_del(timer_panel_);  // deletes its children too
  timer_panel_ = nullptr;
  timer_digits_ = nullptr;
  SetSubtitleHidden(false);
}

void Display::ShowAlert(const char* title, const char* body, const uint32_t duration_ms) {
  if (title == nullptr) {
    title = "";
  }
  if (body == nullptr) {
    body = "";
  }

  lvgl_port_lock(0);

  if (alert_card_ == nullptr) {
    // See EnsureTimerPanel() - the viewfinder owns the panel and would paint over this.
    const size_t free_heap = esp_get_free_heap_size();
    if (ui_mode_ == UiMode::kCameraView || free_heap < 12000) {
      printf("[display] alert card refused (free %u)\n", static_cast<unsigned>(free_heap));
      lvgl_port_unlock();
      return;
    }

    // Right-hand square-ish sci-fi HUD alert box (144x148 at x=92, y=40) with a protruding
    // top-left amber folder tab, horizontal telemetry rules, and a bottom warning badge.
    alert_card_ = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(alert_card_, 92, 40);
    lv_obj_set_size(alert_card_, 144, 148);
    lv_obj_set_style_pad_all(alert_card_, 0, 0);
    lv_obj_set_style_bg_opa(alert_card_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(alert_card_, 0, 0);
    lv_obj_clear_flag(alert_card_, LV_OBJ_FLAG_SCROLLABLE);

    // Protruding top-left solid orange sci-fi tab (matching reference HUD)
    lv_obj_t* tab = lv_label_create(alert_card_);
    lv_obj_set_pos(tab, 0, 0);
    lv_obj_set_size(tab, 78, 15);
    lv_obj_set_style_radius(tab, 2, 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(tab, 5, 0);
    lv_obj_set_style_pad_top(tab, 0, 0);
    lv_obj_set_style_text_font(tab, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(0x180E02), 0);
    lv_label_set_text(tab, "[+] ALERT");

    // Main square-ish dark amber holographic frame (144x135 below the tab)
    lv_obj_t* frame = lv_obj_create(alert_card_);
    lv_obj_set_pos(frame, 0, 13);
    lv_obj_set_size(frame, 144, 135);
    lv_obj_set_style_radius(frame, 2, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x231405), 0);
    lv_obj_set_style_bg_opa(frame, 235, 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_shadow_width(frame, 8, 0);
    lv_obj_set_style_shadow_color(frame, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_shadow_opa(frame, LV_OPA_30, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    // Header title with bottom amber divider rule
    alert_title_ = lv_label_create(frame);
    lv_obj_set_pos(alert_title_, 7, 5);
    lv_obj_set_size(alert_title_, 126, 23);
    lv_obj_set_style_text_font(alert_title_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(alert_title_, lv_color_hex(0xFFB830), 0);
    lv_obj_set_style_border_side(alert_title_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(alert_title_, 1, 0);
    lv_obj_set_style_border_color(alert_title_, lv_color_hex(0x9A5B0A), 0);
    lv_label_set_long_mode(alert_title_, LV_LABEL_LONG_DOT);

    // Body text area (up to 3 wrapped lines) with bottom telemetry divider rule
    alert_body_ = lv_label_create(frame);
    lv_obj_set_pos(alert_body_, 7, 32);
    lv_obj_set_size(alert_body_, 126, 72);
    lv_obj_set_style_text_font(alert_body_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(alert_body_, lv_color_hex(0xFFF0D4), 0);
    lv_obj_set_style_border_side(alert_body_, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(alert_body_, 1, 0);
    lv_obj_set_style_border_color(alert_body_, lv_color_hex(0x6B3E07), 0);
    lv_label_set_long_mode(alert_body_, LV_LABEL_LONG_WRAP);

    // Bottom sci-fi warning badge + telemetry readout
    lv_obj_t* footer = lv_label_create(frame);
    lv_obj_set_pos(footer, 7, 109);
    lv_obj_set_size(footer, 126, 18);
    lv_obj_set_style_radius(footer, 2, 0);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x3A2006), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(footer, 18, 0);
    lv_obj_set_style_border_color(footer, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_pad_left(footer, 6, 0);
    lv_obj_set_style_pad_top(footer, 1, 0);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(0xFFB830), 0);
    lv_label_set_text(footer, "!  SYS // DATA");
  }

  // A second alert rewrites the card and resets its opacity and auto-dismiss timer rather than
  // stacking one on top of the other.
  lv_obj_set_style_opa(alert_card_, LV_OPA_COVER, 0);
  lv_label_set_text(alert_title_, title);
  lv_label_set_text(alert_body_, body);
  alert_hide_at_ms_ = lv_tick_get() + (duration_ms > 0 ? duration_ms : 5000);
  lv_obj_move_foreground(alert_card_);
  // Below the countdown, so a timer that fires while a card is up still reads.
  if (timer_panel_ != nullptr) {
    lv_obj_move_foreground(timer_panel_);
  }

  lvgl_port_unlock();
}

void Display::HideAlert() {
  lvgl_port_lock(0);
  if (alert_card_ != nullptr) {
    lv_obj_del(alert_card_);
    alert_card_ = nullptr;
    alert_title_ = nullptr;
    alert_body_ = nullptr;
  }
  alert_hide_at_ms_ = 0;
  lvgl_port_unlock();
}

// Paints the remaining time into the status bar, e.g. "计时 04:32".
//
// Called once a second from loop(); LVGL redraws a label only when the text actually changes, so
// re-sending an identical string is cheap and the caller does not have to track what is on screen.
void Display::ShowTimer(const uint32_t remaining_seconds) {
  char text[24];
  const unsigned hours = static_cast<unsigned>(remaining_seconds / 3600);
  const unsigned minutes = static_cast<unsigned>((remaining_seconds % 3600) / 60);
  const unsigned seconds = static_cast<unsigned>(remaining_seconds % 60);
  if (hours > 0) {
    snprintf(text, sizeof(text), "计时 %u:%02u:%02u", hours, minutes, seconds);
  } else {
    snprintf(text, sizeof(text), "计时 %02u:%02u", minutes, seconds);
  }

  lvgl_port_lock(0);
  if (timer_label_ != nullptr) {
    lv_label_set_text(timer_label_, text);
    if (!timer_visible_) {
      timer_visible_ = true;
      // Take over the grow that status_label_ normally has, so the digits sit in the centre of the
      // screen rather than being crowded against the emotion icon.
      lv_obj_set_flex_grow(timer_label_, 1);
      lv_obj_clear_flag(timer_label_, LV_OBJ_FLAG_HIDDEN);
      if (status_label_ != nullptr) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
      }
      if (notification_label_ != nullptr) {
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }

  // The big T-MINUS panel. The status-bar readout above is kept rather than replaced: it is the
  // fallback for the cases this panel declines - low heap, or the viewfinder owning the screen -
  // and it is what chat mode and the camera view show. Neither path can leave the user with no
  // countdown at all.
  if (EnsureTimerPanel() && timer_digits_ != nullptr) {
    char big[16];
    if (hours > 0) {
      snprintf(big, sizeof(big), "%u:%02u:%02u", hours, minutes, seconds);
    } else {
      snprintf(big, sizeof(big), "%02u:%02u", minutes, seconds);
    }
    lv_label_set_text(timer_digits_, big);
  }
  lvgl_port_unlock();
}

// "时间到" stays up until the caller hides it, which main.cpp does once the assistant has finished
// announcing it. Holding it there is deliberate: the announcement is easy to miss if the room is
// noisy, and the screen is the fallback.
void Display::ShowTimerFinished() {
  lvgl_port_lock(0);
  if (timer_label_ != nullptr) {
    lv_label_set_text(timer_label_, "时间到");
    if (!timer_visible_) {
      timer_visible_ = true;
      lv_obj_set_flex_grow(timer_label_, 1);
      lv_obj_clear_flag(timer_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_label_ != nullptr) {
      lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (notification_label_ != nullptr) {
      lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
  }
  // Park the big digits at zero rather than leaving them on the last value they happened to be
  // polled at. loop() calls this the moment the deadline passes, which is usually somewhere inside
  // the final second, so without this the panel would freeze on "00:01" while the status bar said
  // 时间到 - two readouts disagreeing about whether the timer had finished.
  if (timer_digits_ != nullptr) {
    lv_label_set_text(timer_digits_, "00:00");
  }
  lvgl_port_unlock();
}

void Display::HideTimer() {
  lvgl_port_lock(0);
  if (timer_label_ != nullptr && timer_visible_) {
    timer_visible_ = false;
    lv_obj_add_flag(timer_label_, LV_OBJ_FLAG_HIDDEN);
    // Give the space back before the status text reappears, otherwise two grown children share the
    // centre and the status sits half a bar to the right.
    lv_obj_set_flex_grow(timer_label_, 0);
    lv_label_set_text(timer_label_, "");
    if (status_label_ != nullptr) {
      lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
  }
  // Hand the overlay's heap back. This is the whole reason it is built on demand: holding ~1.3KB
  // of countdown panel for the hours between timers is exactly the kind of idle cost that has the
  // Wi-Fi task failing 2,308-byte allocations during TTS.
  DestroyTimerPanel();
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
  if (mode == UiMode::kCameraView && !cam_built_) {
    // Unlike the face, there is no fallback for this one: the caller asked for
    // the viewfinder specifically, and a viewfinder with no picture is not a
    // useful thing to show. Refuse and leave the screen alone - "这是什么" then
    // takes the path that does not show its work, which still answers.
    //
    // The thresholds are measured, not guessed. On this board the HUD plus the
    // 4KB decoder pool cost 6,480 bytes of free heap and take the largest free
    // block from 11,764 down to 5,364. Idle-but-connected sits around 19,600
    // free, and the wifi task has been logged failing a 2,308 byte allocation
    // when free fell to 7,276 - so opening this below 16,000 would be spending
    // memory the audio path is about to need.
    const size_t free_heap = esp_get_free_heap_size();
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free_heap < 14500 || largest < 6000) {
      printf("[display] camera view refused (free %u, largest %u)\n", static_cast<unsigned>(free_heap), static_cast<unsigned>(largest));
      lvgl_port_unlock();
      return;
    }
    BuildCameraView();
  }

  ui_mode_ = mode;

  // The overlay and the viewfinder cannot share the screen. The video is blitted straight to the
  // ST7789 without going through LVGL, so LVGL has no idea those pixels changed and will not
  // redraw anything sitting on top of them - the countdown would be half-eaten by the next frame.
  //
  // The countdown panel is destroyed rather than hidden: loop() calls ShowTimer() once a second,
  // so it rebuilds itself within a second of the camera closing, and in the meantime the status-bar
  // readout carries the countdown. The alert card is only hidden - nothing re-sends an alert, so
  // deleting it would silently drop the notification the user has not read yet.
  if (mode == UiMode::kCameraView) {
    DestroyTimerPanel();
    if (alert_card_ != nullptr) {
      lv_obj_add_flag(alert_card_, LV_OBJ_FLAG_HIDDEN);
    }
  } else if (alert_card_ != nullptr) {
    lv_obj_clear_flag(alert_card_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(alert_card_);
  }

  // The viewfinder covers the whole panel, status bar included, so it is shown
  // or hidden as a unit with everything else rather than alongside it.
  const bool camera = (ui_mode_ == UiMode::kCameraView);
  if (cam_container_) {
    if (camera) {
      lv_obj_clear_flag(cam_container_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(cam_container_);
    } else {
      lv_obj_add_flag(cam_container_, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (container_) {
    if (camera) {
      lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (ui_mode_ == UiMode::kRobotFace) {
    // Hiding the transcript is not the same as giving its memory back. SetChatMessage() no longer
    // adds to it outside chat mode, but whatever was on screen when the user switched away would
    // otherwise stay allocated for the rest of the session - and the character view is where the
    // device spends nearly all its time. Dropping it here is what makes the saving permanent.
    if (content_) {
      lv_obj_clean(content_);
      chat_message_label_ = nullptr;
      lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
    }
    if (face_container_) lv_obj_clear_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
  } else if (ui_mode_ == UiMode::kChatText) {
    if (face_container_) lv_obj_add_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
    if (content_) lv_obj_clear_flag(content_, LV_OBJ_FLAG_HIDDEN);
  }
  lvgl_port_unlock();
}

void Display::ToggleUiMode() {
  if (ui_mode_ == UiMode::kCameraView) {
    // The button is the way out of a viewfinder that will not close itself -
    // e.g. the camera stopped answering while it was on screen.
    ExitCameraView();
  } else if (ui_mode_ == UiMode::kRobotFace) {
    SetUiMode(UiMode::kChatText);
  } else {
    SetUiMode(UiMode::kRobotFace);
  }
}

// The viewfinder HUD.
//
// Everything here is positioned absolutely against the panel, not laid out by
// flex, and that is deliberate. The picture does not go through LVGL at all -
// there is no framebuffer on this board to put it in - so it is blitted to the
// panel at the fixed rectangle in Display::kCam*. The chrome has to be built
// around that rectangle to the pixel, because anything LVGL draws inside it is
// erased by the next frame 90ms later.
//
// The corner brackets exploit the kCamInset margin: each is an empty box with
// borders on two sides only, positioned so the drawn edges fall in the margin
// and the transparent interior overlaps the picture harmlessly.
void Display::BuildCameraView() {
  if (cam_built_) {
    return;
  }

  auto screen = lv_screen_active();
  cam_container_ = lv_obj_create(screen);
  lv_obj_set_pos(cam_container_, 0, 0);
  lv_obj_set_size(cam_container_, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_radius(cam_container_, 0, 0);
  lv_obj_set_style_pad_all(cam_container_, 0, 0);
  lv_obj_set_style_border_width(cam_container_, 0, 0);
  lv_obj_set_style_bg_color(cam_container_, lv_color_hex(0x050b14), 0);
  lv_obj_set_style_bg_opa(cam_container_, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cam_container_, LV_OBJ_FLAG_SCROLLABLE);

  /* Header: mode on the left, recording state on the right. */
  auto* title = lv_label_create(cam_container_);
  lv_label_set_text(title, "TGT ACQ");
  lv_obj_set_style_text_color(title, current_theme_.jarvis_cyan, 0);
  lv_obj_set_pos(title, 6, 2);

  cam_rec_label_ = lv_label_create(cam_container_);
  lv_label_set_text(cam_rec_label_, "REC");
  lv_obj_set_style_text_color(cam_rec_label_, lv_color_hex(0xef4444), 0);
  lv_obj_align(cam_rec_label_, LV_ALIGN_TOP_RIGHT, -6, 2);
  cam_rec_on_ = true;
  cam_rec_next_tick_ = 0;

  /* The viewfinder frame. Its interior is painted once, then owned by video. */
  auto* frame = lv_obj_create(cam_container_);
  lv_obj_set_pos(frame, kCamFrameX, kCamFrameY);
  lv_obj_set_size(frame, kCamFrameW, kCamFrameH);
  lv_obj_set_style_radius(frame, 0, 0);
  lv_obj_set_style_pad_all(frame, 0, 0);
  lv_obj_set_style_bg_color(frame, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(frame, 1, 0);
  lv_obj_set_style_border_color(frame, current_theme_.jarvis_cyan_dim, 0);
  lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

  // Corner brackets. 18px arms, 2px thick, drawn entirely within the 8px margin.
  struct Corner {
    int x;
    int y;
    lv_border_side_t sides;
  };
  static constexpr int kArm = 18;
  const Corner corners[] = {
      {kCamFrameX + 2, kCamFrameY + 2, static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_TOP)},
      {kCamFrameX + kCamFrameW - 2 - kArm, kCamFrameY + 2, static_cast<lv_border_side_t>(LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_TOP)},
      {kCamFrameX + 2, kCamFrameY + kCamFrameH - 2 - kArm, static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM)},
      {kCamFrameX + kCamFrameW - 2 - kArm, kCamFrameY + kCamFrameH - 2 - kArm,
       static_cast<lv_border_side_t>(LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM)},
  };
  for (const auto& c : corners) {
    auto* bracket = lv_obj_create(cam_container_);
    lv_obj_set_pos(bracket, c.x, c.y);
    lv_obj_set_size(bracket, kArm, kArm);
    lv_obj_set_style_radius(bracket, 0, 0);
    lv_obj_set_style_pad_all(bracket, 0, 0);
    lv_obj_set_style_bg_opa(bracket, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bracket, 2, 0);
    lv_obj_set_style_border_color(bracket, current_theme_.jarvis_cyan, 0);
    lv_obj_set_style_border_side(bracket, c.sides, 0);
    lv_obj_clear_flag(bracket, LV_OBJ_FLAG_SCROLLABLE);
  }

  /* Telemetry strip under the frame. */
  static constexpr int kStripY = kCamFrameY + kCamFrameH + 4;  // 204
  auto* strip_left = lv_label_create(cam_container_);
  lv_label_set_text(strip_left, "TELEMETRY");
  lv_obj_set_style_text_color(strip_left, current_theme_.jarvis_cyan_dim, 0);
  lv_obj_set_pos(strip_left, 6, kStripY);

  cam_telemetry_label_ = lv_label_create(cam_container_);
  lv_label_set_text(cam_telemetry_label_, "STANDBY");
  lv_obj_set_style_text_color(cam_telemetry_label_, current_theme_.jarvis_gold, 0);
  lv_obj_align(cam_telemetry_label_, LV_ALIGN_TOP_RIGHT, -6, kStripY);

  /* Bottom row: status panel on the left, the character on the right. */
  static constexpr int kBottomY = kStripY + 22;  // 226

  auto* info = lv_obj_create(cam_container_);
  lv_obj_set_pos(info, 4, kBottomY);
  lv_obj_set_size(info, 142, 88);
  lv_obj_set_style_radius(info, 4, 0);
  lv_obj_set_style_pad_all(info, 6, 0);
  lv_obj_set_style_bg_color(info, lv_color_hex(0x0a1526), 0);
  lv_obj_set_style_bg_opa(info, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(info, 1, 0);
  lv_obj_set_style_border_color(info, current_theme_.jarvis_cyan_dim, 0);
  lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

  auto* info_title = lv_label_create(info);
  lv_label_set_text(info_title, "OPTICS");
  lv_obj_set_style_text_color(info_title, current_theme_.jarvis_cyan_dim, 0);
  lv_obj_set_pos(info_title, 0, 0);

  auto* info_body = lv_label_create(info);
  lv_label_set_text(info_body, "ESP32-CAM\n240x176 · 1:1");
  lv_obj_set_style_text_color(info_body, current_theme_.assistant_text, 0);
  lv_obj_set_pos(info_body, 0, 22);

  // The character. This is a pre-scaled 82x86 bust (enco_face_thumb), not the
  // full portrait: it used to be the 240x320 image positioned at (-78, -72), so
  // the panel acted as a window onto the middle of her face and cut off the top
  // of her head and her chin.
  //
  // It cannot be fixed with lv_image_set_scale(). Scaling runs through LVGL's
  // transform path, which needs the whole bitmap at once, and this portrait is
  // drawn by the streaming I4 decoder in lv_i4_decoder.c - it leaves
  // dsc->decoded NULL and hands back one row per call, so lv_draw_sw_img.c
  // would take src_w/src_h from a 1-pixel-tall slice and apply the transform to
  // each row independently. The shrink is baked at build time instead; see
  // tools/face_assets/build_face_assets.py.
  auto* avatar_box = lv_obj_create(cam_container_);
  lv_obj_set_pos(avatar_box, 152, kBottomY);
  // +2 for the 1px border on each side, so the image fills the interior exactly.
  lv_obj_set_size(avatar_box, ENCO_FACE_THUMB_W + 2, ENCO_FACE_THUMB_H + 2);
  lv_obj_set_style_radius(avatar_box, 4, 0);
  lv_obj_set_style_pad_all(avatar_box, 0, 0);
  lv_obj_set_style_bg_color(avatar_box, lv_color_hex(0x0c1121), 0);
  lv_obj_set_style_bg_opa(avatar_box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(avatar_box, 1, 0);
  lv_obj_set_style_border_color(avatar_box, current_theme_.jarvis_cyan_dim, 0);
  lv_obj_set_style_clip_corner(avatar_box, true, 0);
  lv_obj_clear_flag(avatar_box, LV_OBJ_FLAG_SCROLLABLE);

  auto* avatar = lv_image_create(avatar_box);
  lv_image_set_src(avatar, &enco_face_thumb);
  lv_obj_set_pos(avatar, 0, 0);

  auto* avatar_tag = lv_label_create(cam_container_);
  lv_label_set_text(avatar_tag, "ENCO");
  lv_obj_set_style_text_color(avatar_tag, current_theme_.jarvis_cyan, 0);
  lv_obj_set_style_bg_color(avatar_tag, lv_color_hex(0x050b14), 0);
  lv_obj_set_style_bg_opa(avatar_tag, LV_OPA_80, 0);
  lv_obj_set_style_pad_hor(avatar_tag, 3, 0);
  lv_obj_set_pos(avatar_tag, 156, kBottomY + 66);

  /* Caption over the picture. Hidden until there is something to say. */
  cam_hint_box_ = lv_obj_create(cam_container_);
  lv_obj_set_pos(cam_hint_box_, 16, kCamFrameY + 108);
  lv_obj_set_size(cam_hint_box_, 208, 58);
  lv_obj_set_style_radius(cam_hint_box_, 6, 0);
  lv_obj_set_style_pad_all(cam_hint_box_, 6, 0);
  lv_obj_set_style_bg_color(cam_hint_box_, lv_color_hex(0x0f172a), 0);
  // Opaque, not translucent. LVGL composites against its own render of the
  // background, not against the panel, so a translucent box over live video
  // would blend with black rather than with the picture.
  lv_obj_set_style_bg_opa(cam_hint_box_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(cam_hint_box_, 1, 0);
  lv_obj_set_style_border_color(cam_hint_box_, current_theme_.jarvis_cyan, 0);
  lv_obj_clear_flag(cam_hint_box_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(cam_hint_box_, LV_OBJ_FLAG_HIDDEN);

  cam_hint_label_ = lv_label_create(cam_hint_box_);
  lv_obj_set_width(cam_hint_label_, 194);
  lv_label_set_long_mode(cam_hint_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(cam_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(cam_hint_label_, current_theme_.assistant_text, 0);
  lv_label_set_text(cam_hint_label_, "");

  cam_built_ = true;
  cam_capturing_ = false;
}

bool Display::EnterCameraView() {
  if (ui_mode_ == UiMode::kCameraView) {
    return true;
  }
  if (panel_ == nullptr) {
    return false;
  }

  const UiMode previous = ui_mode_;
  SetUiMode(UiMode::kCameraView);
  if (ui_mode_ != UiMode::kCameraView) {
    return false;  // SetUiMode refused - not enough heap for the widgets
  }
  cam_return_mode_ = previous;

  // The decoder workspace last, because it is the one allocation that can still
  // fail after the widgets are up. ExitCameraView() is the unwind: the chrome
  // has already been built at this point, and leaving it hidden rather than
  // deleted would leak ~3KB into the fragmented heap for the rest of the
  // session - the exact thing this mode is careful about everywhere else.
  if (!video_sink::Begin(panel_, kCamVideoX, kCamVideoY, kCamInset, kCamInset, kCamCropW, kCamCropH)) {
    ExitCameraView();
    return false;
  }
  SetCameraCapturing(false);
  SetCameraHint(nullptr);
  return true;
}

void Display::ExitCameraView() {
  video_sink::End();

  // Hold the LVGL lock across both the HUD deletion and SetUiMode(back), and delete
  // cam_container_ FIRST. Otherwise SetUiMode(back)'s unlock immediately wakes taskLVGL
  // (priority 2 > loopTask priority 1) to render the homepage while the 15 camera HUD
  // widgets (~2.4KB) are still allocated, starving the heap right as TTS starts.
  lvgl_port_lock(0);
  if (cam_container_ != nullptr) {
    lv_obj_delete(cam_container_);
    cam_container_ = nullptr;
  }
  cam_rec_label_ = nullptr;
  cam_hint_box_ = nullptr;
  cam_hint_label_ = nullptr;
  cam_telemetry_label_ = nullptr;
  cam_built_ = false;
  cam_capturing_ = false;

  const UiMode back = cam_return_mode_;
  SetUiMode(back);
  lvgl_port_unlock();
}

void Display::SuspendCameraVideo() {
  video_sink::End();
}

bool Display::ResumeCameraVideo() {
  if (ui_mode_ != UiMode::kCameraView || panel_ == nullptr) {
    return false;
  }
  return video_sink::Begin(panel_, kCamVideoX, kCamVideoY, kCamInset, kCamInset, kCamCropW, kCamCropH);
}

void Display::SetCameraHint(const char* text) {
  lvgl_port_lock(0);
  if (cam_hint_box_ != nullptr && cam_hint_label_ != nullptr) {
    if (text == nullptr || *text == '\0') {
      lv_obj_add_flag(cam_hint_box_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_label_set_text(cam_hint_label_, text);
      lv_obj_clear_flag(cam_hint_box_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(cam_hint_box_);
    }
  }
  lvgl_port_unlock();
}

void Display::SetCameraTelemetry(const char* text) {
  if (text == nullptr) {
    return;
  }
  lvgl_port_lock(0);
  if (cam_telemetry_label_ != nullptr) {
    lv_label_set_text(cam_telemetry_label_, text);
    lv_obj_align(cam_telemetry_label_, LV_ALIGN_TOP_RIGHT, -6, kCamFrameY + kCamFrameH + 4);
  }
  lvgl_port_unlock();
}

void Display::SetCameraCapturing(bool capturing) {
  lvgl_port_lock(0);
  cam_capturing_ = capturing;
  if (cam_rec_label_ != nullptr) {
    lv_label_set_text(cam_rec_label_, capturing ? "SHOT" : "REC");
    lv_obj_set_style_text_color(cam_rec_label_, capturing ? current_theme_.jarvis_gold : lv_color_hex(0xef4444), 0);
    lv_obj_set_style_text_opa(cam_rec_label_, LV_OPA_COVER, 0);
    lv_obj_align(cam_rec_label_, LV_ALIGN_TOP_RIGHT, -6, 2);
  }
  lvgl_port_unlock();
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
  if (!mouth_open_) {
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

  // Level -2..+2, where 0 means "hidden, the base portrait already shows the hair at rest". The
  // half steps exist purely so a sway never jumps the full 2-3px in one frame.
  static const lv_image_dsc_t* const kBangs[5] = {
      &enco_face_bangs_left, &enco_face_bangs_lhalf, nullptr, &enco_face_bangs_rhalf, &enco_face_bangs_right};
  static const lv_image_dsc_t* const kLocksL[5] = {
      &enco_face_locks_l_left, &enco_face_locks_l_lhalf, nullptr, &enco_face_locks_l_rhalf, &enco_face_locks_l_right};
  static const lv_image_dsc_t* const kLocksR[5] = {
      &enco_face_locks_r_left, &enco_face_locks_r_lhalf, nullptr, &enco_face_locks_r_rhalf, &enco_face_locks_r_right};

  auto set_part = [](lv_obj_t* obj, int8_t level, const lv_image_dsc_t* const* table) {
    const lv_image_dsc_t* dsc = table[level + 2];
    if (dsc == nullptr) {
      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    lv_image_set_src(obj, dsc);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  };

  // Each pose is (bangs, side locks) as a level in [-2, +2]. Consecutive poses never differ by more
  // than one level in either channel, which is what keeps the motion continuous; the side locks are
  // heavier than the bangs, so they lag a beat behind and settle a beat later.
  struct HairPose {
    int8_t bangs;
    int8_t locks;
  };
  static const HairPose kPatterns[4][kHairPoseCount] = {
      // 0: Leftward gust - bangs lift first, the locks follow, then both drift back with a rebound.
      {{-1, 0}, {-2, -1}, {-2, -1}, {-2, -2}, {-2, -2}, {-1, -2}, {0, -2},
       {0, -1}, {1, 0}, {1, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 0}},
      // 1: The same gust from the other side.
      {{1, 0}, {2, 1}, {2, 1}, {2, 2}, {2, 2}, {1, 2}, {0, 2},
       {0, 1}, {-1, 0}, {-1, -1}, {0, -1}, {0, 0}, {0, 0}, {0, 0}},
      // 2: Just the light front bangs stirring; the locks never move, so they are never redrawn.
      {{-1, 0}, {-2, 0}, {-2, 0}, {-1, 0}, {0, 0}, {1, 0}, {2, 0},
       {2, 0}, {1, 0}, {1, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
      // 3: Slow two-way sway, the locks trailing the bangs through the whole arc.
      {{-1, 0}, {-2, -1}, {-1, -2}, {0, -1}, {1, 0}, {2, 1}, {1, 2},
       {0, 1}, {-1, 0}, {-1, -1}, {0, -1}, {0, 0}, {0, 0}, {0, 0}},
  };

  const uint8_t idx = hair_step_ == 0 ? kHairPoseCount : static_cast<uint8_t>(hair_step_ - 1);
  const HairPose pose = idx >= kHairPoseCount ? HairPose{0, 0} : kPatterns[hair_pattern_ & 3][idx];

  // LVGL invalidates an image object whenever its source is set, even to the value it already
  // holds. Redrawing an unchanged side lock costs 8K pixels over the SPI bus, so only touch the
  // sprites whose level actually moved.
  if (pose.bangs != hair_level_bangs_) {
    hair_level_bangs_ = pose.bangs;
    set_part(bangs_overlay_, pose.bangs, kBangs);
  }
  if (pose.locks != hair_level_locks_) {
    hair_level_locks_ = pose.locks;
    set_part(locks_l_overlay_, pose.locks, kLocksL);
    set_part(locks_r_overlay_, pose.locks, kLocksR);
  }
}

void Display::OnFaceTimer(lv_timer_t* timer) {
  auto* self = static_cast<Display*>(lv_timer_get_user_data(timer));
  if (self == nullptr) {
    return;
  }

  // Toast auto-dismiss & smooth fade-out (runs in every UI mode with zero heap overhead).
  if (self->alert_card_ != nullptr && self->alert_hide_at_ms_ != 0) {
    const int32_t remaining_ms = static_cast<int32_t>(self->alert_hide_at_ms_ - lv_tick_get());
    if (remaining_ms <= 0) {
      lv_obj_del(self->alert_card_);
      self->alert_card_ = nullptr;
      self->alert_title_ = nullptr;
      self->alert_body_ = nullptr;
      self->alert_hide_at_ms_ = 0;
    } else if (remaining_ms < 400) {
      const lv_opa_t opa = static_cast<lv_opa_t>((remaining_ms * LV_OPA_COVER) / 400);
      lv_obj_set_style_opa(self->alert_card_, opa, 0);
    }
  }

  // The viewfinder borrows this timer rather than starting one of its own: the
  // only thing moving on that screen is the REC dot, and a second lv_timer is a
  // second allocation plus a second wakeup every 20ms for one label.
  if (self->ui_mode_ == UiMode::kCameraView) {
    if (self->cam_rec_label_ != nullptr && !self->cam_capturing_) {
      self->face_tick_++;
      if (self->face_tick_ >= self->cam_rec_next_tick_) {
        self->cam_rec_on_ = !self->cam_rec_on_;
        self->cam_rec_next_tick_ = self->face_tick_ + 6;  // ~0.5s per phase
        lv_obj_set_style_text_opa(self->cam_rec_label_, self->cam_rec_on_ ? LV_OPA_COVER : LV_OPA_30, 0);
      }
    }
    return;
  }

  if (self->ui_mode_ != UiMode::kRobotFace || self->face_image_ == nullptr) {
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
  // One pose per tick rather than one every other tick: with the half-strength frames available
  // that halves the size of each visible jump instead of doubling the frame rate of a coarse one.
  if (self->hair_step_ != 0) {
    self->hair_step_++;
    if (self->hair_step_ > kHairPoseCount) {
      self->hair_step_ = 0;
      self->next_hair_tick_ = self->face_tick_ + kHairMinGapTicks + (esp_random() % kHairGapSpreadTicks);
    }
    self->ApplyHairFrame();
  } else if (self->face_tick_ >= self->next_hair_tick_) {
    self->hair_pattern_ = static_cast<uint8_t>(esp_random() & 3);
    self->hair_step_ = 1;
    self->ApplyHairFrame();
  }

  // --- Mouth -------------------------------------------------------------------------------
  // Follows the amplifier, not the chat state: see core/audio_playback_signal.h. This is what
  // keeps the lips in step with the speaker when a servo command is answered in the same turn.
  self->mouth_open_ = audio_playback_signal::IsPlaying(kMouthHoldMs);
  self->ApplyMouthFrame();

  // No automatic idle sway. Shifting the portrait repositions every overlay and invalidates the
  // whole 240x240 screen at once, which on this panel reads as the character twitching sideways
  // with a visible flash. The hair breeze above provides the idle movement instead, and it only
  // dirties the three small hair rectangles. LookDirection() still moves the head, but only when
  // the user actually asked for it.
}
