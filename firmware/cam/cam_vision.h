#pragma once

#ifndef _CAM_VISION_H_
#define _CAM_VISION_H_

#include <Arduino.h>

namespace cam_vision {

// Point the module at the xiaozhi server's own vision service.
//
// The main board learns this at connect time: the server advertises it in the
// params of the MCP `initialize` call, as
//
//   "capabilities":{"vision":{"url":"http://.../vision/explain","token":"..."}}
//
// and forwards it here over the UART. Using it means this board needs no API
// key of its own and no billing relationship - the account that already pays
// for the assistant pays for the picture too.
//
// `device_id` must be the MAIN board's MAC, not ours. The token is issued
// against that device, and the server checks the two agree. Our own MAC would
// be rejected.
//
// Passing an empty url reverts to the CAM_VISION_ENDPOINT compiled in from
// cam_config.h, which is the OpenAI-shaped path.
void SetEndpoint(const char* url, const char* token, const char* device_id);

// True once SetEndpoint() has been given a usable url.
bool HasEndpoint();

// Capture one frame and ask the vision API what it is.
//
// Blocks for 3-8 seconds. That is fine: on the main board the matching MCP
// tool call is asynchronous, so nothing there is waiting on us, and the only
// thing this board does otherwise is track - which pauses for the duration.
//
// `question` is what to ask about the image. Pass nullptr or "" to use the
// generic prompt from cam_config.h. The xiaozhi endpoint takes the question as
// a form field, so the assistant can ask something specific ("这是什么牌子的?")
// rather than always getting a generic description back.
//
// Returns true and fills `out` with a short UTF-8 description, or false and
// fills `out` with a reason.
bool Describe(const char* question, String* out);

}  // namespace cam_vision

#endif  // _CAM_VISION_H_
