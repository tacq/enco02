#include "opus_codec_pool.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdio.h>

#include "libopus/opus.h"

#ifndef CLOGGER_SEVERITY
#define CLOGGER_SEVERITY CLOGGER_SEVERITY_WARN
#endif

#include "clogger/clogger.h"

namespace opus_codec_pool {
namespace {

// These must match the rates the engine actually uses (see audio_input_engine.cpp /
// audio_output_engine.cpp). They are only used for the boot-time preallocation; Acquire*() will
// still rebuild a codec if it is ever asked for a different configuration.
constexpr uint32_t kEncoderSampleRate = 16000;
constexpr uint32_t kDecoderSampleRate = 24000;
constexpr uint32_t kChannels = 1;

OpusEncoder* g_encoder = nullptr;
uint32_t g_encoder_sample_rate = 0;
uint32_t g_encoder_channels = 0;
int g_encoder_application = 0;

OpusDecoder* g_decoder = nullptr;
uint32_t g_decoder_sample_rate = 0;
uint32_t g_decoder_channels = 0;

void LogHeap(const char* stage) {
  printf("[opus_pool] %s free heap: %u, largest block: %u\n",
         stage,
         static_cast<unsigned>(esp_get_free_heap_size()),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

OpusEncoder* CreateEncoder(const uint32_t sample_rate, const uint32_t channels, const int application) {
  int error = 0;
  OpusEncoder* encoder = opus_encoder_create(sample_rate, channels, application, &error);
  if (encoder == nullptr) {
    printf("[opus_pool] opus_encoder_create(%u, %u) failed: %d, needed %d bytes\n",
           static_cast<unsigned>(sample_rate),
           static_cast<unsigned>(channels),
           error,
           opus_encoder_get_size(static_cast<int>(channels)));
    LogHeap("encoder alloc failed,");
    return nullptr;
  }

  opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    // No PSRAM: keep the encoder in its cheapest mode. Complexity 0 skips the expensive analysis
    // paths (which also use a lot of stack) and a fixed low bitrate keeps the packets tiny.
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(8000));
  } else {
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
  }
  return encoder;
}

}  // namespace

void Preallocate() {
  if (g_encoder == nullptr) {
    LogHeap("before encoder alloc,");
    g_encoder = CreateEncoder(kEncoderSampleRate, kChannels, OPUS_APPLICATION_VOIP);
    if (g_encoder != nullptr) {
      g_encoder_sample_rate = kEncoderSampleRate;
      g_encoder_channels = kChannels;
      g_encoder_application = OPUS_APPLICATION_VOIP;
      printf("[opus_pool] encoder reserved: %d bytes\n", opus_encoder_get_size(static_cast<int>(kChannels)));
    }
  }

  if (g_decoder == nullptr) {
    int error = 0;
    g_decoder = opus_decoder_create(kDecoderSampleRate, kChannels, &error);
    if (g_decoder != nullptr) {
      g_decoder_sample_rate = kDecoderSampleRate;
      g_decoder_channels = kChannels;
      printf("[opus_pool] decoder reserved: %d bytes\n", opus_decoder_get_size(static_cast<int>(kChannels)));
    } else {
      printf("[opus_pool] opus_decoder_create failed: %d, needed %d bytes\n", error, opus_decoder_get_size(static_cast<int>(kChannels)));
    }
  }

  LogHeap("after codec reservation,");
}

OpusEncoder* AcquireEncoder(const uint32_t sample_rate, const uint32_t channels, const int application) {
  if (g_encoder != nullptr && (g_encoder_sample_rate != sample_rate || g_encoder_channels != channels || g_encoder_application != application)) {
    CLOGW("encoder reconfiguration requested, rebuilding");
    opus_encoder_destroy(g_encoder);
    g_encoder = nullptr;
  }

  if (g_encoder == nullptr) {
    g_encoder = CreateEncoder(sample_rate, channels, application);
    if (g_encoder == nullptr) {
      return nullptr;
    }
    g_encoder_sample_rate = sample_rate;
    g_encoder_channels = channels;
    g_encoder_application = application;
  } else {
    // Same configuration as before: just wipe the history so this listen session starts clean.
    opus_encoder_ctl(g_encoder, OPUS_RESET_STATE);
  }

  return g_encoder;
}

OpusDecoder* AcquireDecoder(const uint32_t sample_rate, const uint32_t channels) {
  if (g_decoder != nullptr && (g_decoder_sample_rate != sample_rate || g_decoder_channels != channels)) {
    CLOGW("decoder reconfiguration requested, rebuilding");
    opus_decoder_destroy(g_decoder);
    g_decoder = nullptr;
  }

  if (g_decoder == nullptr) {
    int error = 0;
    g_decoder = opus_decoder_create(sample_rate, channels, &error);
    if (g_decoder == nullptr) {
      printf("[opus_pool] opus_decoder_create failed: %d\n", error);
      LogHeap("decoder alloc failed,");
      return nullptr;
    }
    g_decoder_sample_rate = sample_rate;
    g_decoder_channels = channels;
  } else {
    opus_decoder_ctl(g_decoder, OPUS_RESET_STATE);
  }

  return g_decoder;
}

}  // namespace opus_codec_pool
