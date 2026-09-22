#pragma once

#ifndef _VIDEO_SINK_H_
#define _VIDEO_SINK_H_

#include <esp_lcd_panel_ops.h>
#include <stddef.h>
#include <stdint.h>

// Draws the camera's viewfinder JPEGs straight onto the ST7789.
//
// The constraint this class exists to satisfy: one screen of RGB565 is 153,600
// bytes and the largest free heap block on this board mid-session is 14,836.
// There is no framebuffer, and there is no room for one. So nothing here ever
// holds a frame:
//
//   UART bytes -> tjpgd (pulls input through a callback)
//              -> 16x16 RGB888 blocks
//              -> converted in a 512 byte scratch
//              -> esp_lcd_panel_draw_bitmap(), one block at a time
//
// Peak cost is the ~4KB decoder workspace, and even that is only held between
// Begin() and End() - i.e. while the viewfinder is actually on screen, which is
// exactly when the assistant is idle and a block that size exists.
namespace video_sink {

// Pulls up to `len` bytes into `dst`. A null `dst` means "discard len bytes".
// Returns the number actually read; short reads end the frame.
using ReadFn = size_t (*)(void* ctx, uint8_t* dst, size_t len);

// Grabs the decoder workspace and remembers where on the panel to draw.
// Returns false if the heap could not spare it, in which case the caller should
// stay out of camera mode rather than proceed without a picture.
//
// (crop_x, crop_y, crop_w, crop_h) is the window of the *source image* that
// actually reaches the panel, and it lands at (origin_x, origin_y).
//
// The crop exists because of a layout problem with no other solution: the
// decoder is built with JD_USE_SCALE off, so a frame can only be drawn 1:1, and
// the camera's 240x176 viewfinder frame is exactly as wide as the screen. Every
// pixel of HUD chrome drawn inside that rectangle - corner brackets, a border,
// the reticle - would be overwritten by the next frame. Cropping a few pixels
// off each edge buys back a margin that LVGL owns and video never touches, at
// the cost of a little field of view.
bool Begin(esp_lcd_panel_handle_t panel,
           int origin_x,
           int origin_y,
           int crop_x,
           int crop_y,
           int crop_w,
           int crop_h);

// Releases the workspace. Safe to call when not open.
void End();

bool IsOpen();

// Draws the targeting reticle over the middle of the last frame. Separate from
// DecodeFrame() because it has to be re-drawn after every frame - it is painted
// onto the picture, not onto the LVGL layer underneath it.
void DrawReticle();

// Decodes one JPEG, pulling its bytes through `read`, and blits it at the
// origin given to Begin(). Returns false on a decode error - which on this link
// usually means a corrupted frame rather than a bug, so callers should count
// failures rather than treat one as fatal.
bool DecodeFrame(ReadFn read, void* ctx);

// Number of frames that failed to decode since Begin(). Used to decide whether
// the fast link is actually viable on this particular set of wires.
uint16_t error_count();

}  // namespace video_sink

#endif  // _VIDEO_SINK_H_
