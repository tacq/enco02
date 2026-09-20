#pragma once

#ifndef _AUDIO_TASK_STACK_H_
#define _AUDIO_TASK_STACK_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// The capture (AudioInput) and playback (AudioOutput) engines are mutually exclusive: the engine
// state machine always destroys one before constructing the other. Giving each of them its own
// statically reserved stack therefore permanently wasted 12KB of the 320KB the ESP32 has - RAM that
// the mbedTLS handshake desperately needs on a board without PSRAM.
//
// This module hands out a single shared, statically reserved stack. Static reservation is still
// required (rather than malloc-per-task) because a 24KB contiguous request fails once the heap is
// fragmented, and xTaskCreateStatic() aborts the firmware when handed a null stack.
namespace audio_task_stack {

// Measured on the device via uxTaskGetStackHighWaterMark(): AudioInput peaks at exactly 19,172
// bytes (opus_encode is the hog) and AudioOutput at 8,260. 23KB leaves the capture path ~4.4KB of
// headroom - a stack overflow here is an instant panic, so this keeps more margin than the 22KB the
// measurement alone would justify, and still hands 1KB back versus the original 24KB guess.
constexpr uint32_t kStackSize = 23 * 1024;  // bytes (StackType_t is uint8_t on ESP-IDF)

// Returns the shared stack, or nullptr if it is already checked out (should not happen; callers
// fall back to a heap allocated stack in that case).
StackType_t* Acquire();

// Must only be called after the owning task has been deleted.
void Release(StackType_t* stack);

}  // namespace audio_task_stack

#endif
