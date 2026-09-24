#include "audio_output_engine.h"

#include <esp_heap_caps.h>

#include "audio_task_stack.h"
#include "flex_array/flex_array.h"
#include "libopus/opus.h"
#include "opus_codec_pool.h"
#include "silk_resampler.h"

#ifndef CLOGGER_SEVERITY
#define CLOGGER_SEVERITY CLOGGER_SEVERITY_WARN
#endif
#include "clogger/clogger.h"

namespace {
constexpr uint32_t kDefaultSampleRate = 24000;
constexpr uint32_t kDefaultChannels = 1;
constexpr uint32_t kDefaultDurationMs = 20;  // Duration in milliseconds
constexpr uint32_t kDefaultFrameSize = kDefaultSampleRate / 1000 * kDefaultChannels * kDefaultDurationMs;

// Shared with AudioInputEngine: the engine state machine never keeps both alive at once, so one
// statically reserved stack is enough and saves 12KB of permanently occupied internal RAM.
constexpr uint32_t kAudioOutputStackSize = audio_task_stack::kStackSize;

enum ScratchSlot { kPcmSlot = 0, kResampleSlot = 1 };

// Playback scratch. First choice is the idle tail of the codec buffer reserved at boot (the
// decoder uses 17800 of its 24548 bytes), which costs no heap at all. Fallback is a grow-only,
// never-freed allocation made with heap_caps_realloc, which returns nullptr instead of throwing,
// so running out of memory drops a frame rather than aborting the firmware.
int16_t* ScratchBuffer(const OpusDecoder* decoder, const ScratchSlot slot, const size_t samples,
                       const size_t pcm_samples) {
  size_t tail_bytes = 0;
  auto* tail = static_cast<int16_t*>(opus_codec_pool::DecoderScratch(decoder, &tail_bytes));
  const size_t tail_samples = tail_bytes / sizeof(int16_t);
  if (tail != nullptr) {
    if (slot == kPcmSlot && samples <= tail_samples) {
      return tail;
    }
    if (slot == kResampleSlot && pcm_samples + samples <= tail_samples) {
      return tail + pcm_samples;
    }
  }
  static int16_t* buffers[2] = {nullptr, nullptr};
  static size_t capacity[2] = {0, 0};
  if (capacity[slot] < samples) {
    void* grown = heap_caps_realloc(buffers[slot], samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (grown == nullptr) {
      return nullptr;
    }
    buffers[slot] = static_cast<int16_t*>(grown);
    capacity[slot] = samples;
  }
  return buffers[slot];
}
}  // namespace

AudioOutputEngine::AudioOutputEngine(std::shared_ptr<ai_vox::AudioOutputDevice> audio_output_device, const uint32_t frame_duration)
    : audio_output_device_(std::move(audio_output_device)), samples_(kDefaultSampleRate / 1000 * kDefaultChannels * frame_duration) {
  CLOGI();
  // The decoder state (~18KB) is reserved once during boot by the codec pool. Recreating it here on
  // every speak transition would eventually fail on the fragmented internal heap and the assert
  // below used to reboot the device.
  opus_decoder_ = opus_codec_pool::AcquireDecoder(kDefaultSampleRate, kDefaultChannels);
  if (opus_decoder_ == nullptr) {
    CLOGE("no opus decoder available, audio playback disabled");
    return;
  }

  audio_output_device_->OpenOutput(kDefaultSampleRate);

  if (audio_output_device_->output_sample_rate() != kDefaultSampleRate) {
    CLOGD("init resampler for %" PRIu32 " -> %" PRIu32, kDefaultSampleRate, audio_output_device_->output_sample_rate());
    resampler_ = std::make_unique<SilkResampler>(kDefaultSampleRate, audio_output_device_->output_sample_rate());
  }

  // nullptr means "no shared stack available" - ActiveTaskQueue then falls back to the heap.
  task_stack_ = audio_task_stack::Acquire();
  task_queue_ = new ActiveTaskQueue("AudioOutput", kAudioOutputStackSize, tskIDLE_PRIORITY + 1, false, task_stack_);
  CLOGI("OK");
}

AudioOutputEngine::~AudioOutputEngine() {
  CLOGI();
  delete task_queue_;
  task_queue_ = nullptr;
  // Only safe once the task above has actually been deleted.
  audio_task_stack::Release(task_stack_);
  task_stack_ = nullptr;
  if (opus_decoder_ != nullptr) {
    audio_output_device_->CloseOutput();
  }
  // The decoder state belongs to opus_codec_pool and is shared with the encoder; hand it back so
  // the capture engine can re-init the same buffer.
  opus_codec_pool::ReleaseDecoder(opus_decoder_);
  opus_decoder_ = nullptr;
  CLOGI("OK");
}

void AudioOutputEngine::Write(FlexArray<uint8_t>&& data) {
  if (task_queue_ == nullptr) {
    // Engine is running degraded (no decoder). Drop the frame rather than crash.
    return;
  }
  // Without PSRAM the heap is tiny. If the decoder cannot keep up with the incoming TTS stream the
  // queue would grow without bound, each entry holding a heap buffer, until malloc() fails.
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0 && task_queue_->size() > 12) {
    CLOGW("audio output backlog too deep, dropping frame");
    return;
  }
  task_queue_->Enqueue([this, data = std::move(data)]() mutable { ProcessData(std::move(data)); });
}

void AudioOutputEngine::NotifyDataEnd(std::function<void()>&& callback) {
  if (task_queue_ == nullptr) {
    // No worker to serialise against: run the completion callback straight away so the engine state
    // machine still advances out of the speaking state.
    if (callback) {
      callback();
    }
    return;
  }
  task_queue_->Enqueue(std::move(callback));
}

void AudioOutputEngine::ProcessData(FlexArray<uint8_t>&& data) {
  if (!data.data() || data.size() == 0 || opus_decoder_ == nullptr) {
    return;
  }

  // Persistent buffer: this runs ~17-50 times a second for the length of every reply, so a
  // per-frame allocation is pure heap churn - and on a heap this tight it eventually fails.
  //
  // It also outlives this engine (one is created per reply) and is never freed. It used to be a
  // std::vector member resized on the first frame of every reply; with the camera view open the
  // heap had 12.7KB free but a 2.4KB largest block, the 2,880-byte resize threw, and with no
  // exception handling that is abort() - a reboot mid-sentence. Now it is taken once, with a
  // non-throwing allocator, and a failure only drops the frame and retries on the next one.
  int16_t* pcm = ScratchBuffer(opus_decoder_, kPcmSlot, samples_, 0);
  if (pcm == nullptr) {
    CLOGE("dropping frame, no memory for decode buffer");
    return;
  }

  const auto ret = opus_decode(opus_decoder_, data.data(), data.size(), pcm, samples_, 0);
  if (ret > 0) {
    WritePcm(pcm, static_cast<size_t>(ret));
  }
}

void AudioOutputEngine::WritePcm(const int16_t* pcm, const size_t samples) {
  if (pcm == nullptr || samples == 0) {
    return;
  }
  if (resampler_) {
    // Persistent buffer for the same reason as the decode buffer above.
    const size_t needed = resampler_->OutputSamplesFor(samples);
    int16_t* out = ScratchBuffer(opus_decoder_, kResampleSlot, needed, samples_);
    if (out == nullptr) {
      CLOGE("dropping frame, no memory for resample buffer");
      return;
    }
    const size_t written = resampler_->Resample(pcm, samples, out, needed);
    if (written == 0) {
      return;
    }
    audio_output_device_->Write(out, written);
  } else {
    audio_output_device_->Write(pcm, samples);
  }
}