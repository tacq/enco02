#pragma once

#ifndef _CAM_VISION_H_
#define _CAM_VISION_H_

#include <Arduino.h>

namespace cam_vision {

// Capture one frame and ask the vision API what it is.
//
// Blocks for 3-8 seconds. That is fine: on the main board the matching MCP
// tool call is asynchronous, so nothing there is waiting on us, and the only
// thing this board does otherwise is track - which pauses for the duration.
//
// Returns true and fills `out` with a short UTF-8 description, or false and
// fills `out` with a reason.
bool Describe(String* out);

}  // namespace cam_vision

#endif  // _CAM_VISION_H_
