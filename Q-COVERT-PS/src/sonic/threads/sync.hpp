#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <climits>

#if SONIC_THREADS_HAS_OS && (defined(__linux__) || defined(__unix__) || defined(__APPLE__))
#include <sched.h>
#endif

namespace sn {
namespace threads {

namespace detail {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

class spin_backoff {
public:
  spin_backoff() noexcept = default;

  void reset() noexcept { spins_ = 0; }

  void pause() noexcept {
    cpu_relax();
    if (spins_ < max_spins_) {
      ++spins_;
    }
  }

private:
  static constexpr unsigned max_spins_ = 1024;
  unsigned spins_ = 0;
};

}

inline void cpu_relax() noexcept { detail::cpu_relax(); }

inline void thread_yield() noexcept {
#if SONIC_THREADS_HAS_OS && (defined(__linux__) || defined(__unix__) || defined(__APPLE__))
  ::sched_yield();
#else
  detail::cpu_relax();
#endif
}

class spin_lock {
public:
  spin_lock() noexcept = default;

  void lock() noexcept {
    while (flag_.test_and_set(std::memory_order_acquire)) {
      detail::cpu_relax();
    }
  }

  bool try_lock() noexcept { return !flag_.test_and_set(std::memory_order_acquire); }

  void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
  std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

class mutex {
public:
  mutex() noexcept = default;

  void lock() noexcept {
    bool expected = false;
    if (state_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
      return;
    }

    detail::spin_backoff backoff;
    for (;;) {
      expected = false;
      while (state_.load(std::memory_order_relaxed)) {
        detail::cpu_relax();
      }
      if (state_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
      }
      backoff.pause();
    }
  }

  bool try_lock() noexcept {
    bool expected = false;
    return state_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed);
  }

  void unlock() noexcept { state_.store(false, std::memory_order_release); }

private:
  std::atomic<bool> state_{false};
};

template <typename Mutex> class unique_lock {
public:
  unique_lock() noexcept = default;
  explicit unique_lock(Mutex& m) noexcept : mutex_(&m) { lock(); }
  unique_lock(unique_lock&& other) noexcept : mutex_(other.mutex_), owns_(other.owns_) {
    other.mutex_ = nullptr;
    other.owns_ = false;
  }
  unique_lock& operator=(unique_lock&& other) noexcept {
    if (this != &other) {
      if (owns_ && mutex_) {
        mutex_->unlock();
      }
      mutex_ = other.mutex_;
      owns_ = other.owns_;
      other.mutex_ = nullptr;
      other.owns_ = false;
    }
    return *this;
  }
  unique_lock(const unique_lock&) = delete;
  unique_lock& operator=(const unique_lock&) = delete;
  ~unique_lock() {
    if (owns_ && mutex_) {
      mutex_->unlock();
    }
  }

  void lock() noexcept {
    if (mutex_ && !owns_) {
      mutex_->lock();
      owns_ = true;
    }
  }

  void unlock() noexcept {
    if (mutex_ && owns_) {
      mutex_->unlock();
      owns_ = false;
    }
  }

  [[nodiscard]] bool owns_lock() const noexcept { return owns_; }
  [[nodiscard]] Mutex* mutex() const noexcept { return mutex_; }

private:
  Mutex* mutex_ = nullptr;
  bool owns_ = false;
};

class lock_guard {
public:
  explicit lock_guard(mutex& m) noexcept : mutex_(m) { mutex_.lock(); }
  lock_guard(const lock_guard&) = delete;
  lock_guard& operator=(const lock_guard&) = delete;
  ~lock_guard() { mutex_.unlock(); }

private:
  mutex& mutex_;
};

class semaphore {
public:
  explicit semaphore(unsigned initial = 0) noexcept : count_(initial) {}

  void reset(unsigned value = 0) noexcept { count_.store(value, std::memory_order_relaxed); }

  void release(unsigned delta = 1) noexcept {
    if (delta != 0) {
      count_.fetch_add(delta, std::memory_order_release);
    }
  }

