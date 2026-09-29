#pragma once

#ifndef _CAM_TRACKER_H_
#define _CAM_TRACKER_H_

#include <stdint.h>

// One analysed frame. Two producers fill it:
//   - the face detector (cam_face.h, the default): one face, kind 2. dx/dy locate the face box,
//     roll is the head tilt read off the line between the eyes.
//   - the finger finder (cam_tracker.cpp / cam_remote, kept but compiled out unless
//     CAM_FINGER_TRACKING is 1): ONE raised finger (伸出一根手指), kind 1.
// Either way dx/dy say where the subject is relative to the middle of the picture.
struct TrackSample {
  int16_t dx;       // subject centre, -100 (hard left) .. +100 (hard right)
  int16_t dy;       // subject centre, -100 (top) .. +100 (bottom)
  uint8_t conf;     // 0 = nothing this frame, otherwise how sure the detector is (0..100)
  int16_t roll;     // lean in degrees, + = the finger tip / the top of the head leans to the RIGHT of
                    // the picture
  uint8_t gesture;  // finger only: 1 = a finger clean enough to ARM tracking. Always 0 for faces.
  uint8_t kind;     // 1 = finger, 2 = face (whenever conf > 0), 0 = nothing
  // Geometry in frame pixels, for the web overlay. Valid when conf > 0.
  // Finger: tip and base of the finger, width across it.
  // Face: tip = left eye, base = right eye (as seen in the picture), width = box width.
  int16_t tip_x;
  int16_t tip_y;
  int16_t base_x;
  int16_t base_y;
  uint8_t width;
  // Face only: the detector's box, frame pixels.
  int16_t box_x0;
  int16_t box_y0;
  int16_t box_x1;
  int16_t box_y1;
  // Face only: millis() at which the analysed picture was taken, so the main board can tell how
  // old a report is when it arrives (0 = unknown).
  uint32_t capture_ms;
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
