#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "audio_device/audio_output_device.h"
#include "components/task_queue/active_task_queue.h"
#include "flex_array/flex_array.h"

class OpusDecoder;
class SilkResampler;
class AudioOutputEngine {
 public:
  explicit AudioOutputEngine(std::shared_ptr<ai_vox::AudioOutputDevice> audio_output_device, const uint32_t frame_duration);
  ~AudioOutputEngine();

  void Write(FlexArray<uint8_t>&& data);
  void NotifyDataEnd(std::function<void()>&& callback);

 private:
  AudioOutputEngine(const AudioOutputEngine&) = delete;
  AudioOutputEngine& operator=(const AudioOutputEngine&) = delete;

  static void Loop(void* self);
  void Loop();
  void ProcessData(FlexArray<uint8_t>&& data);
  void WritePcm(const int16_t* pcm, size_t samples);

  std::shared_ptr<ai_vox::AudioOutputDevice> audio_output_device_;
  struct OpusDecoder* opus_decoder_ = nullptr;
  std::unique_ptr<SilkResampler> resampler_;
  ActiveTaskQueue* task_queue_ = nullptr;
  // Borrowed from audio_task_stack; shared with AudioInputEngine, returned in the destructor.
  StackType_t* task_stack_ = nullptr;
  // Persistent decode scratch buffer; see ProcessData().
  std::vector<int16_t> pcm_buffer_;
  // Persistent resample scratch buffer; only used when the device cannot run at 24kHz.
  std::vector<int16_t> resampled_buffer_;
  const uint32_t samples_ = 0;
};