  void acquire() noexcept {
    detail::spin_backoff backoff;
    for (;;) {
      unsigned current = count_.load(std::memory_order_relaxed);
      while (current == 0) {
        backoff.pause();
        current = count_.load(std::memory_order_relaxed);
      }
      if (count_.compare_exchange_weak(current, current - 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
      }
    }
  }

private:
  std::atomic<unsigned> count_{0};
};

class condition_variable {
public:
  condition_variable() noexcept = default;

  void wait(unique_lock<mutex>& lock) noexcept {
    waiter node;
    node.next = nullptr;
    node.sema.reset();

    queue_lock_.lock();
    if (tail_) {
      tail_->next = &node;
    } else {
      head_ = &node;
    }
    tail_ = &node;
    queue_lock_.unlock();

    lock.unlock();
    node.sema.acquire();
    lock.lock();
  }

  template <typename Predicate> void wait(unique_lock<mutex>& lock, Predicate pred) noexcept {
    while (!pred()) {
      wait(lock);
    }
  }

  void notify_one() noexcept {
    queue_lock_.lock();
    waiter* node = head_;
    if (node) {
      head_ = node->next;
      if (!head_) {
        tail_ = nullptr;
      }
    }
    queue_lock_.unlock();
    if (node) {
      node->sema.release();
    }
  }

  void notify_all() noexcept {
    for (;;) {
      queue_lock_.lock();
      waiter* node = head_;
      if (!node) {
        tail_ = nullptr;
        queue_lock_.unlock();
        return;
      }
      head_ = node->next;
      if (!head_) {
        tail_ = nullptr;
      }
      queue_lock_.unlock();
      node->sema.release();
    }
  }

private:
  struct waiter {
    semaphore sema{0};
    waiter* next = nullptr;
  };

  spin_lock queue_lock_;
  waiter* head_ = nullptr;
  waiter* tail_ = nullptr;
};

class binary_event {
public:
  explicit binary_event(bool signaled = false) noexcept : flag_(signaled) {}

  void signal() noexcept { flag_.store(true, std::memory_order_release); }

  void reset() noexcept { flag_.store(false, std::memory_order_release); }

  void wait() noexcept {
    detail::spin_backoff backoff;
    while (!flag_.load(std::memory_order_acquire)) {
      backoff.pause();
    }
    backoff.reset();
  }

  [[nodiscard]] bool signaled() const noexcept { return flag_.load(std::memory_order_acquire); }

private:
  std::atomic<bool> flag_;
};

class barrier {
public:
  explicit barrier(std::size_t parties) noexcept : parties_(parties), count_(parties) {}

  void arrive_and_wait() noexcept {
    const auto generation = generation_.load(std::memory_order_acquire);

    if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      count_.store(parties_, std::memory_order_release);
      generation_.fetch_add(1, std::memory_order_release);
      return;
    }

    detail::spin_backoff backoff;
    while (generation_.load(std::memory_order_acquire) == generation) {
      backoff.pause();
    }
  }

private:
  const std::size_t parties_;
  std::atomic<std::size_t> count_;
  std::atomic<std::uint64_t> generation_{0};
};

class count_latch {
public:
  explicit count_latch(std::size_t initial = 0) noexcept : count_(initial) {}

  void reset(std::size_t value = 0) noexcept { count_.store(value, std::memory_order_release); }

  void add(std::size_t delta) noexcept { count_.fetch_add(delta, std::memory_order_acq_rel); }

  void arrive(std::size_t delta = 1) noexcept {
    const std::size_t prev = count_.fetch_sub(delta, std::memory_order_acq_rel);
    assert(prev >= delta && "count_latch underflow");
    static_cast<void>(prev);
  }

  void wait() noexcept {
    detail::spin_backoff backoff;
    while (count_.load(std::memory_order_acquire) != 0) {
      backoff.pause();
    }
    backoff.reset();
  }

  [[nodiscard]] std::size_t value() const noexcept { return count_.load(std::memory_order_acquire); }

private:
  std::atomic<std::size_t> count_;
};

}
}
