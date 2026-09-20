#pragma once

#ifndef _OPUS_CODEC_POOL_H_
#define _OPUS_CODEC_POOL_H_

#include <stdint.h>

struct OpusEncoder;
struct OpusDecoder;

// Opus codec states are by far the largest single allocations this firmware makes: on this target
// opus_encoder_get_size(1) is 24548 bytes and opus_decoder_get_size(1) is 17800 bytes. On an ESP32
// without PSRAM there is no contiguous block that big left once WiFi, mbedTLS and LVGL are running,
// so creating them lazily inside AudioInputEngine / AudioOutputEngine (which are torn down and
// rebuilt on *every* listen/speak transition) is guaranteed to fail sooner or later.
//
// Two observations make this cheap to solve:
//   1. The states must be reserved while the heap is still unfragmented, i.e. at boot.
//   2. Capture and playback are mutually exclusive - the engine state machine always destroys one
//      engine before constructing the other - so a *single* buffer sized for the larger of the two
//      can back both, via opus_encoder_init() / opus_decoder_init().
//
// Together that costs 24548 bytes instead of 42348, freeing ~17.8KB for the TLS handshake.
namespace opus_codec_pool {

// Reserves the shared codec buffer. Safe to call more than once; subsequent calls are no-ops. Must
// be called before the first Acquire*() for the "never fails later" guarantee to hold, ideally at
// the very start of setup().
void Preallocate();

// Returns a ready-to-use codec state, or nullptr if memory could not be found. Callers must handle
// nullptr gracefully instead of aborting. Every successful Acquire must be paired with a Release.
OpusEncoder* AcquireEncoder(uint32_t sample_rate, uint32_t channels, int application);
void ReleaseEncoder(OpusEncoder* encoder);

OpusDecoder* AcquireDecoder(uint32_t sample_rate, uint32_t channels);
void ReleaseDecoder(OpusDecoder* decoder);

}  // namespace opus_codec_pool

#endif
