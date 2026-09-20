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

  // Allocation-free variant. Resampling happens on every audio frame, so the callers on this
  // no-PSRAM board hold persistent buffers and use this instead of the FlexArray overload above.
  // Returns the number of samples written to `output_pcm`, or 0 on failure.
  size_t Resample(const int16_t *input_pcm, size_t input_samples, int16_t *output_pcm, size_t output_capacity) const;

  // Number of output samples `input_samples` will produce.
  inline size_t OutputSamplesFor(size_t input_samples) const {
    return input_samples * output_sample_rate_ / input_sample_rate_;
  }

 private:
  const uint32_t input_sample_rate_ = 0;
  const uint32_t output_sample_rate_ = 0;
  void *const silk_resampler_ = nullptr;
};

#endif