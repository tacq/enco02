#include "audio_input_engine.h"

#include "libopus/opus.h"
#include "silk_resampler.h"

#ifndef CLOGGER_SEVERITY
#define CLOGGER_SEVERITY CLOGGER_SEVERITY_WARN
#endif

#include "clogger/clogger.h"

namespace {
constexpr size_t kMaxOpusPacketSize = 1500;
constexpr uint32_t kFrameDuration = 20;                          // ms
constexpr uint32_t kDefaultSampleRate = 16000;                   // Hz
constexpr uint32_t kDefaultChannels = 1;                         // Mono
constexpr size_t kMaxFrameSize = 16000 / 1000 * kFrameDuration;  // 16000 Hz * 20 ms
}  // namespace

AudioInputEngine::AudioInputEngine(std::shared_ptr<ai_vox::AudioInputDevice> audio_input_device,
                                   AudioInputEngine::DataHandler &&handler,
                                   const uint32_t frame_duration)
    : handler_(std::move(handler)), audio_input_device_(std::move(audio_input_device)) {
  CLOGI();
  int error = 0;
  opus_encoder_ = opus_encoder_create(kDefaultSampleRate, kDefaultChannels, OPUS_APPLICATION_VOIP, &error);
  assert(opus_encoder_ != nullptr);
  if (opus_encoder_ == nullptr) {
    CLOG("opus_encoder_create failed: %d", error);
    abort();
    return;
  }

  uint32_t stack_size = 32 << 10;
  opus_encoder_ctl(opus_encoder_, OPUS_SET_DTX(0));
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    opus_encoder_ctl(opus_encoder_, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(opus_encoder_, OPUS_SET_BITRATE(8000));
    stack_size = 20 << 10;
  } else {
    opus_encoder_ctl(opus_encoder_, OPUS_SET_COMPLEXITY(5));
  }
  CLOGI();

  audio_input_device_->OpenInput(kDefaultSampleRate);
  CLOGI();

  if (audio_input_device_->input_sample_rate() != kDefaultSampleRate) {
    resampler_ = std::make_unique<SilkResampler>(audio_input_device_->input_sample_rate(), kDefaultSampleRate);
  }
  CLOGI();
  task_queue_ = new ActiveTaskQueue("AudioInput", stack_size, tskIDLE_PRIORITY + 1);
  task_queue_->Enqueue([this, samples = audio_input_device_->input_sample_rate() / 1000 * frame_duration]() { PullData(samples); });
  CLOGI("OK");
}

AudioInputEngine::~AudioInputEngine() {
  CLOGI();
  delete task_queue_;
  // delete reinterpret_cast<silk_resampler_state_struct *>(silk_resampler_);
  audio_input_device_->CloseInput();
  opus_encoder_destroy(opus_encoder_);
  CLOG("OK");
}

FlexArray<int16_t> AudioInputEngine::ReadPcm(const uint32_t samples) {
  FlexArray<int16_t> pcm(samples);
  audio_input_device_->Read(pcm.data(), pcm.size());
  if (resampler_) {
    return resampler_->Resample(std::move(pcm));
  } else {
    return pcm;
  }
  // return resampler_ ? resampler_->Resample(std::move(pcm)) : pcm;
}

// FlexArray<int16_t> AudioInputEngine::Resample(FlexArray<int16_t> &&input_pcm) {
//   if (silk_resampler_ == nullptr) {
//     return input_pcm;
//   }

//   FlexArray<int16_t> resampled_pcm(input_pcm.size() * kDefaultSampleRate / audio_input_device_->input_sample_rate());
//   const auto ret =
//       silk_resampler(reinterpret_cast<silk_resampler_state_struct *>(silk_resampler_), resampled_pcm.data(), input_pcm.data(), input_pcm.size());
//   if (ret != 0) {
//     CLOGE("silk_resampler_process failed with: %d", ret);
//     abort();
//   }
//   return resampled_pcm;
// }

void AudioInputEngine::PullData(const uint32_t samples) {
  auto pcm = ReadPcm(samples);
  FlexArray<uint8_t> data(kMaxOpusPacketSize);
  const auto ret = opus_encode(opus_encoder_, pcm.data(), pcm.size(), data.data(), data.size());
  if (ret > 0) {
    data.Resize(ret);
    handler_(std::move(data));
  } else {
    CLOGE("opus_encode failed with: %d", ret);
    abort();
  }

  task_queue_->Enqueue([this, samples]() { PullData(samples); });
}