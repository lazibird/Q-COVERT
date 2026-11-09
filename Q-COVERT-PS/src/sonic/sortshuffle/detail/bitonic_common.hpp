#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

#include "sonic/obliv/ops/core_ops.hpp"

namespace sn {
namespace sortshuffle {
namespace detail {
namespace bitonic {

template <typename KeyType, typename Compare>
inline bool bitonic_should_swap(const KeyType& left_key, const KeyType& right_key, bool ascending, Compare& comp) {
  using compare_type = std::decay_t<Compare>;
  constexpr bool key_is_integral = std::is_integral_v<KeyType>;

  constexpr bool comp_is_less =
      std::is_same_v<compare_type, std::less<KeyType>> || std::is_same_v<compare_type, std::less<void>>;
  constexpr bool comp_is_greater =
      std::is_same_v<compare_type, std::greater<KeyType>> || std::is_same_v<compare_type, std::greater<void>>;

  if constexpr (key_is_integral && comp_is_less) {
    return ascending ? sn::obliv::ct_lt(right_key, left_key) : sn::obliv::ct_lt(left_key, right_key);
  } else if constexpr (key_is_integral && comp_is_greater) {
    return ascending ? sn::obliv::ct_lt(left_key, right_key) : sn::obliv::ct_lt(right_key, left_key);
  } else {
    return ascending ? comp(right_key, left_key) : comp(left_key, right_key);
  }
}

inline bool bitonic_adjust_lane(bool sort_ascending, bool lane_ascending) noexcept {
  return sort_ascending ? lane_ascending : !lane_ascending;
}

struct bitonic_network_op {
  std::uint16_t left;
  std::uint16_t right;
  bool ascending;
};

template <std::size_t N> constexpr std::size_t bitonic_operation_count() {
  std::size_t total = 0;
  for (std::size_t k = 2; k <= N; k <<= 1U) {
    for (std::size_t j = k >> 1U; j > 0; j >>= 1U) {
      total += N >> 1U;
    }
  }
  return total;
}

template <std::size_t N>
constexpr std::array<bitonic_network_op, bitonic_operation_count<N>()> make_bitonic_network_ops() {
  std::array<bitonic_network_op, bitonic_operation_count<N>()> ops{};
  std::size_t index = 0;

  for (std::size_t k = 2; k <= N; k <<= 1U) {

    for (std::size_t j = k >> 1U; j > 0; j >>= 1U) {
      for (std::size_t i = 0; i < N; ++i) {

        const std::size_t ixj = i ^ j;
        if (ixj <= i) {
          continue;
        }

        ops[index++] =
            bitonic_network_op{static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(ixj), (i & k) == 0U};
      }
    }
  }
  return ops;
}

template <std::size_t N> struct bitonic_network_ops {
  static constexpr std::size_t count = bitonic_operation_count<N>();
  static constexpr std::array<bitonic_network_op, count> table = make_bitonic_network_ops<N>();
};

template <std::size_t N>
constexpr std::array<bitonic_network_op, bitonic_network_ops<N>::count> bitonic_network_ops<N>::table;

}
}
}
}
