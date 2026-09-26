#pragma once

#ifndef _CAM_REMOTE_H_
#define _CAM_REMOTE_H_

#include <stdint.h>

#include "cam_tracker.h"

// Finger samples worked out OFF this board, by tools/hand_tracker/hand_tracker.py on a computer on the
// LAN, and sent back up the very TCP connection that carries the video it is looking at.
//
// Why: recognising a hand needs a neural network (Google MediaPipe). Every "gesture tracking with an
// ESP32-CAM" project does the same split - the ESP32-CAM only streams, a PC does the recognising.
// This board has no SIMD, 520KB of SRAM and a 240MHz core; the skin-colour finger finder in
// cam_tracker.cpp is what fits, and it cannot tell a finger from a beige wall reliably enough. When a
// tracker is connected its samples replace that finder's; when it goes away the finder takes over again.
//
// Wire format, one line per analysed frame, client -> cam on the /stream?raw=1&tracker=1 socket:
//
//   S <seq> <dx> <dy> <conf> <lean> <kind> <gesture> <mac>\n
//
//   seq      1..2147483647, strictly increasing within one connection (replay protection)
//   dx, dy   finger centre, -100..100 from the middle of the picture (+ = right / down)
//   conf     0..100, 0 = no finger this frame
//   lean     -90..90 degrees, + = fingertip leans to the right of the picture
//   kind     1 = finger sample, 0 = nothing
//   gesture  1 = a clear "index finger up" - counts towards arming tracking (debounced in cam_main)
//   mac      first 16 bytes of HMAC-SHA256(CAM_TRACK_KEY, "<nonce> S <seq> ... <gesture>"), lower-case hex
//
// The nonce is 16 random bytes this board picks for every new tracker connection and sends in the
// X-Track-Nonce response header, so a line recorded on one connection is worthless on the next, and
// the sequence number stops it being replayed within the same one.
//
// TODO(security): the video itself is unauthenticated and unencrypted, like every other route on this
// bring-up server - home LAN only. The key only protects the direction that moves the head.
namespace cam_remote {

// True when cam_config.h defines a CAM_TRACK_KEY of at least 32 characters. Without one no line is
// ever accepted and /stream?tracker=1 is refused (fail closed).
bool Enabled();

// A tracker just connected: forget the previous connection's state and pick a fresh nonce. Writes it
// to `nonce_hex` as 32 lower-case hex characters plus a terminating NUL.
void BeginSession(char nonce_hex[33]);

// The tracker connection closed. Active() is false from here on.
void EndSession();

// Checks one line from the tracker (without its newline). Returns true and fills `*out` only for an
// authentic, fresh, in-range sample; everything else is counted as rejected and ignored.
bool HandleLine(const char* line, uint32_t now_ms, TrackSample* out);

// True while authentic samples keep arriving on the current connection.
bool Active(uint32_t now_ms);

// True once the current connection has sent so much garbage it should be dropped.
bool ShouldDrop();

// For /status.
uint32_t AcceptedCount();
uint32_t RejectedCount();
// Milliseconds since the last accepted sample, -1 if none since boot.
long LastAcceptedAgeMs(uint32_t now_ms);

}  // namespace cam_remote

#endif  // _CAM_REMOTE_H_
