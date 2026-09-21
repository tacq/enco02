#pragma once

#ifndef _AUDIO_PLAYBACK_SIGNAL_H_
#define _AUDIO_PLAYBACK_SIGNAL_H_

#include <atomic>
#include <cstdint>

// Tells the UI when the speaker is *actually* producing sound.
//
// The chat state machine enters kSpeaking the moment the server's {"type":"tts","state":"start"}
// frame arrives, which is a network round trip, an Opus decode and a playback queue ahead of the
// first sample reaching the amplifier. Driving the character's mouth from that state made her lips
// move well before any audio, and freeze mid-reply whenever some other ShowStatus() call (a servo
// command, say) overwrote the status string.
//
// The audio output device stamps this beacon as it hands PCM to I2S, so the mouth can follow the
// amplifier instead of the protocol. It is deliberately a single relaxed atomic: the writer is the
// audio task in its hot path and the reader is the LVGL timer, so it must never block either.
namespace audio_playback_signal {

// esp_timer milliseconds of the most recent PCM write. 0 means nothing has been played yet.
extern std::atomic<uint32_t> g_last_pcm_write_ms;

// Called from the audio output device for every buffer handed to the speaker.
void NotifyPcmWritten();

// True while PCM reached the speaker within the last `grace_ms`. The grace window has to be wider
// than one audio frame (20-60ms) so ordinary frame boundaries do not make the mouth stutter, but
// short enough that the mouth closes promptly at the end of a reply.
bool IsPlaying(uint32_t grace_ms);

}  // namespace audio_playback_signal

#endif
