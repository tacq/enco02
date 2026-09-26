#pragma once

#ifndef _CAM_TRACKER_H_
#define _CAM_TRACKER_H_

#include <stdint.h>

// One analysed frame. The tracker looks for ONE raised finger (伸出一根手指) and nothing else:
// dx/dy say where the finger is relative to the middle of the picture, roll how far it leans.
struct TrackSample {
  int16_t dx;       // finger centre, -100 (hard left) .. +100 (hard right)
  int16_t dy;       // finger centre, -100 (top) .. +100 (bottom)
  uint8_t conf;     // 0 = no finger this frame, 50..100 = finger found
  int16_t roll;     // finger lean in degrees, + = its tip leans to the RIGHT of the picture
  uint8_t gesture;  // 1 = a finger clean enough to ARM tracking (tip inside the frame, strict shape)
  uint8_t kind;     // 1 = finger sample (whenever conf > 0), 0 = nothing
  // Finger geometry in frame pixels, for the web overlay. Valid when conf > 0.
  int16_t tip_x;
  int16_t tip_y;
  int16_t base_x;
  int16_t base_y;
  uint8_t width;
};

namespace cam_tracker {

// Analyse one RGB565 frame (big-endian, as esp32-camera delivers it). width/height are the real
// frame dimensions. Returns conf == 0 when there is no finger in view.
TrackSample Analyse(const uint8_t* rgb565, int width, int height);

// Forget the previous finger and colour balance, e.g. after the sensor was reconfigured.
void Reset();

// Debug aid for /snapshot.jpg?mask=1: paints every pixel the skin test accepts magenta (blue where
// it belongs to the bright layer that is searched separately) and blown-out highlights cyan, using
// the colour balance of the last Analyse(). Call after Analyse() on the same frame.
void PaintSkin(uint8_t* rgb565, int width, int height);

// Grey-world gains currently applied to red and blue, x256 (256 = 1.0).
void Gains(int* red_q8, int* blue_q8);

// Luma floor of the bright skin layer in the last analysed frame, 0 if it had none.
int SplitLuma();

}  // namespace cam_tracker

#endif  // _CAM_TRACKER_H_
