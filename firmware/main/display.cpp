#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>

#include "esp_lvgl_port.h"
#include "face_assets.h"
#include "font_awesome_symbols.h"
#include "core/audio_playback_signal.h"
#include "display.h"
#include "video_sink.h"

// Animation profiler: every 5 s, how long LVGL frames take, how many pixels go to the panel and
// what the warps cost. Off by default; -D ENCO_ANIM_PROFILE=1 to measure.
#ifndef ENCO_ANIM_PROFILE
#define ENCO_ANIM_PROFILE 0
#endif
#if ENCO_ANIM_PROFILE
#include <esp_timer.h>
#include "display/lv_display_private.h"
extern "C" uint32_t lvgl_port_prof_wait_us;
extern "C" uint32_t lvgl_port_prof_chunks;
static struct {
  int64_t window_us, refr_start_us;
  uint32_t frames, refr_us, refr_max_us, px, hair_us, ear_us, ticks, tick_gap_max, last_tick_ms;
  uint32_t layout_us, areas, areas_max, inv_px, full, renders;
} g_prof;
static uint32_t g_obj_us[4];
static void ProfRefr(lv_event_t* e) {
  const int64_t now = esp_timer_get_time();
  if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
    g_prof.refr_start_us = now;
    return;
  }
  if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
    lv_display_t* d = lv_display_get_default();
    g_prof.layout_us += static_cast<uint32_t>(now - g_prof.refr_start_us);
    g_prof.renders++;
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->inv_p; i++) {
      if (d->inv_area_joined[i]) continue;
      n++;
      g_prof.inv_px += lv_area_get_size(&d->inv_areas[i]);
      if (lv_area_get_size(&d->inv_areas[i]) >= 240u * 300u) g_prof.full++;
    }
    g_prof.areas += n;
    g_prof.areas_max = std::max(g_prof.areas_max, static_cast<uint32_t>(d->inv_p));
    return;
  }
  if (g_prof.refr_start_us == 0) {
    return;
  }
  const uint32_t d = static_cast<uint32_t>(now - g_prof.refr_start_us);
  g_prof.refr_start_us = 0;
  g_prof.frames++;
  g_prof.refr_us += d;
  g_prof.refr_max_us = std::max(g_prof.refr_max_us, d);
}
static void ProfTick() {
  const uint32_t ms = lv_tick_get();
  if (g_prof.last_tick_ms != 0) {
    g_prof.tick_gap_max = std::max(g_prof.tick_gap_max, ms - g_prof.last_tick_ms);
  }
  g_prof.last_tick_ms = ms;
  g_prof.ticks++;
  const int64_t now = esp_timer_get_time();
  if (g_prof.window_us == 0) {
    g_prof.window_us = now;
  }
  if (now - g_prof.window_us >= 5000000) {
    const float s = (now - g_prof.window_us) * 1e-6f;
    printf("[anim] spi wait %.1f ms/s, %.1f chunks/frame\n", lvgl_port_prof_wait_us / 1000.0f / s,
           g_prof.frames ? static_cast<float>(lvgl_port_prof_chunks) / g_prof.frames : 0.0f);
    printf("[anim] renders %u, layout %.1f ms/s, areas/render %.1f (max inv_p %u), inv kpx/render %.1f, full %u\n",
           static_cast<unsigned>(g_prof.renders), g_prof.layout_us / 1000.0f / s,
           g_prof.renders ? static_cast<float>(g_prof.areas) / g_prof.renders : 0.0f,
           static_cast<unsigned>(g_prof.areas_max),
           g_prof.renders ? g_prof.inv_px / 1000.0f / g_prof.renders : 0.0f, static_cast<unsigned>(g_prof.full));
    lvgl_port_prof_wait_us = 0;
    lvgl_port_prof_chunks = 0;
    printf("[anim] draw ms/s: image %.1f petals %.1f face-tree %.1f screen-tree %.1f\n", g_obj_us[0] / 1000.0f / s,
           g_obj_us[1] / 1000.0f / s, g_obj_us[2] / 1000.0f / s, g_obj_us[3] / 1000.0f / s);
    memset(g_obj_us, 0, sizeof(g_obj_us));
    {
      // CPU share per task over the window (run-time counters are esp_timer microseconds).
      static TaskStatus_t st[28];
      static struct { UBaseType_t num; uint32_t rt; } prev[28];
      static int n_prev = 0;
      uint32_t total = 0;
      const UBaseType_t n = uxTaskGetSystemState(st, 28, &total);
      printf("[anim] cpu:");
      for (UBaseType_t i = 0; i < n; i++) {
        uint32_t before = st[i].ulRunTimeCounter;
        for (int j = 0; j < n_prev; j++) {
          if (prev[j].num == st[i].xTaskNumber) before = prev[j].rt;
        }
        const float pct = (st[i].ulRunTimeCounter - before) / (s * 1e6f) * 100.0f;
        if (pct >= 2.0f) printf(" %s(p%u,c%d)=%.0f%%", st[i].pcTaskName, (unsigned)st[i].uxCurrentPriority,
                                (int)st[i].xCoreID, pct);
      }
      printf("\n");
      n_prev = static_cast<int>(n);
      for (UBaseType_t i = 0; i < n; i++) {
        prev[i].num = st[i].xTaskNumber;
        prev[i].rt = st[i].ulRunTimeCounter;
      }
    }
    printf("[anim] %.1f fps, frame avg %.1f max %.1f ms, %u kpx/s, hair %.1f ear %.1f ms/s, "
           "motion ticks %.1f/s gap max %u ms\n",
           g_prof.frames / s, g_prof.frames ? g_prof.refr_us / 1000.0f / g_prof.frames : 0.0f,
           g_prof.refr_max_us / 1000.0f, static_cast<unsigned>(g_prof.px / s / 1000), g_prof.hair_us / 1000.0f / s,
           g_prof.ear_us / 1000.0f / s, g_prof.ticks / s, static_cast<unsigned>(g_prof.tick_gap_max));
    const int64_t w = now;
    memset(&g_prof, 0, sizeof(g_prof));
    g_prof.window_us = w;
    g_prof.last_tick_ms = ms;
  }
}
#define PROF_TIME(field, stmt)                          \
  do {                                                  \
    const int64_t _t0 = esp_timer_get_time();           \
    stmt;                                               \
    g_prof.field += static_cast<uint32_t>(esp_timer_get_time() - _t0); \
  } while (0)
