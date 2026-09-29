#pragma once

#ifndef _CAM_FACE_H_
#define _CAM_FACE_H_

#include <stdint.h>

#include "cam_tracker.h"

// What the head follows.
//
//   0 (default): a FACE, found on this board by Espressif's ESP-WHO detector - ESP-DL's two-stage
//     MSR01 + MNP01 cascade, the successor to MTMN: a bounding box (pan / pitch) plus five
//     landmarks, of which the two eyes give the head tilt (roll). Runs from PSRAM, so this needs
//     the AI-Thinker board's 4MB PSRAM (BOARD_HAS_PSRAM).
//   1: the old finger tracking - the on-board skin finder (cam_tracker.cpp), the Mac hand tracker
//     (cam_remote / tools/hand_tracker) and the one-finger gesture that arms tracking. Kept in the
//     tree, not built into the behaviour.
#ifndef CAM_FINGER_TRACKING
#define CAM_FINGER_TRACKING 0
#endif

namespace cam_face {

// Runs the detector on one RGB565 frame (big-endian, as esp32-camera delivers it) and returns the
// most prominent face as a kind 2 TrackSample, conf 0 if there is none. Blocks for the length of the
// inference (~60-200ms at QVGA on the classic ESP32 - see LastInferMs()).
TrackSample Analyse(const uint8_t* rgb565, int width, int height);

// Forget the face being followed (smoothing state), e.g. after the sensor was reconfigured or
// tracking was switched off.
void Reset();

// Duration of the last Analyse() and its running average, for the logs and the web page.
uint32_t LastInferMs();
uint32_t AvgInferMs();

// True if the last Analyse() found its face in the window round the previous one (the cheap path)
// rather than by searching the whole frame.
bool LastWasWindowed();

}  // namespace cam_face

// The one entry point the rest of the cam firmware calls: whichever tracker is compiled in.
inline TrackSample AnalyseFrame(const uint8_t* rgb565, int width, int height) {
#if CAM_FINGER_TRACKING
  return cam_tracker::Analyse(rgb565, width, height);
#else
  return cam_face::Analyse(rgb565, width, height);
#endif
}

inline void ResetTracker() {
#if CAM_FINGER_TRACKING
  cam_tracker::Reset();
#else
  cam_face::Reset();
#endif
}

#endif  // _CAM_FACE_H_
