#pragma once

#ifndef _AUDIO_DEVICE_ES8311_H_
#define _AUDIO_DEVICE_ES8311_H_

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#include <memory>

#include "audio_input_device.h"
#include "audio_output_device.h"

struct audio_codec_data_if_t;
struct audio_codec_ctrl_if_t;
struct audio_codec_if_t;

namespace ai_vox {
class AudioDeviceEs8311 : public AudioInputDevice, public AudioOutputDevice {
 public:
  explicit AudioDeviceEs8311(const i2c_master_bus_handle_t i2c_master_bus_handle,
                             const uint8_t i2c_addr,
                             const i2c_port_t i2c_port,
                             const uint32_t sample_rate,
                             const gpio_num_t mclk,
                             const gpio_num_t bclk,
                             const gpio_num_t ws,
                             const gpio_num_t din,
                             const gpio_num_t dout);
  ~AudioDeviceEs8311();
  inline auto sample_rate() const {
    return sample_rate_;
  }

  bool OpenInput(uint32_t sample_rate) override;
  void CloseInput() override;
  size_t Read(int16_t* buffer, uint32_t samples) override;
  uint32_t input_sample_rate() override {
    return sample_rate_;
  }

  bool OpenOutput(uint32_t sample_rate) override;
  void CloseOutput() override;
  size_t Write(const int16_t* pcm, size_t samples) override;
  uint32_t output_sample_rate() override {
    return sample_rate_;
  }

  void set_volume(uint16_t volume) override;
  uint16_t volume() const override;

 private:
  AudioDeviceEs8311(const AudioDeviceEs8311&) = delete;
  AudioDeviceEs8311& operator=(const AudioDeviceEs8311&) = delete;

  uint32_t sample_rate_ = 0;
  i2s_chan_handle_t tx_handle_ = nullptr;
  i2s_chan_handle_t rx_handle_ = nullptr;
  const audio_codec_data_if_t* data_if_ = nullptr;
  const audio_codec_ctrl_if_t* out_ctrl_if_ = nullptr;
  const void* gpio_if_ = nullptr;
  const audio_codec_if_t* codec_if_ = nullptr;
  void* audio_device_ = nullptr;
  uint16_t volume_ = 80;
  std::shared_ptr<AudioInputDevice> audio_input_device_;
  std::shared_ptr<AudioOutputDevice> audio_output_device_;
};
}  // namespace ai_vox
#endif