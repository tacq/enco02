#include "audio_output_engine.h"

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
  // opus_decoder_ is owned by opus_codec_pool and deliberately outlives this object.
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
  if (!data.data() || data.size() == 0) {
    return;
  }

  auto pcm = FlexArray<int16_t>(samples_);
  if (!pcm.data() || pcm.size() == 0) {
    CLOGE("dropping frame, no memory for decode buffer");
    return;
  }

  const auto ret = opus_decode(opus_decoder_, data.data(), data.size(), pcm.data(), pcm.size(), 0);
  if (ret >= 0) {
    WritePcm(std::move(pcm));
  }
}

void AudioOutputEngine::WritePcm(FlexArray<int16_t>&& pcm) {
  if (resampler_) {
    auto resampled_pcm = resampler_->Resample(std::move(pcm));
    audio_output_device_->Write(resampled_pcm.data(), resampled_pcm.size());
  } else {
    audio_output_device_->Write(pcm.data(), pcm.size());
  }
}