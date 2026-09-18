#pragma once

#ifndef _SILK_RESAMPLER_H_
#define _SILK_RESAMPLER_H_

#include <cstdint>

#include "flex_array/flex_array.h"

class SilkResampler {
 public:
  SilkResampler(const uint32_t input_sample_rate, const uint32_t output_sample_rate);
  ~SilkResampler();
  inline uint32_t input_sample_rate() const {
    return input_sample_rate_;
  }

  inline uint32_t output_sample_rate() const {
    return output_sample_rate_;
  }

  FlexArray<int16_t> Resample(FlexArray<int16_t> &&input_pcm) const;

 private:
  const uint32_t input_sample_rate_ = 0;
  const uint32_t output_sample_rate_ = 0;
  void *const silk_resampler_ = nullptr;
};

#endif