// Per-object draw time: [0] portrait image, [1] petal layer, [2] face container incl. children,
// [3] whole screen tree.
static int64_t g_obj_t0[4];
static void ProfObj(lv_event_t* e) {
  const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  const lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_DRAW_MAIN_BEGIN) {
    g_obj_t0[i] = esp_timer_get_time();
  } else if (g_obj_t0[i] != 0) {
    g_obj_us[i] += static_cast<uint32_t>(esp_timer_get_time() - g_obj_t0[i]);
    g_obj_t0[i] = 0;
  }
}
static void ProfWatch(lv_obj_t* obj, int i, bool children) {
  lv_obj_add_event_cb(obj, ProfObj, LV_EVENT_DRAW_MAIN_BEGIN, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
  lv_obj_add_event_cb(obj, ProfObj, children ? LV_EVENT_DRAW_POST_END : LV_EVENT_DRAW_MAIN_END,
                      reinterpret_cast<void*>(static_cast<intptr_t>(i)));
}
#else
#define PROF_TIME(field, stmt) stmt
#endif

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

// Ambient expressions: a random face now and then while nothing is happening, and a quicker,
// attentive one while she listens. Idle is kept rare so it stays a surprise rather than a loop.
// No "angry" in either pool - unprompted, it reads as the device being cross with the user.
static constexpr uint32_t kAmbientIdleMinTicks = 125;      // 10s
static constexpr uint32_t kAmbientIdleSpreadTicks = 190;   // up to a further ~15s
static constexpr uint32_t kAmbientListenMinTicks = 30;     // 2.4s
static constexpr uint32_t kAmbientListenSpreadTicks = 35;  // up to a further 2.8s
static const char* const kAmbientIdleFaces[] = {"happy", "wink", "pout", "shy", "surprised", "thinking", "sad"};
static const char* const kAmbientListenFaces[] = {"thinking", "happy", "surprised", "shy", "wink"};

// The characters compiled in (tools/face_assets, ENCO_CHAR_* in face_assets.h). The first one is
// the default until a saved choice or SetCharacter() says otherwise: the fox leads so she is what a
// fresh flash shows. K3 stays fully available - SetCharacter("k3") or "换角色" brings her back.
static const enco_character_t* const kCharacters[] = {
#if ENCO_CHAR_FOX
    &enco_char_fox,
#endif
#if ENCO_CHAR_K3
    &enco_char_k3,
#endif
};
static constexpr size_t kCharacterCount = sizeof(kCharacters) / sizeof(kCharacters[0]);
static_assert(kCharacterCount > 0, "face_assets.h: at least one ENCO_CHAR_* must be enabled");

static int8_t FindExpression(const enco_character_t* chr, const char* name) {
  for (int8_t i = 0; i < chr->expr_count; i++) {
    if (strcmp(name, chr->exprs[i].name) == 0) {
      return i;
    }
  }
  return -1;
}

// Continuous motion - falling petals and wind in the hair - runs on its own, faster timer. Both
// move a fraction of a pixel per step, and 20 steps a second is what makes that read as gliding
// rather than ticking.
static constexpr uint32_t kMotionTickMs = 50;

// Falling petals. They advance on the motion timer (kMotionTickMs), so speeds are per 50ms tick, in
// 1/16 px. Big petals fall faster than small ones, which reads as depth. The sway is a gentle
// side-to-side swing added at draw time; kPetalSway is one period of it, in px.
static constexpr int8_t kPetalSway[16] = {0, 1, 2, 3, 3, 3, 2, 1, 0, -1, -2, -3, -3, -3, -2, -1};

// Rectangle helpers for the petal layer. LVGL 9.2 has these, but only in a private header.
static bool AreasOverlap(const lv_area_t& a, const lv_area_t& b) {
  return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}
static lv_area_t AreaUnion(const lv_area_t& a, const lv_area_t& b) {
  lv_area_t u;
  u.x1 = std::min(a.x1, b.x1);
  u.y1 = std::min(a.y1, b.y1);
  u.x2 = std::max(a.x2, b.x2);
  u.y2 = std::max(a.y2, b.y2);
  return u;
}

// --- Wind in the hair ---------------------------------------------------------------------------
// sin(2*pi*t), to ~0.1%. Called a few times per warped row, where newlib's sinf would cost more
// than the warp itself.
static inline __attribute__((always_inline)) float SinTurns(float t) {
  t -= floorf(t + 0.5f);                  // [-0.5, 0.5)
  float y = 8.0f * t - 16.0f * t * fabsf(t);  // parabola through the sine's zeros and peaks
  return y + 0.225f * (y * fabsf(y) - y);  // one refinement step
}

// a + (b - a) * f / 32 per channel, for native RGB565. Green is moved to the top half so all three
// channels can be scaled with one multiply without spilling into each other.
static inline __attribute__((always_inline)) uint16_t Lerp565(uint16_t a, uint16_t b, uint32_t f) {
  if (f == 0) {
    return a;
  }
  const uint32_t ea = (a | (static_cast<uint32_t>(a) << 16)) & 0x07E0F81Fu;
  const uint32_t eb = (b | (static_cast<uint32_t>(b) << 16)) & 0x07E0F81Fu;
  const uint32_t r = ((ea * (32 - f) + eb * f) >> 5) & 0x07E0F81Fu;
  return static_cast<uint16_t>(r | (r >> 16));
}

// Warps one side of one row in place. The sideways displacement is 0 at p0, `k` px at p1 and 0 at
// p2, linear in between; each pixel in (p0, p2) is resampled from x + that displacement with 1/32 px
// interpolation. `row` holds screen x ax1 .. ax1 + aw - 1 and [lo, hi] is the part to rewrite.
//
// [b0, b1] (if b0 <= b1) is body - shoulder, arm - lying inside the hair span. It is never moved,
// and the displacement fades out linearly over the `feather` px next to it. Since |k| < feather the
// displacement at distance `dist` stays below `dist`, so body pixels are never sampled either.
//
// No scratch row needed: within one side the displacement never changes sign, so every pixel reads
// only from positions the sweep has not written yet as long as the sweep runs towards the reads -
// left to right when sampling from the right (k > 0), right to left otherwise.
static void IRAM_ATTR WarpSpan(uint16_t* row, int ax1, int aw, int p0, int p1, int p2, int b0, int b1,
                              int feather, float k, int lo, int hi) {
  if (lo > hi || p1 <= p0 || p2 <= p1) {
    return;
  }
  const bool body = b0 <= b1 && feather > 0;
  const int32_t step1 = static_cast<int32_t>(k * 65536.0f / static_cast<float>(p1 - p0));
  const int32_t step2 = static_cast<int32_t>(k * 65536.0f / static_cast<float>(p2 - p1));
  const int xmax = ax1 + aw - 1;
  auto px = [&](int x) __attribute__((always_inline)) {
    int32_t d = x <= p1 ? (x - p0) * step1 : (p2 - x) * step2;
    if (body) {
      const int dist = x < b0 ? b0 - x : (x > b1 ? x - b1 : 0);
      if (dist == 0) {
        return;  // body pixel: left as is
      }
      if (dist < feather) {
        d = d * dist / feather;  // |d| < 8 px in 16.16, so no overflow
      }
    }
    const int32_t s = (x << 16) + d;  // 16.16 source position
    int i = s >> 16;                  // arithmetic shift: floor, also for negatives
    const uint32_t f = static_cast<uint32_t>(s >> 11) & 31;
    const int i0 = std::min(xmax, std::max(ax1, i));
    const int i1 = std::min(xmax, std::max(ax1, i + 1));
    row[x - ax1] = Lerp565(row[i0 - ax1], row[i1 - ax1], f);
  };
  if (k > 0) {
    for (int x = lo; x <= hi; x++) px(x);
  } else {
    for (int x = hi; x >= lo; x--) px(x);
  }
}

static uint32_t AmbientGapTicks(uint8_t mode) {
  return mode == 2 ? kAmbientListenMinTicks + esp_random() % kAmbientListenSpreadTicks
                   : kAmbientIdleMinTicks + esp_random() % kAmbientIdleSpreadTicks;
}

// What the caption pill says when nothing is going on.
//
// It used to read "Enco 正在待命..." - true, but it told the user nothing they could act on. This
// device has exactly one control and no labels on it, so the idle state is the only moment there is
// room to explain it. Both routes named here are real: WakeNet runs continuously (see
// EngineImpl::OnWakeUp), and the boot button is wired to Engine::Advance(), which starts a session
// from standby and interrupts her while she is talking.
static constexpr const char* kIdleCaption = "待命中 · 说 \"Hi 安可\" 唤醒";
static bool s_has_active_chat_subtitle = false;

// Overlay layout (240x320 panel). The status bar is ~26px tall (16px glyphs + 3px padding top and
// bottom + 1px rule). The T-MINUS panel (186x38) sits 8px below it; the alert card (144x148) sits on
// the right 8px below the panel, with an 8px right margin.
static constexpr lv_coord_t kTimerPanelY = 34;
static constexpr lv_coord_t kAlertCardX = 240 - 8 - 144;          // 88
static constexpr lv_coord_t kAlertCardY = kTimerPanelY + 38 + 8;  // 80

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
  chr_ = kCharacters[0];
  // Clear screen to deep space black initially
  std::vector<uint16_t> buffer(width_, 0x0821);
  for (int y = 0; y < height_; y++) {
    esp_lcd_panel_draw_bitmap(panel, 0, y, width_, y + 1, buffer.data());
  }

  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
  lv_init();
  // The character portrait and its sprites are RGB565 C arrays. LVGL's built-in decoder hands
  // those to the renderer in place, straight out of flash, so no custom decoder is needed.

  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  port_cfg.task_priority = 2;
  // Core 0. With the character animating, rendering keeps this task busy nearly all the time, and
  // on core 1 it sat above Arduino's loopTask (pinned there at priority 1), which pumps the voice
  // engine's events. Core 0 otherwise only runs the Wi-Fi stack, which outranks it anyway, and was
  // measured >90% idle.
  port_cfg.task_affinity = 0;
  port_cfg.timer_period_ms = 20;
  // Measured on-device with the RGB565 portrait: peak 7,312 bytes while talking (LVGL's built-in
  // image decoder path runs deeper than the old streaming I4 one, and the vendor default of 7168
  // overflowed at boot). 8704 leaves ~1.4KB of margin.
  port_cfg.task_stack = 8704;
  lvgl_port_init(&port_cfg);

  const lvgl_port_display_cfg_t display_cfg = {
      .io_handle = panel_io,
      .panel_handle = panel,
      .control_handle = nullptr,
      // Two half-size buffers rather than one of width * 10: same 4.8 KB of DMA memory, but LVGL
      // renders (and the pre-flush warps run) on one while the other is still going out over SPI.
      // With a single buffer the CPU sat idle for every transfer - about a fifth of each second
      // once the hair is moving.
      .buffer_size = static_cast<uint32_t>(width * 5),
      .double_buffer = true,
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
  // flush_cb areas include the panel offset; the hair warp works in LVGL screen coordinates.
  disp_off_x_ = static_cast<int16_t>(offset_x);
  disp_off_y_ = static_cast<int16_t>(offset_y);
  lvgl_port_set_pre_flush_cb(OnPreFlush, this);
#if ENCO_ANIM_PROFILE
  lv_display_add_event_cb(display_, ProfRefr, LV_EVENT_REFR_START, nullptr);
  lv_display_add_event_cb(display_, ProfRefr, LV_EVENT_RENDER_START, nullptr);
  lv_display_add_event_cb(display_, ProfRefr, LV_EVENT_REFR_READY, nullptr);
#endif
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

  // Wall clock at the far left ("07:24 PM"). It replaced a 16px emotion glyph inherited from the
  // stock XiaoZhi UI - too small to read, and her face already shows the emotion. Shows --:-- until
  // SNTP has synced; OnFaceTimer() refreshes it once the minute changes.
  clock_label_ = lv_label_create(status_bar_);
  lv_obj_set_style_text_color(clock_label_, current_theme_.jarvis_cyan, 0);
  lv_label_set_text(clock_label_, "--:--");
  lv_obj_set_style_margin_right(clock_label_, 6, 0);  // 添加右边距，与后面的元素分隔

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

// Builds the bitmap character. The picture itself is a 240x320 RGB565 image that LVGL reads
// straight out of flash, so the only heap cost here is the handful of LVGL widget structs.
// Caller must already hold the LVGL lock.
//
// Animation is deliberately done by swapping small sprites over the eyes and mouth rather than by
// redrawing the whole portrait: a blink only dirties a ~116x30 rectangle, a few percent of the
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
  // Matches the portrait's backdrop (the builder snaps K3's to this exact colour), so the strip
  // below the image is seamless with it.
  lv_obj_set_style_bg_color(face_container_, lv_color_hex(chr_->bg_color), 0);
  lv_obj_set_style_bg_opa(face_container_, LV_OPA_COVER, 0);
  lv_obj_set_scrollbar_mode(face_container_, LV_SCROLLBAR_MODE_OFF);
  // The portrait is taller than the area left under the status bar; without this LVGL would make
  // the container scrollable and the image would drift when children are repositioned.
  lv_obj_clear_flag(face_container_, LV_OBJ_FLAG_SCROLLABLE);

  if (ui_mode_ == UiMode::kChatText) {
    lv_obj_add_flag(face_container_, LV_OBJ_FLAG_HIDDEN);
  }

  face_image_ = lv_image_create(face_container_);
  lv_image_set_src(face_image_, chr_->base);
  lv_obj_set_pos(face_image_, 0, 0);

  // All overlays are opaque crops of frames that are bit-identical to the base outside the changed
  // feature, so they composite over it with no seam. Hidden means "use whatever the base already
  // shows there". Hair is
  // created before the eyes and mouth so blinking and speaking always sit above it in z-order.
  // (Only for a character that has hair-breeze frames - see EnsureHairOverlays().)
  EnsureHairOverlays();

  eyes_overlay_ = lv_image_create(face_container_);
  lv_image_set_src(eyes_overlay_, chr_->eyes_shut);
  lv_obj_set_pos(eyes_overlay_, chr_->eyes_x, chr_->eyes_y);
  lv_obj_add_flag(eyes_overlay_, LV_OBJ_FLAG_HIDDEN);

  mouth_overlay_ = lv_image_create(face_container_);
  lv_image_set_src(mouth_overlay_, chr_->mouth_small);
  lv_obj_set_pos(mouth_overlay_, chr_->mouth_x, chr_->mouth_y);
  lv_obj_add_flag(mouth_overlay_, LV_OBJ_FLAG_HIDDEN);

  // Falling petals sit above the face sprites and below the caption pill.
  EnsurePetalLayer();
#if ENCO_ANIM_PROFILE
  ProfWatch(face_image_, 0, false);
  if (petal_layer_ != nullptr) ProfWatch(petal_layer_, 1, false);
  ProfWatch(face_container_, 2, true);
  ProfWatch(lv_screen_active(), 3, true);
#endif

  // The caption pill. This is the bottom half of the reference HUD, reduced to the part that
  // carries information: what she is saying, or - when nothing is happening - how to talk to her.
  //
  // The reference also has an oscilloscope trace, a spectrum bar graph and a second copy of the
  // connection banner. The banner is a duplicate of the top bar and is simply dropped. The two
  // visualisers are not built: at ~430 bytes of heap per LVGL object they would cost several KB of
  // a board that has ~6KB free during TTS, and neither is driven by anything real - there is a
  // playback beacon (audio_playback_signal) but no amplitude, so they would animate to nothing.
  subtitle_box_ = lv_obj_create(face_container_);
  // One line (16px font + 5px padding). Long captions scroll as a marquee rather than wrapping, so
  // the pill never grows over her chin and more of the character stays visible.
  lv_obj_set_size(subtitle_box_, 232, 32);
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
  lv_obj_clear_flag(subtitle_box_, LV_OBJ_FLAG_SCROLLABLE);

  subtitle_label_ = lv_label_create(subtitle_box_);
  lv_obj_set_width(subtitle_label_, 218);
  lv_obj_center(subtitle_label_);
  lv_obj_set_style_text_font(subtitle_label_, &font_puhui_16_4, 0);
  lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xf1f5f9), 0);
  // Centred while it fits; LVGL drops the centring by itself once the text has to scroll.
  lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
  // Circular marquee: text that does not fit glides across and wraps round seamlessly. Short
  // text does not move. 45 px/s is about three characters a second - readable while she speaks.
  lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_anim_duration(subtitle_label_, lv_anim_speed_clamped(45, 300, 30000), 0);
  lv_label_set_text(subtitle_label_, kIdleCaption);

  next_blink_tick_ = kBlinkMinTicks;
  next_hair_tick_ = 14;
  face_timer_ = lv_timer_create(OnFaceTimer, kFaceTickMs, this);
  motion_timer_ = lv_timer_create(OnMotionTimer, kMotionTickMs, this);
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
    s_has_active_chat_subtitle = true;
    if (subtitle_box_ != nullptr) {
      ApplySubtitleVisibility();
    }
    lv_obj_clear_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
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

  // Ambient expressions only run while idle or listening. Leaving those states drops a random face
  // at once (its hold only counts quiet ticks, so it would otherwise ride along into the reply).
  const uint8_t ambient = s == "聆听中" ? 2 : (s.find("待命") == 0 ? 1 : 0);
  if (ambient != ambient_mode_) {
    ambient_mode_ = ambient;
    next_ambient_tick_ = face_tick_ + AmbientGapTicks(ambient);
    if (expr_ambient_ && expr_index_ >= 0) {
      expr_index_ = -1;
      expr_ambient_ = false;
      next_blink_tick_ = face_tick_ + kBlinkMinTicks;
      ApplyBlinkFrame();
      ApplyMouthFrame();
    }
  }

  if (subtitle_label_ != nullptr) {
    if (s == "聆听中") {
      if (!s_has_active_chat_subtitle) {
        lv_label_set_text(subtitle_label_, "正在聆听你的指令...");
      }
      UpdateRobotFaceEmotion("neutral");
    } else if (s.find("待命") == 0) {
      s_has_active_chat_subtitle = false;
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
    // One glyph: the fan's arc count and colour already carry the strength. A signal-bars glyph
    // used to sit beside it, which read as a third, unexplained status icon.
    if (rssi_dbm >= -80) {
      glyphs = FONT_AWESOME_WIFI;
    } else if (rssi_dbm >= -88) {
      glyphs = FONT_AWESOME_WIFI_FAIR;
      colour = current_theme_.jarvis_cyan_dim;  // marginal, but still carrying audio
    } else {
      glyphs = FONT_AWESOME_WIFI_WEAK;
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
  subtitle_timer_hidden_ = hidden;
  ApplySubtitleVisibility();
}

// Caller must hold the LVGL lock.
void Display::ApplySubtitleVisibility() {
  if (subtitle_box_ == nullptr) {
    return;
  }
  if (!caption_enabled_ || subtitle_timer_hidden_) {
    lv_obj_add_flag(subtitle_box_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(subtitle_box_, LV_OBJ_FLAG_HIDDEN);
  }
}

void Display::SetCaptionEnabled(const bool enabled) {
  lvgl_port_lock(0);
  caption_enabled_ = enabled;
  ApplySubtitleVisibility();
  lvgl_port_unlock();
}

void Display::SetAmbientEnabled(const bool enabled) {
  lvgl_port_lock(0);
  ambient_enabled_ = enabled;
  next_ambient_tick_ = face_tick_ + AmbientGapTicks(ambient_mode_);
  if (!enabled && expr_ambient_ && expr_index_ >= 0) {
    expr_index_ = -1;
    expr_ambient_ = false;
    next_blink_tick_ = face_tick_ + kBlinkMinTicks;
    ApplyBlinkFrame();
    ApplyMouthFrame();
  }
  lvgl_port_unlock();
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

  // Top of the screen, just below the status bar with a small gap, so it no longer covers the
  // blue caption/message pill at the bottom.
  timer_panel_ = lv_obj_create(lv_screen_active());
  lv_obj_set_pos(timer_panel_, 27, kTimerPanelY);
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
  return true;
}

void Display::DestroyTimerPanel() {
  if (timer_panel_ == nullptr) {
    return;
  }
  lv_obj_del(timer_panel_);  // deletes its children too
  timer_panel_ = nullptr;
  timer_digits_ = nullptr;
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

    // Right-hand square-ish sci-fi HUD alert box (144x148) with a protruding top-left amber
    // folder tab, horizontal telemetry rules, and a bottom warning badge. Sits below the timer
    // panel with an 8px gap and an 8px right margin.
    alert_card_ = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(alert_card_, kAlertCardX, kAlertCardY);
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
  // The T-MINUS panel just below the status bar is the primary readout. The status bar only carries
  // the countdown as a fallback when the panel is declined - the viewfinder owns the screen, or the
  // heap is low - so the user is never left with no countdown at all.
  if (EnsureTimerPanel() && timer_digits_ != nullptr) {
    char big[16];
    if (hours > 0) {
      snprintf(big, sizeof(big), "%u:%02u:%02u", hours, minutes, seconds);
    } else {
      snprintf(big, sizeof(big), "%02u:%02u", minutes, seconds);
    }
    lv_label_set_text(timer_digits_, big);
    // Panel is up: make sure the status bar is showing its normal status, not a second countdown.
    if (timer_label_ != nullptr && timer_visible_) {
      timer_visible_ = false;
      lv_obj_add_flag(timer_label_, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_flex_grow(timer_label_, 0);
      lv_label_set_text(timer_label_, "");
      if (status_label_ != nullptr) {
        lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
      }
    }
  } else if (timer_label_ != nullptr) {
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
  lvgl_port_unlock();
}

// "时间到" stays up until the caller hides it, which main.cpp does once the assistant has finished
// announcing it. Holding it there is deliberate: the announcement is easy to miss if the room is
// noisy, and the screen is the fallback.
void Display::ShowTimerFinished() {
  lvgl_port_lock(0);
  // With the T-MINUS panel up it alone reports completion; the status bar is only used as the
  // fallback readout when the panel was declined.
  if (timer_panel_ == nullptr && timer_label_ != nullptr) {
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
  // the final second, so without this the panel would freeze on "00:01".
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
  // The status bar used to show a Font Awesome glyph per emotion here; that spot is the clock now,
  // and the emotion is shown on her face instead (below).
  lvgl_port_lock(0);
  UpdateRobotFaceEmotion(emotion);

  // The same emotion on her face. The server's 21 moods fold onto the 8 drawn expressions; the
  // understated ones (neutral, relaxed, confident, sleepy) leave the face alone, and a mood that
  // arrives while an expression is showing replaces it. Held for 2.5s once she stops talking.
  struct EmotionFace {
    const char* emotion;
    const char* face;
  };
  static constexpr EmotionFace kFaces[] = {
      {"happy", "happy"},         {"laughing", "happy"},      {"funny", "happy"},
      {"delicious", "happy"},     {"sad", "sad"},             {"crying", "sad"},
      {"winking", "wink"},        {"silly", "wink"},          {"cool", "wink"},
      {"kissy", "pout"},          {"loving", "pout"},         {"surprised", "surprised"},
      {"shocked", "surprised"},   {"angry", "angry"},         {"embarrassed", "shy"},
      {"thinking", "thinking"},   {"confused", "thinking"},
  };
  for (const auto& entry : kFaces) {
    if (emotion == entry.emotion) {
      ShowExpression(entry.face, 2500, false);
      break;
    }
  }
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
  // It is shrunk at build time rather than with lv_image_set_scale(): scaling
  // at runtime goes through LVGL's transform path, which needs scratch buffers
  // this heap cannot spare while the camera is streaming. See
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
  lv_image_set_src(avatar, chr_->thumb);
  lv_obj_set_pos(avatar, 0, 0);
  cam_avatar_ = avatar;

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
  cam_avatar_ = nullptr;
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
  lvgl_port_lock(0);
  current_emotion_ = emotion;
  if (eyes_overlay_ != nullptr) {
    if (emotion == "sleepy") {
      blink_frame_ = kBlinkFrameShut;
    } else if (blink_frame_ == kBlinkFrameShut) {
      blink_frame_ = 0;
    }
    ApplyBlinkFrame();
  }
  lvgl_port_unlock();
}

bool Display::ShowExpression(const std::string& name, uint32_t hold_ms, bool pinned) {
  int8_t index = -1;
  const bool clear = name.empty() || name == "neutral" || name == "none";
  if (!clear) {
    index = FindExpression(chr_, name.c_str());
    if (index < 0) {
      return false;
    }
  }

  lvgl_port_lock(0);
  if (!pinned && expr_pinned_ && expr_index_ >= 0) {
    lvgl_port_unlock();
    return true;  // An explicit request is still showing; the assistant's mood can wait.
  }
  expr_index_ = index;
  expr_pinned_ = pinned && index >= 0;
  expr_ambient_ = false;
  const uint32_t ticks = hold_ms / kFaceTickMs;
  expr_quiet_ticks_ = static_cast<uint16_t>(ticks > 0xFFFF ? 0xFFFF : ticks);
  if (current_emotion_ != "sleepy") {
    blink_frame_ = 0;  // Never leave a half-finished blink over the new eyes.
  }
  if (index < 0) {
    next_blink_tick_ = face_tick_ + kBlinkMinTicks;
  }
  ApplyBlinkFrame();
  ApplyMouthFrame();
  lvgl_port_unlock();
  return true;
}

void Display::LookDirection(const char* dir) {
  (void)dir;
  // Keep the base portrait and its cropped overlays (eyes, bangs, mouth) strictly at
  // (0, 0). Shifting the base portrait by +/-4px causes partial-redraw misalignment between
  // eyes_overlay_ and face_image_ on the ST7789 panel; physical head motion is handled by the
  // 3-DOF servos instead.
  lvgl_port_lock(0);
  ApplyHeadOffset(0, 0);
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
    lv_obj_set_pos(bangs_overlay_, chr_->bangs_x + dx, chr_->bangs_y + dy);
  }
  if (locks_l_overlay_) {
    lv_obj_set_pos(locks_l_overlay_, chr_->locks_l_x + dx, chr_->locks_l_y + dy);
  }
  if (locks_r_overlay_) {
    lv_obj_set_pos(locks_r_overlay_, chr_->locks_r_x + dx, chr_->locks_r_y + dy);
  }
  if (eyes_overlay_) {
    lv_obj_set_pos(eyes_overlay_, chr_->eyes_x + dx, chr_->eyes_y + dy);
  }
  if (mouth_overlay_) {
    lv_obj_set_pos(mouth_overlay_, chr_->mouth_x + dx, chr_->mouth_y + dy);
  }
}

// Points an overlay at `dsc`, or hides it for nullptr, but only if that is not already what it
// shows: LVGL invalidates an image on every lv_image_set_src(), even to the same source.
static void SetSprite(lv_obj_t* obj, const lv_image_dsc_t* dsc, const void** shown) {
  if (*shown == dsc) {
    return;
  }
  *shown = dsc;
  if (dsc == nullptr) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_image_set_src(obj, dsc);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

void Display::ApplyBlinkFrame() {
  if (eyes_overlay_ == nullptr) {
    return;
  }
  const lv_image_dsc_t* dsc = nullptr;  // Base portrait already has open eyes.
  if (blink_frame_ != 0) {
    dsc = blink_frame_ == kBlinkFrameShut ? chr_->eyes_shut : chr_->eyes_half;
  } else if (expr_index_ >= 0) {
    dsc = chr_->exprs[expr_index_].eyes;
  }
  SetSprite(eyes_overlay_, dsc, &eyes_src_);
}

void Display::ApplyMouthFrame() {
  if (mouth_overlay_ == nullptr) {
    return;
  }
  // Between syllables, and whenever she is quiet, the mouth rests: on the base portrait's soft
  // smile, or on the active expression's mouth.
  const lv_image_dsc_t* rest = expr_index_ >= 0 ? chr_->exprs[expr_index_].mouth : nullptr;
  if (!mouth_open_) {
    SetSprite(mouth_overlay_, rest, &mouth_src_);
    return;
  }
  // A closed / small / wide cycle reads as speech without needing to know anything about the audio.
  // The irregular pattern stops it looking like a metronome.
  static const uint8_t kMouthCycle[] = {1, 2, 1, 0, 2, 1, 2, 0};
  const uint8_t frame = kMouthCycle[(face_tick_ / 2) % (sizeof(kMouthCycle) / sizeof(kMouthCycle[0]))];
  const lv_image_dsc_t* dsc = frame == 0 ? rest : (frame == 2 ? chr_->mouth_wide : chr_->mouth_small);
  SetSprite(mouth_overlay_, dsc, &mouth_src_);
}

void Display::ApplyHairFrame() {
  if (bangs_overlay_ == nullptr || locks_l_overlay_ == nullptr || locks_r_overlay_ == nullptr ||
      chr_->bangs == nullptr || chr_->locks_l == nullptr || chr_->locks_r == nullptr) {
    return;
  }

  // Level -2..+2, where 0 means "hidden, the base portrait already shows the hair at rest". The
  // half steps exist purely so a sway never jumps the full 2-3px in one frame.
  const lv_image_dsc_t* const* kBangs = chr_->bangs;
  const lv_image_dsc_t* const* kLocksL = chr_->locks_l;
  const lv_image_dsc_t* const* kLocksR = chr_->locks_r;

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

bool Display::SetCharacter(const std::string& id) {
  const enco_character_t* found = nullptr;
  for (const enco_character_t* c : kCharacters) {
    if (id == c->id) {
      found = c;
    }
  }
  if (found == nullptr) {
    return false;
  }
  lvgl_port_lock(0);
  if (found != chr_) {
    // A switch briefly holds both characters' hair objects (~1.3KB) and LVGL aborts on a failed
    // allocation, so refuse on a heap that is already on the floor - same bar as SetUiMode().
    if (face_built_ && esp_get_free_heap_size() < 10000) {
      printf("[display] character switch refused (free %u)\n", static_cast<unsigned>(esp_get_free_heap_size()));
      lvgl_port_unlock();
      return false;
    }
    chr_ = found;
    ApplyCharacter();
    printf("[display] character: %s\n", chr_->id);
  }
  lvgl_port_unlock();
  return true;
}

const char* Display::NextCharacter() {
  size_t i = 0;
  while (i < kCharacterCount && kCharacters[i] != chr_) {
    i++;
  }
  SetCharacter(kCharacters[(i + 1) % kCharacterCount]->id);
  return chr_->id;
}

void Display::ApplyCharacter() {
  // Whatever was showing is a sprite of the old character; drop it rather than map it across.
  expr_index_ = -1;
  expr_pinned_ = false;
  expr_ambient_ = false;
  blink_frame_ = current_emotion_ == "sleepy" ? kBlinkFrameShut : 0;
  next_blink_tick_ = face_tick_ + kBlinkMinTicks;
  hair_step_ = 0;
  hair_level_bangs_ = 0;
  hair_level_locks_ = 0;
  next_hair_tick_ = face_tick_ + kHairMinGapTicks;
  ResetEars();

  if (cam_avatar_ != nullptr) {
    lv_image_set_src(cam_avatar_, chr_->thumb);
  }
  if (!face_built_) {
    return;  // BuildRobotFace() will read chr_ when it runs.
  }

  lv_obj_set_style_bg_color(face_container_, lv_color_hex(chr_->bg_color), 0);
  lv_image_set_src(face_image_, chr_->base);

  DestroyHairOverlays();
  EnsureHairOverlays();

  // Sprite rectangles differ per character. Hide both overlays and forget what they showed, so the
  // Apply*Frame() calls below re-point them from scratch.
  lv_obj_set_pos(eyes_overlay_, chr_->eyes_x + head_offset_x_, chr_->eyes_y + head_offset_y_);
  lv_obj_set_pos(mouth_overlay_, chr_->mouth_x + head_offset_x_, chr_->mouth_y + head_offset_y_);
  lv_obj_add_flag(eyes_overlay_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(mouth_overlay_, LV_OBJ_FLAG_HIDDEN);
  eyes_src_ = nullptr;
  mouth_src_ = nullptr;

  DestroyPetalLayer();
  EnsurePetalLayer();

  ApplyBlinkFrame();
  ApplyMouthFrame();
}

void Display::EnsureHairOverlays() {
  if (face_container_ == nullptr || chr_->bangs == nullptr || chr_->locks_l == nullptr ||
      chr_->locks_r == nullptr) {
    return;
  }
  auto make = [this](lv_obj_t*& obj, const lv_image_dsc_t* const* table, int x, int y) {
    if (obj == nullptr) {
      obj = lv_image_create(face_container_);
      // On a character switch the eyes and mouth already exist; hair has to sit underneath them.
      if (eyes_overlay_ != nullptr) {
        lv_obj_move_to_index(obj, lv_obj_get_index(eyes_overlay_));
      }
    }
    lv_image_set_src(obj, table[1]);
    lv_obj_set_pos(obj, x + head_offset_x_, y + head_offset_y_);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  };
  make(bangs_overlay_, chr_->bangs, chr_->bangs_x, chr_->bangs_y);
  make(locks_l_overlay_, chr_->locks_l, chr_->locks_l_x, chr_->locks_l_y);
  make(locks_r_overlay_, chr_->locks_r, chr_->locks_r_x, chr_->locks_r_y);
  hair_level_bangs_ = 0;
  hair_level_locks_ = 0;
}

void Display::DestroyHairOverlays() {
  for (lv_obj_t** obj : {&bangs_overlay_, &locks_l_overlay_, &locks_r_overlay_}) {
    if (*obj != nullptr) {
      lv_obj_delete(*obj);
      *obj = nullptr;
    }
  }
}

void Display::EnsurePetalLayer() {
  if (face_container_ == nullptr || chr_->petals == nullptr || chr_->petal_frames == 0 ||
      chr_->petal_sizes == 0) {
    return;
  }
  if (petal_layer_ == nullptr) {
    // A bare, style-less object the size of the portrait. It paints nothing itself; OnPetalDraw()
    // draws the petals into whatever region LVGL is refreshing.
    petal_layer_ = lv_obj_create(face_container_);
    lv_obj_remove_style_all(petal_layer_);
    lv_obj_set_size(petal_layer_, ENCO_FACE_W, ENCO_FACE_H);
    lv_obj_set_pos(petal_layer_, 0, 0);
    lv_obj_clear_flag(petal_layer_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(petal_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(petal_layer_, OnPetalDraw, LV_EVENT_DRAW_MAIN, this);
    // Above the face sprites, below the caption pill.
    if (subtitle_box_ != nullptr) {
      lv_obj_move_to_index(petal_layer_, lv_obj_get_index(subtitle_box_));
    }
  }
  // Scatter the first batch over the whole height so the screen is not empty for the first
  // few seconds while they fall in from the top.
  for (int i = 0; i < kPetalCount; i++) {
    SpawnPetal(i, true);
  }
  lv_obj_invalidate(petal_layer_);
}

void Display::DestroyPetalLayer() {
  if (petal_layer_ != nullptr) {
    lv_obj_delete(petal_layer_);  // deleting invalidates the area it covered
    petal_layer_ = nullptr;
  }
}

// Puts petal `index` at a random spot in its lane: even petals fall left of the face, odd ones to
// the right. `anywhere` scatters it over the full height; otherwise it starts just above the top.
void Display::SpawnPetal(int index, bool anywhere) {
  Petal& p = petals_[index];
  // One in three is a big (near) petal.
  p.size = static_cast<uint8_t>(esp_random() % 3 == 0 ? 1 : 0);
  if (p.size >= chr_->petal_sizes) {
    p.size = chr_->petal_sizes - 1;
  }
  const lv_image_dsc_t* dsc = chr_->petals[p.size * chr_->petal_frames];
  const int w = dsc->header.w;
  const int h = dsc->header.h;
  const bool left = (index & 1) == 0;
  // The lane leaves room for the sway (kPetalSway, +-3px) and a little inward drift.
  int x0 = left ? -w / 2 : chr_->petal_lane_r + 6;
  int x1 = left ? chr_->petal_lane_l - w - 6 : ENCO_FACE_W - w / 2;
  if (x1 < x0) {
    x1 = x0;
  }
  const int x = x0 + static_cast<int>(esp_random() % static_cast<uint32_t>(x1 - x0 + 1));
  const int y = anywhere ? static_cast<int>(esp_random() % ENCO_FACE_H) - h
                         : -h - static_cast<int>(esp_random() % 90);
  p.x16 = static_cast<int16_t>(x * 16);
  p.y16 = static_cast<int16_t>(y * 16);
  // 0.5-0.75 px per tick for small petals, 0.75-1.1 for big ones (10-22 px/s).
  p.vy16 = static_cast<int8_t>(p.size ? 12 + esp_random() % 6 : 8 + esp_random() % 5);
  // Mostly drifting outward, never more than 1/16 px per tick toward her face.
  const int drift = static_cast<int>(esp_random() % 4) - 1;  // -1..2
  p.vx16 = static_cast<int8_t>(left ? -drift : drift);
  p.frame = static_cast<uint8_t>(esp_random() % chr_->petal_frames);
  p.spin = static_cast<uint8_t>(3 + esp_random() % 4);
  p.age = static_cast<uint8_t>(esp_random());
  p.draw_x = static_cast<int16_t>(x + kPetalSway[(p.age >> 3) & 15]);
  p.draw_y = static_cast<int16_t>(y);
}

void Display::UpdatePetals() {
  if (petal_layer_ == nullptr || chr_->petals == nullptr) {
    return;
  }
  lv_area_t origin;
  lv_obj_get_coords(petal_layer_, &origin);
  const int frames = chr_->petal_frames;
  // Only the part of the portrait under the status bar is on screen.
  const int bottom = face_container_ != nullptr ? lv_obj_get_height(face_container_) : ENCO_FACE_H;

  auto area_of = [&](const Petal& p) {
    const lv_image_dsc_t* dsc = chr_->petals[p.size * frames + p.frame];
    lv_area_t a;
    a.x1 = origin.x1 + p.draw_x;
    a.y1 = origin.y1 + p.draw_y;
    a.x2 = a.x1 + dsc->header.w - 1;
    a.y2 = a.y1 + dsc->header.h - 1;
    return a;
  };

  for (int i = 0; i < kPetalCount; i++) {
    Petal& p = petals_[i];
    lv_area_t before = area_of(p);
    const uint8_t old_frame = p.frame;

    p.x16 = static_cast<int16_t>(p.x16 + p.vx16);
    p.y16 = static_cast<int16_t>(p.y16 + p.vy16);
    p.age++;
    if (p.age % p.spin == 0) {
      p.frame = static_cast<uint8_t>((p.frame + 1) % frames);
    }
    int x = (p.x16 >> 4) + kPetalSway[(p.age >> 3) & 15];
    int y = p.y16 >> 4;
    if (y >= bottom || x < -24 || x > ENCO_FACE_W + 8) {
      SpawnPetal(i, false);
      x = p.draw_x;
      y = p.draw_y;
    }
    if (x == p.draw_x && y == p.draw_y && p.frame == old_frame) {
      continue;  // nothing visible changed; leave the pixels alone
    }
    p.draw_x = static_cast<int16_t>(x);
    p.draw_y = static_cast<int16_t>(y);
    lv_area_t after = area_of(p);
    // One rectangle if the two overlap (the usual 1-2px step), two otherwise (a respawn).
    if (AreasOverlap(before, after)) {
      lv_area_t both = AreaUnion(before, after);
      lv_obj_invalidate_area(petal_layer_, &both);
    } else {
      lv_obj_invalidate_area(petal_layer_, &before);
      lv_obj_invalidate_area(petal_layer_, &after);
    }
  }
}

void Display::OnPetalDraw(lv_event_t* e) {
  auto* self = static_cast<Display*>(lv_event_get_user_data(e));
  if (self == nullptr || self->chr_->petals == nullptr) {
    return;
  }
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_obj_t* obj = lv_event_get_current_target_obj(e);
  lv_area_t origin;
  lv_obj_get_coords(obj, &origin);
  const int frames = self->chr_->petal_frames;
  for (const Petal& p : self->petals_) {
    const lv_image_dsc_t* dsc = self->chr_->petals[p.size * frames + p.frame];
    lv_area_t a;
    a.x1 = origin.x1 + p.draw_x;
    a.y1 = origin.y1 + p.draw_y;
    a.x2 = a.x1 + dsc->header.w - 1;
    a.y2 = a.y1 + dsc->header.h - 1;
    if (!AreasOverlap(a, layer->_clip_area)) {
      continue;  // Most redraws (a blink, a mouth frame) touch none of the petals.
    }
    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src = dsc;
    lv_draw_image(layer, &img, &a);
  }
}

void Display::OnMotionTimer(lv_timer_t* timer) {
  auto* self = static_cast<Display*>(lv_timer_get_user_data(timer));
  if (self == nullptr || self->ui_mode_ != UiMode::kRobotFace || self->face_image_ == nullptr) {
    return;
  }
  // Same floor as the face timer: when memory is this scarce the audio pipeline comes first. The
  // picture simply holds still - the warp keeps using the last wind state, so nothing tears.
  if (esp_get_free_heap_size() < 12000) {
    return;
  }
  // Hair and ears first: LVGL drops a newly invalidated area that lies inside one it already has,
  // so petals drifting through the hair bands then cost nothing extra.
  self->UpdateHairFlow();
  self->UpdateEars();
  self->UpdatePetals();
#if ENCO_ANIM_PROFILE
  ProfTick();
#endif
}

// Advances the wind and marks the hair bands for repaint. The warp itself happens in
// WarpHairChunk(), on the pixels LVGL has just rendered, so it needs no image memory of its own.
//
// The wind is a few slow waves layered so it never visibly repeats:
//   billow  - a swell travelling down the hair (2.6s, 140px wavelength): the whole lock lifts and
//             settles, lower strands a beat behind the upper ones;
//   flutter - a quicker, shorter ripple (0.95s, 48px) along the strands;
//   gust    - how hard it is blowing, drifting over ~7s;
//   wind    - a slow sideways lean (11s): one side is blown out while the other is pressed in.
void Display::UpdateHairFlow() {
  const enco_hair_flow_t* f = chr_->hair_flow;
  if (f == nullptr || face_container_ == nullptr || lv_obj_has_flag(face_container_, LV_OBJ_FLAG_HIDDEN)) {
    return;
  }
  // Phases straight from the tick counter, each wrapped to its own period, so they stay exact no
  // matter how long she has been running (a float seconds counter would lose precision in hours).
  const uint32_t ms = lv_tick_get();
  auto turn = [ms](uint32_t period) { return static_cast<float>(ms % period) / static_cast<float>(period); };
  flow_gust_ = 0.65f + 0.35f * SinTurns(turn(6700)) * SinTurns(turn(2300) + 0.11f);
  flow_wind_ = 0.55f * SinTurns(turn(11000));
  flow_billow_ = turn(2600);
  flow_flutter_ = turn(950);

  // Repaint each side in horizontal slabs hugging the band's outline, rather than one box: the
  // bands are narrow up by her temples and widen towards the shoulders. Lower down, where a still
  // shoulder or arm lies inside the band, a slab is split into the hair outside it and the hair
  // inside it - WarpSpan() never changes body pixels, so there is nothing new to show there.
  // Rows with no amplitude are never warped and are left out too. Stops above the caption pill,
  // which is never warped. At most 2 sides x 5 slabs x 2 boxes = 20 areas, well within LVGL's
  // 32-area invalidation buffer alongside the petals (overflowing it repaints the whole screen).
  lv_area_t img;
  lv_obj_get_coords(face_image_, &img);
  lv_area_t box;
  lv_obj_get_coords(face_container_, &box);
  int32_t bottom = box.y2;
  if (subtitle_box_ != nullptr && !lv_obj_has_flag(subtitle_box_, LV_OBJ_FLAG_HIDDEN)) {
    lv_area_t pill;
    lv_obj_get_coords(subtitle_box_, &pill);
    bottom = std::min<int32_t>(bottom, pill.y1 - 4);
  }
  const int last_row = std::min<int>(f->rows - 1, bottom - img.y1 - f->y0);
  constexpr int kSlabs = 5;
  auto invalidate = [&](int lo, int hi, int r0, int r1) {
    lv_area_t a;
    a.x1 = img.x1 + std::max(0, lo);
    a.x2 = img.x1 + std::min(ENCO_FACE_W - 1, hi);
    a.y1 = img.y1 + f->y0 + r0;
    a.y2 = img.y1 + f->y0 + r1;
    if (a.x1 <= a.x2 && a.y1 <= a.y2) {
      lv_obj_invalidate_area(face_container_, &a);
    }
  };
  for (int side = 0; side < 2; side++) {
    for (int slab = 0; slab < kSlabs; slab++) {
      const int s0 = f->rows * slab / kSlabs;
      const int s1 = std::min(f->rows * (slab + 1) / kSlabs - 1, last_row);
      // Outer box: from p0 to the body's near edge; inner box: from the body's far edge to p2
      // (rows without body count towards both, so each box still covers their whole span).
      int olo = ENCO_FACE_W, ohi = -1, ilo = ENCO_FACE_W, ihi = -1;
      int r0 = -1, r1 = -1;
      for (int r = s0; r <= s1; r++) {
        if (f->amp[r] == 0) {
          continue;
        }
        const int16_t* s = f->spans + r * 10 + side * 5;
        const bool body = s[3] <= s[4] && f->feather > 0;
        olo = std::min<int>(olo, s[0] + 1);
        ohi = std::max<int>(ohi, body ? s[3] - 1 : s[2] - 1);
        ilo = std::min<int>(ilo, body ? s[4] + 1 : s[0] + 1);
        ihi = std::max<int>(ihi, s[2] - 1);
        if (r0 < 0) r0 = r;
        r1 = r;
      }
      if (r0 < 0) {
        continue;
      }
      if (ohi + 1 >= ilo) {
        invalidate(std::min(olo, ilo), std::max(ohi, ihi), r0, r1);  // they touch: one box
      } else {
        invalidate(olo, ohi, r0, r1);
        invalidate(ilo, ihi, r0, r1);
      }
    }
  }
}

void Display::OnPreFlush(const lv_area_t* area, uint8_t* px_map, void* ctx) {
  auto* self = static_cast<Display*>(ctx);
  if (self != nullptr) {
    // Ears first: they compare against the untouched portrait to spot overlays (see there).
    PROF_TIME(ear_us, self->WarpEarsChunk(area, reinterpret_cast<uint16_t*>(px_map)));
    PROF_TIME(hair_us, self->WarpHairChunk(area, reinterpret_cast<uint16_t*>(px_map)));
#if ENCO_ANIM_PROFILE
    g_prof.px += static_cast<uint32_t>((area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1));
#endif
  }
}

// Called for every chunk LVGL sends to the panel (at most 5 full-width rows' worth of pixels with
// our draw buffers), right
// before the byte swap. Rows that cross a hair band are resampled sideways in place.
void IRAM_ATTR Display::WarpHairChunk(const lv_area_t* area, uint16_t* px) {
  const enco_hair_flow_t* f = chr_->hair_flow;
  if (f == nullptr || ui_mode_ != UiMode::kRobotFace || face_image_ == nullptr || face_container_ == nullptr ||
      lv_obj_has_flag(face_container_, LV_OBJ_FLAG_HIDDEN)) {
    return;
  }
  const int ax1 = area->x1 - disp_off_x_;
  const int ay1 = area->y1 - disp_off_y_;
  const int ax2 = area->x2 - disp_off_x_;
  const int ay2 = area->y2 - disp_off_y_;
  const int aw = ax2 - ax1 + 1;

  lv_area_t img;
  lv_obj_get_coords(face_image_, &img);
  const int first = std::max<int>(ay1, img.y1 + f->y0);
  const int last = std::min<int>(ay2, img.y1 + f->y0 + f->rows - 1);
  if (first > last) {
    return;
  }

  // Things drawn over the portrait that must not bend with the hair.
  lv_area_t keep[3];
  int n_keep = 0;
  auto keep_out = [&](lv_obj_t* obj, int pad) {
    if (obj != nullptr && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_get_coords(obj, &keep[n_keep]);
      keep[n_keep].x1 -= pad;
      keep[n_keep].y1 -= pad;
      keep[n_keep].x2 += pad;
      keep[n_keep].y2 += pad;
      n_keep++;
    }
  };
  keep_out(subtitle_box_, 4);  // border + outline ring
  keep_out(alert_card_, 2);
  keep_out(timer_panel_, 2);

  const float unit = 1.0f / static_cast<float>(f->amp_unit);
  for (int y = first; y <= last; y++) {
    const int r = y - img.y1 - f->y0;
    const uint8_t amp = f->amp[r];
    if (amp == 0) {
      continue;
    }
    const float a = amp * unit;
    const float py = static_cast<float>(f->y0 + r);
    const float billow = flow_billow_ - py * (1.0f / 140.0f);
    const float flutter = flow_flutter_ - py * (1.0f / 48.0f);
    // Outward displacement of each side's outer strands, in px (negative = blown inward).
    const float out_l = a * (0.75f * flow_gust_ * (0.5f + 0.5f * SinTurns(billow)) +
                             0.22f * SinTurns(flutter) - 0.5f * flow_wind_);
    const float out_r = a * (0.75f * flow_gust_ * (0.5f + 0.5f * SinTurns(billow + 0.3f)) +
                             0.22f * SinTurns(flutter + 0.38f) + 0.5f * flow_wind_);

    // Per side: p0, p1, p2 (hair band), b0, b1 (body inside it; b0 > b1 when none).
    const int16_t* s = f->spans + r * 10;
    uint16_t* row = px + (y - ay1) * aw;
    for (int side = 0; side < 2; side++) {
      const int16_t* ss = s + side * 5;
      const int p0 = img.x1 + ss[0];
      const int p1 = img.x1 + ss[1];
      const int p2 = img.x1 + ss[2];
      const int b0 = img.x1 + ss[3];
      const int b1 = img.x1 + ss[4];
      // Left: sample from x + out (content moves left = outward). Right: from x - out.
      const float k = side == 0 ? out_l : -out_r;
      if (fabsf(k) < 0.03f) {
        continue;
      }
      int lo = std::max(p0 + 1, ax1);
      int hi = std::min(p2 - 1, ax2);
      for (int i = 0; i < n_keep && lo <= hi; i++) {
        const lv_area_t& e = keep[i];
        if (y < e.y1 || y > e.y2 || e.x2 < lo || e.x1 > hi) {
          continue;
        }
        if (e.x1 <= lo && e.x2 >= hi) {
          lo = hi + 1;  // fully covered
        } else if (e.x1 <= lo) {
          lo = e.x2 + 1;
        } else {
          hi = e.x1 - 1;
        }
      }
      WarpSpan(row, ax1, aw, p0, p1, p2, b0, b1, f->feather, k, lo, hi);
    }
  }
}

// --- Twitching ears ------------------------------------------------------------------------------
//
// The motion follows how fox- and cat-eared characters are animated to read as cute and alive:
//  - a twitch is a flick, not a sweep: a very fast push (held ~60-70 ms) and a springy return
//    that overshoots a little past rest before it settles;
//  - the tip trails the root, so the ear bends and whips rather than turning like a board;
//  - the two ears are rarely in lockstep - one leads and the other follows a few tens of ms later,
//    and single or double flicks of one ear read as more alive than both at once;
//  - the ears carry the mood: perked (tips in) when alert or surprised, splayed and drooping when
//    sad or shy, pinned back when cross, one tilted when thinking, perked while listening;
//  - dangling ornaments hang plumb and keep swinging after the ear stops (secondary motion).

using EarStep = Display::EarStep;

// 抖耳朵: a quick shake of both ears (the second trailing by 30 ms), a flick of each, then a perk.
static const EarStep kEarShake[] = {
    {0, 60, 1, 11},    {30, 60, 2, 11},   {120, 60, 1, -7}, {150, 60, 2, -7},
    {240, 60, 1, 10},  {270, 60, 2, 10},  {360, 60, 1, -6}, {390, 60, 2, -6},
    {480, 55, 1, 8},   {510, 55, 2, 8},   {600, 50, 3, -3},
    {950, 70, 1, 12},  {1250, 70, 2, 12}, {1600, 110, 3, -6},
};
static const EarStep kEarFlick[] = {{0, 70, 1, 10}};
static const EarStep kEarDoubleFlick[] = {{0, 60, 1, 10}, {170, 60, 1, 8}};
static const EarStep kEarBoth[] = {{0, 70, 1, 9}, {40, 70, 2, 9}};
static const EarStep kEarAlternate[] = {{0, 70, 1, 10}, {220, 70, 2, 10}};
static const EarStep kEarPerk[] = {{0, 90, 3, -8}};
#define EAR_SCRIPT(s) s, static_cast<uint8_t>(sizeof(s) / sizeof(s[0]))

constexpr float kDeg = 0.01745329f;
constexpr float kEarMaxOut = 0.30f;      // rad either way; the rig's margins are sized for this
constexpr float kEarTipMaxBend = 0.12f;  // rad the tip may trail the root
constexpr float kEarRootHz = 7.0f, kEarRootZeta = 0.35f;  // snappy, ~30% overshoot
constexpr float kEarTipHz = 5.0f, kEarTipZeta = 0.3f;
constexpr float kEarPendulumPx = 60.0f;  // how hard an ornament is swung by its anchor's jerk
constexpr float kTwoPi = 6.2831853f;

// Mood poses: outward degrees for (ear 0, ear 1) and how quickly the ears settle into them.
struct EarPose {
  const char* expr;
  int8_t out0, out1;
  uint16_t tau_ms;
};
static const EarPose kEarPoses[] = {
    {"happy", -3, -3, 120}, {"surprised", -7, -7, 40}, {"sad", 14, 14, 450},  {"angry", 11, 11, 90},
    {"shy", 8, 8, 220},     {"thinking", 7, -2, 260},  {"pout", 5, 5, 160},   {"wink", 0, 0, 150},
};

// One semi-implicit Euler step of a damped spring pulling `x` towards `target`.
static inline void EarSpring(float& x, float& v, float target, float hz, float zeta, float h) {
  const float w = kTwoPi * hz;
  v += (w * w * (target - x) - 2.0f * zeta * w * v) * h;
  x += v * h;
}

void Display::ResetEars() {
  for (auto& e : ears_) {
    e = Ear{};
  }
  ear_script_ = nullptr;
  ear_last_ms_ = 0;
  ear_last_expr_ = -1;
  ear_next_idle_ms_ = lv_tick_get() + 6000 + esp_random() % 6000;
}

void Display::StartEarScript(const EarStep* steps, uint8_t len, float gain, bool mirror) {
  ear_script_ = steps;
  ear_script_len_ = len;
  ear_script_gain_ = gain;
  ear_script_mirror_ = mirror ? 1 : 0;
  ear_script_t0_ = lv_tick_get();
}

bool Display::TwitchEars() {
  lvgl_port_lock(0);
  const bool ok = chr_->ear_count > 0;
  if (ok) {
    StartEarScript(EAR_SCRIPT(kEarShake), 1.0f, false);
    ear_next_idle_ms_ = lv_tick_get() + 8000 + esp_random() % 6000;
  }
  lvgl_port_unlock();
  return ok;
}

// Advances the ear springs by the time since the last tick and repaints the ears if they moved.
void Display::UpdateEars() {
  const int n = std::min<int>(chr_->ear_count, kMaxEars);
  if (n == 0 || face_container_ == nullptr || lv_obj_has_flag(face_container_, LV_OBJ_FLAG_HIDDEN)) {
    return;
  }
  const uint32_t now = lv_tick_get();
  uint32_t dt_ms = now - ear_last_ms_;
  if (ear_last_ms_ == 0) {
    // First tick (boot, new character): ornament anchors start where the art has them.
    for (int e = 0; e < n; e++) {
      for (int i = 0; i < chr_->ears[e].deco_count && i < 4; i++) {
        ears_[e].anchor_x[i] = chr_->ears[e].deco[i].x;
        ears_[e].anchor_vx[i] = 0;
      }
    }
    dt_ms = kMotionTickMs;
  }
  ear_last_ms_ = now;
  if (dt_ms > 200) {
    dt_ms = kMotionTickMs;  // back from a pause (chat mode, low heap): carry on, don't leap
  }

  // Mood pose from the expression on screen, with a little accent when it changes.
  float pose[kMaxEars] = {0, 0};
  uint16_t tau_ms = 200;
  const char* expr = expr_index_ >= 0 ? chr_->exprs[expr_index_].name : nullptr;
  if (expr != nullptr) {
    for (const auto& p : kEarPoses) {
      if (strcmp(p.expr, expr) == 0) {
        pose[0] = p.out0 * kDeg;
        pose[1] = p.out1 * kDeg;
        tau_ms = p.tau_ms;
        break;
      }
    }
  } else if (current_emotion_ == "sleepy") {
    pose[0] = pose[1] = 10 * kDeg;
    tau_ms = 600;
  } else if (ambient_mode_ == 2) {
    pose[0] = pose[1] = -3 * kDeg;  // listening: perked, all ears
    tau_ms = 150;
  }
  if (expr_index_ != ear_last_expr_) {
    ear_last_expr_ = expr_index_;
    if (expr != nullptr && ear_script_ == nullptr) {
      const bool mirror = esp_random() & 1;
      if (strcmp(expr, "happy") == 0) {
        StartEarScript(EAR_SCRIPT(kEarPerk), 0.8f, false);
      } else if (strcmp(expr, "surprised") == 0) {
        StartEarScript(EAR_SCRIPT(kEarPerk), 1.2f, false);
      } else if (strcmp(expr, "wink") == 0) {
        StartEarScript(EAR_SCRIPT(kEarFlick), 1.0f, mirror);
      } else if (strcmp(expr, "pout") == 0) {
        StartEarScript(EAR_SCRIPT(kEarDoubleFlick), 0.9f, mirror);
      } else if (strcmp(expr, "shy") == 0) {
        StartEarScript(EAR_SCRIPT(kEarFlick), 0.6f, mirror);
      }
    }
  }

  // Now and then, an idle twitch - mostly one ear, never the same pattern on a clock.
  if (ear_script_ == nullptr && static_cast<int32_t>(now - ear_next_idle_ms_) >= 0) {
    const uint32_t r = esp_random() % 100;
    const bool mirror = esp_random() & 1;
    const float gain = 0.7f + static_cast<float>(esp_random() % 31) * 0.01f;
    if (r < 45) {
      StartEarScript(EAR_SCRIPT(kEarFlick), gain, mirror);
    } else if (r < 70) {
      StartEarScript(EAR_SCRIPT(kEarDoubleFlick), gain, mirror);
    } else if (r < 85) {
      StartEarScript(EAR_SCRIPT(kEarBoth), gain, mirror);
    } else {
      StartEarScript(EAR_SCRIPT(kEarAlternate), gain, mirror);
    }
    ear_next_idle_ms_ = now + 5000 + esp_random() % 9000;
  }

  // Integrate in ~5 ms steps: the kicks are only 60-70 ms long and the springs are stiff.
  const int steps = std::max<int>(1, (dt_ms + 4) / 5);
  const float h = static_cast<float>(dt_ms) * 0.001f / static_cast<float>(steps);
  const float pose_k = std::min(1.0f, h * 1000.0f / static_cast<float>(tau_ms));
  uint32_t script_end = 0;
  for (int k = 0; k < steps; k++) {
    float kick[kMaxEars] = {0, 0};
    if (ear_script_ != nullptr) {
      const uint32_t t = now - dt_ms + static_cast<uint32_t>((k + 1) * dt_ms / steps) - ear_script_t0_;
      for (int s = 0; s < ear_script_len_; s++) {
        const EarStep& st = ear_script_[s];
        script_end = std::max<uint32_t>(script_end, st.at_ms + st.hold_ms);
        if (t >= st.at_ms && t < static_cast<uint32_t>(st.at_ms + st.hold_ms)) {
          const uint8_t m = ear_script_mirror_ ? static_cast<uint8_t>(((st.ears & 1) << 1) | ((st.ears >> 1) & 1))
                                               : st.ears;
          for (int e = 0; e < n; e++) {
            if (m & (1 << e)) {
              kick[e] += st.deg * kDeg * ear_script_gain_;
            }
          }
        }
      }
    }
    for (int e = 0; e < n; e++) {
      Ear& E = ears_[e];
      const enco_ear_t& r = chr_->ears[e];
      E.pose += (pose[e] - E.pose) * pose_k;
      const float target = std::min(kEarMaxOut, std::max(-kEarMaxOut, E.pose + kick[e]));
      EarSpring(E.base, E.base_v, target, kEarRootHz, kEarRootZeta, h);
      E.base = std::min(kEarMaxOut, std::max(-kEarMaxOut, E.base));
      EarSpring(E.tip, E.tip_v, E.base, kEarTipHz, kEarTipZeta, h);
      E.tip = std::min(E.base + kEarTipMaxBend, std::max(E.base - kEarTipMaxBend, E.tip));

      // Ornaments: a spring towards the ear's angle - or, by `hang`, towards plumb - swung by the
      // sideways jerk of the point they hang from.
      const float sb = r.out_sign * E.base;
      const float st = r.out_sign * E.tip;
      const float lx = static_cast<float>(r.tip_x - r.pivot_x);
      const float ly = static_cast<float>(r.tip_y - r.pivot_y);
      const float inv_l2 = 1.0f / (lx * lx + ly * ly);
      for (int i = 0; i < r.deco_count && i < 4; i++) {
        const enco_ear_deco_t& d = r.deco[i];
        const float vx = static_cast<float>(d.x - r.pivot_x);
        const float vy = static_cast<float>(d.y - r.pivot_y);
        const float s = std::min(1.0f, std::max(0.0f, (vx * lx + vy * ly) * inv_l2));
        const float th = sb + (st - sb) * s;
        const float x = r.pivot_x + cosf(th) * vx - sinf(th) * vy;
        const float vel = (x - E.anchor_x[i]) / h;
        const float acc = (vel - E.anchor_vx[i]) / h;
        E.anchor_x[i] = x;
        E.anchor_vx[i] = vel;
        const float hang = d.hang * (1.0f / 255.0f);
        const float w = kTwoPi * d.hz10 * 0.1f;
        const float z = d.zeta100 * 0.01f;
        // Anchor jerked right -> the ornament's lower end lags left -> clockwise (+).
        E.deco_v[i] += (w * w * ((1.0f - hang) * sb - E.deco[i]) - 2.0f * z * w * E.deco_v[i] +
                        acc / kEarPendulumPx) * h;
        E.deco[i] = std::min(0.4f, std::max(-0.4f, E.deco[i] + E.deco_v[i] * h));
      }
    }
  }
  if (ear_script_ != nullptr && now - ear_script_t0_ > script_end + 20) {
    ear_script_ = nullptr;
  }

  // Warp constants for WarpEarsChunk(), and a repaint if anything visibly moved.
  lv_area_t img;
  lv_obj_get_coords(face_image_, &img);
  lv_area_t clip;
  lv_obj_get_coords(face_container_, &clip);
  for (int e = 0; e < n; e++) {
    Ear& E = ears_[e];
    const enco_ear_t& r = chr_->ears[e];
    E.sb = r.out_sign * E.base;
    E.st = r.out_sign * E.tip;
    float state[6] = {E.sb, E.st, 0, 0, 0, 0};
    float biggest = std::max(fabsf(E.sb), fabsf(E.st));
    const float lx = static_cast<float>(r.tip_x - r.pivot_x);
    const float ly = static_cast<float>(r.tip_y - r.pivot_y);
    const float inv_l2 = 1.0f / (lx * lx + ly * ly);
    for (int i = 0; i < r.deco_count && i < 4; i++) {
      const enco_ear_deco_t& d = r.deco[i];
      const float vx = static_cast<float>(d.x - r.pivot_x);
      const float vy = static_cast<float>(d.y - r.pivot_y);
      const float s = std::min(1.0f, std::max(0.0f, (vx * lx + vy * ly) * inv_l2));
      const float th = E.sb + (E.st - E.sb) * s;
      E.fx[i] = r.pivot_x + cosf(th) * vx - sinf(th) * vy;
      E.fy[i] = r.pivot_y + sinf(th) * vx + cosf(th) * vy;
      E.dcos[i] = cosf(E.deco[i]);
      E.dsin[i] = sinf(E.deco[i]);
      state[2 + i] = E.deco[i];
      biggest = std::max(biggest, fabsf(E.deco[i]));
    }
    E.active = biggest > 0.002f;
    float moved = 0;
    for (int j = 0; j < 6; j++) {
      moved = std::max(moved, fabsf(state[j] - E.drawn[j]));
    }
    if (moved > 0.0006f) {
      memcpy(E.drawn, state, sizeof(state));
      lv_area_t a;
      a.x1 = std::max<int32_t>(clip.x1, img.x1 + r.box_x);
      a.y1 = std::max<int32_t>(clip.y1, img.y1 + r.box_y);
      a.x2 = std::min<int32_t>(clip.x2, img.x1 + r.box_x + r.box_w - 1);
      a.y2 = std::min<int32_t>(clip.y2, img.y1 + r.box_y + r.box_h - 1);
      if (a.x1 <= a.x2 && a.y1 <= a.y2) {
        lv_obj_invalidate_area(face_container_, &a);
      }
    }
  }
}

// Rotates the ears inside one flushed chunk. Unlike the hair, a rotation moves pixels across rows,
// and a chunk only holds a few of them - so the source is the base portrait in flash, which is always
// all there. Anything drawn over the portrait (a falling petal, a card) shows up as a pixel that
// differs from the portrait at that spot, and is left alone.
void IRAM_ATTR Display::WarpEarsChunk(const lv_area_t* area, uint16_t* px) {
  const int n = std::min<int>(chr_->ear_count, kMaxEars);
  if (n == 0 || ui_mode_ != UiMode::kRobotFace || face_image_ == nullptr || face_container_ == nullptr ||
      lv_obj_has_flag(face_container_, LV_OBJ_FLAG_HIDDEN)) {
    return;
  }
  bool any = false;
  for (int e = 0; e < n; e++) {
    any = any || ears_[e].active;
  }
  if (!any) {
    return;
  }
  const int ax1 = area->x1 - disp_off_x_;
  const int ay1 = area->y1 - disp_off_y_;
  const int ax2 = area->x2 - disp_off_x_;
  const int ay2 = area->y2 - disp_off_y_;
  const int aw = ax2 - ax1 + 1;
  lv_area_t img;
  lv_obj_get_coords(face_image_, &img);
  lv_area_t clip;
  lv_obj_get_coords(face_container_, &clip);
  const uint16_t* base = reinterpret_cast<const uint16_t*>(chr_->base->data);
  constexpr int W = ENCO_FACE_W;
  constexpr int H = ENCO_FACE_H;

  for (int e = 0; e < n; e++) {
    const Ear& E = ears_[e];
    if (!E.active) {
      continue;
    }
    const enco_ear_t& r = chr_->ears[e];
    const int x_lo = std::max<int>(std::max<int>(ax1, clip.x1), img.x1 + r.box_x);
    const int x_hi = std::min<int>(std::min<int>(ax2, clip.x2), img.x1 + r.box_x + r.box_w - 1);
    const int y_lo = std::max<int>(std::max<int>(ay1, clip.y1), img.y1 + r.box_y);
    const int y_hi = std::min<int>(std::min<int>(ay2, clip.y2), img.y1 + r.box_y + r.box_h - 1);
    if (x_lo > x_hi || y_lo > y_hi) {
      continue;
    }
    const float cx = r.pivot_x;
    const float cy = r.pivot_y;
    const float lx = static_cast<float>(r.tip_x - r.pivot_x);
    const float ly = static_cast<float>(r.tip_y - r.pivot_y);
    const float inv_l2 = 1.0f / (lx * lx + ly * ly);
    const float bend = E.st - E.sb;
    for (int y = y_lo; y <= y_hi; y++) {
      const int py = y - img.y1;  // portrait coordinates from here on
      if (py < 0 || py >= H) {
        continue;
      }
      const int bi = (py - r.box_y) * r.box_w - r.box_x;
      const uint8_t* wrow = r.weight + bi;
      const uint8_t* drow = r.deco_map + bi;
      uint16_t* row = px + (y - ay1) * aw - ax1;
      const uint16_t* brow = base + py * W;
      const float vy = static_cast<float>(py) - cy;
      for (int x = x_lo; x <= x_hi; x++) {
        const int pxx = x - img.x1;
        if (pxx < 0 || pxx >= W) {
          continue;
        }
        const uint8_t wv = wrow[pxx];
        const uint8_t dv = drow[pxx];
        if (wv == 0 && (dv & 63) == 0) {
          continue;
        }
        if (row[x] != brow[pxx]) {
          continue;  // something is drawn over her here
        }
        const float vx = static_cast<float>(pxx) - cx;
        const float s = std::min(1.0f, std::max(0.0f, (vx * lx + vy * ly) * inv_l2));
        const float th = E.sb + bend * s;
        const float th2 = th * th;
        const float c = 1.0f - 0.5f * th2;                  // cos, sin to < 0.1% at 0.4 rad
        const float sn = th * (1.0f - th2 * (1.0f / 6.0f));
        const float wd = (dv & 63) * (1.0f / 63.0f);
        const float k = (1.0f - wd) * wv * (1.0f / 255.0f);
        // Where this pixel comes from: turned back about the root by the local bend angle...
        float sx = pxx + k * (cx + c * vx + sn * vy - pxx);
        float sy = py + k * (cy - sn * vx + c * vy - py);
        if (wd > 0.0f) {
          // ...or about the ornament's carried anchor by the ornament's own angle.
          const int j = dv >> 6;
          const enco_ear_deco_t& d = r.deco[j];
          const float qx = pxx - E.fx[j];
          const float qy = py - E.fy[j];
          sx += wd * (d.x + E.dcos[j] * qx + E.dsin[j] * qy - pxx);
          sy += wd * (d.y - E.dsin[j] * qx + E.dcos[j] * qy - py);
        }
        sx = std::min(static_cast<float>(W - 1), std::max(0.0f, sx));
        sy = std::min(static_cast<float>(H - 1), std::max(0.0f, sy));
        const int qx32 = static_cast<int>(sx * 32.0f);
        const int qy32 = static_cast<int>(sy * 32.0f);
        const int ix = qx32 >> 5;
        const int iy = qy32 >> 5;
        const int ix1 = std::min(ix + 1, W - 1);
        const int iy1 = std::min(iy + 1, H - 1);
        const uint32_t fx = qx32 & 31;
        const uint32_t fy = qy32 & 31;
        const uint16_t* s0 = base + iy * W;
        const uint16_t* s1 = base + iy1 * W;
        row[x] = Lerp565(Lerp565(s0[ix], s0[ix1], fx), Lerp565(s1[ix], s1[ix1], fx), fy);
      }
    }
  }
}

void Display::UpdateClock() {
  if (clock_label_ == nullptr) {
    return;
  }
  const time_t now = time(nullptr);
  // Before SNTP answers the RTC counts from 1970; keep the placeholder rather than show 12:00 AM.
  if (now < 1700000000) {
    return;
  }
  struct tm local;
  localtime_r(&now, &local);
  const int minute_of_day = local.tm_hour * 60 + local.tm_min;
  if (minute_of_day == clock_minute_) {
    return;
  }
  char text[12];
  strftime(text, sizeof(text), "%I:%M %p", &local);  // "07:24 PM"
  if (clock_minute_ < 0) {
    printf("[clock] synced: %s\n", text);
  }
  clock_minute_ = minute_of_day;
  lv_label_set_text(clock_label_, text);
}

void Display::OnFaceTimer(lv_timer_t* timer) {
  auto* self = static_cast<Display*>(lv_timer_get_user_data(timer));
  if (self == nullptr) {
    return;
  }

  // Status-bar clock: checked about once a second, in every UI mode. Only a minute change repaints.
  static uint8_t clock_ticks = 0;
  if (++clock_ticks >= 1000 / kFaceTickMs) {
    clock_ticks = 0;
    self->UpdateClock();
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
  // randomised few seconds so it never looks mechanical. An expression owns the eyes while it is
  // up (a blink would briefly swap in the neutral lids), so blinking pauses until it clears.
  if (self->current_emotion_ != "sleepy" && self->expr_index_ < 0) {
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
  if (self->chr_->bangs == nullptr) {
    // This character has no breeze frames.
  } else if (self->hair_step_ != 0) {
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

  // Falling petals and the wind in her hair run on the motion timer (OnMotionTimer).

  // --- Mouth -------------------------------------------------------------------------------
  // Follows the amplifier, not the chat state: see core/audio_playback_signal.h. This is what
  // keeps the lips in step with the speaker when a servo command is answered in the same turn.
  self->mouth_open_ = audio_playback_signal::IsPlaying(kMouthHoldMs);
  self->ApplyMouthFrame();

  // --- Expression hold ---------------------------------------------------------------------
  // Counts only quiet ticks, so a spoken reply never eats the time the expression was asked for.
  if (self->expr_index_ >= 0 && !self->mouth_open_) {
    if (self->expr_quiet_ticks_ > 0) {
      self->expr_quiet_ticks_--;
    } else {
      self->expr_index_ = -1;
      self->expr_pinned_ = false;
      self->expr_ambient_ = false;
      self->next_blink_tick_ = self->face_tick_ + kBlinkMinTicks;
      self->next_ambient_tick_ = self->face_tick_ + AmbientGapTicks(self->ambient_mode_);
      self->ApplyBlinkFrame();
      self->ApplyMouthFrame();
    }
  }

  // --- Ambient expressions -----------------------------------------------------------------
  // Only on a neutral, quiet, open-eyed face: never over an expression someone asked for, a
  // server emotion, a blink in progress, or a sleepy face.
  if (self->ambient_enabled_ && self->ambient_mode_ != 0 && self->expr_index_ < 0 && !self->mouth_open_ &&
      self->blink_frame_ == 0 && self->current_emotion_ != "sleepy" &&
      self->face_tick_ >= self->next_ambient_tick_) {
    const bool listening = self->ambient_mode_ == 2;
    const char* const* pool = listening ? kAmbientListenFaces : kAmbientIdleFaces;
    const size_t n = listening ? sizeof(kAmbientListenFaces) / sizeof(kAmbientListenFaces[0])
                               : sizeof(kAmbientIdleFaces) / sizeof(kAmbientIdleFaces[0]);
    int8_t pick = FindExpression(self->chr_, pool[esp_random() % n]);
    if (pick == self->last_ambient_expr_) {  // one re-roll keeps back-to-back repeats rare
      pick = FindExpression(self->chr_, pool[esp_random() % n]);
    }
    if (pick >= 0) {
      self->expr_index_ = pick;
      self->expr_pinned_ = false;
      self->expr_ambient_ = true;
      self->last_ambient_expr_ = pick;
      // 1.6-2.4s listening, 2.0-3.6s idle.
      self->expr_quiet_ticks_ = listening ? 20 + esp_random() % 10 : 25 + esp_random() % 20;
      self->ApplyBlinkFrame();
      self->ApplyMouthFrame();
    } else {
      self->next_ambient_tick_ = self->face_tick_ + AmbientGapTicks(self->ambient_mode_);
    }
  }

  // No automatic idle sway. Shifting the portrait repositions every overlay and invalidates the
  // whole 240x240 screen at once, which on this panel reads as the character twitching sideways
  // with a visible flash. The hair breeze above provides the idle movement instead, and it only
  // dirties the three small hair rectangles. LookDirection() still moves the head, but only when
  // the user actually asked for it.
}
