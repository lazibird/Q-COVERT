#pragma once

#include "sonic/sgxbridge/common/time.hpp"
#include "sonic/threads/sync.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(SN_SGX_ENCLAVE)
#include <sgx_thread.h>
#else
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace sn::sgxbridge::sync {

enum class wait_result { signaled = 0, timeout = 1 };

#if defined(SN_SGX_ENCLAVE)

class mutex {
public:
  mutex() noexcept { sgx_thread_mutex_init(&native_, nullptr); }
  ~mutex() { sgx_thread_mutex_destroy(&native_); }

  mutex(const mutex&) = delete;
  mutex& operator=(const mutex&) = delete;

  void lock() noexcept { sgx_thread_mutex_lock(&native_); }
  bool try_lock() noexcept { return sgx_thread_mutex_trylock(&native_) == 0; }
  void unlock() noexcept { sgx_thread_mutex_unlock(&native_); }

  sgx_thread_mutex_t* native_handle() noexcept { return &native_; }
  const sgx_thread_mutex_t* native_handle() const noexcept { return &native_; }

private:
  sgx_thread_mutex_t native_{};
};

class recursive_mutex {
public:
  recursive_mutex() noexcept { sgx_thread_mutex_init(&native_, nullptr); }
  ~recursive_mutex() { sgx_thread_mutex_destroy(&native_); }

  recursive_mutex(const recursive_mutex&) = delete;
  recursive_mutex& operator=(const recursive_mutex&) = delete;

  void lock() noexcept {
    auto self = sgx_thread_self();
    if (owner_ == self) {
      ++recursion_;
      return;
    }
    sgx_thread_mutex_lock(&native_);
    owner_ = self;
    recursion_ = 1;
  }

  bool try_lock() noexcept {
    auto self = sgx_thread_self();
    if (owner_ == self) {
      ++recursion_;
      return true;
    }
    if (sgx_thread_mutex_trylock(&native_) != 0) {
      return false;
    }
    owner_ = self;
    recursion_ = 1;
    return true;
  }

  void unlock() noexcept {
    if (owner_ != sgx_thread_self()) {
      return;
    }
    if (--recursion_ == 0) {
      owner_ = SGX_THREAD_T_NULL;
      sgx_thread_mutex_unlock(&native_);
    }
  }

  sgx_thread_mutex_t* native_handle() noexcept { return &native_; }

private:
  sgx_thread_mutex_t native_{};
  sgx_thread_t owner_{SGX_THREAD_T_NULL};
  std::uint32_t recursion_{0};
};

template <typename Mutex> class lock_guard {
public:
  explicit lock_guard(Mutex& mutex) noexcept : mutex_(mutex) { mutex_.lock(); }
  ~lock_guard() { mutex_.unlock(); }

  lock_guard(const lock_guard&) = delete;
  lock_guard& operator=(const lock_guard&) = delete;

private:
  Mutex& mutex_;
};

template <typename Mutex> class unique_lock {
public:
  unique_lock() = default;
  explicit unique_lock(Mutex& mutex) : mutex_(&mutex) {
    mutex_->lock();
    owns_ = true;
  }
  ~unique_lock() { unlock(); }

  unique_lock(const unique_lock&) = delete;
  unique_lock& operator=(const unique_lock&) = delete;

  void lock() {
    if (mutex_ != nullptr && !owns_) {
      mutex_->lock();
      owns_ = true;
    }
  }

  void unlock() {
    if (mutex_ != nullptr && owns_) {
      mutex_->unlock();
      owns_ = false;
    }
  }

  [[nodiscard]] bool owns_lock() const noexcept { return owns_; }
  [[nodiscard]] Mutex* mutex() const noexcept { return mutex_; }
  [[nodiscard]] sgx_thread_mutex_t* native_handle() noexcept {
    return mutex_ != nullptr ? mutex_->native_handle() : nullptr;
  }

private:
  Mutex* mutex_{nullptr};
  bool owns_{false};
};

class condition_variable {
public:
  condition_variable() noexcept { sgx_thread_cond_init(&native_, nullptr); }
  ~condition_variable() { sgx_thread_cond_destroy(&native_); }

  condition_variable(const condition_variable&) = delete;
  condition_variable& operator=(const condition_variable&) = delete;

  template <typename Mutex> void wait(unique_lock<Mutex>& lock) {
    if (!lock.mutex()) {
      return;
    }
    if (!lock.owns_lock()) {
      lock.lock();
    }
    sgx_thread_cond_wait(&native_, lock.native_handle());
  }

  template <typename Mutex, typename Predicate> void wait(unique_lock<Mutex>& lock, Predicate pred) {
    while (!pred()) {
      wait(lock);
    }
  }

  template <typename Mutex> wait_result wait_for(unique_lock<Mutex>& lock, std::uint32_t) {
    wait(lock);
    return wait_result::signaled;
  }

  template <typename Mutex, typename Predicate>
  bool wait_for(unique_lock<Mutex>& lock, std::uint32_t, Predicate pred) {
    wait(lock, pred);
    return true;
  }

  void notify_one() noexcept { sgx_thread_cond_signal(&native_); }
  void notify_all() noexcept { sgx_thread_cond_broadcast(&native_); }

  template <typename Mutex> wait_result wait_until(unique_lock<Mutex>& lock, const time::deadline&) {
    wait(lock);
    return wait_result::signaled;
  }

  template <typename Mutex, typename Predicate>
  bool wait_until(unique_lock<Mutex>& lock, const time::deadline& deadline, Predicate pred) {
    while (!pred()) {
      if (wait_until(lock, deadline) == wait_result::timeout) {
        return pred();
      }
    }
    return true;
  }

private:
  sgx_thread_cond_t native_{};
};

#else

using mutex = std::mutex;
using recursive_mutex = std::recursive_mutex;
template <typename Mutex> using lock_guard = std::lock_guard<Mutex>;
template <typename Mutex> using unique_lock = std::unique_lock<Mutex>;

class condition_variable {
public:
  condition_variable() = default;
  ~condition_variable() = default;

  condition_variable(const condition_variable&) = delete;
  condition_variable& operator=(const condition_variable&) = delete;

  template <typename MutexType> void wait(unique_lock<MutexType>& lock) { native_.wait(lock); }

  template <typename MutexType, typename Predicate> void wait(unique_lock<MutexType>& lock, Predicate pred) {
    native_.wait(lock, pred);
  }

  template <typename MutexType> wait_result wait_for(unique_lock<MutexType>& lock, std::uint32_t timeout_ms) {
    if (timeout_ms == time::infinite_timeout) {
      native_.wait(lock);
      return wait_result::signaled;
    }
    const auto status = native_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    return status == std::cv_status::timeout ? wait_result::timeout : wait_result::signaled;
  }

  template <typename MutexType, typename Predicate>
  bool wait_for(unique_lock<MutexType>& lock, std::uint32_t timeout_ms, Predicate pred) {
    if (timeout_ms == time::infinite_timeout) {
      native_.wait(lock, pred);
      return true;
    }
    auto deadline = time::deadline::from_timeout_ms(timeout_ms);
    return wait_until(lock, deadline, pred);
  }

  template <typename MutexType> wait_result wait_until(unique_lock<MutexType>& lock, const time::deadline& deadline) {
    if (deadline.infinite) {
      native_.wait(lock);
      return wait_result::signaled;
    }
    while (true) {
      if (deadline.expired()) {
        return wait_result::timeout;
      }
      const auto remaining = deadline.remaining_ms();
      if (remaining == 0) {
        return wait_result::timeout;
      }
      const auto status = native_.wait_for(lock, std::chrono::milliseconds(remaining));
      if (status == std::cv_status::timeout) {
        if (deadline.expired()) {
          return wait_result::timeout;
        }
        continue;
      }
      return wait_result::signaled;
    }
  }

  template <typename MutexType, typename Predicate>
  bool wait_until(unique_lock<MutexType>& lock, const time::deadline& deadline, Predicate pred) {
    while (!pred()) {
      if (wait_until(lock, deadline) == wait_result::timeout) {
        return pred();
      }
    }
    return true;
  }

  void notify_one() noexcept { native_.notify_one(); }
  void notify_all() noexcept { native_.notify_all(); }

private:
  std::condition_variable native_;
};

#endif

class event {
public:
  explicit event(bool signaled = false) noexcept : signaled_(signaled) {}

  void set() {
    lock_guard<mutex> guard(mutex_);
    signaled_ = true;
    cond_.notify_all();
  }

  void reset() {
    lock_guard<mutex> guard(mutex_);
    signaled_ = false;
  }

  wait_result wait(std::uint32_t timeout_ms = time::infinite_timeout) {
    unique_lock<mutex> lock(mutex_);
    if (timeout_ms == time::infinite_timeout) {
      cond_.wait(lock, [&] { return signaled_; });
      return wait_result::signaled;
    }
    auto deadline = time::deadline::from_timeout_ms(timeout_ms);
    if (!cond_.wait_until(lock, deadline, [&] { return signaled_; })) {
      return wait_result::timeout;
    }
    return wait_result::signaled;
  }

private:
  mutex mutex_{};
  condition_variable cond_{};
  bool signaled_{false};
};

class semaphore {
public:
  explicit semaphore(std::size_t initial = 0) noexcept : count_(initial) {}

  void release(std::size_t delta = 1) {
    lock_guard<mutex> guard(mutex_);
    count_ += delta;
    cond_.notify_all();
  }

  void acquire() {
    unique_lock<mutex> lock(mutex_);
    cond_.wait(lock, [&] { return count_ > 0; });
    --count_;
  }

  bool try_acquire() {
    lock_guard<mutex> guard(mutex_);
    if (count_ == 0) {
      return false;
    }
    --count_;
    return true;
  }

  wait_result try_acquire_for(std::uint32_t timeout_ms) {
    unique_lock<mutex> lock(mutex_);
    auto deadline = time::deadline::from_timeout_ms(timeout_ms);
    if (!cond_.wait_until(lock, deadline, [&] { return count_ > 0; })) {
      return wait_result::timeout;
    }
    --count_;
    return wait_result::signaled;
  }

private:
  mutex mutex_{};
  condition_variable cond_{};
  std::size_t count_{0};
};

class latch {
public:
  explicit latch(std::size_t count) noexcept : remaining_(count) {}

  void count_down(std::size_t delta = 1) {
    lock_guard<mutex> guard(mutex_);
    if (remaining_ <= delta) {
      remaining_ = 0;
      cond_.notify_all();
      return;
    }
    remaining_ -= delta;
    if (remaining_ == 0) {
      cond_.notify_all();
    }
  }

  void wait() {
    unique_lock<mutex> lock(mutex_);
    cond_.wait(lock, [&] { return remaining_ == 0; });
  }

private:
  mutex mutex_{};
  condition_variable cond_{};
  std::size_t remaining_{0};
};

#if defined(SN_SGX_ENCLAVE)
class thread {
public:
  thread() = delete;
  template <typename... Args> explicit thread(Args&&...) {
    static_assert(sizeof...(Args) == 0, "sync::thread cannot be constructed inside enclaves");
  }
};
#else
using thread = std::thread;
#endif

class external_event {
public:
  external_event() noexcept = default;

  void signal() noexcept { flag_.store(true, std::memory_order_release); }

  void reset() noexcept { flag_.store(false, std::memory_order_release); }

  wait_result wait(std::uint32_t timeout_ms = time::infinite_timeout) noexcept {
    if (flag_.load(std::memory_order_acquire)) {
      return wait_result::signaled;
    }
    if (timeout_ms == 0) {
      return wait_result::timeout;
    }
    if (timeout_ms == time::infinite_timeout) {
      while (!flag_.load(std::memory_order_acquire)) {
        sn::threads::cpu_relax();
      }
      return wait_result::signaled;
    }
    auto deadline = time::deadline::from_timeout_ms(timeout_ms);
    while (!flag_.load(std::memory_order_acquire)) {
      if (deadline.expired()) {
        return wait_result::timeout;
      }
      sn::threads::cpu_relax();
    }
    return wait_result::signaled;
  }

private:
  std::atomic<bool> flag_{false};
};

class dispatcher {
public:
  dispatcher() = default;

  external_event make_event() { return external_event{}; }
};

}
