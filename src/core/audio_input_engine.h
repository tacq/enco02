#pragma once

#ifndef _AUDIO_INPUT_ENGINE_H_
#define _AUDIO_INPUT_ENGINE_H_

#include <functional>
#include <memory>
#include <vector>

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
  // Borrowed from audio_task_stack; shared with AudioOutputEngine, returned in the destructor.
  StackType_t *task_stack_ = nullptr;
  // Persistent scratch buffers. The capture loop runs continuously, so allocating these per frame
  // would fragment the (very limited) internal heap until malloc() eventually returns nullptr.
  std::vector<int16_t> pcm_buffer_;
  std::vector<uint8_t> opus_buffer_;
  // Only used when the microphone cannot run at the engine's 16kHz capture rate.
  std::vector<int16_t> resampled_buffer_;
};

#endif