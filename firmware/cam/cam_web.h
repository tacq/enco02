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
//   /stream           MJPEG, with the tracker overlay
//   /snapshot.jpg     one frame, same pipeline
//   /status           JSON: heap, psram, wifi, tracking, vision route
//   /vision?q=...     capture and describe, returns the answer as text
//
// The stream is built from the tracker's own RGB565 frames, converted with
// frame2jpg(), rather than by switching the sensor to JPEG mode. That costs
// some frame rate but means you are looking at the exact pixels the tracker is
// deciding on - including the crosshair where it thinks the subject is and the
// deadband box inside which the main board will not move the head. A stream
// that showed a different image from the one being analysed would be a much
// worse debugging tool.
namespace cam_web {

// Called with every sample the stream computes, so tracking keeps reporting to
// the main board while a browser is watching. Optional.
using SampleSink = void (*)(const TrackSample&);

// Runs a vision query. cam_main supplies this because it owns the sensor mode
// switching that has to happen around a capture. Optional; without it the
// /vision endpoint reports that it is unavailable.
using VisionHandler = bool (*)(const char* question, String* out);

void SetSampleSink(SampleSink sink);
void SetVisionHandler(VisionHandler handler);
void SetLastSample(const TrackSample& s);

// Publishes UART link activity for /status: total bytes received from the main
// board, how many of them formed commands we understood, and when the last
// byte arrived (millis(), or 0 if never).
//
// This exists because "the cam is silent" is ambiguous from the main board's
// side - it looks the same whether our transmit line is broken or its receive
// line is. A byte counter here resolves it without a serial adapter.
void SetLinkStats(uint32_t rx_bytes, uint32_t commands, uint32_t last_rx_ms);
bool IsStreaming();

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
