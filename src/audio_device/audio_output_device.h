#pragma once

#ifndef _AUDIO_OUTPUT_DEVICE_H_
#define _AUDIO_OUTPUT_DEVICE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ai_vox {
class AudioOutputDevice {
 public:
  constexpr static uint16_t kMaxVolume = 100;

  virtual ~AudioOutputDevice() = default;
  virtual bool OpenOutput(uint32_t sample_rate) = 0;
  virtual void CloseOutput() = 0;
  virtual size_t Write(const int16_t* pcm, size_t samples) = 0;
  virtual void set_volume(uint16_t volume) = 0;
  virtual uint16_t volume() const = 0;
  virtual uint32_t output_sample_rate() = 0;
};
}  // namespace ai_vox

#endif