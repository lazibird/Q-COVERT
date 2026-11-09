#pragma once

#include <atomic>
#include <cstddef>

#ifdef SONIC_THREADS_DEBUG
#include "sonic/util/log.hpp"
#endif

#include "sonic/threads/sync.hpp"

namespace sn::threads {

class work_item {
public:
  using single_fn = void (*)(void*) noexcept;

  work_item() noexcept = default;
  work_item(const work_item&) = delete;
  work_item& operator=(const work_item&) = delete;

  void set_single(single_fn fn, void* arg) noexcept {
    fn_ = fn;
    arg_ = arg;
  }

  [[nodiscard]] single_fn function() const noexcept { return fn_; }
  [[nodiscard]] void* argument() const noexcept { return arg_; }

  void prepare_launch() noexcept {
    done_.reset();
    next = nullptr;
#ifdef SONIC_THREADS_DEBUG
    in_flight_.store(true, std::memory_order_release);
#endif
  }

  void complete() noexcept {
#ifdef SONIC_THREADS_DEBUG
    in_flight_.store(false, std::memory_order_release);
#endif
    done_.release();
  }
  void wait() noexcept {
    done_.acquire();
#ifdef SONIC_THREADS_DEBUG
    in_flight_.store(false, std::memory_order_release);
#endif
  }

#ifdef SONIC_THREADS_DEBUG
  [[nodiscard]] bool in_flight() const noexcept { return in_flight_.load(std::memory_order_acquire); }
#endif

  work_item* next = nullptr;

private:
  single_fn fn_ = nullptr;
  void* arg_ = nullptr;
  semaphore done_{0};
#ifdef SONIC_THREADS_DEBUG
  std::atomic<bool> in_flight_{false};
#endif
};

}
