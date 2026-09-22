#pragma once

#ifndef _CAM_TRACKER_H_
#define _CAM_TRACKER_H_

#include <stdint.h>

// Where the subject is, relative to the centre of the frame.
struct TrackSample {
  int16_t dx;      // -100 (hard left) .. +100 (hard right)
  int16_t dy;      // -100 (top) .. +100 (bottom)
  uint8_t conf;    // 0 = nothing found, 100 = unambiguous
  int16_t roll;    // -100 (head tilted left / 歪头) .. +100 (head tilted right)
  uint8_t gesture; // 0 = none, 1 = one raised finger (食指) held steady
};

namespace cam_tracker {

// Analyse one RGB565 frame. width/height are the real frame dimensions.
// Returns conf == 0 when it cannot find anybody.
TrackSample Analyse(const uint8_t* rgb565, int width, int height);

// Drop the motion history, e.g. after the sensor has been reconfigured and the
// next frame will differ from the last one for reasons that are not a person.
void Reset();

}  // namespace cam_tracker

#endif  // _CAM_TRACKER_H_
