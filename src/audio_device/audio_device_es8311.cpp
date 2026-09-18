#include "audio_device_es8311.h"

#include "core/espressif_esp_codec_dev/esp_codec_dev.h"
#include "core/espressif_esp_codec_dev/esp_codec_dev_defaults.h"

#ifndef CLOGGER_SEVERITY
#define CLOGGER_SEVERITY CLOGGER_SEVERITY_WARN
#endif
#include "core/clogger/clogger.h"

namespace ai_vox {
AudioDeviceEs8311::AudioDeviceEs8311(const i2c_master_bus_handle_t i2c_master_bus_handle,
                                     const uint8_t i2c_addr,
                                     const i2c_port_t i2c_port,
                                     const uint32_t sample_rate,
                                     const gpio_num_t mclk,
                                     const gpio_num_t bclk,
                                     const gpio_num_t ws,
                                     const gpio_num_t din,
                                     const gpio_num_t dout)
    : sample_rate_(sample_rate) {
  i2s_chan_config_t chan_cfg = {
      .id = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 4,
      .dma_frame_num = 240,
      .auto_clear_after_cb = true,
      .auto_clear_before_cb = false,
      .allow_pd = false,
      .intr_priority = 0,
  };

  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = mclk,
              .bclk = bclk,
              .ws = ws,
              .dout = dout,
              .din = din,
              .invert_flags =
                  {
                      .mclk_inv = 0,
                      .bclk_inv = 0,
                      .ws_inv = 0,
                  },
          },
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));

  audio_codec_i2s_cfg_t i2s_cfg = {
      .port = I2S_NUM_0,
      .rx_handle = rx_handle_,
      .tx_handle = tx_handle_,
  };

  data_if_ = audio_codec_new_i2s_data(&i2s_cfg);

  audio_codec_i2c_cfg_t i2c_cfg = {
      .port = i2c_port,
      .addr = i2c_addr,
      .bus_handle = i2c_master_bus_handle,
  };

  out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);

  gpio_if_ = audio_codec_new_gpio();

  es8311_codec_cfg_t es8311_cfg = {
      .ctrl_if = out_ctrl_if_,
      .gpio_if = reinterpret_cast<const audio_codec_gpio_if_t*>(gpio_if_),
      .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
      .pa_pin = GPIO_NUM_NC,
      .pa_reverted = false,
      .master_mode = false,
      .use_mclk = true,
      .digital_mic = false,
      .invert_mclk = false,
      .invert_sclk = false,
      .hw_gain =
          {
              .pa_voltage = 5.0,
              .codec_dac_voltage = 3.3,
              .pa_gain = 0.0,
          },
      .no_dac_ref = false,
      .mclk_div = 0,
  };

  codec_if_ = es8311_codec_new(&es8311_cfg);
  CLOGD("codec_if: %p", codec_if_);

  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
      .codec_if = codec_if_,
      .data_if = data_if_,
  };

  audio_device_ = esp_codec_dev_new(&dev_cfg);
  CLOGD("audio_device: %p", audio_device_);

  esp_codec_dev_sample_info_t sample_info = {
      .bits_per_sample = 16,
      .channel = 1,
      .channel_mask = 0,
      .sample_rate = sample_rate,
      .mclk_multiple = 0,
  };
  ESP_ERROR_CHECK(esp_codec_dev_open(audio_device_, &sample_info));
  ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(audio_device_, 30.0));
  ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(audio_device_, volume_));
}

AudioDeviceEs8311::~AudioDeviceEs8311() {
  // TODO:
}

bool AudioDeviceEs8311::OpenInput(uint32_t sample_rate) {
  CLOGI("sample rate: %" PRIu32, sample_rate);
  return true;
}

void AudioDeviceEs8311::CloseInput() {
}

size_t AudioDeviceEs8311::Read(int16_t* buffer, uint32_t samples) {
  const auto ret = esp_codec_dev_read(audio_device_, buffer, sizeof(int16_t) * samples);
  //   CLOGD("read %" PRIu32 " samples, ret: %d", samples, ret);
  if (ret != ESP_CODEC_DEV_OK) {
    CLOGE("read failed with: %d", ret);
    abort();
  }
  return samples;
}

bool AudioDeviceEs8311::OpenOutput(uint32_t sample_rate) {
  return true;
}

void AudioDeviceEs8311::CloseOutput() {
}

size_t AudioDeviceEs8311::Write(const int16_t* pcm, size_t samples) {
  const auto ret = esp_codec_dev_write(audio_device_, const_cast<int16_t*>(pcm), sizeof(int16_t) * samples);
  if (ret != ESP_CODEC_DEV_OK) {
    CLOGE("read failed with: %d", ret);
    abort();
  }
  return samples;
}

void AudioDeviceEs8311::set_volume(uint16_t volume) {
  volume_ = volume;
  ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(audio_device_, volume_));
}
uint16_t AudioDeviceEs8311::volume() const {
  return volume_;
}
}  // namespace ai_vox