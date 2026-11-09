#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sn {
namespace obliv {
namespace backends {

struct backend_cpp final {
  static constexpr const char* name = "cpp";
  static constexpr bool supports_uint128 = false;
  static constexpr bool supports_uint256 = false;
  static constexpr bool supports_uint512 = false;

  using uint128_t = void;
  using uint256_t = void;
  using uint512_t = void;

  template <typename T> static inline void ct_set(T* out, T src, bool cond) {
    static_assert(std::is_integral<T>::value, "ct_set: T must be an integral type");
    using UT = std::make_unsigned_t<T>;
    auto* uout = reinterpret_cast<UT*>(out);
    const UT usrc = static_cast<UT>(src);
    const UT mask = cond ? ~UT(0) : UT(0);
    *uout ^= (usrc ^ *uout) & mask;
  }

  template <typename T> static inline void ct_swap(T* a, T* b, bool cond) {
    static_assert(std::is_integral<T>::value, "ct_swap: T must be an integral type");
    using UT = std::make_unsigned_t<T>;
    auto* ua = reinterpret_cast<UT*>(a);
    auto* ub = reinterpret_cast<UT*>(b);
    const UT mask = cond ? ~UT(0) : UT(0);
    *ua ^= *ub;
    *ub ^= (*ua & mask);
    *ua ^= *ub;
  }

  template <typename T> static inline bool ct_eq(const T a, const T b) {
    static_assert(std::is_integral<T>::value, "ct_eq: T must be an integral type");
    uint8_t result = 0;
    ct_set(&result, static_cast<uint8_t>(1), a == b);
    return result != 0;
  }

  template <typename T> static inline bool ct_gt(const T a, const T b) {
    static_assert(std::is_integral<T>::value, "ct_gt: T must be an integral type");
    uint8_t result = 0;
    ct_set(&result, static_cast<uint8_t>(1), a > b);
    return result != 0;
  }

  template <typename T> static inline bool ct_lt(const T a, const T b) {
    static_assert(std::is_integral<T>::value, "ct_lt: T must be an integral type");
    uint8_t result = 0;
    ct_set(&result, static_cast<uint8_t>(1), a < b);
    return result != 0;
  }

  template <typename T> static inline bool ct_le(const T a, const T b) {
    static_assert(std::is_integral<T>::value, "ct_le: T must be an integral type");
    uint8_t result = 0;
    ct_set(&result, static_cast<uint8_t>(1), a <= b);
    return result != 0;
  }

  template <typename T> static inline int ct_cmp(const T a, const T b) {
    static_assert(std::is_integral<T>::value, "ct_cmp: T must be an integral type");
    int result = 0;
    ct_set(&result, 1, a > b);
    ct_set(&result, -1, a < b);
    return result;
  }

  template <typename T> static inline T ct_select(const T a, const T b, bool cond) {
    static_assert(std::is_integral<T>::value, "ct_select: T must be an integral type");
    using UT = std::make_unsigned_t<T>;
    const UT ua = static_cast<UT>(a);
    const UT ub = static_cast<UT>(b);
    const UT mask = cond ? ~UT(0) : UT(0);
    return static_cast<T>((ua & mask) | (ub & ~mask));
  }

  template <typename T> static inline T ct_div_pow2(const T a, const uint8_t log2_divisor) {
    static_assert(std::is_integral<T>::value, "ct_div_pow2: T must be an integral type");
    return static_cast<T>(a >> log2_divisor);
  }

  template <typename T> static inline T ct_mod_pow2(const T a, const uint8_t log2_divisor) {
    static_assert(std::is_integral<T>::value, "ct_mod_pow2: T must be an integral type");
    using UT = std::make_unsigned_t<T>;
    const UT mask = (UT(1) << log2_divisor) - 1;
    return static_cast<T>(static_cast<UT>(a) & mask);
  }

  template <typename T> static inline T ct_mul_pow2(const T a, const uint8_t log2_multiplier) {
    static_assert(std::is_integral<T>::value, "ct_mul_pow2: T must be an integral type");
    using UT = std::make_unsigned_t<T>;
    return static_cast<T>(static_cast<UT>(a) << log2_multiplier);
  }

