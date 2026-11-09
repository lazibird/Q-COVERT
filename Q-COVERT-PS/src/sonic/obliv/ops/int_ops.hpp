#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/types/choice.hpp"

namespace sn::obliv {
namespace detail {

template <typename T> inline constexpr bool is_int_v = std::is_integral_v<T> && !std::is_same_v<T, bool>;

template <typename T> inline constexpr bool is_unsigned_int_v = is_int_v<T> && std::is_unsigned_v<T>;

template <typename Backend> struct int_ops_backend {
private:
  using core = core_ops_backend<Backend>;

public:
  template <typename T> [[nodiscard]] static inline T ct_min(const T a, const T b) noexcept {
    static_assert(is_int_v<T>, "ct_min: T must be an integer type (non-bool)");
    const bool a_lt_b = core::template ct_lt<T>(a, b);
    return core::template ct_select<T>(a, b, a_lt_b);
  }

  template <typename T> [[nodiscard]] static inline T ct_max(const T a, const T b) noexcept {
    static_assert(is_int_v<T>, "ct_max: T must be an integer type (non-bool)");
    const bool a_lt_b = core::template ct_lt<T>(a, b);
    return core::template ct_select<T>(b, a, a_lt_b);
  }

  template <typename T> [[nodiscard]] static inline T ct_clamp(const T x, const T lo, const T hi) noexcept {
    static_assert(is_int_v<T>, "ct_clamp: T must be an integer type (non-bool)");
    const T x_ge_lo = ct_max<T>(x, lo);
    return ct_min<T>(x_ge_lo, hi);
  }

  template <typename T> [[nodiscard]] static inline T ct_saturating_sub(const T a, const T b) noexcept {
    static_assert(is_unsigned_int_v<T>, "ct_saturating_sub: T must be an unsigned integer type (non-bool)");
    const bool ge = core::template ct_ge<T>(a, b);
    const T diff = static_cast<T>(a - b);
    return core::template ct_select<T>(diff, T{0}, ge);
  }

  template <typename T> [[nodiscard]] static inline T ct_saturating_add(const T a, const T b) noexcept {
    static_assert(is_unsigned_int_v<T>, "ct_saturating_add: T must be an unsigned integer type (non-bool)");
    const T sum = static_cast<T>(a + b);
    const bool overflow = core::template ct_lt<T>(sum, a);
    return core::template ct_select<T>(std::numeric_limits<T>::max(), sum, overflow);
  }

  template <typename T> [[nodiscard]] static inline choice ct_is_zero(const T x) noexcept {
    static_assert(is_int_v<T>, "ct_is_zero: T must be an integer type (non-bool)");
    return choice(core::template ct_eq<T>(x, T{0}));
  }

  template <typename T> [[nodiscard]] static inline choice ct_is_nonzero(const T x) noexcept {
    static_assert(is_int_v<T>, "ct_is_nonzero: T must be an integer type (non-bool)");
    return !ct_is_zero<T>(x);
  }
};

}

template <typename Backend = default_backend> using int_ops = detail::int_ops_backend<Backend>;

namespace detail {
using default_int_ops = int_ops_backend<default_backend>;
}

template <typename T> [[nodiscard]] inline T ct_min(const T a, const T b) noexcept {
  return detail::default_int_ops::template ct_min<T>(a, b);
}

template <typename T> [[nodiscard]] inline T ct_max(const T a, const T b) noexcept {
  return detail::default_int_ops::template ct_max<T>(a, b);
}

template <typename T> [[nodiscard]] inline T ct_clamp(const T x, const T lo, const T hi) noexcept {
  return detail::default_int_ops::template ct_clamp<T>(x, lo, hi);
}

template <typename T> [[nodiscard]] inline T ct_saturating_sub(const T a, const T b) noexcept {
  return detail::default_int_ops::template ct_saturating_sub<T>(a, b);
}

template <typename T> [[nodiscard]] inline T ct_saturating_add(const T a, const T b) noexcept {
  return detail::default_int_ops::template ct_saturating_add<T>(a, b);
}

template <typename T> [[nodiscard]] inline choice ct_is_zero(const T x) noexcept {
  return detail::default_int_ops::template ct_is_zero<T>(x);
}

template <typename T> [[nodiscard]] inline choice ct_is_nonzero(const T x) noexcept {
  return detail::default_int_ops::template ct_is_nonzero<T>(x);
}

}
