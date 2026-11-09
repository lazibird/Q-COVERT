

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "sonic/obliv/ops/backends/backend_select.hpp"

namespace sn {
namespace obliv {

namespace detail {

template <typename Backend> struct backend_contract {
  using sample_t = uint32_t;
  using sample_ptr = sample_t*;
  using sample_const_ptr = const sample_t*;
  using sample_bool = bool;
  using sample_u8 = uint8_t;
  using sample_u64 = uint64_t;

  static_assert(
      std::is_same<
          decltype(Backend::template ct_eq<sample_t>(std::declval<sample_t>(), std::declval<sample_t>())), bool>::value,
      "backend must implement ct_eq<T>(T, T) -> bool"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_set<
                   sample_t>(std::declval<sample_ptr>(), std::declval<sample_t>(), std::declval<sample_bool>())),
          void>::value,
      "backend must implement ct_set<T>(T*, T, bool) -> void"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_select<
                   sample_t>(std::declval<sample_t>(), std::declval<sample_t>(), std::declval<sample_bool>())),
          sample_t>::value,
      "backend must implement ct_select<T>(T, T, bool) -> T"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_swap<
                   sample_t>(std::declval<sample_ptr>(), std::declval<sample_ptr>(), std::declval<sample_bool>())),
          void>::value,
      "backend must implement ct_swap<T>(T*, T*, bool) -> void"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_set_array<sample_t>(
              std::declval<sample_ptr>(), std::declval<sample_const_ptr>(), std::declval<sample_bool>()
          )),
          void>::value,
      "backend must implement ct_set_array<T>(T*, const T*, bool) -> void"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_select_array<sample_t>(
              std::declval<sample_ptr>(), std::declval<sample_const_ptr>(), std::declval<sample_const_ptr>(),
              std::declval<sample_bool>()
          )),
          void>::value,
      "backend must implement ct_select_array<T>(T*, const T*, const T*, bool) -> void"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_swap_array<
                   sample_t>(std::declval<sample_ptr>(), std::declval<sample_ptr>(), std::declval<sample_bool>())),
          void>::value,
      "backend must implement ct_swap_array<T>(T*, T*, bool) -> void"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_eq_array<sample_t>(
              std::declval<sample_const_ptr>(), std::declval<sample_const_ptr>(), std::declval<size_t>()
          )),
          sample_bool>::value,
      "backend must implement ct_eq_array<T>(const T*, const T*, size_t) -> bool"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_set_array<sample_t>(
              std::declval<sample_ptr>(), std::declval<sample_const_ptr>(), std::declval<size_t>(),
              std::declval<sample_bool>()
          )),
          void>::value,
      "backend must implement ct_set_array<T>(T*, const T*, size_t, bool) -> void"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_select_array<sample_t>(
              std::declval<sample_ptr>(), std::declval<sample_const_ptr>(), std::declval<sample_const_ptr>(),
              std::declval<size_t>(), std::declval<sample_bool>()
          )),
          void>::value,
      "backend ct_select_array"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_swap_array<sample_t>(
              std::declval<sample_ptr>(), std::declval<sample_ptr>(), std::declval<size_t>(),
              std::declval<sample_bool>()
          )),
          void>::value,
      "backend must implement ct_swap_array<T>(T*, T*, size_t, bool) -> void"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_gt<sample_t>(std::declval<sample_t>(), std::declval<sample_t>())), bool>::value,
      "backend must implement ct_gt<T>(T, T) -> bool"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_lt<sample_t>(std::declval<sample_t>(), std::declval<sample_t>())), bool>::value,
      "backend must implement ct_lt<T>(T, T) -> bool"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_le<sample_t>(std::declval<sample_t>(), std::declval<sample_t>())), bool>::value,
      "backend must implement ct_le<T>(T, T) -> bool"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_cmp<sample_t>(std::declval<sample_t>(), std::declval<sample_t>())), int>::value,
      "backend must implement ct_cmp<T>(T, T) -> int"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_div_pow2<sample_t>(std::declval<sample_t>(), std::declval<sample_u8>())),
          sample_t>::value,
      "backend must implement ct_div_pow2<T>(T, uint8_t) -> T"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_mod_pow2<sample_t>(std::declval<sample_t>(), std::declval<sample_u8>())),
          sample_t>::value,
      "backend must implement ct_mod_pow2<T>(T, uint8_t) -> T"
  );

  static_assert(
      std::is_same<
          decltype(Backend::template ct_mul_pow2<sample_t>(std::declval<sample_t>(), std::declval<sample_u8>())),
          sample_t>::value,
      "backend must implement ct_mul_pow2<T>(T, uint8_t) -> T"
  );

  static_assert(
      std::is_same<decltype(Backend::template ct_pow2<sample_t>(std::declval<sample_u8>())), sample_t>::value,
      "backend must implement ct_pow2<T>(uint8_t) -> T"
  );

  static_assert(
      std::is_same<decltype(Backend::ct_log2(std::declval<sample_u64>())), int>::value,
      "backend must implement ct_log2(uint64_t) -> int"
  );

  static_assert(
      std::is_same<
          decltype(Backend::ct_madd(
              std::declval<sample_u64>(), std::declval<sample_u64>(), std::declval<sample_u64>()
          )),
          sample_u64>::value,
      "backend must implement ct_madd(uint64_t, uint64_t, uint64_t) -> uint64_t"
  );

  static_assert(
      std::is_same<decltype(Backend::ct_count_leading_zeros(std::declval<sample_u64>())), sample_u64>::value,
      "backend must implement ct_count_leading_zeros(uint64_t) -> uint64_t"
  );

  static constexpr bool value = true;
};

