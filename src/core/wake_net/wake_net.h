#pragma once

#ifndef _WAKE_NET_H_
#define _WAKE_NET_H_

#include <functional>
#include <memory>

#include "audio_device/audio_input_device.h"
#include "components/task_queue/active_task_queue.h"
#include "core/flex_array/flex_array.h"

struct esp_afe_sr_data_t;
class SilkResampler;

class WakeNet {
 public:
  explicit WakeNet(std::function<void()>&& handler, std::shared_ptr<ai_vox::AudioInputDevice> audio_input_device);
  ~WakeNet();
  void Start();
  void Stop();

 private:
  WakeNet(const WakeNet&) = delete;
  WakeNet& operator=(const WakeNet&) = delete;
  void FeedData(const uint32_t samples);
  void DetectWakeWord();
  FlexArray<int16_t> ReadPcm(const uint32_t samples);

  std::function<void()> handler_;
  std::shared_ptr<ai_vox::AudioInputDevice> audio_input_device_;
  ActiveTaskQueue* detect_task_ = nullptr;
  ActiveTaskQueue* feed_task_ = nullptr;
  std::unique_ptr<SilkResampler> resampler_;
  esp_afe_sr_data_t* afe_data_ = nullptr;
};

#endif  // _WAKE_NET_H_