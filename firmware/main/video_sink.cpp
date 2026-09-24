#include "video_sink.h"

#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "libs/tjpgd/tjpgd.h"

namespace video_sink {
namespace {

// TJpgDec's working pool. 3,100 bytes is the documented requirement for a
// baseline JPEG with JD_FASTDECODE=1, plus JD_SZBUF (512) for the input FIFO
// and a little slack for alignment. Rounded to 4KB.
constexpr size_t kPoolSize = 4096;

// The largest MCU tjpgd will hand back is 16x16 (4:2:0 chroma subsampling),
// which is 256 pixels. RGB565 out, so 512 bytes.
constexpr size_t kBlockPixels = 16 * 16;

uint8_t* g_pool = nullptr;
uint16_t g_block[kBlockPixels];
esp_lcd_panel_handle_t g_panel = nullptr;
int g_origin_x = 0;
int g_origin_y = 0;
// Window of the source image that reaches the panel. See Begin().
int g_crop_x = 0;
int g_crop_y = 0;
int g_crop_w = 0;
int g_crop_h = 0;
uint16_t g_errors = 0;

// Reticle geometry: four 10px arms with a 12px hole in the middle, so the thing
// being looked at is never hidden by the mark pointing at it.
constexpr int kReticleArm = 10;
constexpr int kReticleGap = 6;
// 0x00f0ff (the HUD's neon cyan) as RGB565, byte-swapped for the ST7789.
constexpr uint16_t kReticleColor = 0x9F07;

struct Source {
  ReadFn read;
  void* ctx;
};

// tjpgd pulls its input through here rather than being handed a buffer, which
// is the whole reason a 6KB JPEG fits on a board with a 14KB largest block.
size_t InFunc(JDEC* jd, uint8_t* buf, size_t len) {
  auto* src = static_cast<Source*>(jd->device);
  if (src == nullptr || src->read == nullptr) {
    return 0;
  }
  return src->read(src->ctx, buf, len);
}

// One decoded MCU. tjpgd is built with JD_FORMAT=0 (RGB888) in LVGL's config,
// and the panel wants big-endian RGB565, so the conversion happens here - in a
// fixed 512 byte scratch, never a frame buffer.
int OutFunc(JDEC* jd, void* bitmap, JRECT* rect) {
  (void)jd;
  if (g_panel == nullptr) {
    return 0;
  }

  const int blk_w = rect->right - rect->left + 1;
  const int blk_h = rect->bottom - rect->top + 1;
  if (blk_w <= 0 || blk_h <= 0 || static_cast<size_t>(blk_w) * static_cast<size_t>(blk_h) > kBlockPixels) {
    return 0;  // a block this size means the stream is bad, not the maths
  }

  // Intersect the MCU with the crop window. Returning 1 for a block that falls
  // entirely outside it is not an error: the decoder walks the whole image and
  // we simply do not want the edges of it.
  const int x0 = rect->left > g_crop_x ? rect->left : g_crop_x;
  const int y0 = rect->top > g_crop_y ? rect->top : g_crop_y;
  const int crop_right = g_crop_x + g_crop_w - 1;
  const int crop_bottom = g_crop_y + g_crop_h - 1;
  const int x1 = rect->right < crop_right ? rect->right : crop_right;
  const int y1 = rect->bottom < crop_bottom ? rect->bottom : crop_bottom;
  if (x0 > x1 || y0 > y1) {
    return 1;
  }

  const int w = x1 - x0 + 1;
  const int h = y1 - y0 + 1;
  const uint8_t* rows = static_cast<const uint8_t*>(bitmap);
  uint16_t* out = g_block;
  for (int y = y0; y <= y1; ++y) {
    // Source is packed RGB888, blk_w pixels per row, indexed from rect->left.
    // Read right-to-left (from x1 down to x0) to un-mirror the camera image horizontally.
    const uint8_t* rgb =
        rows + (static_cast<size_t>(y - rect->top) * static_cast<size_t>(blk_w) + static_cast<size_t>(x1 - rect->left)) * 3;
    for (int x = 0; x < w; ++x) {
      const uint16_t c = static_cast<uint16_t>(((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
      // Byte-swapped on the way in: the ST7789 clocks RGB565 MSB first, and this
      // buffer goes to the panel by DMA with no further processing. (The LVGL
      // port does the same thing via its swap_bytes flag.)
      *out++ = static_cast<uint16_t>((c >> 8) | (c << 8));
      rgb -= 3;
    }
  }

  const int sx = g_origin_x + g_crop_w - (x1 - g_crop_x + 1);
  const int sy = g_origin_y + (y0 - g_crop_y);
  esp_lcd_panel_draw_bitmap(g_panel, sx, sy, sx + w, sy + h, g_block);
  return 1;
}

// A solid run of reticle colour. Reuses the MCU scratch - nothing else is in
// flight by the time this runs.
void FillRect(int x, int y, int w, int h) {
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (n == 0 || n > kBlockPixels) {
    return;
  }
  for (size_t i = 0; i < n; ++i) {
    g_block[i] = kReticleColor;
  }
  esp_lcd_panel_draw_bitmap(g_panel, x, y, x + w, y + h, g_block);
}

}  // namespace

bool Begin(esp_lcd_panel_handle_t panel, int origin_x, int origin_y, int crop_x, int crop_y, int crop_w, int crop_h) {
  g_panel = panel;
  g_origin_x = origin_x;
  g_origin_y = origin_y;
  g_crop_x = crop_x;
  g_crop_y = crop_y;
  g_crop_w = crop_w;
  g_crop_h = crop_h;
  g_errors = 0;
  if (g_pool != nullptr) {
    return true;
  }
  // Internal DRAM explicitly: this board has no PSRAM, but being explicit means
  // a future one with PSRAM does not silently put the decoder somewhere slow.
  g_pool = static_cast<uint8_t*>(heap_caps_malloc(kPoolSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (g_pool == nullptr) {
    printf("video: no room for a %u byte decoder pool\n", static_cast<unsigned>(kPoolSize));
    return false;
  }
  return true;
}

void End() {
  if (g_pool != nullptr) {
    heap_caps_free(g_pool);
    g_pool = nullptr;
  }
  g_panel = nullptr;
}

void DrawReticle() {
  if (!IsOpen()) {
    return;
  }
  const int cx = g_origin_x + g_crop_w / 2;
  const int cy = g_origin_y + g_crop_h / 2;
  if (!lvgl_port_lock(50)) {
    return;
  }
  FillRect(cx - kReticleGap - kReticleArm, cy, kReticleArm, 1);
  FillRect(cx + kReticleGap, cy, kReticleArm, 1);
  FillRect(cx, cy - kReticleGap - kReticleArm, 1, kReticleArm);
  FillRect(cx, cy + kReticleGap, 1, kReticleArm);
  lvgl_port_unlock();
}

bool IsOpen() { return g_pool != nullptr && g_panel != nullptr; }

uint16_t error_count() { return g_errors; }

void ResetErrors() { g_errors = 0; }

bool DecodeFrame(ReadFn read, void* ctx) {
  if (!IsOpen()) {
    return false;
  }

  Source src{read, ctx};
  JDEC jd;

  // The LVGL task flushes to this same panel over the same SPI bus. Without the
  // port lock the two interleave mid-transaction and the screen tears into
  // diagonal garbage. Held for the whole frame (~70ms) rather than per block:
  // the HUD around the picture is static while the viewfinder is up, so there
  // is nothing for LVGL to miss.
  if (!lvgl_port_lock(100)) {
    return false;
  }

  bool ok = false;
  const JRESULT prep = jd_prepare(&jd, InFunc, g_pool, kPoolSize, &src);
  JRESULT decomp = JDR_OK;
  if (prep == JDR_OK) {
    decomp = jd_decomp(&jd, OutFunc, 0);
    ok = (decomp == JDR_OK);
  }
  lvgl_port_unlock();

  if (!ok) {
    ++g_errors;
    // The first few only. "No picture" has several very different causes -
    // JDR_INP means the bytes stopped arriving, JDR_MEM1 means the 4KB pool is
    // too small for this image, JDR_FMT1/3 mean it is not the baseline JPEG the
    // decoder was built for - and they need opposite fixes. Without the code
    // they are indistinguishable from a blank screen.
    if (g_errors <= 3) {
      printf("video: decode failed (jd_prepare=%d, jd_decomp=%d)\n", static_cast<int>(prep), static_cast<int>(decomp));
    }
  }
  return ok;
}

}  // namespace video_sink
