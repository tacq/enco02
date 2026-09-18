#pragma once

#include <driver/i2s_pdm.h>

#include "audio_input_device.h"

namespace ai_vox {
class PdmAudioInputDevice : public AudioInputDevice {
 public:
  PdmAudioInputDevice(gpio_num_t clk, gpio_num_t din) : clk_pin_(clk), din_pin_(din) {
  }
  ~PdmAudioInputDevice() {
    CloseInput();
  }

  bool OpenInput(uint32_t sample_rate) override {
    CloseInput();

    i2s_chan_config_t rx_chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 2,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, nullptr, &i2s_rx_handle_));

    i2s_pdm_rx_config_t rx_pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .clk = clk_pin_,
                .din = din_pin_,
                .invert_flags =
                    {
                        .clk_inv = false,
                    },
            },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(i2s_rx_handle_, &rx_pdm_cfg));
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
    i2s_channel_read(i2s_rx_handle_, buffer, samples * sizeof(int16_t), nullptr, 1000);
    return samples;
  }

  uint32_t input_sample_rate() override {
    return sample_rate_;
  }

 private:
  const gpio_num_t clk_pin_ = GPIO_NUM_NC;
  const gpio_num_t din_pin_ = GPIO_NUM_NC;
  i2s_chan_handle_t i2s_rx_handle_ = nullptr;
  uint32_t sample_rate_ = 0;
};
}  // namespace ai_vox
