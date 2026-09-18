#include "audio_output_engine.h"

#include "flex_array/flex_array.h"
#include "libopus/opus.h"
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
}  // namespace

AudioOutputEngine::AudioOutputEngine(std::shared_ptr<ai_vox::AudioOutputDevice> audio_output_device, const uint32_t frame_duration)
    : audio_output_device_(std::move(audio_output_device)), samples_(kDefaultSampleRate / 1000 * kDefaultChannels * frame_duration) {
  CLOGI();
  int error = -1;
  opus_decoder_ = opus_decoder_create(kDefaultSampleRate, kDefaultChannels, &error);
  assert(opus_decoder_ != nullptr);
  audio_output_device_->OpenOutput(kDefaultSampleRate);

  if (audio_output_device_->output_sample_rate() != kDefaultSampleRate) {
    CLOGD("init resampler for %" PRIu32 " -> %" PRIu32, kDefaultSampleRate, audio_output_device_->output_sample_rate());
    resampler_ = std::make_unique<SilkResampler>(kDefaultSampleRate, audio_output_device_->output_sample_rate());
  }

  uint32_t stack_size = 9 << 10;
  task_queue_ = new ActiveTaskQueue("AudioOutput", stack_size, tskIDLE_PRIORITY + 1);
  CLOGI("OK");
}

AudioOutputEngine::~AudioOutputEngine() {
  CLOGI();
  delete task_queue_;
  audio_output_device_->CloseOutput();
  opus_decoder_destroy(opus_decoder_);
  CLOGI("OK");
}

void AudioOutputEngine::Write(FlexArray<uint8_t>&& data) {
  task_queue_->Enqueue([this, data = std::move(data)]() mutable { ProcessData(std::move(data)); });
}

void AudioOutputEngine::NotifyDataEnd(std::function<void()>&& callback) {
  task_queue_->Enqueue(std::move(callback));
}

void AudioOutputEngine::ProcessData(FlexArray<uint8_t>&& data) {
  auto pcm = FlexArray<int16_t>(samples_);

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