  template <typename T> static inline T ct_pow2(const uint8_t exponent) {
    static_assert(std::is_integral<T>::value, "ct_pow2: T must be an integral type");
    return static_cast<T>(T(1) << exponent);
  }

  static inline uint64_t ct_madd(const uint64_t a, const uint64_t b, const uint64_t c) { return (a * b) + c; }

  static inline int ct_log2(const uint64_t value) {
    if (value == 0) {
      return -1;
    }
    return 63 - __builtin_clzll(value);
  }

  static inline uint64_t ct_count_leading_zeros(uint64_t x) {
    constexpr uint64_t zero = 0;
    constexpr uint64_t dummy = ~zero;
    const bool is_zero = x == zero;
    ct_set(&x, dummy, is_zero);
    uint64_t result = __builtin_clzll(x);
    ct_set(&result, static_cast<uint64_t>(64), is_zero);
    return result;
  }

  template <typename T> static inline void ct_set_array(T* dst, const T* src, bool cond) {
    static_assert(std::is_integral_v<T>, "ct_set_array: T must be an integral type");
    ct_set(dst, *src, cond);
  }

  template <typename T> static inline void ct_select_array(T* out, const T* a, const T* b, bool cond) {
    static_assert(std::is_integral_v<T>, "ct_select_array: T must be an integral type");
    *out = ct_select(*a, *b, cond);
  }

  template <typename T> static inline void ct_swap_array(T* a, T* b, bool cond) {
    static_assert(std::is_integral_v<T>, "ct_swap_array: T must be an integral type");
    ct_swap(a, b, cond);
  }

  template <typename T> static inline bool ct_eq_array(const T* a, const T* b, size_t count) {
    static_assert(std::is_integral_v<T>, "ct_eq_array: T must be an integral type");
    uint8_t accumulator = 1;
    for (size_t i = 0; i < count; ++i) {
      accumulator &= static_cast<uint8_t>(ct_eq(a[i], b[i]));
    }
    return accumulator != 0;
  }

  template <typename T> static inline void ct_set_array(T* dst, const T* src, size_t count, bool cond) {
    static_assert(std::is_integral_v<T>, "ct_set_array: T must be an integral type");
    using UT = std::make_unsigned_t<T>;
    const UT mask = cond ? ~UT(0) : UT(0);
    auto* d = reinterpret_cast<UT*>(dst);
    auto const* s = reinterpret_cast<const UT*>(src);
    for (size_t i = 0; i < count; ++i) {
      const UT sv = s[i];
      UT dv = d[i];
      d[i] = dv ^ ((sv ^ dv) & mask);
    }
  }

  template <typename T> static inline void ct_select_array(T* out, const T* a, const T* b, size_t count, bool cond) {
    static_assert(std::is_integral_v<T>, "ct_select_array: T must be an integral type");
    using UT = std::make_unsigned_t<T>;
    const UT mask = cond ? ~UT(0) : UT(0);
    auto* dst = reinterpret_cast<UT*>(out);
    auto const* lhs = reinterpret_cast<const UT*>(a);
    auto const* rhs = reinterpret_cast<const UT*>(b);
    for (size_t i = 0; i < count; ++i) {
      dst[i] = (lhs[i] & mask) | (rhs[i] & ~mask);
    }
  }

  template <typename T> static inline void ct_swap_array(T* a, T* b, size_t count, bool cond) {
    static_assert(std::is_integral_v<T>, "ct_swap_array: T must be an integral type");
    using UT = std::make_unsigned_t<T>;
    const UT mask = cond ? ~UT(0) : UT(0);
    auto* lhs = reinterpret_cast<UT*>(a);
    auto* rhs = reinterpret_cast<UT*>(b);
    for (size_t i = 0; i < count; ++i) {
      const UT diff = (lhs[i] ^ rhs[i]) & mask;
      lhs[i] ^= diff;
      rhs[i] ^= diff;
    }
  }
};

}
}
}
