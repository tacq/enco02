#pragma once

#ifndef _TASK_QUEUE_H_
#define _TASK_QUEUE_H_

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <utility>

#define TASK_QUEUE_DEBUG (0)

class ActiveTaskQueue {
 public:
  // NOTE: On the ESP-IDF Xtensa port `StackType_t` is `uint8_t`, so `xTaskCreateStatic()`'s
  // `ulStackDepth` and the stack buffer size are both expressed in BYTES (unlike vanilla FreeRTOS,
  // which counts words).
  //
  // `external_stack` lets the caller supply a statically allocated buffer. Tasks that are created
  // and destroyed repeatedly at runtime (the audio engines) must use this: requesting a 20KB+
  // contiguous block from a fragmented heap eventually fails, and xTaskCreateStatic() then aborts
  // inside xPortcheckValidStackMem().
  ActiveTaskQueue(const std::string& name,
                  const uint32_t stack_depth,
                  UBaseType_t priority,
                  const bool internal_memory = false,
                  StackType_t* external_stack = nullptr)
      :
#if TASK_QUEUE_DEBUG
        name_(name),
#endif
        owns_stack_(external_stack == nullptr),
        stack_buffer_(external_stack != nullptr ? external_stack : AllocateStack(stack_depth, internal_memory)),
        task_handle_(stack_buffer_ == nullptr
                         ? nullptr
                         : xTaskCreateStatic(&Loop, name.c_str(), stack_depth, this, priority, stack_buffer_, &task_buffer_)) {
    if (stack_buffer_ == nullptr || task_handle_ == nullptr) {
      printf("FATAL: task '%s' stack allocation failed (%u bytes). free heap: %u, largest block: %u\n",
             name.c_str(),
             static_cast<unsigned>(stack_depth),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
      abort();
    }
  }

  ~ActiveTaskQueue() {
    const auto termination_sem = xSemaphoreCreateBinary();
    Enqueue([termination_sem]() {
      xSemaphoreGive(termination_sem);
      vTaskDelay(portMAX_DELAY);
    });
    xSemaphoreTake(termination_sem, portMAX_DELAY);
    vSemaphoreDelete(termination_sem);
    // Task stacks are the biggest fixed RAM cost left on this no-PSRAM board, so always report the
    // high-water mark: "unused" is how much could safely be reclaimed from this task's stack.
    printf("task '%s' stack unused: %u bytes\n",
           pcTaskGetName(task_handle_),
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(task_handle_)));
    vTaskDelete(task_handle_);
    if (owns_stack_) {
      heap_caps_free(stack_buffer_);
    }
  }

  template <class F, class... Args>
  void Enqueue(F&& f, Args&&... args) {
    auto func = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::forward<Args>(args)...); };
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.emplace_back(Task{order_++, std::chrono::steady_clock::now(), std::make_unique<TaskImpl<decltype(func)>>(std::move(func)), std::nullopt});
      std::make_heap(tasks_.begin(), tasks_.end(), std::greater<>{});
    }
    condition_.notify_one();
  }

  template <class F, class... Args>
  void EnqueueAt(std::chrono::time_point<std::chrono::steady_clock> time_point, F&& f, Args&&... args) {
    auto func = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::forward<Args>(args)...); };
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.emplace_back(Task{order_++, std::move(time_point), std::make_unique<TaskImpl<decltype(func)>>(std::move(func)), std::nullopt});
      std::make_heap(tasks_.begin(), tasks_.end(), std::greater<>{});
    }
    condition_.notify_one();
  }

  template <class F, class... Args>
  void Enqueue(const uint64_t id, F&& f, Args&&... args) {
    auto func = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::forward<Args>(args)...); };
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.emplace_back(Task{order_++, std::chrono::steady_clock::now(), std::make_unique<TaskImpl<decltype(func)>>(std::move(func)), id});
      std::make_heap(tasks_.begin(), tasks_.end(), std::greater<>{});
    }
    condition_.notify_one();
  }

  template <class F, class... Args>
  void EnqueueAt(const uint64_t id, std::chrono::time_point<std::chrono::steady_clock> time_point, F&& f, Args&&... args) {
    auto func = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::forward<Args>(args)...); };
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.emplace_back(Task{order_++, std::move(time_point), std::make_unique<TaskImpl<decltype(func)>>(std::move(func)), id});
      std::make_heap(tasks_.begin(), tasks_.end(), std::greater<>{});
    }
    condition_.notify_one();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
  }

  void Erase(const uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto new_end = std::remove_if(tasks_.begin(), tasks_.end(), [id](const Task& task) { return task.id.has_value() && *task.id == id; });
    tasks_.erase(new_end, tasks_.end());
    condition_.notify_one();
  }

 private:
  ActiveTaskQueue(const ActiveTaskQueue&) = delete;
  ActiveTaskQueue& operator=(const ActiveTaskQueue&) = delete;

  // Task stacks must live in byte-accessible internal RAM. Try the requested pool first, then fall
  // back to an explicit internal-RAM request before giving up.
  static StackType_t* AllocateStack(const uint32_t stack_depth, const bool internal_memory) {
    auto* buffer = static_cast<StackType_t*>(internal_memory
                                                 ? heap_caps_malloc(stack_depth, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)
                                                 : heap_caps_malloc(stack_depth, MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
    if (buffer == nullptr) {
      buffer = static_cast<StackType_t*>(heap_caps_malloc(stack_depth, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    }
    return buffer;
  }

  struct TaskInterface {
    virtual void Invoke() = 0;
    virtual ~TaskInterface() = default;
  };

  template <typename Callable>
  struct TaskImpl : TaskInterface {
    Callable callable;
    explicit TaskImpl(Callable&& c) : callable(std::move(c)) {
    }
    void Invoke() override {
      callable();
    }
  };

  struct Task {
    uint64_t order;
    std::chrono::time_point<std::chrono::steady_clock> scheduled_time;
    std::unique_ptr<TaskInterface> task;
    std::optional<uint64_t> id;

    bool operator>(const Task& other) const {
      return scheduled_time == other.scheduled_time ? order > other.order : scheduled_time > other.scheduled_time;
    }
  };

  static void Loop(void* self) {
    reinterpret_cast<ActiveTaskQueue*>(self)->Loop();
  }

  void Loop() {
    while (true) {
      std::unique_ptr<TaskInterface> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return !tasks_.empty(); });

        auto scheduled_time = tasks_.front().scheduled_time;

        condition_.wait_until(lock, scheduled_time, [this, &scheduled_time] {
          return tasks_.empty() ||
                 (std::chrono::steady_clock::now() >= tasks_.front().scheduled_time ? true : (scheduled_time = tasks_.front().scheduled_time, false));
        });

        if (tasks_.empty()) {
          continue;
        }

        task = std::move(const_cast<Task&>(tasks_.front()).task);
        tasks_.pop_front();
      }
      task->Invoke();
    }
  }
#if TASK_QUEUE_DEBUG
  const std::string name_;
#endif
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  // std::priority_queue<Task, std::vector<Task>, std::greater<>> tasks_;
  std::deque<Task> tasks_;
  const bool owns_stack_ = true;
  StackType_t* stack_buffer_ = nullptr;
  StaticTask_t task_buffer_;
  TaskHandle_t task_handle_ = nullptr;
  uint64_t order_ = 0;
};

#endif