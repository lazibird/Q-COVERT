#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "sonic/threads/detail/park.hpp"
#include "sonic/threads/sync.hpp"
#include "sonic/threads/task.hpp"

namespace sn::threads {

class thread_pool;

namespace detail {

inline thread_local std::size_t tls_worker_index = static_cast<std::size_t>(-1);

#if defined(SONIC_THREADS_DEBUG)
inline constexpr bool kTaskNoexcept = false;
inline void task_assert(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
#else
inline constexpr bool kTaskNoexcept = true;
inline constexpr void task_assert(bool, const char*) {}
#endif
}

class worker_slot {
public:
  worker_slot() noexcept = default;

  [[nodiscard]] std::size_t index() const noexcept { return index_; }
  [[nodiscard]] bool valid() const noexcept { return pool_ != nullptr; }

private:
  friend class thread_pool;
  worker_slot(thread_pool* pool, std::size_t index) noexcept : pool_(pool), index_(index) {}

  thread_pool* pool_ = nullptr;
  std::size_t index_ = 0;
};

class thread_pool {
public:
  static constexpr std::size_t invalid_worker_index = static_cast<std::size_t>(-1);

  explicit thread_pool(std::size_t worker_capacity, park_mode mode = kDefaultParkMode);
  thread_pool(const thread_pool&) = delete;
  thread_pool& operator=(const thread_pool&) = delete;
  ~thread_pool();

  [[nodiscard]] std::size_t worker_capacity() const noexcept { return workers_.size(); }

  worker_slot attach_worker(std::size_t index);
  void detach_worker(worker_slot& slot);

  void worker_loop(worker_slot& slot);

  [[nodiscard]] park_mode park_strategy() const noexcept { return park_mode_; }

  void park() noexcept;

  void unpark() noexcept;

  void request_stop() noexcept;
  [[nodiscard]] bool stop_requested() const noexcept { return shutting_down_.load(std::memory_order_acquire); }

  void schedule(work_item& work);
  void wait(work_item& work) noexcept { work.wait(); }

  class task;

  static std::size_t current_worker_index() noexcept { return detail::tls_worker_index; }

  static void reset_worker_index() noexcept { detail::tls_worker_index = invalid_worker_index; }

private:
  struct worker_state {
    std::atomic<bool> attached{false};
  };

  struct alignas(64) queue_state {
    work_item* head = nullptr;
    work_item* tail = nullptr;
  };

  work_item* pop_next_work() noexcept;
  work_item* wait_for_work() noexcept;

  park_mode park_mode_ = kDefaultParkMode;
  std::vector<worker_state> workers_;
  mutex queue_lock_;
  condition_variable queue_cond_;
  queue_state queue_;
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> park_requested_{false};
  std::atomic<std::uint32_t> park_seq_{0};
};

class thread_pool::task : public work_item {
public:
  task() noexcept = default;

  task(task&& other) noexcept(detail::kTaskNoexcept) { move_from(std::move(other)); }
  task& operator=(task&& other) noexcept(detail::kTaskNoexcept) {
    if (this != &other) {
      reset();
      move_from(std::move(other));
    }
    return *this;
  }

  task(const task&) = delete;
  task& operator=(const task&) = delete;

  ~task() noexcept(detail::kTaskNoexcept) {
#ifdef SONIC_THREADS_DEBUG
    detail::task_assert(!this->in_flight(), "thread_pool::task destroyed while work still in flight");
#endif
    reset();
  }

  template <typename Fn> explicit task(Fn&& fn) noexcept(detail::kTaskNoexcept) { assign(std::forward<Fn>(fn)); }

  template <typename Fn> void assign(Fn&& fn) noexcept(detail::kTaskNoexcept) {
    using fn_type = std::decay_t<Fn>;
    static_assert(std::is_nothrow_invocable_v<fn_type&>, "task callable must be nothrow invocable");
    static_assert(std::is_nothrow_move_constructible_v<fn_type>, "task callable must be nothrow move constructible");
    static_assert(sizeof(fn_type) <= storage_size, "task callable too large");
    reset();
    new (&buffer_) fn_type(std::forward<Fn>(fn));
    invoke_ = [](void* ptr) noexcept { (*reinterpret_cast<fn_type*>(ptr))(); };
    destroy_ = [](void* ptr) noexcept { reinterpret_cast<fn_type*>(ptr)->~fn_type(); };
    move_ = [](void* dst, void* src) noexcept {
      auto* dst_fn = reinterpret_cast<fn_type*>(dst);
      auto* src_fn = reinterpret_cast<fn_type*>(src);
      new (dst_fn) fn_type(std::move(*src_fn));
      src_fn->~fn_type();
    };
    this->set_single(&task::thunk, this);
  }

  void reset() noexcept(detail::kTaskNoexcept) {
#ifdef SONIC_THREADS_DEBUG
    detail::task_assert(!this->in_flight(), "thread_pool::task::reset while work still in flight");
#endif
    if (destroy_) {
      destroy_(&buffer_);
    }
    invoke_ = nullptr;
    destroy_ = nullptr;
    move_ = nullptr;
    this->set_single(nullptr, nullptr);
  }

private:
  inline static constexpr std::size_t storage_size = 128;
  using invoke_fn = void (*)(void*) noexcept;
  using move_fn = void (*)(void*, void*) noexcept;