template <typename Backend> struct core_ops_backend {
  static_assert(backend_contract<Backend>::value, "backend does not satisfy core ops contract");

  template <typename T> static inline bool ct_eq(const T a, const T b) { return Backend::template ct_eq<T>(a, b); }

  template <typename T> static inline bool ct_eq_array(const T* a, const T* b, size_t n) {
    static_assert(std::is_integral<T>::value, "ct_eq_array: T must be an integral type");
    return Backend::template ct_eq_array<T>(a, b, n);
  }

  template <typename T> static inline void ct_set(T* a, const T b, bool cond) {
    Backend::template ct_set<T>(a, b, cond);
  }

  template <typename T> static inline void ct_set_ref(T& a, const T b, bool cond) {
    Backend::template ct_set<T>(&a, b, cond);
  }

  template <typename T> static inline T ct_select(const T a, const T b, bool cond) {
    return Backend::template ct_select<T>(a, b, cond);
  }

  template <typename T> static inline void ct_select_ref(T& out, const T a, const T b, bool cond) {
    out = Backend::template ct_select<T>(a, b, cond);
  }

  template <typename T> static inline void ct_set_array(T* dst, const T* src, bool cond) {
    Backend::template ct_set_array<T>(dst, src, cond);
  }

  template <typename T> static inline void ct_select_array(T* out, const T* a, const T* b, bool cond) {
    Backend::template ct_select_array<T>(out, a, b, cond);
  }

  template <typename T> static inline void ct_set_array(T* dst, const T* src, size_t n, bool cond) {
    Backend::template ct_set_array<T>(dst, src, n, cond);
  }

  template <typename T> static inline void ct_select_array(T* out, const T* a, const T* b, size_t n, bool cond) {
    Backend::template ct_select_array<T>(out, a, b, n, cond);
  }

  template <typename T> static inline void ct_swap(T* a, T* b, bool cond) { Backend::template ct_swap<T>(a, b, cond); }

  template <typename T> static inline void ct_swap_array(T* a, T* b, size_t n, bool cond) {
    Backend::template ct_swap_array<T>(a, b, n, cond);
  }

  template <typename T> static inline void ct_swap_array(T* a, T* b, bool cond) {
    Backend::template ct_swap_array<T>(a, b, cond);
  }

  template <typename T> static inline bool ct_gt(const T a, const T b) { return Backend::template ct_gt<T>(a, b); }

  template <typename T> static inline bool ct_lt(const T a, const T b) { return Backend::template ct_lt<T>(a, b); }

  template <typename T> static inline bool ct_le(const T a, const T b) { return Backend::template ct_le<T>(a, b); }

  template <typename T> static inline bool ct_ge(const T a, const T b) { return !Backend::template ct_lt<T>(a, b); }

  template <typename T> static inline int ct_cmp(const T a, const T b) { return Backend::template ct_cmp<T>(a, b); }

  template <typename T> static inline T ct_div_pow2(const T a, const uint8_t log2_divisor) {
    return Backend::template ct_div_pow2<T>(a, log2_divisor);
  }

  template <typename T> static inline T ct_mod_pow2(const T a, const uint8_t log2_divisor) {
    return Backend::template ct_mod_pow2<T>(a, log2_divisor);
  }

  template <typename T> static inline T ct_mul_pow2(const T a, const uint8_t log2_multiplier) {
    return Backend::template ct_mul_pow2<T>(a, log2_multiplier);
  }

  template <typename T> static inline T ct_pow2(const uint8_t exponent) {
    return Backend::template ct_pow2<T>(exponent);
  }

  static inline int ct_log2(const uint64_t value) { return Backend::ct_log2(value); }

  static inline uint64_t ct_madd(const uint64_t a, const uint64_t b, const uint64_t c) {
    return Backend::ct_madd(a, b, c);
  }

  static inline uint64_t ct_count_leading_zeros(uint64_t x) { return Backend::ct_count_leading_zeros(x); }
};

}

