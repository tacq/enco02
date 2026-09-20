#include "opus_codec_pool.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>

#include "libopus/opus.h"

#ifndef CLOGGER_SEVERITY
#define CLOGGER_SEVERITY CLOGGER_SEVERITY_WARN
#endif

#include "clogger/clogger.h"

namespace opus_codec_pool {
namespace {

// The rates the engine actually uses (see audio_input_engine.cpp / audio_output_engine.cpp).
constexpr uint32_t kEncoderSampleRate = 16000;
constexpr uint32_t kDecoderSampleRate = 24000;
constexpr int kChannels = 1;

// The single buffer that backs whichever codec is currently active.
void* g_shared_state = nullptr;
size_t g_shared_size = 0;
bool g_shared_in_use = false;

void LogHeap(const char* stage) {
  printf("[opus_pool] %s free heap: %u, largest block: %u\n",
         stage,
         static_cast<unsigned>(esp_get_free_heap_size()),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

void ApplyEncoderSettings(OpusEncoder* encoder) {
  opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    // No PSRAM: keep the encoder in its cheapest mode. Complexity 0 skips the expensive analysis
    // paths (which also use a lot of stack) and a fixed low bitrate keeps the packets tiny.
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(8000));
  } else {
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
  }
}

}  // namespace

void Preallocate() {
  if (g_shared_state != nullptr) {
    return;
  }

  const int encoder_size = opus_encoder_get_size(kChannels);
  const int decoder_size = opus_decoder_get_size(kChannels);
  if (encoder_size <= 0 || decoder_size <= 0) {
    printf("[opus_pool] bad codec sizes: encoder %d, decoder %d\n", encoder_size, decoder_size);
    return;
  }

  g_shared_size = static_cast<size_t>(std::max(encoder_size, decoder_size));

  LogHeap("before reservation,");
  g_shared_state = heap_caps_malloc(g_shared_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (g_shared_state == nullptr) {
    printf("[opus_pool] FAILED to reserve %u bytes\n", static_cast<unsigned>(g_shared_size));
    g_shared_size = 0;
    return;
  }

  printf("[opus_pool] shared codec state reserved: %u bytes (encoder %d, decoder %d)\n",
         static_cast<unsigned>(g_shared_size),
         encoder_size,
         decoder_size);
  LogHeap("after reservation,");
}

OpusEncoder* AcquireEncoder(const uint32_t sample_rate, const uint32_t channels, const int application) {
  if (g_shared_state != nullptr && !g_shared_in_use && g_shared_size >= static_cast<size_t>(opus_encoder_get_size(static_cast<int>(channels)))) {
    auto* encoder = reinterpret_cast<OpusEncoder*>(g_shared_state);
    const int error = opus_encoder_init(encoder, sample_rate, channels, application);
    if (error != OPUS_OK) {
      printf("[opus_pool] opus_encoder_init failed: %d\n", error);
      return nullptr;
    }
    ApplyEncoderSettings(encoder);
    g_shared_in_use = true;
    return encoder;
  }

  // Shared buffer unavailable (should not happen - capture and playback never overlap). Fall back
  // to a private allocation so the caller can at least try.
  CLOGW("shared codec state unavailable, falling back to opus_encoder_create");
  int error = 0;
  OpusEncoder* encoder = opus_encoder_create(sample_rate, channels, application, &error);
  if (encoder == nullptr) {
    printf("[opus_pool] opus_encoder_create failed: %d\n", error);
    LogHeap("encoder fallback failed,");
    return nullptr;
  }
  ApplyEncoderSettings(encoder);
  return encoder;
}

void ReleaseEncoder(OpusEncoder* encoder) {
  if (encoder == nullptr) {
    return;
  }
  if (reinterpret_cast<void*>(encoder) == g_shared_state) {
    g_shared_in_use = false;
    return;
  }
  opus_encoder_destroy(encoder);
}

OpusDecoder* AcquireDecoder(const uint32_t sample_rate, const uint32_t channels) {
  if (g_shared_state != nullptr && !g_shared_in_use && g_shared_size >= static_cast<size_t>(opus_decoder_get_size(static_cast<int>(channels)))) {
    auto* decoder = reinterpret_cast<OpusDecoder*>(g_shared_state);
    const int error = opus_decoder_init(decoder, sample_rate, channels);
    if (error != OPUS_OK) {
      printf("[opus_pool] opus_decoder_init failed: %d\n", error);
      return nullptr;
    }
    g_shared_in_use = true;
    return decoder;
  }

  CLOGW("shared codec state unavailable, falling back to opus_decoder_create");
  int error = 0;
  OpusDecoder* decoder = opus_decoder_create(sample_rate, channels, &error);
  if (decoder == nullptr) {
    printf("[opus_pool] opus_decoder_create failed: %d\n", error);
    LogHeap("decoder fallback failed,");
    return nullptr;
  }
  return decoder;
}

void ReleaseDecoder(OpusDecoder* decoder) {
  if (decoder == nullptr) {
    return;
  }
  if (reinterpret_cast<void*>(decoder) == g_shared_state) {
    g_shared_in_use = false;
    return;
  }
  opus_decoder_destroy(decoder);
}

}  // namespace opus_codec_pool
