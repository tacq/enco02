#include "silk_resampler.h"

#include "libopus/opus.h"
#include "libopus/resampler_structs.h"

#ifndef CLOGGER_SEVERITY
#define CLOGGER_SEVERITY CLOGGER_SEVERITY_WARN
#endif
#include "clogger/clogger.h"

extern "C" opus_int silk_resampler_init(silk_resampler_state_struct *S, /* I/O  Resampler state                                             */
                                        opus_int32 Fs_Hz_in,            /* I    Input sampling rate (Hz)                                    */
                                        opus_int32 Fs_Hz_out,           /* I    Output sampling rate (Hz)                                   */
                                        opus_int forEnc                 /* I    If 1: encoder; if 0: decoder                                */
);

extern "C" opus_int silk_resampler(silk_resampler_state_struct *S, /* I/O  Resampler state                                             */
                                   opus_int16 out[],               /* O    Output signal                                               */
                                   const opus_int16 in[],          /* I    Input signal                                                */
                                   opus_int32 inLen                /* I    Number of input samples                                     */
);

SilkResampler::SilkResampler(const uint32_t input_sample_rate, const uint32_t output_sample_rate)
    : input_sample_rate_(input_sample_rate), output_sample_rate_(output_sample_rate), silk_resampler_(new silk_resampler_state_struct) {
  const auto ret = silk_resampler_init(reinterpret_cast<silk_resampler_state_struct *>(silk_resampler_),
                                       input_sample_rate,
                                       output_sample_rate,
                                       input_sample_rate > output_sample_rate ? 1 : 0);

  if (ret != 0) {
    CLOGE("silk_resampler_init failed with: %d", ret);
    abort();
  }
}

SilkResampler::~SilkResampler() {
  delete reinterpret_cast<silk_resampler_state_struct *>(silk_resampler_);
}

FlexArray<int16_t> SilkResampler::Resample(FlexArray<int16_t> &&input_pcm) const {
  FlexArray<int16_t> output_pcm(input_pcm.size() * output_sample_rate_ / input_sample_rate_);
  const auto ret =
      silk_resampler(reinterpret_cast<silk_resampler_state_struct *>(silk_resampler_), output_pcm.data(), input_pcm.data(), input_pcm.size());
  if (ret != 0) {
    CLOGE("silk_resampler_process failed with: %d", ret);
    abort();
  }
  return output_pcm;
}