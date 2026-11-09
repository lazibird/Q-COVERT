#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/sortshuffle/detail/bitonic_common.hpp"
#include "sonic/sortshuffle/detail/util.hpp"

namespace sn {
namespace sortshuffle {
namespace ser {
namespace bitonic {

#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
namespace counting {
enum class category { other, eviction_main, eviction_top, bucket_shuffle };
inline thread_local std::uint64_t total = 0;
inline thread_local std::uint64_t eviction_main = 0;
inline thread_local std::uint64_t eviction_top = 0;
inline thread_local std::uint64_t bucket_shuffle = 0;
inline thread_local category current = category::other;
inline void reset() noexcept { total = eviction_main = eviction_top = bucket_shuffle = 0; current = category::other; }
inline void record() noexcept {
  ++total;
  eviction_main += static_cast<std::uint64_t>(current == category::eviction_main);
  eviction_top += static_cast<std::uint64_t>(current == category::eviction_top);
  bucket_shuffle += static_cast<std::uint64_t>(current == category::bucket_shuffle);
}
struct category_scope {
  category previous;
  explicit category_scope(category next) noexcept : previous(current) { current = next; }
  ~category_scope() noexcept { current = previous; }
};
} // namespace counting
#endif

namespace detail {

using ::sn::sortshuffle::detail::default_key;
using ::sn::sortshuffle::detail::has_key_member;
using ::sn::sortshuffle::detail::is_pow2;
using ::sn::sortshuffle::detail::key_result_t;
using ::sn::sortshuffle::detail::noop_hook;
using ::sn::sortshuffle::detail::pow2_leq;
using ::sn::sortshuffle::detail::swap_element;
using ::sn::sortshuffle::detail::bitonic::bitonic_adjust_lane;
using ::sn::sortshuffle::detail::bitonic::bitonic_network_ops;
using ::sn::sortshuffle::detail::bitonic::bitonic_should_swap;

template <typename T> constexpr std::size_t bitonic_smallsort_limit() {
  if constexpr (sizeof(T) <= 64) {
    return 1024;
  } else {
    return 0;
  }
}

template <typename T> constexpr std::size_t bitonic_iterative_limit() {
  if constexpr (sizeof(T) <= 16) {
    return 65536;
  } else if constexpr (sizeof(T) <= 32) {
    return 16384;
  } else if constexpr (sizeof(T) <= 64) {
    return 8192;
  } else if constexpr (sizeof(T) <= 128) {
    return 256;
  } else {
    return 0;
  }
}

constexpr std::size_t bitonic_smallsort_max_item_size = 32;

template <typename T, typename KeyExtractor, typename Compare, typename SwapHook>
inline void bitonic_smallsort_iterative(
    T* data, std::size_t count, bool sort_ascending, KeyExtractor& key, Compare& comp, SwapHook& hook
) {

  for (std::size_t k = 2; k <= count; k <<= 1U) {

    for (std::size_t j = k >> 1U; j > 0; j >>= 1U) {
      const std::size_t segment = j << 1U;
      for (std::size_t base = 0; base < count; base += segment) {
        const bool lane_ascending = bitonic_adjust_lane(sort_ascending, (base & k) == 0U);
        for (std::size_t offset = 0; offset < j; ++offset) {
          T* left = data + base + offset;
          T* right = left + j;
          const auto left_key = key(*left);
          const auto right_key = key(*right);
          const bool should_swap = bitonic_should_swap(left_key, right_key, lane_ascending, comp);
#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
          counting::record();
#endif

          swap_element(left, right, should_swap);
          hook(left, right, should_swap);
        }
      }
    }
  }
}

template <std::size_t N, typename T, typename KeyExtractor, typename Compare, typename SwapHook>
inline void bitonic_smallsort_precomputed(
    T* data, bool sort_ascending, KeyExtractor& key, Compare& comp, SwapHook& hook
) {
  for (const auto& op : bitonic_network_ops<N>::table) {

    T* left = data + op.left;
    T* right = data + op.right;
    const auto left_key = key(*left);
    const auto right_key = key(*right);
    const bool lane_ascending = bitonic_adjust_lane(sort_ascending, op.ascending);
    const bool should_swap = bitonic_should_swap(left_key, right_key, lane_ascending, comp);
#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
    counting::record();
#endif

    swap_element(left, right, should_swap);
    hook(left, right, should_swap);
  }
}

template <typename T, typename KeyExtractor, typename Compare, typename SwapHook> struct smallsort_router {
  static constexpr std::size_t smallsort_limit = bitonic_smallsort_limit<T>();
  static constexpr bool is_element_small = (sizeof(T) <= bitonic_smallsort_max_item_size);
  static constexpr bool is_hook_noop = std::is_same_v<std::decay_t<SwapHook>, noop_hook>;

  static constexpr bool use_precomputed = is_element_small && is_hook_noop;

  static constexpr bool eligible(std::size_t count) {
    if constexpr (smallsort_limit == 0) {
      (void) count;
      return false;
    } else {
      return count <= smallsort_limit && is_pow2(count);
    }
  }

  static inline bool dispatch(
      T* data, std::size_t count, bool sort_ascending, KeyExtractor& key, Compare& comp, SwapHook& hook
  ) {
    if (!eligible(count)) {
      return false;
    }

    if constexpr (use_precomputed) {

      switch (count) {
      case 8:
        bitonic_smallsort_precomputed<8>(data, sort_ascending, key, comp, hook);
        return true;
      case 16:
        bitonic_smallsort_precomputed<16>(data, sort_ascending, key, comp, hook);
        return true;
      case 32:
        bitonic_smallsort_precomputed<32>(data, sort_ascending, key, comp, hook);
        return true;
      case 64:
        bitonic_smallsort_precomputed<64>(data, sort_ascending, key, comp, hook);
        return true;
      case 128:
        bitonic_smallsort_precomputed<128>(data, sort_ascending, key, comp, hook);
        return true;
      case 256:
        bitonic_smallsort_precomputed<256>(data, sort_ascending, key, comp, hook);
        return true;
      case 512:
        bitonic_smallsort_precomputed<512>(data, sort_ascending, key, comp, hook);
        return true;
      case 1024:
        if constexpr (smallsort_limit >= 1024) {
          bitonic_smallsort_precomputed<1024>(data, sort_ascending, key, comp, hook);
          return true;
        }
        break;
      default:
        break;
      }
    }

    bitonic_smallsort_iterative(data, count, sort_ascending, key, comp, hook);
    return true;
  }
};

template <typename T, typename KeyExtractor, typename Compare, typename SwapHook>
inline void bitonic_merge_recursive(
    T* data, std::size_t offset, std::size_t length, bool ascending, KeyExtractor& key, Compare& comp, SwapHook& hook
) {
  if (length <= 1) {
    return;
  }

  std::size_t stride = pow2_leq(length);
  if (stride == length) {
    stride >>= 1U;
  }

  const std::size_t compare_limit = length - stride;
  for (std::size_t i = 0; i < compare_limit; ++i) {

    T* left = data + offset + i;
    T* right = left + stride;
    const auto left_key = key(*left);
    const auto right_key = key(*right);
    const bool should_swap = ascending ? comp(right_key, left_key) : comp(left_key, right_key);
#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
    counting::record();
#endif

    swap_element(left, right, should_swap);
    hook(left, right, should_swap);
  }

  bitonic_merge_recursive(data, offset, stride, ascending, key, comp, hook);
  bitonic_merge_recursive(data, offset + stride, length - stride, ascending, key, comp, hook);
}

template <typename T, typename KeyExtractor, typename Compare, typename SwapHook>
inline void bitonic_sort_dispatch(
    T* data, std::size_t offset, std::size_t length, bool ascending, KeyExtractor& key, Compare& comp, SwapHook& hook
) {
  if (length <= 1) {
    return;
  }

  using pow2_route = smallsort_router<T, KeyExtractor, Compare, SwapHook>;
  if (pow2_route::dispatch(data + offset, length, ascending, key, comp, hook)) {
    return;
  }

  if constexpr (std::is_same_v<std::decay_t<SwapHook>, noop_hook>) {
    constexpr std::size_t iterative_limit = bitonic_iterative_limit<T>();
    if (iterative_limit != 0 && length <= iterative_limit && is_pow2(length)) {
      bitonic_smallsort_iterative(data + offset, length, ascending, key, comp, hook);
      return;
    }
  }

  const std::size_t left_length = length >> 1U;
  const std::size_t right_length = length - left_length;

  bitonic_sort_dispatch(data, offset, left_length, !ascending, key, comp, hook);
  bitonic_sort_dispatch(data, offset + left_length, right_length, ascending, key, comp, hook);
  bitonic_merge_recursive(data, offset, length, ascending, key, comp, hook);
}

template <typename T, typename KeyExtractor, typename Compare, typename SwapHook>
inline void bitonic_sort_impl(T* data, std::size_t count, KeyExtractor& key, Compare& comp, SwapHook& hook) {
  if (count <= 1) {
    return;
  }

  bitonic_sort_dispatch(data, 0, count, true, key, comp, hook);
}

template <typename T, typename Key> struct key_buffer_swap_hook {
  T* data;
  Key* keys;

  inline void operator()(T* left, T* right, bool cond) const noexcept {
    const auto left_index = static_cast<std::size_t>(left - data);
    const auto right_index = static_cast<std::size_t>(right - data);
    sn::obliv::ct_swap<Key>(&keys[left_index], &keys[right_index], cond);
  }
};

template <typename T, typename Key> struct key_buffer_extractor {
  const T* data;
  const Key* keys;

  inline Key operator()(const T& value) const noexcept {
    const auto index = static_cast<std::size_t>(&value - data);
    return keys[index];
  }
};

}

template <
    typename T, typename KeyExtractor = detail::default_key<T>,
    typename Compare = std::less<detail::key_result_t<KeyExtractor, T>>>
inline void bitonic_sort(T* data, std::size_t count, KeyExtractor key = KeyExtractor{}, Compare comp = Compare{}) {
  detail::noop_hook noop{};
  detail::bitonic_sort_impl(data, count, key, comp, noop);
}

template <typename T, typename Key, typename Compare = std::less<Key>>
inline void bitonic_sort_with_keys(T* data, std::size_t count, Key* key_buffer, Compare comp = Compare{}) {
  detail::key_buffer_extractor<T, Key> key_extractor{data, key_buffer};
  detail::key_buffer_swap_hook<T, Key> hook{data, key_buffer};
  detail::bitonic_sort_impl(data, count, key_extractor, comp, hook);
}

template <typename T, typename Rng> inline void bitonic_shuffle(T* data, std::size_t count, Rng& rng) {
  static_assert(detail::has_key_member<T>::value, "bitonic_shuffle: T must expose a key member");
  using key_type = detail::key_result_t<detail::default_key<T>, T>;
  static_assert(std::is_integral_v<key_type>, "bitonic_shuffle: embedded key must be integer");

  for (std::size_t i = 0; i < count; ++i) {
    auto* key_ptr = &data[i].key;
    rng.random_bytes(reinterpret_cast<std::uint8_t*>(key_ptr), sizeof(key_type));
  }

  if (count <= 1) {
    return;
  }

  detail::default_key<T> key{};
  std::less<key_type> comp{};
  detail::noop_hook noop{};
  detail::bitonic_sort_impl(data, count, key, comp, noop);
}

template <typename T, typename Key, typename Rng>
inline void bitonic_shuffle_with_keys(T* data, std::size_t count, Key* key_buffer, Rng& rng) {
  static_assert(std::is_integral_v<Key>, "bitonic_shuffle_with_keys: key must be integer");

  for (std::size_t i = 0; i < count; ++i) {
    rng.random_bytes(reinterpret_cast<std::uint8_t*>(&key_buffer[i]), sizeof(Key));
  }

  if (count <= 1) {
    return;
  }

  bitonic_sort_with_keys(data, count, key_buffer, std::less<Key>{});
}

}
}
}
}
