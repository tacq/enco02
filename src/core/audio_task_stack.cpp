#include "audio_task_stack.h"

#include <stdio.h>

namespace audio_task_stack {
namespace {
alignas(16) StackType_t g_stack[kStackSize];
bool g_in_use = false;
}  // namespace

StackType_t* Acquire() {
  if (g_in_use) {
    // The engine state machine guarantees the capture and playback engines never overlap, so this
    // should be unreachable. Report it and let the caller fall back to a heap stack.
    printf("[audio_stack] shared stack already in use\n");
    return nullptr;
  }
  g_in_use = true;
  return g_stack;
}

void Release(StackType_t* stack) {
  if (stack == g_stack) {
    g_in_use = false;
  }
}

}  // namespace audio_task_stack
