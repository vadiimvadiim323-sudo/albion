#pragma once
// FEATURE-V11-DIAG: Bounded queue for background log/performance/NVML work.
// Stop joins the owner; no detached callback can outlive its App/resources.
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "worker_guard.hpp"
namespace albion {
class TaskQueue {
    std::mutex mutex_; std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
    bool stop_ = false; std::thread worker_;
public:
    TaskQueue() : worker_([this] {
        // Also guard queue infrastructure, not just the individual task body.
        GuardWorker([this] { Loop(); }, [this](const char*) {
            std::lock_guard<std::mutex> lock(mutex_); stop_ = true; tasks_.clear();
        }, [] {});
    }) {}
    ~TaskQueue() { Stop(); }
    TaskQueue(const TaskQueue&) = delete; TaskQueue& operator=(const TaskQueue&) = delete;
    bool Post(std::function<void()> task) {
        { std::lock_guard<std::mutex> lock(mutex_);
          if (stop_ || tasks_.size() >= 64) return false;
          tasks_.push_back(std::move(task)); }
        ready_.notify_one(); return true;
    }
    void Stop(bool drain = false) noexcept {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; if (!drain) tasks_.clear(); }
        ready_.notify_all();
        // FIX-V11.1-QUEUE: a task may request shutdown of its own queue; joining
        // the current thread would throw inside noexcept and terminate.
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    }
private:
    void Loop() {
        for (;;) {
            std::function<void()> task;
            { std::unique_lock<std::mutex> lock(mutex_);
              ready_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
              if (stop_ && tasks_.empty()) return;
              task = std::move(tasks_.front()); tasks_.pop_front(); }
            GuardWorker(task, [](const char*) {}, [] {});
        }
    }
};
} // namespace albion
