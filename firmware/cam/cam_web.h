#pragma once

#ifndef _CAM_WEB_H_
#define _CAM_WEB_H_

#include <Arduino.h>

#include "cam_tracker.h"

// Bring-up web server for the camera board, on port 80.
//
// The point of this is to answer one question without any of the rest of the
// robot existing: does this module actually work? Power the ESP32-CAM on its
// own, open its IP in a browser, and you get a live view with the tracker's
// decision drawn on top, plus a box to ask the vision service a question.
//
//   /                 the page
//   /stream           MJPEG, with the tracker overlay (?mask=1: skin mask,
//                     ?raw=1: no overlay)
//   /stream?raw=1&tracker=1
//                     the same, for tools/hand_tracker: raw frames, a nonce
//                     in X-Track-Nonce and X-Armed on every frame, and the
//                     socket accepts finger samples back (cam_remote.h).
//                     While the tracker is steering, other /stream requests
//                     get 409 - use /snapshot.jpg.
//   /snapshot.jpg     one frame, same pipeline (?mask=1: skin mask,
//                     ?raw=1: no overlay at all, for offline analysis)
//   /status           JSON: heap, psram, wifi, tracking, finger, vision route,
//                     hand tracker link
//   /configure        ?vflip=0|1 &hmirror=0|1 &ae=-2..2 (exposure target)
//   /vision?q=...     capture and describe, returns the answer as text
//
// The stream is built from the tracker's own RGB565 frames, converted with
// frame2jpg(), rather than by switching the sensor to JPEG mode. That costs
// some frame rate but means you are looking at the exact pixels the tracker is
// deciding on - including the line along the finger it found and the deadband
// box inside which the main board will not move the head. A stream that
// showed a different image from the one being analysed would be a much worse
// debugging tool.
//
// The exception is the hand tracker. While it is steering, nothing on this
// board looks at the pixels, so cam_main switches the sensor to hardware JPEG
// and the frames go out exactly as the OV2640 compressed them - VGA, no
// overlay, several times the frame rate. /snapshot.jpg returns the same raw
// frames then.
//
// TODO(security): like the rest of this bring-up page, these routes are
// unauthenticated and meant for the home LAN only. None of them changes
// anything that survives a reboot. The one input that can move the head - the
// hand tracker's samples - is authenticated (HMAC, see cam_remote.h).
namespace cam_web {

// Called with every sample the stream computes, so tracking keeps reporting to
// the main board while a browser is watching. Optional.
using SampleSink = void (*)(const TrackSample&);

// Runs a vision query. cam_main supplies this because it owns the sensor mode
// switching that has to happen around a capture. Optional; without it the
// /vision endpoint reports that it is unavailable.
using VisionHandler = bool (*)(const char* question, String* out);

// Sets the sensor's auto-exposure target (-2..2). cam_main owns it because it
// has to be re-applied whenever the sensor is re-initialised.
using ExposureHandler = void (*)(int level);

void SetSampleSink(SampleSink sink);
void SetVisionHandler(VisionHandler handler);
void SetExposureHandler(ExposureHandler handler);
void SetLastSample(const TrackSample& s);

// The exposure target currently applied, for /status.
void SetExposureLevel(int level);

// Publishes UART link activity for /status: total bytes received from the main
// board, how many of them formed commands we understood, and when the last
// byte arrived (millis(), or 0 if never).
//
// This exists because "the cam is silent" is ambiguous from the main board's
// side - it looks the same whether our transmit line is broken or its receive
// line is. A byte counter here resolves it without a serial adapter.
void SetLinkStats(uint32_t rx_bytes, uint32_t commands, uint32_t last_rx_ms);
bool IsStreaming();

// Whether the main board has tracking armed. Sets the hand tracker's frame
// rate (faster while armed) and is passed to it with every frame.
void SetTrackingArmed(bool armed);

// True while the hand tracker (tools/hand_tracker) is connected and its
// authenticated finger samples keep arriving - i.e. it is doing the finger
// finding instead of cam_tracker.
bool TrackerActive();

// True while the hand tracker's /stream socket is open, whether or not a sample
// has arrived lately. Lets cam_main keep the sensor in hardware JPEG mode for
// the whole session instead of flipping modes (100-500ms each) whenever the
// tracker stalls for a moment.
bool TrackerConnected();

// Drops any browser currently pulling /stream.
//
// The screen and a browser cannot both have the camera: SendVideoFrame() and
// the MJPEG writer would interleave on the same sensor, and the stream holds
// the single-threaded WebServer for as long as the browser stays connected.
// When the robot's own screen wants the viewfinder it wins, because that is
// the one the user is looking at. The browser sees the connection close and
// can simply reload once the screen is done.
void StopStream();

// Starts the server. Safe to call with Wi-Fi down - it simply never serves.
void Begin();

// Non-blocking; call from loop().
//
// One exception: while a browser is pulling /stream this blocks until the
// browser disconnects or a byte arrives on either serial port. That is
// deliberate - it keeps the camera and the tracker consistent - and the serial
// check means a command from the main board still gets through promptly.
void Poll();

}  // namespace cam_web

#endif  // _CAM_WEB_H_
