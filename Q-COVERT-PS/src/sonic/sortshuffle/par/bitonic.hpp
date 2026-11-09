#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

#include "sonic/sortshuffle/detail/bitonic_common.hpp"
#include "sonic/sortshuffle/detail/util.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"
#include "sonic/threads/thread_team.hpp"

namespace sn {
namespace sortshuffle {
namespace par {

template <std::size_t MinParallelSpan> struct bitonic_options {
  template <typename> static constexpr std::size_t min_parallel_span_for() { return MinParallelSpan; }
};

struct bitonic_options_default {
  template <typename T> static constexpr std::size_t min_parallel_span_for() {
    constexpr std::size_t bytes = sizeof(T);
    if constexpr (bytes <= 128) {
      return 2048;
    } else if constexpr (bytes <= 256) {
      return 1536;
    } else if constexpr (bytes <= 1024) {
      return 3072;
    } else {
      return 4096;
    }
  }
};

namespace bitonic_detail {

template <typename T, typename Options> struct option_traits {
  static constexpr std::size_t min_parallel_span = Options::template min_parallel_span_for<T>();
};

using ::sn::sortshuffle::detail::default_key;
using ::sn::sortshuffle::detail::key_result_t;
using ::sn::sortshuffle::detail::noop_hook;
using ::sn::sortshuffle::detail::pow2_leq;
using ::sn::sortshuffle::detail::swap_element;
using ::sn::sortshuffle::detail::bitonic::bitonic_should_swap;
namespace ser_bitonic = ::sn::sortshuffle::ser::bitonic;

template <typename T, typename KeyExtractor, typename Compare, typename SwapHook> struct context {
  using value_type = T;
  T* data;
  KeyExtractor* key;
  Compare* comp;
  SwapHook* hook;
};

inline std::pair<std::size_t, std::size_t> split_threads(std::size_t threads) noexcept {
  if (threads <= 1) {
    return {1, 1};
  }

  const std::size_t left = threads / 2;
  const std::size_t right = threads - left;
  return {left ? left : 1, right ? right : 1};
}

template <typename Ctx>
inline void compare_stride(Ctx& ctx, std::size_t start, std::size_t stride, bool ascending) noexcept {
  auto* left = ctx.data + start;
  auto* right = left + stride;
  const auto left_key = (*ctx.key)(*left);
  const auto right_key = (*ctx.key)(*right);
  const bool should_swap = bitonic_should_swap(left_key, right_key, ascending, *ctx.comp);
  swap_element(left, right, should_swap);
  (*ctx.hook)(left, right, should_swap);
}

template <typename Ctx> inline void merge_serial(Ctx& ctx, std::size_t start, std::size_t length, bool ascending) {
  ser_bitonic::detail::bitonic_merge_recursive(ctx.data, start, length, ascending, *ctx.key, *ctx.comp, *ctx.hook);
}

template <typename Ctx> inline void sort_serial(Ctx& ctx, std::size_t start, std::size_t length, bool ascending) {
  ser_bitonic::detail::bitonic_sort_dispatch(ctx.data, start, length, ascending, *ctx.key, *ctx.comp, *ctx.hook);
}

template <typename Ctx, typename Options>
void merge_parallel(
    Ctx& ctx, sn::threads::thread_pool& pool, std::size_t start, std::size_t length, bool ascending,
    std::size_t threads, std::size_t workers
) {
  if (length <= 1) {
    return;
  }
  constexpr std::size_t min_span = option_traits<typename Ctx::value_type, Options>::min_parallel_span;
  if (threads <= 1 || length <= min_span || workers == 0) {
    merge_serial(ctx, start, length, ascending);
    return;
  }

  if (length == 2) {
    compare_stride(ctx, start, 1, ascending);
    return;
  }

  std::size_t stride = pow2_leq(length);
  if (stride == length) {
    stride >>= 1U;
  }

  const std::size_t limit = length - stride;
  for (std::size_t i = 0; i < limit; ++i) {
    compare_stride(ctx, start + i, stride, ascending);
  }

  const std::size_t right_start = start + stride;
  const std::size_t right_length = length - stride;
  const auto split = split_threads(threads);
  const std::size_t left_threads = split.first;
  const std::size_t right_threads = split.second;

  sn::threads::thread_pool::task task;
  task.assign([&ctx, &pool, right_start, right_length, ascending, right_threads, workers]() noexcept {
    merge_parallel<Ctx, Options>(ctx, pool, right_start, right_length, ascending, right_threads, workers);
  });
  pool.schedule(task);
  merge_parallel<Ctx, Options>(ctx, pool, start, stride, ascending, left_threads, workers);
  pool.wait(task);
  task.reset();
}

template <typename Ctx, typename Options>
void sort_parallel(
    Ctx& ctx, sn::threads::thread_pool& pool, std::size_t start, std::size_t length, bool ascending,
    std::size_t threads, std::size_t workers
) {
  if (length <= 1) {
    return;
  }
  constexpr std::size_t min_span = option_traits<typename Ctx::value_type, Options>::min_parallel_span;
  if (threads <= 1 || length <= min_span || workers == 0) {
    sort_serial(ctx, start, length, ascending);
    return;
  }

  if (length == 2) {
    compare_stride(ctx, start, 1, ascending);
    return;
  }

  const std::size_t left_length = length >> 1U;
  const std::size_t right_length = length - left_length;
  const auto split = split_threads(threads);
  const std::size_t left_threads = split.first;
  const std::size_t right_threads = split.second;
  const std::size_t right_start = start + left_length;

  sn::threads::thread_pool::task task;
  task.assign([&ctx, &pool, right_start, right_length, ascending, right_threads, workers]() noexcept {
    sort_parallel<Ctx, Options>(ctx, pool, right_start, right_length, ascending, right_threads, workers);
  });
  pool.schedule(task);
  sort_parallel<Ctx, Options>(ctx, pool, start, left_length, !ascending, left_threads, workers);
  pool.wait(task);
  task.reset();

  merge_parallel<Ctx, Options>(ctx, pool, start, length, ascending, threads, workers);
}

}

template <
    typename T, typename KeyExtractor = detail::default_key<T>,
    typename Compare = std::less<detail::key_result_t<KeyExtractor, T>>, typename SwapHook = detail::noop_hook,
    typename Options = bitonic_options_default>
void bitonic_sort(
    T* data, std::size_t count, sn::threads::thread_team& team, KeyExtractor key = KeyExtractor{},
    Compare comp = Compare{}, SwapHook hook = SwapHook{}, [[maybe_unused]] Options opts = Options{}
) {
  if (count <= 1) {
    return;
  }

  auto& pool = team.pool();
  const std::size_t workers = team.background_threads();
  const std::size_t threads = team.logical_threads();
  bitonic_detail::context<T, KeyExtractor, Compare, SwapHook> ctx{data, &key, &comp, &hook};
  bitonic_detail::sort_parallel<bitonic_detail::context<T, KeyExtractor, Compare, SwapHook>, Options>(
      ctx, pool, 0, count, true, threads, workers
  );
}

template <typename T, typename Key, typename Compare = std::less<Key>, typename Options = bitonic_options_default>
void bitonic_sort_with_keys(
    T* data, std::size_t count, Key* key_buffer, sn::threads::thread_team& team, Compare comp = Compare{},
    [[maybe_unused]] Options opts = Options{}
) {
  if (count <= 1) {
    return;
  }

  typename ::sn::sortshuffle::ser::bitonic::detail::key_buffer_extractor<T, Key> extractor{data, key_buffer};
  typename ::sn::sortshuffle::ser::bitonic::detail::key_buffer_swap_hook<T, Key> swapper{data, key_buffer};
  bitonic_sort<T, decltype(extractor), Compare, decltype(swapper), Options>(
      data, count, team, extractor, comp, swapper
  );
}

}
}
}
