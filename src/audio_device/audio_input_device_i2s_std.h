#pragma once

#ifndef _AUDIO_INPUT_DEVICE_I2S_STD_H_
#define _AUDIO_INPUT_DEVICE_I2S_STD_H_

#include <driver/i2s_std.h>

#include "audio_input_device.h"

namespace ai_vox {
class AudioInputDeviceI2sStd : public AudioInputDevice {
 public:
  AudioInputDeviceI2sStd(gpio_num_t bclk, gpio_num_t ws, gpio_num_t din) : pin_bclk_(bclk), pin_ws_(ws), pin_din_(din) {
  }

  ~AudioInputDeviceI2sStd() {
    CloseInput();
  }

 private:
  bool OpenInput(uint32_t sample_rate) override {
    CloseInput();
    i2s_chan_config_t rx_chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 2,
        .dma_frame_num = 320,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, nullptr, &i2s_rx_handle_));

    i2s_std_config_t rx_std_cfg = {.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
                                   .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
                                   .gpio_cfg = {
                                       .mclk = I2S_GPIO_UNUSED,
                                       .bclk = pin_bclk_,
                                       .ws = pin_ws_,
                                       .dout = I2S_GPIO_UNUSED,
                                       .din = pin_din_,
                                       .invert_flags =
                                           {
                                               .mclk_inv = 0,
                                               .bclk_inv = 0,
                                               .ws_inv = 0,
                                           },
                                   }};
    rx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_handle_, &rx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_handle_));
    sample_rate_ = sample_rate;
    return true;
  }

  void CloseInput() override {
    if (i2s_rx_handle_ == nullptr) {
      return;
    }
    i2s_channel_disable(i2s_rx_handle_);
    i2s_del_channel(i2s_rx_handle_);
    i2s_rx_handle_ = nullptr;
    sample_rate_ = 0;
  }

  size_t Read(int16_t* buffer, uint32_t samples) override {
    auto raw_32bit_samples = new int32_t[samples];
    i2s_channel_read(i2s_rx_handle_, raw_32bit_samples, samples * sizeof(raw_32bit_samples[0]), nullptr, 1000);

    for (int i = 0; i < samples; i++) {
      int32_t value = raw_32bit_samples[i] >> 12;
      buffer[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : (int16_t)value;
    }
    delete[] raw_32bit_samples;
    return samples;
  }

  uint32_t input_sample_rate() override {
    return sample_rate_;
  }

  const gpio_num_t pin_bclk_ = GPIO_NUM_NC;
  const gpio_num_t pin_ws_ = GPIO_NUM_NC;
  const gpio_num_t pin_din_ = GPIO_NUM_NC;
  i2s_chan_handle_t i2s_rx_handle_ = nullptr;
  uint32_t sample_rate_ = 0;
};
}  // namespace ai_vox
#endif