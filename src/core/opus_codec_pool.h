#pragma once

#ifndef _OPUS_CODEC_POOL_H_
#define _OPUS_CODEC_POOL_H_

#include <stdint.h>

struct OpusEncoder;
struct OpusDecoder;

// Opus codec states are by far the largest single allocations this firmware makes (the mono encoder
// state alone is ~20KB, the decoder ~18KB). On an ESP32 without PSRAM there is simply no contiguous
// block that big left once WiFi, mbedTLS and LVGL are running, so creating them lazily inside
// AudioInputEngine / AudioOutputEngine (which are torn down and rebuilt on *every* listen/speak
// transition) is guaranteed to fail sooner or later - and opus_encoder_create() returning nullptr
// used to abort() the whole system.
//
// This pool allocates each codec exactly once, as early as possible during boot when the heap is
// still unfragmented, and then hands out the same state forever. Acquire() resets the codec so the
// caller always gets a clean state, and there is no Release() - the memory is intentionally never
// returned to the heap.
namespace opus_codec_pool {

// Allocates the encoder and decoder states up front. Safe to call more than once; subsequent calls
// are no-ops. Must be called before the first Acquire*() for the "never fails later" guarantee to
// hold, ideally right at the start of setup().
void Preallocate();

// Returns a reset encoder/decoder state, or nullptr if the allocation could not be satisfied.
// Callers must handle nullptr gracefully instead of aborting.
OpusEncoder* AcquireEncoder(uint32_t sample_rate, uint32_t channels, int application);
OpusDecoder* AcquireDecoder(uint32_t sample_rate, uint32_t channels);

}  // namespace opus_codec_pool

#endif
