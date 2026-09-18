#pragma once

#ifndef _AUDIO_INPUT_ENGINE_H_
#define _AUDIO_INPUT_ENGINE_H_

#include <functional>
#include <memory>

#include "audio_device//audio_input_device.h"
#include "components/task_queue/active_task_queue.h"
#include "flex_array/flex_array.h"

struct OpusDecoder;
class SilkResampler;
class AudioInputEngine {
 public:
  using DataHandler = std::function<void(FlexArray<uint8_t> &&)>;

  explicit AudioInputEngine(std::shared_ptr<ai_vox::AudioInputDevice> audio_input_device,
                            AudioInputEngine::DataHandler &&handler,
                            const uint32_t frame_duration);
  ~AudioInputEngine();

 private:
  AudioInputEngine(const AudioInputEngine &) = delete;
  AudioInputEngine &operator=(const AudioInputEngine &) = delete;

  FlexArray<int16_t> ReadPcm(const uint32_t samples);
  void PullData(const uint32_t samples);

  const DataHandler handler_;
  std::shared_ptr<ai_vox::AudioInputDevice> audio_input_device_;
  struct OpusEncoder *opus_encoder_ = nullptr;
  std::unique_ptr<SilkResampler> resampler_;
  ActiveTaskQueue *task_queue_ = nullptr;
};

#endif