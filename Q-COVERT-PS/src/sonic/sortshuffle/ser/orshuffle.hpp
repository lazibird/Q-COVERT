#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/sortshuffle/detail/util.hpp"

namespace sn {
namespace sortshuffle {
namespace ser {
namespace orshuffle {

#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
namespace counting {
inline thread_local std::uint64_t compact_conditional_swaps = 0;
inline void reset() noexcept { compact_conditional_swaps = 0; }
} // namespace counting
#endif

namespace detail {

using ::sn::sortshuffle::detail::noop_hook;
using ::sn::sortshuffle::detail::pow2_leq;
using ::sn::sortshuffle::detail::swap_element;

template <typename PrefixT>
inline void prefix_sum_slice(const std::uint8_t* marks, PrefixT* prefix, std::size_t begin, std::size_t length) {
  if (length == 0) {
    return;
  }
  PrefixT running = static_cast<PrefixT>(0);
  prefix[begin] = static_cast<PrefixT>(0);
  for (std::size_t i = 0; i < length; ++i) {
    running += static_cast<PrefixT>(marks[begin + i]);
    prefix[begin + i + 1] = running;
  }
}

template <typename PrefixT> inline void prefix_sum(const std::uint8_t* marks, PrefixT* prefix, std::size_t count) {
  PrefixT running = static_cast<PrefixT>(0);
  prefix[0] = static_cast<PrefixT>(0);
  for (std::size_t i = 0; i < count; ++i) {
    running += static_cast<PrefixT>(marks[i]);
    prefix[i + 1] = running;
  }
}

template <typename PrefixT> inline PrefixT prefix_range_sum(const PrefixT* prefix, std::size_t begin, std::size_t end) {
  return prefix[end] - prefix[begin];
}

template <typename PrefixT> inline std::uint8_t mark_at(const PrefixT* prefix, std::size_t i) {
  return static_cast<std::uint8_t>(prefix[i + 1] - prefix[i]);
}

constexpr std::size_t oroffcompact_small_limit = 32;

template <std::size_t Length> struct oroffcompact_small;

template <> struct oroffcompact_small<2> {
  template <typename T, typename PrefixT, typename SwapHook>
  static inline void run(T* data, const PrefixT* prefix, std::size_t begin, std::size_t offset, SwapHook& hook) {
    const bool left_mark = static_cast<bool>(mark_at(prefix, begin));
    const bool right_mark = static_cast<bool>(mark_at(prefix, begin + 1));
    const bool cond = (static_cast<bool>((!left_mark) & right_mark)) != static_cast<bool>(offset);
    swap_element(data + begin, data + begin + 1, cond);
    hook(data + begin, data + begin + 1, cond);
  }
};

template <std::size_t Length> struct oroffcompact_small {
  static_assert(Length >= 4, "oroffcompact_small requires Length >= 4");
  static_assert((Length & (Length - 1)) == 0, "oroffcompact_small expects power-of-two Length");

  template <typename T, typename PrefixT, typename SwapHook>
  static inline void run(T* data, const PrefixT* prefix, std::size_t begin, std::size_t offset, SwapHook& hook) {
    constexpr std::size_t half = Length / 2;

    const PrefixT left_sum_prefix = prefix_range_sum(prefix, begin, begin + half);
    const std::size_t left_sum = static_cast<std::size_t>(left_sum_prefix);

    const std::size_t left_offset = offset & (half - 1);
    oroffcompact_small<half>::run(data, prefix, begin, left_offset, hook);

    const std::size_t right_begin = begin + half;
    const std::size_t right_offset = (offset + left_sum) & (half - 1);
    oroffcompact_small<half>::run(data, prefix, right_begin, right_offset, hook);

    const std::size_t offset_mod = offset & (half - 1);
    const bool s = ((offset_mod + left_sum) >= half) != (offset >= half);
    const std::size_t boundary = (offset + left_sum) & (half - 1);

    for (std::size_t i = 0; i < half; ++i) {
      const bool ix_val = i >= boundary;
      const bool cond = s != ix_val;
      swap_element(data + begin + i, data + begin + i + half, cond);
      hook(data + begin + i, data + begin + i + half, cond);
    }
  }
};

template <std::size_t Length> struct oroffuncompact_small;

template <> struct oroffuncompact_small<2> {
  template <typename T, typename PrefixT, typename SwapHook>
  static inline void run(T* data, const PrefixT* prefix, std::size_t begin, std::size_t offset, SwapHook& hook) {
    const bool left_mark = static_cast<bool>(mark_at(prefix, begin));
    const bool right_mark = static_cast<bool>(mark_at(prefix, begin + 1));
    const bool cond = (static_cast<bool>((!left_mark) & right_mark)) != static_cast<bool>(offset);
    swap_element(data + begin, data + begin + 1, cond);
    hook(data + begin, data + begin + 1, cond);
  }
};

template <std::size_t Length> struct oroffuncompact_small {
  static_assert(Length >= 4, "oroffuncompact_small requires Length >= 4");
  static_assert((Length & (Length - 1)) == 0, "oroffuncompact_small expects power-of-two Length");

  template <typename T, typename PrefixT, typename SwapHook>
  static inline void run(T* data, const PrefixT* prefix, std::size_t begin, std::size_t offset, SwapHook& hook) {
    constexpr std::size_t half = Length / 2;

    const PrefixT left_sum_prefix = prefix_range_sum(prefix, begin, begin + half);
    const std::size_t left_sum = static_cast<std::size_t>(left_sum_prefix);

    const std::size_t left_offset = offset & (half - 1);
    const std::size_t right_offset = (offset + left_sum) & (half - 1);

    const bool s = ((left_offset + left_sum) >= half) != (offset >= half);
    const std::size_t boundary = (offset + left_sum) & (half - 1);

    for (std::size_t i = 0; i < half; ++i) {
      const bool ix_val = i >= boundary;
      const bool cond = s != ix_val;
      swap_element(data + begin + i, data + begin + i + half, cond);
      hook(data + begin + i, data + begin + i + half, cond);
    }

    oroffuncompact_small<half>::run(data, prefix, begin + half, right_offset, hook);
    oroffuncompact_small<half>::run(data, prefix, begin, left_offset, hook);
  }
};

template <typename T, typename PrefixT, typename SwapHook>
void oroffcompact_slice(
    T* data, const PrefixT* prefix, std::size_t begin, std::size_t length, std::size_t offset, SwapHook& hook
) {
  if (length <= 1) {
    return;
  }

  if (length <= oroffcompact_small_limit && ::sn::sortshuffle::detail::is_pow2(length)) {
    switch (length) {
    case 2:
      oroffcompact_small<2>::run(data, prefix, begin, offset, hook);
      return;
    case 4:
      oroffcompact_small<4>::run(data, prefix, begin, offset, hook);
      return;
    case 8:
      oroffcompact_small<8>::run(data, prefix, begin, offset, hook);
      return;
    case 16:
      oroffcompact_small<16>::run(data, prefix, begin, offset, hook);
      return;
    case 32:
      oroffcompact_small<32>::run(data, prefix, begin, offset, hook);
      return;
    default:
      break;
    }
  }

  if (length == 2) {
    const bool left_mark = static_cast<bool>(mark_at(prefix, begin));
    const bool right_mark = static_cast<bool>(mark_at(prefix, begin + 1));

    const bool cond = (static_cast<bool>((!left_mark) & right_mark)) != static_cast<bool>(offset);
    swap_element(data + begin, data + begin + 1, cond);
    hook(data + begin, data + begin + 1, cond);
    return;
  }

  const std::size_t half = length / 2;

  const PrefixT left_sum_prefix = prefix_range_sum(prefix, begin, begin + half);
  const std::size_t left_sum = static_cast<std::size_t>(left_sum_prefix);

  const std::size_t left_offset = offset & (half - 1);

  oroffcompact_slice(data, prefix, begin, half, left_offset, hook);

  const std::size_t right_begin = begin + half;
  const std::size_t right_offset = (offset + left_sum) & (half - 1);

  oroffcompact_slice(data, prefix, right_begin, half, right_offset, hook);

  const std::size_t offset_mod = offset & (half - 1);
  const bool s = ((offset_mod + left_sum) >= half) != (offset >= half);

  for (std::size_t i = 0; i < half; ++i) {
    const bool ix_val = i >= ((offset + left_sum) & (half - 1));
    const bool cond = s != ix_val;

    swap_element(data + begin + i, data + begin + i + half, cond);
    hook(data + begin + i, data + begin + i + half, cond);
  }
}

template <typename T, typename PrefixT, typename SwapHook>
void oroffuncompact_slice(
    T* data, const PrefixT* prefix, std::size_t begin, std::size_t length, std::size_t offset, SwapHook& hook
) {
  if (length <= 1) {
    return;
  }

  if (length <= oroffcompact_small_limit && ::sn::sortshuffle::detail::is_pow2(length)) {
    switch (length) {
    case 2:
      oroffuncompact_small<2>::run(data, prefix, begin, offset, hook);
      return;
    case 4:
      oroffuncompact_small<4>::run(data, prefix, begin, offset, hook);
      return;
    case 8:
      oroffuncompact_small<8>::run(data, prefix, begin, offset, hook);
      return;
    case 16:
      oroffuncompact_small<16>::run(data, prefix, begin, offset, hook);
      return;
    case 32:
      oroffuncompact_small<32>::run(data, prefix, begin, offset, hook);
      return;
    default:
      break;
    }
  }

  if (length == 2) {
    const bool left_mark = static_cast<bool>(mark_at(prefix, begin));
    const bool right_mark = static_cast<bool>(mark_at(prefix, begin + 1));
    const bool cond = (static_cast<bool>((!left_mark) & right_mark)) != static_cast<bool>(offset);
    swap_element(data + begin, data + begin + 1, cond);
    hook(data + begin, data + begin + 1, cond);
    return;
  }

  const std::size_t half = length / 2;
  const PrefixT left_sum_prefix = prefix_range_sum(prefix, begin, begin + half);
  const std::size_t left_sum = static_cast<std::size_t>(left_sum_prefix);

  const std::size_t left_offset = offset & (half - 1);
  const std::size_t right_offset = (offset + left_sum) & (half - 1);

  const bool s = ((left_offset + left_sum) >= half) != (offset >= half);

  for (std::size_t i = 0; i < half; ++i) {
    const bool ix_val = i >= ((offset + left_sum) & (half - 1));
    const bool cond = s != ix_val;
    swap_element(data + begin + i, data + begin + i + half, cond);
    hook(data + begin + i, data + begin + i + half, cond);
  }

  oroffuncompact_slice(data, prefix, begin + half, half, right_offset, hook);
  oroffuncompact_slice(data, prefix, begin, half, left_offset, hook);
}

template <typename T, typename PrefixT, typename SwapHook>
void orcompact_slice(T* data, const PrefixT* prefix, std::size_t begin, std::size_t length, SwapHook& hook) {
  if (length <= 1) {
    return;
  }

  const std::size_t pow2 = pow2_leq(length);

  const std::size_t left_length = length - pow2;

  std::size_t left_marked = 0;

  if (left_length > 0) {
    const PrefixT left_marked_prefix = prefix_range_sum(prefix, begin, begin + left_length);
    left_marked = static_cast<std::size_t>(left_marked_prefix);

    orcompact_slice<T, PrefixT, SwapHook>(data, prefix, begin, left_length, hook);
  }

  const std::size_t right_begin = begin + left_length;
  const std::size_t right_length = pow2;

  std::size_t right_offset = right_length - left_length + left_marked;
  const bool offset_eq = sn::obliv::ct_eq(right_offset, right_length);
  right_offset = sn::obliv::ct_select<std::size_t>(0, right_offset, offset_eq);

  oroffcompact_slice(data, prefix, right_begin, right_length, right_offset, hook);

  for (std::size_t i = 0; i < left_length; ++i) {
    const bool cond = i >= left_marked;

    swap_element(data + begin + i, data + begin + i + right_length, cond);
    hook(data + begin + i, data + begin + i + right_length, cond);
  }
}

template <typename T, typename PrefixT, typename SwapHook>
void oruncompact_slice(T* data, const PrefixT* prefix, std::size_t begin, std::size_t length, SwapHook& hook) {
  if (length <= 1) {
    return;
  }

  const std::size_t pow2 = pow2_leq(length);
  const std::size_t left_length = length - pow2;

  std::size_t left_marked = 0;
  if (left_length > 0) {
    const PrefixT left_marked_prefix = prefix_range_sum(prefix, begin, begin + left_length);
    left_marked = static_cast<std::size_t>(left_marked_prefix);

    for (std::size_t i = 0; i < left_length; ++i) {
      const bool cond = i >= left_marked;
      swap_element(data + begin + i, data + begin + pow2 + i, cond);

      hook(data + begin + i, data + begin + pow2 + i, cond);
    }
  }

  const std::size_t right_begin = begin + left_length;
  const std::size_t right_length = pow2;
  std::size_t right_offset = right_length - left_length + left_marked;
  const bool offset_eq = sn::obliv::ct_eq(right_offset, right_length);
  right_offset = sn::obliv::ct_select<std::size_t>(0, right_offset, offset_eq);

  oroffuncompact_slice(data, prefix, right_begin, right_length, right_offset, hook);

  if (left_length > 0) {

    oruncompact_slice(data, prefix, begin, left_length, hook);
  }
}

template <typename T, typename PrefixT, typename SwapHook>
void orcompact(T* data, std::size_t length, std::uint8_t* marks, PrefixT* prefix, SwapHook& hook) {
  prefix_sum<PrefixT>(marks, prefix, length);
  orcompact_slice<T, PrefixT, SwapHook>(data, prefix, 0, length, hook);
}

template <typename T, typename PrefixT, typename SwapHook>
void oruncompact(T* data, std::size_t length, std::uint8_t* marks, PrefixT* prefix, SwapHook& hook) {
  prefix_sum<PrefixT>(marks, prefix, length);
  oruncompact_slice<T, PrefixT, SwapHook>(data, prefix, 0, length, hook);
}

template <typename T, typename PrefixT, typename SwapHook>
void orrearrange_slice(
    T* data, const PrefixT* compact_prefix, const PrefixT* uncompact_prefix, std::size_t begin, std::size_t length,
    SwapHook& hook
) {
  if (length <= 1) {
    return;
  }

  orcompact_slice<T, PrefixT, SwapHook>(data, compact_prefix, begin, length, hook);
  oruncompact_slice<T, PrefixT, SwapHook>(data, uncompact_prefix, begin, length, hook);
}

template <typename T, typename PrefixT, typename SwapHook>
void orrearrange(
    T* data, std::size_t length, std::uint8_t* compact_marks, PrefixT* compact_prefix, std::uint8_t* uncompact_marks,
    PrefixT* uncompact_prefix, SwapHook& hook
) {
  prefix_sum<PrefixT>(compact_marks, compact_prefix, length);
  prefix_sum<PrefixT>(uncompact_marks, uncompact_prefix, length);
  orrearrange_slice<T, PrefixT, SwapHook>(data, compact_prefix, uncompact_prefix, 0, length, hook);
}

template <typename Rng> void mark_half(std::uint8_t* marks, std::size_t length, Rng& rng) {

  const std::size_t required = (length + 1) / 2;

  std::size_t assigned = 0;

  for (std::size_t i = 0; i < length; ++i) {

    std::uint32_t rv = 0;
    rng.random_bytes(reinterpret_cast<std::uint8_t*>(&rv), sizeof(rv));

    const std::size_t total_left = length - i;
    const std::size_t remaining = required - assigned;

    const std::size_t lhs = (static_cast<std::uint64_t>(rv) * total_left) >> 32;
    const std::uint8_t mark = static_cast<std::uint8_t>(lhs < remaining);

    marks[i] = mark;
    assigned += mark;
  }
}

template <typename T, typename PrefixT, typename Rng, typename SwapHook>
void orshuffle_slice(
    T* data, std::uint8_t* marks, PrefixT* prefix, std::size_t begin, std::size_t length, Rng& rng, SwapHook& hook
) {
  if (length <= 1) {
    return;
  }

  if (length == 2) {

    std::uint8_t coin = 0;
    rng.random_bytes(&coin, sizeof(coin));
    const bool cond = (coin & 1U) != 0U;
    swap_element(data + begin, data + begin + 1, cond);
    hook(data + begin, data + begin + 1, cond);
    return;
  }

  mark_half(marks + begin, length, rng);

  prefix_sum_slice<PrefixT>(marks, prefix, begin, length);
  orcompact_slice(data, prefix, begin, length, hook);

  const std::size_t left_length = (length + 1) / 2;
  orshuffle_slice<T, PrefixT, Rng, SwapHook>(data, marks, prefix, begin, left_length, rng, hook);

  const std::size_t right_begin = begin + left_length;
  const std::size_t right_length = length - left_length;
  orshuffle_slice<T, PrefixT, Rng, SwapHook>(data, marks, prefix, right_begin, right_length, rng, hook);
}

template <typename T, typename PrefixT, typename Rng, typename SwapHook>
void orshuffle_impl(T* data, std::size_t count, std::uint8_t* marks, PrefixT* prefix, Rng& rng, SwapHook& hook) {

  orshuffle_slice<T, PrefixT, Rng, SwapHook>(data, marks, prefix, 0, count, rng, hook);
}

}

template <typename T, typename PrefixT = std::uint64_t>
inline void orcompact(T* data, std::size_t count, std::uint8_t* marks, PrefixT* prefix) {
#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
  auto hook = [](auto*, auto*, bool) { ++counting::compact_conditional_swaps; };
#else
  detail::noop_hook hook{};
#endif
  detail::orcompact<T, PrefixT, decltype(hook)>(data, count, marks, prefix, hook);
}

template <typename T, typename PrefixT = std::uint64_t>
inline void oruncompact(T* data, std::size_t count, std::uint8_t* marks, PrefixT* prefix) {
  detail::noop_hook hook{};
  detail::oruncompact<T, PrefixT, decltype(hook)>(data, count, marks, prefix, hook);
}

template <typename T, typename PrefixT = std::uint64_t>
inline void orrearrange(
    T* data, std::size_t count, std::uint8_t* compact_marks, PrefixT* compact_prefix, std::uint8_t* uncompact_marks,
    PrefixT* uncompact_prefix
) {
  detail::noop_hook hook{};
  detail::orrearrange<T, PrefixT, decltype(hook)>(
      data, count, compact_marks, compact_prefix, uncompact_marks, uncompact_prefix, hook
  );
}

template <typename T, typename PrefixT = std::uint64_t, typename Rng>
inline void orshuffle(T* data, std::size_t count, std::uint8_t* marks, PrefixT* prefix, Rng& rng) {
  detail::noop_hook hook{};
  detail::orshuffle_impl<T, PrefixT, Rng, decltype(hook)>(data, count, marks, prefix, rng, hook);
}

}
}
}
}