using default_backend = detail::default_backend;

template <typename Backend = default_backend> using core_ops = detail::core_ops_backend<Backend>;

namespace detail {
using default_core_ops = core_ops_backend<default_backend>;
}

template <typename T> inline bool ct_eq(const T a, const T b) {
  return detail::default_core_ops::template ct_eq<T>(a, b);
}

template <typename T> inline bool ct_eq_array(const T* a, const T* b, size_t n) {
  return detail::default_core_ops::template ct_eq_array<T>(a, b, n);
}

template <typename T> inline void ct_set(T* a, const T b, bool cond) {
  detail::default_core_ops::template ct_set<T>(a, b, cond);
}

template <typename T> inline void ct_set_ref(T& a, const T b, bool cond) {
  detail::default_core_ops::template ct_set_ref<T>(a, b, cond);
}

template <typename T> inline void ct_set_array(T* dst, const T* src, size_t n, bool cond) {
  detail::default_core_ops::template ct_set_array<T>(dst, src, n, cond);
}

template <typename T> inline T ct_select(const T a, const T b, bool cond) {
  return detail::default_core_ops::template ct_select<T>(a, b, cond);
}

template <typename T> inline void ct_select_ref(T& out, const T a, const T b, bool cond) {
  detail::default_core_ops::template ct_select_ref<T>(out, a, b, cond);
}

template <typename T> inline void ct_select_array(T* out, const T* a, const T* b, size_t n, bool cond) {
  detail::default_core_ops::template ct_select_array<T>(out, a, b, n, cond);
}

template <typename T> inline void ct_swap(T* a, T* b, bool cond) {
  detail::default_core_ops::template ct_swap<T>(a, b, cond);
}

template <typename T> inline void ct_swap_array(T* a, T* b, size_t n, bool cond) {
  detail::default_core_ops::template ct_swap_array<T>(a, b, n, cond);
}

template <typename T> inline bool ct_gt(const T a, const T b) {
  return detail::default_core_ops::template ct_gt<T>(a, b);
}

template <typename T> inline bool ct_lt(const T a, const T b) {
  return detail::default_core_ops::template ct_lt<T>(a, b);
}

template <typename T> inline bool ct_le(const T a, const T b) {
  return detail::default_core_ops::template ct_le<T>(a, b);
}

template <typename T> inline bool ct_ge(const T a, const T b) {
  return detail::default_core_ops::template ct_ge<T>(a, b);
}

template <typename T> inline int ct_cmp(const T a, const T b) {
  return detail::default_core_ops::template ct_cmp<T>(a, b);
}

template <typename T> inline T ct_div_pow2(const T a, const uint8_t log2_divisor) {
  return detail::default_core_ops::template ct_div_pow2<T>(a, log2_divisor);
}

template <typename T> inline T ct_mod_pow2(const T a, const uint8_t log2_divisor) {
  return detail::default_core_ops::template ct_mod_pow2<T>(a, log2_divisor);
}

template <typename T> inline T ct_mul_pow2(const T a, const uint8_t log2_multiplier) {
  return detail::default_core_ops::template ct_mul_pow2<T>(a, log2_multiplier);
}

template <typename T> inline T ct_pow2(const uint8_t exponent) {
  return detail::default_core_ops::template ct_pow2<T>(exponent);
}

inline int ct_log2(const uint64_t value) { return detail::default_core_ops::ct_log2(value); }

inline uint64_t ct_madd(const uint64_t a, const uint64_t b, const uint64_t c) {
  return detail::default_core_ops::ct_madd(a, b, c);
}

inline uint64_t ct_count_leading_zeros(uint64_t value) {
  return detail::default_core_ops::ct_count_leading_zeros(value);
}

}
}
