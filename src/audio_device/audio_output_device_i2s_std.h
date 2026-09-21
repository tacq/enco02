#pragma once

#ifndef _I2S_STD_AUDIO_OUTPUT_DEVICE_H_
#define _I2S_STD_AUDIO_OUTPUT_DEVICE_H_

#include <driver/i2s_std.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "audio_output_device.h"
#include "core/audio_playback_signal.h"

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
    if (pcm == nullptr || samples == 0 || i2s_tx_handle_ == nullptr) {
      return 0;
    }

    // Everything audible goes through here - TTS replies and the boot / notification clips alike -
    // which makes it the one honest answer to "is the speaker making a sound right now". The UI
    // reads this to drive the character's mouth; see core/audio_playback_signal.h.
    audio_playback_signal::NotifyPcmWritten();

    // This runs ~17-50 times a second for the whole duration of every reply. The previous
    // implementation allocated a std::vector<int32_t> of `samples` entries per call (5.7KB for a
    // 60ms 24kHz frame); once the heap ran low, operator new threw and -fno-exceptions turned that
    // into std::terminate() -> abort() -> reboot, right in the middle of the robot talking.
    //
    // Converting in fixed-size chunks on the stack removes the allocation entirely, so playback is
    // now immune to heap pressure.
    constexpr size_t kChunkSamples = 256;  // 1KB of stack
    int32_t chunk[kChunkSamples];

    const int32_t volume_factor = volume_factor_;
    size_t written_samples = 0;
    while (written_samples < samples) {
      const size_t count = std::min(kChunkSamples, samples - written_samples);
      for (size_t i = 0; i < count; i++) {
        const int64_t temp = static_cast<int64_t>(pcm[written_samples + i]) * volume_factor;
        if (temp > INT32_MAX) {
          chunk[i] = INT32_MAX;
        } else if (temp < INT32_MIN) {
          chunk[i] = INT32_MIN;
        } else {
          chunk[i] = static_cast<int32_t>(temp);
        }
      }

      size_t bytes_written = 0;
      const auto err = i2s_channel_write(i2s_tx_handle_, chunk, count * sizeof(int32_t), &bytes_written, 1000);
      if (err != ESP_OK) {
        // Don't ESP_ERROR_CHECK here: a timeout while the speaker is busy must not reboot the
        // device.
        break;
      }
      written_samples += count;
    }

    return written_samples;
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
