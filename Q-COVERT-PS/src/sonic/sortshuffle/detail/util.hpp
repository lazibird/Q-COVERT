#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/struct_ops.hpp"

namespace sn {
namespace sortshuffle {
namespace detail {

template <typename T, typename = void> struct has_key_member : std::false_type {};

template <typename T> struct has_key_member<T, std::void_t<decltype(std::declval<T&>().key)>> : std::true_type {};

template <typename T, typename = void> struct has_key_method : std::false_type {};

template <typename T>
struct has_key_method<T, std::void_t<decltype(std::declval<const T&>().key())>> : std::true_type {};

template <typename T> struct default_key {
  constexpr decltype(auto) operator()(const T& value) const noexcept {
    if constexpr (has_key_member<T>::value) {
      return (value.key);
    } else if constexpr (has_key_method<T>::value) {
      return value.key();
    } else {
      return value;
    }
  }
};

template <typename KeyExtractor, typename T>
using key_result_t = std::decay_t<decltype(std::declval<KeyExtractor&>()(std::declval<const T&>()))>;

struct noop_hook {
  template <typename U> inline void operator()(U*, U*, bool) const noexcept {}
};

inline bool is_pow2(std::size_t value) { return value != 0 && (value & (value - 1)) == 0; }

inline std::size_t pow2_leq(std::size_t n) {
  if (n <= 1) {
    return 1;
  }
#if defined(__clang__) || defined(__GNUC__)
  const unsigned total_bits = static_cast<unsigned>(sizeof(unsigned long long) * 8);
  const auto value = static_cast<unsigned long long>(n);
  const unsigned leading = static_cast<unsigned>(__builtin_clzll(value));
  return static_cast<std::size_t>(1ULL << (total_bits - leading - 1));
#else
  std::size_t value = 1;
  while ((value << 1U) <= n) {
    value <<= 1U;
  }
  return value;
#endif
}

inline std::size_t next_pow2(std::size_t n) {
  if (n <= 1) {
    return 1;
  }
  --n;
  for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1) {
    n |= n >> shift;
  }
  return n + 1;
}

template <typename T> inline void swap_element(T* left, T* right, bool cond) {
  if constexpr (std::is_integral_v<T>) {
    sn::obliv::ct_swap(left, right, cond);
  } else {
    static_assert(std::is_trivially_copyable_v<T>, "swap_element copyable");
    static_assert(alignof(T) >= alignof(std::uint64_t), "swap_element aligned");
    sn::obliv::ct_swap_data<T>(left, right, cond);
  }
}

}
}
}
