#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "sonic/threads/detail/invoke.hpp"
#include "sonic/threads/task.hpp"
#include "sonic/threads/thread_pool.hpp"
#include "sonic/threads/util.hpp"

namespace sn::threads {

class thread_team {
public:

  explicit thread_team(thread_pool& pool) noexcept : thread_team(pool, 0) {}

  thread_team(thread_pool& pool, std::size_t logical_threads) noexcept :
      pool_(&pool), logical_threads_(logical_threads), cached_tasks_(pool.worker_capacity()) {}

  thread_team(const thread_team&) = delete;
  thread_team& operator=(const thread_team&) = delete;
  thread_team(thread_team&&) noexcept = default;
  thread_team& operator=(thread_team&&) noexcept = default;

  [[nodiscard]] thread_team limited_to(std::size_t logical_threads) const noexcept {
    return thread_team(*pool_, logical_threads);
  }

  [[nodiscard]] std::size_t logical_threads() const noexcept { return total_logical_threads(); }
  [[nodiscard]] std::size_t background_threads() const noexcept { return logical_threads() - 1; }

  [[nodiscard]] thread_pool& pool() noexcept { return *pool_; }
  [[nodiscard]] const thread_pool& pool() const noexcept { return *pool_; }

  template <typename Fn> void parallel_work(Fn&& fn) {
    using fn_type = std::decay_t<Fn>;
    static_assert(std::is_copy_constructible_v<fn_type>, "thread_team callable must be copy constructible");
    fn_type base(std::forward<Fn>(fn));

    const std::size_t background = background_threads();

    if (background == 0) {
      detail::invoke_worker_callable(base, 0, 0);
      return;
    }

    for (std::size_t logical = 0; logical < background; ++logical) {
      cached_tasks_[logical].assign([copy = base, logical]() mutable noexcept {
        detail::invoke_worker_callable(copy, logical, logical);
      });
      pool_->schedule(cached_tasks_[logical]);
    }

    detail::invoke_worker_callable(base, background, background);

    for (std::size_t logical = 0; logical < background; ++logical) {
      cached_tasks_[logical].wait();
    }
  }

  template <typename Index, typename Fn> void parallel_for(Index begin, Index end, Fn&& fn) {
    static_assert(std::is_integral_v<Index>, "thread_team parallel_for requires integral indices");
    if (begin >= end) {
      return;
    }

    const Index total = end - begin;
    const std::size_t workers = total_logical_threads();

    parallel_work([begin, total, workers, fn = std::forward<Fn>(fn)](std::size_t logical) mutable noexcept {
      auto [chunk_begin, chunk_end] = partition_evenly(logical, static_cast<std::size_t>(total), workers);
      for (std::size_t offset = chunk_begin; offset < chunk_end; ++offset) {
        const Index idx = begin + static_cast<Index>(offset);
        detail::invoke_range_callable(fn, idx, logical);
      }
    });
  }

  template <typename Index, typename Fn> void parallel_for(Index begin, Index end, Index chunk, Fn&& fn) {
    static_assert(std::is_integral_v<Index>, "thread_team parallel_for requires integral indices");
    if (begin >= end || chunk <= 0) {
      return;
    }

    const auto total_span = static_cast<std::size_t>(end - begin);
    if (total_span == 0) {
      return;
    }

    const std::size_t workers = total_logical_threads();
    std::size_t partitions = workers;

    const auto chunk_size = static_cast<std::size_t>(chunk);
    const std::size_t desired = chunk_size == 0 ? partitions : (total_span + (chunk_size - 1)) / chunk_size;
    partitions = desired == 0 ? 1 : (desired < partitions ? desired : partitions);

    parallel_work([begin, total_span, partitions, fn = std::forward<Fn>(fn)](std::size_t logical) mutable noexcept {
      auto [chunk_begin, chunk_end] = partition_evenly(logical, total_span, partitions);
      for (std::size_t offset = chunk_begin; offset < chunk_end; ++offset) {
        const Index idx = begin + static_cast<Index>(offset);
        detail::invoke_range_callable(fn, idx, logical);
      }
    });
  }

private:
  [[nodiscard]] std::size_t available_logical_threads() const noexcept {
    const std::size_t background = pool_->worker_capacity();
    return background == 0 ? 1 : background + 1;
  }

  [[nodiscard]] std::size_t total_logical_threads() const noexcept {
    const std::size_t available = available_logical_threads();
    if (logical_threads_ == 0 || logical_threads_ >= available) {
      return available;
    }
    return logical_threads_;
  }

  thread_pool* pool_;
  std::size_t logical_threads_ = 0;
  std::vector<thread_pool::task> cached_tasks_;
};

}
