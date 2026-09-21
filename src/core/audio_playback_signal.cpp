#include "audio_playback_signal.h"

#include <esp_timer.h>

namespace audio_playback_signal {
namespace {
inline uint32_t NowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}
}  // namespace

std::atomic<uint32_t> g_last_pcm_write_ms{0};

void NotifyPcmWritten() {
  // Relaxed is enough: the only consumer is a "how long ago" comparison, and a tick of skew either
  // way is invisible. Anything stronger would put a barrier in the audio hot path.
  g_last_pcm_write_ms.store(NowMs(), std::memory_order_relaxed);
}

bool IsPlaying(const uint32_t grace_ms) {
  const uint32_t last = g_last_pcm_write_ms.load(std::memory_order_relaxed);
  if (last == 0) {
    return false;
  }
  // Unsigned arithmetic, so this stays correct across the 49-day wrap of the millisecond counter.
  return (NowMs() - last) < grace_ms;
}

}  // namespace audio_playback_signal
