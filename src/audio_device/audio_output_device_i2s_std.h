#pragma once

#ifndef _I2S_STD_AUDIO_OUTPUT_DEVICE_H_
#define _I2S_STD_AUDIO_OUTPUT_DEVICE_H_

#include <driver/i2s_std.h>

#include <atomic>
#include <cmath>

#include "audio_output_device.h"

namespace ai_vox {
class AudioOutputDeviceI2sStd : public AudioOutputDevice {
 public:
  AudioOutputDeviceI2sStd(gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout) : pin_bclk_(bclk), pin_ws_(ws), pin_dout_(dout) {
  }

  ~AudioOutputDeviceI2sStd() {
    CloseOutput();
  }

  uint16_t volume() const override {
    return volume_;
  }

  void set_volume(uint16_t volume) override {
    if (volume > kMaxVolume) {
      volume = kMaxVolume;
    }
    volume_ = volume;
    volume_factor_ = pow(double(volume_) / 100.0, 2) * 65536;
  }

  bool OpenOutput(uint32_t sample_rate) override {
    CloseOutput();
    i2s_chan_config_t tx_chan_cfg = {
#if SOC_I2S_NUM > 1
        .id = I2S_NUM_1,
#else
        .id = I2S_NUM_0,
#endif
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 2,
        .dma_frame_num = 480,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &i2s_tx_handle_, nullptr));

    i2s_std_config_t tx_std_cfg = {.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
                                   .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
                                   .gpio_cfg = {
                                       .mclk = I2S_GPIO_UNUSED,
                                       .bclk = pin_bclk_,
                                       .ws = pin_ws_,
                                       .dout = pin_dout_,
                                       .din = I2S_GPIO_UNUSED,
                                       .invert_flags =
                                           {
                                               .mclk_inv = 0,
                                               .bclk_inv = 0,
                                               .ws_inv = 0,
                                           },
                                   }};
    tx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_handle_, &tx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_handle_));
    sample_rate_ = sample_rate;
    return true;
  }

  void CloseOutput() override {
    if (i2s_tx_handle_ == nullptr) {
      return;
    }

    i2s_channel_disable(i2s_tx_handle_);
    i2s_del_channel(i2s_tx_handle_);
    i2s_tx_handle_ = nullptr;
    sample_rate_ = 0;
  }
  size_t Write(const int16_t* pcm, size_t samples) override {
    std::vector<int32_t> buffer(samples);

    for (size_t i = 0; i < samples; i++) {
      int64_t temp = int64_t(pcm[i]) * volume_factor_;
      if (temp > INT32_MAX) {
        buffer[i] = INT32_MAX;
      } else if (temp < INT32_MIN) {
        buffer[i] = INT32_MIN;
      } else {
        buffer[i] = static_cast<int32_t>(temp);
      }
    }

    size_t bytes_written = 0;
    ESP_ERROR_CHECK(i2s_channel_write(i2s_tx_handle_, buffer.data(), buffer.size() * sizeof(int32_t), &bytes_written, 1000));
    return buffer.size();
  }
  uint32_t output_sample_rate() override {
    return sample_rate_;
  }

 private:
  i2s_chan_handle_t i2s_tx_handle_ = nullptr;
  const gpio_num_t pin_bclk_ = I2S_GPIO_UNUSED;
  const gpio_num_t pin_ws_ = I2S_GPIO_UNUSED;
  const gpio_num_t pin_dout_ = I2S_GPIO_UNUSED;
  std::atomic<uint16_t> volume_ = 70;
  std::atomic<int32_t> volume_factor_ = pow(double(volume_) / 100.0, 2) * 65536;
  uint32_t sample_rate_ = 0;
};
}  // namespace ai_vox

#endif
