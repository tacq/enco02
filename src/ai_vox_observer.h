#pragma once

#ifndef _AI_VOX_OBSERVER_H_
#define _AI_VOX_OBSERVER_H_

#include <deque>
#include <mutex>
#include <string>
#include <variant>

#include "ai_vox_types.h"

namespace ai_vox {

using Event = std::variant<TextReceivedEvent, TextTranslatedEvent, StateChangedEvent, ActivationEvent, ChatMessageEvent, EmotionEvent, McpToolCallEvent>;

class Observer {
 public:
  static constexpr size_t kMaxQueueSize = 10;

  Observer() = default;
  virtual ~Observer() = default;

  virtual std::deque<Event> PopEvents() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(event_queue_);
  }

  virtual void PushEvent(Event&& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event_queue_.size() >= kMaxQueueSize) {
      event_queue_.pop_front();
    }
    event_queue_.emplace_back(std::move(event));
  }

 private:
  Observer(const Observer&) = delete;
  Observer& operator=(const Observer&) = delete;

  mutable std::mutex mutex_;
  std::deque<Event> event_queue_;
};

}  // namespace ai_vox

#endif