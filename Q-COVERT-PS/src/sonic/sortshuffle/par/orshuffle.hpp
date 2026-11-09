#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/sortshuffle/detail/util.hpp"
#include "sonic/sortshuffle/ser/orshuffle.hpp"
#include "sonic/threads/thread_team.hpp"

namespace sn {
namespace sortshuffle {
namespace par {

template <std::size_t MinParallelSpan = 2048, std::size_t LoopGrain = 256> struct orshuffle_options {
  static constexpr std::size_t min_parallel_span = MinParallelSpan;
  static constexpr std::size_t loop_grain = LoopGrain;

  template <typename T> static constexpr std::size_t min_parallel_span_for() noexcept {
    constexpr std::size_t bytes = sizeof(T);
    if constexpr (bytes <= 32) {
      return MinParallelSpan * 2;
    } else if constexpr (bytes <= 128) {
      return MinParallelSpan;
    } else if constexpr (bytes <= 512) {
      return MinParallelSpan;
    } else {
      return MinParallelSpan / 2 > 0 ? MinParallelSpan / 2 : 1;
    }
  }

  template <typename T> static constexpr std::size_t loop_grain_for() noexcept {
    constexpr std::size_t bytes = sizeof(T);
    if constexpr (bytes <= 32) {
      constexpr std::size_t scaled = LoopGrain * 4;
      return scaled > 512 ? scaled : 512;
    } else if constexpr (bytes <= 128) {
      constexpr std::size_t scaled = LoopGrain * 2;
      return scaled > 512 ? scaled : 512;
    } else if constexpr (bytes <= 512) {
      return LoopGrain > 512 ? LoopGrain : 512;
    } else {
      constexpr std::size_t scaled = LoopGrain / 2;
      return scaled > 256 ? scaled : 256;
    }
  }
};

namespace orshuffle_detail {
using ::sn::sortshuffle::detail::noop_hook;
}

namespace orshuffle_impl {

using ::sn::sortshuffle::detail::pow2_leq;
using ::sn::sortshuffle::detail::swap_element;
namespace ser = ::sn::sortshuffle::ser::orshuffle;

inline std::pair<std::size_t, std::size_t> split_threads(std::size_t threads) noexcept {
  if (threads <= 1) {
    return {1, 1};
  }
  const std::size_t left = threads / 2;
  const std::size_t right = threads - left;
  return {left ? left : 1, right ? right : 1};
}

template <typename PrefixT>
inline PrefixT prefix_range_sum(const PrefixT* prefix, std::size_t begin, std::size_t end) noexcept {
  return prefix[end] - prefix[begin];
}

template <typename T, typename PrefixT, typename Hook, typename Options> struct context {
  using options_type = Options;
  using value_type = T;
  T* data;
  std::uint8_t* marks;
  PrefixT* prefix;
  Hook& hook;
  sn::threads::thread_pool& pool;
  std::size_t workers;
};

template <typename Ctx>
inline bool should_run_parallel(const Ctx& ctx, std::size_t length, std::size_t threads) noexcept {
  const std::size_t min_span = Ctx::options_type::template min_parallel_span_for<typename Ctx::value_type>();
  return threads > 1 && ctx.workers > 0 && length >= min_span;
}

template <typename Options, typename ValueT, typename Fn>
inline void for_each_index(
    sn::threads::thread_pool& pool, std::size_t workers, std::size_t threads, std::size_t count, Fn&& fn
) {
  if (count == 0) {
    return;
  }

  const std::size_t grain = Options::template loop_grain_for<ValueT>();
  if (threads <= 1 || workers == 0 || count <= grain) {
    for (std::size_t i = 0; i < count; ++i) {
      fn(i);
    }
    return;
  }

  const std::size_t helpers = std::min<std::size_t>(workers, threads - 1);
  if (helpers == 0) {
    for (std::size_t i = 0; i < count; ++i) {
      fn(i);
    }
    return;
  }

  const std::size_t active = helpers + 1;
  const std::size_t scaled_threshold = grain * active;
  if (count <= scaled_threshold * 4) {
    for (std::size_t i = 0; i < count; ++i) {
      fn(i);
    }
    return;
  }

  const std::size_t base_chunk = (count + active - 1) / active;
  const std::size_t chunk = std::max<std::size_t>(grain, base_chunk);
  const std::size_t total_chunks = (count + chunk - 1) / chunk;
  if (total_chunks <= 1) {
    for (std::size_t i = 0; i < count; ++i) {
      fn(i);
    }
    return;
  }

  const std::size_t helper_chunks = total_chunks - 1;
  const std::size_t spawn_count = std::min<std::size_t>(helpers, helper_chunks);
  if (spawn_count == 0) {
    for (std::size_t i = 0; i < count; ++i) {
      fn(i);
    }
    return;
  }

  auto* fn_ptr = &fn;
  auto* raw = static_cast<unsigned char*>(__builtin_alloca(sizeof(sn::threads::thread_pool::task) * spawn_count));
  auto* tasks = reinterpret_cast<sn::threads::thread_pool::task*>(raw);
  for (std::size_t idx = 0; idx < spawn_count; ++idx) {
    new (std::addressof(tasks[idx])) sn::threads::thread_pool::task();
  }

  std::size_t scheduled = 0;
  for (std::size_t idx = 0; idx < spawn_count; ++idx) {
    const std::size_t begin = idx * chunk;
    const std::size_t end = std::min<std::size_t>(count, begin + chunk);
    tasks[idx].assign([begin, end, fn_ptr]() noexcept {
      for (std::size_t i = begin; i < end; ++i) {
        (*fn_ptr)(i);
      }
    });
    pool.schedule(tasks[idx]);
    ++scheduled;
  }

  for (std::size_t begin = spawn_count * chunk; begin < count; begin += chunk) {
    const std::size_t end = std::min<std::size_t>(count, begin + chunk);
    for (std::size_t i = begin; i < end; ++i) {
      fn(i);
    }
  }

  for (std::size_t idx = 0; idx < scheduled; ++idx) {
    tasks[idx].wait();
    tasks[idx].reset();
    tasks[idx].~task();
  }
}

template <typename Ctx>
void oroffcompact_parallel(Ctx& ctx, std::size_t begin, std::size_t length, std::size_t offset, std::size_t threads);

template <typename Ctx>
void oroffuncompact_parallel(Ctx& ctx, std::size_t begin, std::size_t length, std::size_t offset, std::size_t threads);

template <typename Ctx> void orcompact_parallel(Ctx& ctx, std::size_t begin, std::size_t length, std::size_t threads);

template <typename Ctx> void oruncompact_parallel(Ctx& ctx, std::size_t begin, std::size_t length, std::size_t threads);

template <typename Ctx>
void oroffcompact_parallel(Ctx& ctx, std::size_t begin, std::size_t length, std::size_t offset, std::size_t threads) {
  if (length <= 1) {
    return;
  }
  if (!should_run_parallel(ctx, length, threads)) {
    ser::detail::oroffcompact_slice(ctx.data, ctx.prefix, begin, length, offset, ctx.hook);
    return;
  }

  const std::size_t half = length >> 1U;

  const auto left_sum = static_cast<std::size_t>(prefix_range_sum(ctx.prefix, begin, begin + half));

  const std::size_t left_offset = offset & (half - 1);

  const std::size_t right_offset = (offset + left_sum) & (half - 1);
  const auto split = split_threads(threads);
  const std::size_t left_threads = split.first;
  const std::size_t right_threads = split.second;

  if (left_threads <= 1 && right_threads <= 1) {
    ser::detail::oroffcompact_slice(ctx.data, ctx.prefix, begin, length, offset, ctx.hook);
    return;
  }

  if (right_threads > 1) {
    sn::threads::thread_pool::task task;
    task.assign([&ctx, begin, half, right_offset, right_threads]() noexcept {
      oroffcompact_parallel(ctx, begin + half, half, right_offset, right_threads);
    });
    ctx.pool.schedule(task);
    if (left_threads <= 1) {
      ser::detail::oroffcompact_slice(ctx.data, ctx.prefix, begin, half, left_offset, ctx.hook);
    } else {
      oroffcompact_parallel(ctx, begin, half, left_offset, left_threads);
    }
    ctx.pool.wait(task);
    task.reset();
  } else {
    if (left_threads <= 1) {
      ser::detail::oroffcompact_slice(ctx.data, ctx.prefix, begin, half, left_offset, ctx.hook);
    } else {
      oroffcompact_parallel(ctx, begin, half, left_offset, left_threads);
    }
    ser::detail::oroffcompact_slice(ctx.data, ctx.prefix, begin + half, half, right_offset, ctx.hook);
  }

  const bool s = ((left_offset + left_sum) >= half) != (offset >= half);
  const std::size_t marker = (offset + left_sum) & (half - 1);

  auto loop = [&](std::size_t i) noexcept {
    auto* left = ctx.data + begin + i;
    auto* right = left + half;
    const bool cond = s != (i >= marker);
    swap_element(left, right, cond);
    ctx.hook(left, right, cond);
  };
  for_each_index<typename Ctx::options_type, typename Ctx::value_type>(ctx.pool, ctx.workers, threads, half, loop);
}

template <typename Ctx> void orcompact_parallel(Ctx& ctx, std::size_t begin, std::size_t length, std::size_t threads) {
  if (length <= 1) {
    return;
  }
  if (!should_run_parallel(ctx, length, threads)) {
    ser::detail::orcompact_slice(ctx.data, ctx.prefix, begin, length, ctx.hook);
    return;
  }

  const std::size_t pow2 = pow2_leq(length);
  const std::size_t left_length = length - pow2;
  const std::size_t right_begin = begin + left_length;
  const std::size_t right_length = pow2;

  if (left_length == 0) {
    oroffcompact_parallel(ctx, begin, right_length, 0, threads);
    return;
  }

  const std::size_t left_marked = static_cast<std::size_t>(prefix_range_sum(ctx.prefix, begin, right_begin));

  std::size_t right_offset = right_length - left_length + left_marked;
  const bool offset_eq = sn::obliv::ct_eq(right_offset, right_length);
  right_offset = sn::obliv::ct_select<std::size_t>(0, right_offset, offset_eq);

  const auto split = split_threads(threads);
  const std::size_t left_threads = split.first;
  const std::size_t right_threads = split.second;

  if (left_threads <= 1 && right_threads <= 1) {
    ser::detail::orcompact_slice(ctx.data, ctx.prefix, begin, length, ctx.hook);
    return;
  }

  if (right_threads > 1) {
    sn::threads::thread_pool::task task;
    task.assign([&ctx, right_begin, right_length, right_offset, right_threads]() noexcept {
      oroffcompact_parallel(ctx, right_begin, right_length, right_offset, right_threads);
    });
    ctx.pool.schedule(task);
    if (left_threads <= 1) {
      ser::detail::orcompact_slice(ctx.data, ctx.prefix, begin, left_length, ctx.hook);
    } else {
      orcompact_parallel(ctx, begin, left_length, left_threads);
    }
    ctx.pool.wait(task);
    task.reset();
  } else {
    if (left_threads <= 1) {
      ser::detail::orcompact_slice(ctx.data, ctx.prefix, begin, left_length, ctx.hook);
    } else {
      orcompact_parallel(ctx, begin, left_length, left_threads);
    }
    ser::detail::oroffcompact_slice(ctx.data, ctx.prefix, right_begin, right_length, right_offset, ctx.hook);
  }

  auto loop = [&](std::size_t i) noexcept {
    auto* left = ctx.data + begin + i;
    auto* right = ctx.data + right_begin + i;
    const bool cond = i >= left_marked;
    swap_element(left, right, cond);
    ctx.hook(left, right, cond);
  };
  for_each_index<typename Ctx::options_type, typename Ctx::value_type>(
      ctx.pool, ctx.workers, threads, left_length, loop
  );
}

template <typename Ctx>
void oroffuncompact_parallel(Ctx& ctx, std::size_t begin, std::size_t length, std::size_t offset, std::size_t threads) {
  if (length <= 1) {
    return;
  }
  if (!should_run_parallel(ctx, length, threads)) {
    ser::detail::oroffuncompact_slice(ctx.data, ctx.prefix, begin, length, offset, ctx.hook);
    return;
  }

  const std::size_t half = length >> 1U;

  const auto left_sum = static_cast<std::size_t>(prefix_range_sum(ctx.prefix, begin, begin + half));

  const std::size_t left_offset = offset & (half - 1);

  const std::size_t right_offset = (offset + left_sum) & (half - 1);

  const auto split = split_threads(threads);
  const std::size_t left_threads = split.first;
  const std::size_t right_threads = split.second;

  if (left_threads <= 1 && right_threads <= 1) {
    ser::detail::oroffuncompact_slice(ctx.data, ctx.prefix, begin, length, offset, ctx.hook);
    return;
  }

  const bool s = ((left_offset + left_sum) >= half) != (offset >= half);
  const std::size_t marker = (offset + left_sum) & (half - 1);

  for (std::size_t i = 0; i < half; ++i) {
    auto* left = ctx.data + begin + i;
    auto* right = left + half;
    const bool cond = s != (i >= marker);
    swap_element(left, right, cond);
    ctx.hook(left, right, cond);
  }

  if (right_threads > 1) {
    sn::threads::thread_pool::task task;
    task.assign([&ctx, begin, half, right_offset, right_threads]() noexcept {
      oroffuncompact_parallel(ctx, begin + half, half, right_offset, right_threads);
    });
    ctx.pool.schedule(task);
    if (left_threads <= 1) {
      ser::detail::oroffuncompact_slice(ctx.data, ctx.prefix, begin, half, left_offset, ctx.hook);
    } else {
      oroffuncompact_parallel(ctx, begin, half, left_offset, left_threads);
    }
    ctx.pool.wait(task);
    task.reset();
  } else {
    if (left_threads <= 1) {
      ser::detail::oroffuncompact_slice(ctx.data, ctx.prefix, begin, half, left_offset, ctx.hook);
    } else {
      oroffuncompact_parallel(ctx, begin, half, left_offset, left_threads);
    }
    ser::detail::oroffuncompact_slice(ctx.data, ctx.prefix, begin + half, half, right_offset, ctx.hook);
  }
}

template <typename Ctx>
void oruncompact_parallel(Ctx& ctx, std::size_t begin, std::size_t length, std::size_t threads) {
  if (length <= 1) {
    return;
  }
  if (!should_run_parallel(ctx, length, threads)) {
    ser::detail::oruncompact_slice(ctx.data, ctx.prefix, begin, length, ctx.hook);
    return;
  }

  const std::size_t pow2 = pow2_leq(length);
  const std::size_t left_length = length - pow2;
  const std::size_t right_begin = begin + left_length;
  const std::size_t right_length = pow2;

  std::size_t left_marked = 0;
  if (left_length > 0) {

    left_marked = static_cast<std::size_t>(prefix_range_sum(ctx.prefix, begin, right_begin));
  }

  const auto split = split_threads(threads);
  const std::size_t left_threads = split.first;
  const std::size_t right_threads = split.second;

  if (left_threads <= 1 && right_threads <= 1) {
    ser::detail::oruncompact_slice(ctx.data, ctx.prefix, begin, length, ctx.hook);
    return;
  }

  if (left_length > 0) {

    for (std::size_t i = 0; i < left_length; ++i) {
      auto* left = ctx.data + begin + i;
      auto* right = ctx.data + begin + pow2 + i;
      const bool cond = i >= left_marked;
      swap_element(left, right, cond);
      ctx.hook(left, right, cond);
    }
  }

  std::size_t right_offset = right_length - left_length + left_marked;
  const bool offset_eq = sn::obliv::ct_eq(right_offset, right_length);
  right_offset = sn::obliv::ct_select<std::size_t>(0, right_offset, offset_eq);

  if (right_threads > 1) {
    sn::threads::thread_pool::task task;
    task.assign([&ctx, right_begin, right_length, right_offset, right_threads]() noexcept {
      oroffuncompact_parallel(ctx, right_begin, right_length, right_offset, right_threads);
    });
    ctx.pool.schedule(task);
    if (left_length > 0) {
      if (left_threads <= 1) {
        ser::detail::oruncompact_slice(ctx.data, ctx.prefix, begin, left_length, ctx.hook);
      } else {
        oruncompact_parallel(ctx, begin, left_length, left_threads);
      }
    }
    ctx.pool.wait(task);
    task.reset();
  } else {
    if (left_length > 0) {
      if (left_threads <= 1) {
        ser::detail::oruncompact_slice(ctx.data, ctx.prefix, begin, left_length, ctx.hook);
      } else {
        oruncompact_parallel(ctx, begin, left_length, left_threads);
      }
    }
    ser::detail::oroffuncompact_slice(ctx.data, ctx.prefix, right_begin, right_length, right_offset, ctx.hook);
  }
}

}

namespace orshuffle {

template <
    typename T, typename PrefixT = std::size_t, typename Hook = orshuffle_detail::noop_hook,
    typename Options = orshuffle_options<>>
void orcompact(
    T* data, std::size_t count, std::uint8_t* marks, PrefixT* prefix, sn::threads::thread_team& team,
    Hook hook = Hook{}, [[maybe_unused]] Options opts = Options{}
) {
  ::sn::sortshuffle::ser::orshuffle::detail::prefix_sum<PrefixT>(marks, prefix, count);

  if (count <= 1) {
    return;
  }

  auto& pool = team.pool();
  const std::size_t workers = team.background_threads();
  const std::size_t threads = team.logical_threads();

  orshuffle_impl::context<T, PrefixT, Hook, Options> ctx{data, marks, prefix, hook, pool, workers};
  orshuffle_impl::orcompact_parallel(ctx, 0, count, threads);
}

template <
    typename T, typename PrefixT = std::size_t, typename Hook = orshuffle_detail::noop_hook,
    typename Options = orshuffle_options<>>
void oruncompact(
    T* data, std::size_t count, std::uint8_t* marks, PrefixT* prefix, sn::threads::thread_team& team,
    Hook hook = Hook{}, [[maybe_unused]] Options opts = Options{}
) {
  ::sn::sortshuffle::ser::orshuffle::detail::prefix_sum<PrefixT>(marks, prefix, count);

  if (count <= 1) {
    return;
  }

  auto& pool = team.pool();
  const std::size_t workers = team.background_threads();
  const std::size_t threads = team.logical_threads();

  orshuffle_impl::context<T, PrefixT, Hook, Options> ctx{data, marks, prefix, hook, pool, workers};
  orshuffle_impl::oruncompact_parallel(ctx, 0, count, threads);
}

}

}
}
}
