// Streaming decoder for 16-colour (I4) C-array images.
//
// Why this exists
// ---------------
// A 240x320 portrait costs 153,600 bytes as RGB565, which is two thirds of the flash we have left.
// The same picture as a 16-colour indexed image is 38,464 bytes, and flat cel-shaded art loses
// almost nothing to a 16-entry palette.
//
// LVGL can already parse I4, but its built-in decoder expands the *whole* image to ARGB8888 in RAM
// first (lv_bin_decoder.c: decode_indexed()).  That is 307,200 bytes - this board has no PSRAM and
// about 30KB of free heap, so it is not even close.
//
// LVGL's draw pipeline has a second, streaming path: if a decoder leaves dsc->decoded NULL after
// open, lv_draw_image() repeatedly calls get_area_cb() and draws whatever slice it hands back
// (lv_draw_image.c: img_decode_and_draw()).  This decoder uses that path to convert one scanline at
// a time, straight into the display's native RGB565, from a single static 480-byte buffer.  Peak
// RAM cost is therefore constant and tiny no matter how large the image is.
//
// Decoding to RGB565 rather than ARGB8888 also means the blend into the frame buffer is a plain
// copy instead of a per-pixel alpha composite.

#include "lv_i4_decoder.h"

#include "lvgl.h"
#include "src/lvgl_private.h"

// The widest image we will ever stream. Sized for the panel; a wider image is simply declined so it
// falls through to LVGL's own decoder rather than overflowing the row buffer.
#define I4_MAX_W 240
#define I4_PALETTE_ENTRIES 16
#define I4_PALETTE_BYTES (I4_PALETTE_ENTRIES * 4)

LV_DRAW_BUF_DEFINE_STATIC(row_buf, I4_MAX_W, 1, LV_COLOR_FORMAT_RGB565);

// The palette converted to the display's pixel format, refreshed on every open. Images are drawn
// one at a time from the single LVGL task, so one shared table is enough.
static uint16_t palette_rgb565[I4_PALETTE_ENTRIES];

static bool is_i4_variable(const lv_image_decoder_dsc_t *dsc) {
  if (dsc->src_type != LV_IMAGE_SRC_VARIABLE || dsc->src == NULL) {
    return false;
  }
  const lv_image_dsc_t *img = (const lv_image_dsc_t *)dsc->src;
  return img->header.cf == LV_COLOR_FORMAT_I4 && img->data != NULL && img->header.w <= I4_MAX_W &&
         img->data_size >= I4_PALETTE_BYTES;
}

static lv_result_t I4Info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header) {
  LV_UNUSED(decoder);
  if (!is_i4_variable(dsc)) {
    return LV_RESULT_INVALID;  // Let the next decoder in the list try.
  }
  *header = ((const lv_image_dsc_t *)dsc->src)->header;
  return LV_RESULT_OK;
}

static lv_result_t I4Open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
  LV_UNUSED(decoder);
  if (!is_i4_variable(dsc)) {
    return LV_RESULT_INVALID;
  }

  // The palette is stored ahead of the bitmap, four bytes per entry in BGRA order - the same layout
  // LVGL's own indexed images use.
  const uint8_t *palette = ((const lv_image_dsc_t *)dsc->src)->data;
  for (uint32_t i = 0; i < I4_PALETTE_ENTRIES; i++) {
    const uint8_t b = palette[i * 4 + 0];
    const uint8_t g = palette[i * 4 + 1];
    const uint8_t r = palette[i * 4 + 2];
    palette_rgb565[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }

  // Leaving this NULL is what tells lv_draw_image() to stream the image through get_area_cb.
  dsc->decoded = NULL;
  dsc->palette = NULL;
  dsc->palette_size = 0;
  return LV_RESULT_OK;
}

static lv_result_t I4GetArea(lv_image_decoder_t *decoder,
                             lv_image_decoder_dsc_t *dsc,
                             const lv_area_t *full_area,
                             lv_area_t *decoded_area) {
  LV_UNUSED(decoder);
  const lv_image_dsc_t *img = (const lv_image_dsc_t *)dsc->src;

  // LVGL seeds decoded_area with LV_COORD_MIN to ask for the first slice, then expects us to walk
  // down one row per call and fail once we pass the bottom of the requested area.
  if (decoded_area->y1 == LV_COORD_MIN) {
    *decoded_area = *full_area;
    decoded_area->y2 = decoded_area->y1;
  } else {
    decoded_area->y1++;
    decoded_area->y2++;
  }
  if (decoded_area->y1 > full_area->y2) {
    return LV_RESULT_INVALID;  // Ends the draw loop; not an error.
  }

  const int32_t width = lv_area_get_width(full_area);
  if (width <= 0 || width > I4_MAX_W) {
    return LV_RESULT_INVALID;
  }
  lv_draw_buf_t *row = lv_draw_buf_reshape(&row_buf, LV_COLOR_FORMAT_RGB565, width, 1, LV_STRIDE_AUTO);
  if (row == NULL) {
    return LV_RESULT_INVALID;
  }

  const uint8_t *src = img->data + I4_PALETTE_BYTES + (uint32_t)decoded_area->y1 * img->header.stride +
                       ((uint32_t)decoded_area->x1 >> 1);
  uint16_t *dst = (uint16_t *)(void *)row->data;
  // Two pixels share a byte, so an odd start column begins on the low nibble.
  bool high_nibble = (decoded_area->x1 & 1) == 0;
  for (int32_t i = 0; i < width; i++) {
    uint8_t index;
    if (high_nibble) {
      index = (uint8_t)(*src >> 4);
    } else {
      index = (uint8_t)(*src++ & 0x0F);
    }
    high_nibble = !high_nibble;
    *dst++ = palette_rgb565[index];
  }

  dsc->decoded = row;
  return LV_RESULT_OK;
}

static void I4Close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
  LV_UNUSED(decoder);
  // Nothing was allocated: the row buffer is static and the pixels live in flash.
  dsc->decoded = NULL;
}

void enco_i4_decoder_init(void) {
  static bool registered = false;
  if (registered) {
    return;
  }
  LV_DRAW_BUF_INIT_STATIC(row_buf);

  // Decoders are searched newest-first, so this is consulted before LVGL's built-in one. Images we
  // decline fall straight through to it.
  lv_image_decoder_t *decoder = lv_image_decoder_create();
  if (decoder == NULL) {
    return;
  }
  lv_image_decoder_set_info_cb(decoder, I4Info);
  lv_image_decoder_set_open_cb(decoder, I4Open);
  lv_image_decoder_set_get_area_cb(decoder, I4GetArea);
  lv_image_decoder_set_close_cb(decoder, I4Close);
  registered = true;
}
