#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "sonic/threads/thread_pool.hpp"

namespace sn::threads {

class stop_token {
public:
  stop_token() = default;
  [[nodiscard]] bool stop_requested() const noexcept {
    return flag_ != nullptr && flag_->load(std::memory_order_acquire);
  }

private:
  friend class task_group;
  explicit stop_token(std::atomic<bool>* flag) : flag_(flag) {}
  std::atomic<bool>* flag_{nullptr};
};

class task_group {
public:
  task_group() = default;
  explicit task_group(thread_pool* pool) noexcept : pool_(pool) {}
  ~task_group() {
    request_stop();
    join();
  }

  void bind(thread_pool* pool) noexcept { pool_ = pool; }

  [[nodiscard]] bool running() const noexcept { return running_; }

  void request_stop() noexcept { stop_requested_.store(true, std::memory_order_release); }

  void join() noexcept {
    if (!running_) {
      tasks_.clear();
      return;
    }
    for (auto& entry : tasks_) {
      if (entry) {
        entry->task.wait();
      }
    }
    tasks_.clear();
    running_ = false;
  }

  template <typename Fn> bool start(std::size_t task_count, Fn&& fn) {
    if (pool_ == nullptr || running_) {
      return false;
    }
    if (task_count == 0) {
      task_count = 1;
    }
    stop_requested_.store(false, std::memory_order_release);
    tasks_.clear();
    tasks_.reserve(task_count);
    using callable_type = std::decay_t<Fn>;
    auto callable = std::make_shared<callable_type>(std::forward<Fn>(fn));
    try {
      for (std::size_t idx = 0; idx < task_count; ++idx) {
        auto entry = std::make_unique<task_entry>();
        entry->task.assign([this, callable, idx]() noexcept {
          stop_token token(&stop_requested_);
          try {
            (*callable)(idx, token);
          } catch (...) {
            stop_requested_.store(true, std::memory_order_release);
          }
        });
        pool_->schedule(entry->task);
        tasks_.push_back(std::move(entry));
      }
      running_ = true;
      return true;
    } catch (...) {
      request_stop();
      for (auto& entry : tasks_) {
        if (entry) {
          entry->task.wait();
        }
      }
      tasks_.clear();
      running_ = false;
      throw;
    }
  }

private:
  struct task_entry {
    thread_pool::task task;
  };

  thread_pool* pool_{nullptr};
  std::vector<std::unique_ptr<task_entry>> tasks_;
  std::atomic<bool> stop_requested_{false};
  bool running_{false};
};

class blocking_executor {
public:
  explicit blocking_executor(thread_pool& pool) noexcept : pool_(pool) {}

  template <typename Fn> void run(Fn&& fn) {
    using callable_type = std::decay_t<Fn>;
    callable_type callable(std::forward<Fn>(fn));
    thread_pool::task task;
    task.assign([callable = std::move(callable)]() mutable noexcept {
      try {
        callable();
      } catch (...) {
      }
    });
    pool_.schedule(task);
    task.wait();
  }

private:
  thread_pool& pool_;
};

class managed_task_group {
public:
  managed_task_group() = default;
  managed_task_group(const managed_task_group&) = delete;
  managed_task_group& operator=(const managed_task_group&) = delete;
  managed_task_group(managed_task_group&&) = delete;
  managed_task_group& operator=(managed_task_group&&) = delete;

  template <typename Fn> bool start(thread_pool* pool, std::atomic<bool>& stop_flag, std::size_t task_count, Fn&& fn) {
    if (group_.running()) {
      return false;
    }
    if (pool == nullptr) {
      return false;
    }
    group_.bind(pool);
    stop_flag_ = &stop_flag;
    stop_flag_->store(false, std::memory_order_release);
    if (!group_.start(task_count, std::forward<Fn>(fn))) {
      stop_flag_ = nullptr;
      return false;
    }
    return true;
  }

  template <typename Fn> bool start(thread_pool* pool, std::atomic<bool>& stop_flag, Fn&& fn) {
    return start(pool, stop_flag, 1, std::forward<Fn>(fn));
  }

  [[nodiscard]] bool running() const noexcept { return group_.running(); }

  void request_stop() noexcept {
    if (stop_flag_ != nullptr) {
      stop_flag_->store(true, std::memory_order_release);
    }
    group_.request_stop();
  }

  void stop() {
    stop([]() noexcept {});
  }

  template <typename Fn> void stop(Fn&& before_join) { stop_impl(std::forward<Fn>(before_join)); }

private:
  template <typename Fn> void stop_impl(Fn&& before_join) {
    if (stop_flag_ != nullptr) {
      stop_flag_->store(true, std::memory_order_release);
    }
    before_join();
    group_.request_stop();
    group_.join();
    stop_flag_ = nullptr;
  }

  task_group group_{};
  std::atomic<bool>* stop_flag_{nullptr};
};

}
