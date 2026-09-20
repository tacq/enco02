#include "audio_input_engine.h"

#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>

#include "libopus/opus.h"
#include "opus_codec_pool.h"
#include "silk_resampler.h"

#ifndef CLOGGER_SEVERITY
#define CLOGGER_SEVERITY CLOGGER_SEVERITY_WARN
#endif

#include "clogger/clogger.h"

namespace {
// A 20ms mono frame at the bitrates this firmware uses (8kbps without PSRAM) is ~20 bytes; even at
// the Opus maximum for this frame size it stays far below 512. Reserving 1500 bytes per engine was
// wasted internal RAM on a board that has none to spare.
constexpr size_t kMaxOpusPacketSize = 512;
constexpr uint32_t kFrameDuration = 20;                          // ms
constexpr uint32_t kDefaultSampleRate = 16000;                   // Hz
constexpr uint32_t kDefaultChannels = 1;                         // Mono
constexpr size_t kMaxFrameSize = 16000 / 1000 * kFrameDuration;  // 16000 Hz * 20 ms

// The Opus encoder task is created and destroyed on every listen/speak transition. Asking the heap
// for a ~24KB contiguous block each time fails once the heap fragments (WiFi + TLS + LVGL leave
// very little contiguous internal RAM), and xTaskCreateStatic() then aborts the whole system.
// Reserving the stack statically makes task creation infallible and removes the churn entirely.
constexpr uint32_t kAudioInputStackSize = 24 * 1024;  // bytes (StackType_t is uint8_t on ESP-IDF)
alignas(16) StackType_t g_audio_input_stack[kAudioInputStackSize];
}  // namespace

AudioInputEngine::AudioInputEngine(std::shared_ptr<ai_vox::AudioInputDevice> audio_input_device,
                                   AudioInputEngine::DataHandler &&handler,
                                   const uint32_t frame_duration)
    : handler_(std::move(handler)), audio_input_device_(std::move(audio_input_device)) {
  CLOGI();
  // The encoder state (~20KB) is owned by the codec pool and reserved during boot. Allocating it
  // here would fail once WiFi + TLS + LVGL have fragmented the internal heap, and a failed
  // allocation used to abort() the firmware into a reboot loop.
  opus_encoder_ = opus_codec_pool::AcquireEncoder(kDefaultSampleRate, kDefaultChannels, OPUS_APPLICATION_VOIP);
  if (opus_encoder_ == nullptr) {
    // Degrade gracefully: the device stays alive (display, servos, playback) and simply cannot
    // capture audio, which is far better than a boot loop.
    CLOGE("no opus encoder available, microphone capture disabled");
    printf("AudioInput: encoder unavailable, free heap: %u, largest block: %u\n",
           static_cast<unsigned>(esp_get_free_heap_size()),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return;
  }

  const uint32_t stack_size = kAudioInputStackSize;
  CLOGI();

  audio_input_device_->OpenInput(kDefaultSampleRate);
  CLOGI();

  if (audio_input_device_->input_sample_rate() != kDefaultSampleRate) {
    resampler_ = std::make_unique<SilkResampler>(audio_input_device_->input_sample_rate(), kDefaultSampleRate);
  }
  CLOGI();

  const uint32_t samples_per_frame = audio_input_device_->input_sample_rate() / 1000 * frame_duration;
  pcm_buffer_.resize(samples_per_frame);
  opus_buffer_.resize(kMaxOpusPacketSize);

  task_queue_ = new ActiveTaskQueue("AudioInput", stack_size, tskIDLE_PRIORITY + 1, false, g_audio_input_stack);
  task_queue_->Enqueue([this, samples_per_frame]() { PullData(samples_per_frame); });
  printf("AudioInput started, stack: %u bytes, free heap: %u, largest block: %u\n",
         static_cast<unsigned>(stack_size),
         static_cast<unsigned>(esp_get_free_heap_size()),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

AudioInputEngine::~AudioInputEngine() {
  CLOGI();
  delete task_queue_;
  task_queue_ = nullptr;
  if (opus_encoder_ != nullptr) {
    // Only ever opened when the encoder was available.
    audio_input_device_->CloseInput();
  }
  // opus_encoder_ is owned by opus_codec_pool and deliberately outlives this object.
  opus_encoder_ = nullptr;
  CLOG("OK");
}

FlexArray<int16_t> AudioInputEngine::ReadPcm(const uint32_t samples) {
  FlexArray<int16_t> pcm(samples);
  if (!pcm.data() || pcm.size() == 0) {
    return FlexArray<int16_t>(0);
  }
  audio_input_device_->Read(pcm.data(), pcm.size());
  if (resampler_) {
    return resampler_->Resample(std::move(pcm));
  } else {
    return pcm;
  }
}

void AudioInputEngine::PullData(const uint32_t samples) {
  // Reuse persistent buffers: the capture loop runs ~17 times per second forever, so per-frame
  // malloc()/free() would steadily fragment the heap until an allocation returns nullptr and the
  // null pointer gets handed to opus_encode() (StoreProhibited crash).
  if (pcm_buffer_.size() != samples) {
    pcm_buffer_.resize(samples);
  }
  if (opus_buffer_.size() != kMaxOpusPacketSize) {
    opus_buffer_.resize(kMaxOpusPacketSize);
  }

  if (pcm_buffer_.empty() || opus_buffer_.empty()) {
    CLOGE("audio scratch buffers unavailable, free heap: %u", static_cast<unsigned>(esp_get_free_heap_size()));
    task_queue_->Enqueue([this, samples]() { PullData(samples); });
    return;
  }

  audio_input_device_->Read(pcm_buffer_.data(), samples);

  const int16_t *encode_src = pcm_buffer_.data();
  uint32_t encode_samples = samples;
  FlexArray<int16_t> resampled(0);
  if (resampler_) {
    FlexArray<int16_t> raw(samples);
    if (!raw.data()) {
      task_queue_->Enqueue([this, samples]() { PullData(samples); });
      return;
    }
    std::copy(pcm_buffer_.begin(), pcm_buffer_.begin() + samples, raw.data());
    resampled = resampler_->Resample(std::move(raw));
    if (!resampled.data() || resampled.size() == 0) {
      task_queue_->Enqueue([this, samples]() { PullData(samples); });
      return;
    }
    encode_src = resampled.data();
    encode_samples = resampled.size();
  }

  const auto ret = opus_encode(opus_encoder_, encode_src, encode_samples, opus_buffer_.data(), opus_buffer_.size());
  if (ret > 0) {
    // Only the compressed payload (typically ~60 bytes) is allocated, instead of the full 1500 byte
    // worst-case packet buffer.
    FlexArray<uint8_t> packet(static_cast<size_t>(ret));
    if (packet.data() != nullptr) {
      std::copy(opus_buffer_.begin(), opus_buffer_.begin() + ret, packet.data());
      handler_(std::move(packet));
    } else {
      CLOGE("dropping frame, free heap: %u", static_cast<unsigned>(esp_get_free_heap_size()));
    }
  } else {
    CLOGE("opus_encode failed with: %d", ret);
  }

  task_queue_->Enqueue([this, samples]() { PullData(samples); });
}