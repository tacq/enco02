#pragma once

#ifndef _PASSIVE_TASK_QUEUE_H_
#define _PASSIVE_TASK_QUEUE_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

class PassiveTaskQueue {
 public:
  PassiveTaskQueue() = default;
  ~PassiveTaskQueue() = default;

  template <class F, class... Args>
  void Enqueue(F&& f, Args&&... args) {
    auto func = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::forward<Args>(args)...); };
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.emplace(Task{id_++, std::chrono::steady_clock::now(), std::make_unique<TaskImpl<decltype(func)>>(std::move(func))});
    }
  }

  template <class F, class... Args>
  void EnqueueAt(std::chrono::time_point<std::chrono::steady_clock> time_point, F&& f, Args&&... args) {
    auto func = [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable { f(std::forward<Args>(args)...); };
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.emplace(Task{id_++, std::move(time_point), std::make_unique<TaskImpl<decltype(func)>>(std::move(func))});
    }
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
  }

  void Process() {
    std::unique_ptr<TaskInterface> task;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (tasks_.empty()) {
        return;
      }
      task = std::move(const_cast<Task&>(tasks_.top()).task);
      tasks_.pop();
    }
    task->Invoke();
  }

 private:
  PassiveTaskQueue(const PassiveTaskQueue&) = delete;
  PassiveTaskQueue& operator=(const PassiveTaskQueue&) = delete;

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
    uint64_t id;
    std::chrono::time_point<std::chrono::steady_clock> scheduled_time;
    std::unique_ptr<TaskInterface> task;

    bool operator>(const Task& other) const {
      return scheduled_time == other.scheduled_time ? id > other.id : scheduled_time > other.scheduled_time;
    }
  };

  mutable std::mutex mutex_;
  std::priority_queue<Task, std::vector<Task>, std::greater<>> tasks_;
  uint64_t id_ = 0;
};

#endif