  static void thunk(void* raw) noexcept {
    auto* self = static_cast<task*>(raw);
    if (self->invoke_) {
      self->invoke_(&self->buffer_);
    }
  }

  void move_from(task&& other) noexcept(detail::kTaskNoexcept) {
#ifdef SONIC_THREADS_DEBUG
    detail::task_assert(!other.in_flight(), "thread_pool::task move while work still in flight");
#endif
    invoke_ = other.invoke_;
    destroy_ = other.destroy_;
    move_ = other.move_;
    if (other.invoke_) {
      move_(&buffer_, &other.buffer_);
      this->set_single(&task::thunk, this);
    } else {
      this->set_single(nullptr, nullptr);
    }
    other.invoke_ = nullptr;
    other.destroy_ = nullptr;
    other.move_ = nullptr;
    other.set_single(nullptr, nullptr);
  }

  invoke_fn invoke_ = nullptr;
  invoke_fn destroy_ = nullptr;
  move_fn move_ = nullptr;
  alignas(std::max_align_t) unsigned char buffer_[storage_size];
};

inline thread_pool::thread_pool(std::size_t worker_capacity, park_mode mode) :
    park_mode_(mode), workers_(worker_capacity) {}

inline thread_pool::~thread_pool() { request_stop(); }

inline void thread_pool::park() noexcept {
  if (!detail::park_supports_sleep(park_mode_)) {
    return;
  }
  park_requested_.store(true, std::memory_order_release);

  queue_cond_.notify_all();
}

inline void thread_pool::unpark() noexcept {
  if (!detail::park_supports_sleep(park_mode_)) {
    return;
  }
  park_requested_.store(false, std::memory_order_release);
  park_seq_.fetch_add(1, std::memory_order_acq_rel);
  detail::park_wake(park_mode_, park_seq_, detail::kParkWakeAll);
  queue_cond_.notify_all();
}

inline worker_slot thread_pool::attach_worker(std::size_t index) {
  if (index >= workers_.size()) {
    throw std::out_of_range("worker index out of range");
  }
  bool expected = false;
  if (!workers_[index].attached.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    throw std::logic_error("worker already attached");
  }
  return worker_slot(this, index);
}

inline void thread_pool::detach_worker(worker_slot& slot) {
  if (!slot.valid() || slot.pool_ != this) {
    throw std::logic_error("slot does not belong to this pool");
  }
  workers_[slot.index_].attached.store(false, std::memory_order_release);
  slot.pool_ = nullptr;
}

inline void thread_pool::request_stop() noexcept {
  shutting_down_.store(true, std::memory_order_release);
  if (detail::park_supports_sleep(park_mode_)) {

    park_requested_.store(false, std::memory_order_release);
    park_seq_.fetch_add(1, std::memory_order_acq_rel);
  }
  queue_lock_.lock();
  queue_cond_.notify_all();
  queue_lock_.unlock();
  if (detail::park_supports_sleep(park_mode_)) {
    detail::park_wake(park_mode_, park_seq_, detail::kParkWakeAll);
  }
}

inline void thread_pool::schedule(work_item& work) {
  if (stop_requested()) {
    work.complete();
    throw std::runtime_error("thread pool stop requested");
  }

  work.prepare_launch();

  queue_lock_.lock();
  if (shutting_down_.load(std::memory_order_acquire)) {
    queue_lock_.unlock();
    work.complete();
    throw std::runtime_error("thread pool stop requested");
  }

  work.next = nullptr;
  if (!queue_.tail) {
    queue_.head = &work;
    queue_.tail = &work;
  } else {
    queue_.tail->next = &work;
    queue_.tail = &work;
  }
  queue_cond_.notify_one();
  if (detail::park_supports_sleep(park_mode_) && park_requested_.load(std::memory_order_acquire)) {

    park_seq_.fetch_add(1, std::memory_order_acq_rel);
    detail::park_wake(park_mode_, park_seq_, 1);
  }
  queue_lock_.unlock();
}

inline work_item* thread_pool::pop_next_work() noexcept {
  work_item* work = queue_.head;
  if (work) {
    queue_.head = work->next;
    if (!queue_.head) {
      queue_.tail = nullptr;
    }
  }
  return work;
}

inline work_item* thread_pool::wait_for_work() noexcept {
  unique_lock<mutex> lock(queue_lock_);
  for (;;) {
    if (auto* work = pop_next_work()) {
      return work;
    }

    if (shutting_down_.load(std::memory_order_acquire)) {
      return nullptr;
    }

    if (detail::park_supports_sleep(park_mode_) && park_requested_.load(std::memory_order_acquire)) {
      const std::uint32_t expected = park_seq_.load(std::memory_order_acquire);
      lock.unlock();
      detail::park_wait(park_mode_, park_seq_, expected);
      lock.lock();
      continue;
    }

    queue_cond_.wait(lock);
  }
}

inline void thread_pool::worker_loop(worker_slot& slot) {
  if (!slot.valid() || slot.pool_ != this) {
    throw std::logic_error("worker slot not attached to this pool");
  }

  detail::tls_worker_index = slot.index_;

  for (;;) {
    work_item* work = wait_for_work();
    if (!work) {
      break;
    }

    if (auto fn = work->function()) {
      fn(work->argument());
    }
    work->complete();
  }

  detail::tls_worker_index = invalid_worker_index;
}

}
