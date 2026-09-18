#pragma once

#ifndef _AUDIO_INPUT_DEVICE_H_
#define _AUDIO_INPUT_DEVICE_H_

#include <cstdint>
#include <vector>

namespace ai_vox {
class AudioInputDevice {
 public:
  virtual ~AudioInputDevice() = default;
  virtual bool OpenInput(uint32_t sample_rate) = 0;
  virtual void CloseInput() = 0;
  virtual size_t Read(int16_t* buffer, uint32_t samples) = 0;
  virtual uint32_t input_sample_rate() = 0;
};
}  // namespace ai_vox

#endif