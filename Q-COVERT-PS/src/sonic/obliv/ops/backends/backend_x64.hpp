

#pragma once

#include <cstdint>
#include <type_traits>

#include <immintrin.h>
#include <cstring>

#if !defined(__x86_64__) && !defined(_M_X64)
#error "backend_x64 requires an x86-64 target"
#endif

namespace sn {
namespace obliv {
namespace backends {

struct backend_x64 final {
  static constexpr const char* name = "x64";

  [[nodiscard]] static inline __attribute__((always_inline)) __mmask8 bool_to_mask8(const bool cond) {
    return static_cast<__mmask8>(0 - static_cast<unsigned int>(cond));
  }

#ifdef __SSE2__
#define SN_OBLIV_CT_SUPPORTS_UINT128 1
#endif
#ifdef __AVX2__
#define SN_OBLIV_CT_SUPPORTS_UINT256 1
#endif
#ifdef __AVX512F__
#define SN_OBLIV_CT_SUPPORTS_UINT512 1
#endif

#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
  using uint128_t = __m128i;
  static constexpr bool supports_uint128 = true;
#else
  using uint128_t = void;
  static constexpr bool supports_uint128 = false;
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
  using uint256_t = __m256i;
  static constexpr bool supports_uint256 = true;
#else
  using uint256_t = void;
  static constexpr bool supports_uint256 = false;
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
  using uint512_t = __m512i;
  static constexpr bool supports_uint512 = true;
#else
  using uint512_t = void;
  static constexpr bool supports_uint512 = false;
#endif

  struct condition_masks {
    bool cond;
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    __m512i vec512;
    __mmask8 blend64;
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    __m256i vec256;
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    __m128i vec128;
#endif
    uint64_t mask64;
    uint32_t mask32;
    uint8_t mask8;
  };

  [[nodiscard]] static inline __attribute__((always_inline)) condition_masks make_condition_masks(bool cond) {
    condition_masks masks{};
    masks.cond = cond;
    const uint64_t mask64 = 0ULL - static_cast<uint64_t>(cond);
    masks.mask64 = mask64;
    masks.mask32 = static_cast<uint32_t>(mask64);
    masks.mask8 = static_cast<uint8_t>(mask64);
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    const long long lane = -static_cast<long long>(cond);
    masks.vec512 = _mm512_set1_epi64(lane);
    masks.blend64 = cond ? 0xFF : 0x00;
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    masks.vec256 = _mm256_set1_epi64x(-static_cast<long long>(cond));
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    masks.vec128 = _mm_set1_epi64x(-static_cast<long long>(cond));
#endif
    return masks;
  }

#if defined(SN_OBLIV_CT_SUPPORTS_UINT512) && defined(__AVX512BW__)
  [[nodiscard]] static inline __attribute__((always_inline)) __mmask64 byte_mask_upto(std::size_t count) {
    if (count >= 64) {
      return static_cast<__mmask64>(~0ULL);
    }
    if (count == 0) {
      return 0;
    }
    return (static_cast<__mmask64>(1) << count) - 1;
  }
#endif

  static inline __attribute__((always_inline)) uint64_t
  blend_scalar_bits(uint64_t when_true, uint64_t when_false, bool cond) {
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    const __m128i vec_true = _mm_set1_epi64x(static_cast<long long>(when_true));
    const __m128i vec_false = _mm_set1_epi64x(static_cast<long long>(when_false));
#if defined(__SSE4_1__)
    const __m128i mask = _mm_set1_epi64x(-static_cast<long long>(cond));
    const __m128i blended = _mm_blendv_epi8(vec_false, vec_true, mask);
#else
    const __m128i mask = _mm_set1_epi64x(-static_cast<long long>(cond));
    const __m128i blended = _mm_or_si128(_mm_and_si128(mask, vec_true), _mm_andnot_si128(mask, vec_false));
#endif
    return static_cast<uint64_t>(_mm_cvtsi128_si64(blended));
#else
    const uint64_t mask = static_cast<uint64_t>(0) - static_cast<uint64_t>(cond);
    return (when_true & mask) | (when_false & ~mask);
#endif
  }

  template <typename T> static inline __attribute__((always_inline)) uint64_t load_scalar_bits(const T* ptr) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "load_scalar_bits expects integral scalars");
    if constexpr (sizeof(T) == 8) {
      return static_cast<uint64_t>(*ptr);
    } else if constexpr (sizeof(T) == 4) {
      return static_cast<uint64_t>(static_cast<uint32_t>(*ptr));
    } else if constexpr (sizeof(T) == 2) {
      return static_cast<uint64_t>(static_cast<uint16_t>(*ptr));
    } else {
      return static_cast<uint64_t>(static_cast<uint8_t>(*ptr));
    }
  }

  template <typename T> static inline __attribute__((always_inline)) uint64_t value_bits(const T& value) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "value_bits expects integral scalars");
    return load_scalar_bits(&value);
  }

  template <typename T> static inline __attribute__((always_inline)) void store_scalar_bits(T* ptr, uint64_t bits) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "store_scalar_bits expects integral scalars");
    if constexpr (sizeof(T) == 8) {
      *ptr = static_cast<T>(bits);
    } else if constexpr (sizeof(T) == 4) {
      *ptr = static_cast<T>(static_cast<uint32_t>(bits));
    } else if constexpr (sizeof(T) == 2) {
      *ptr = static_cast<T>(static_cast<uint16_t>(bits));
    } else {
      *ptr = static_cast<T>(static_cast<uint8_t>(bits));
    }
  }

  template <typename T> static inline __attribute__((always_inline)) T make_scalar_from_bits(uint64_t bits) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "make_scalar_from_bits expects integral scalars");
    if constexpr (sizeof(T) == 8) {
      return static_cast<T>(bits);
    } else if constexpr (sizeof(T) == 4) {
      return static_cast<T>(static_cast<uint32_t>(bits));
    } else if constexpr (sizeof(T) == 2) {
      return static_cast<T>(static_cast<uint16_t>(bits));
    } else {
      return static_cast<T>(static_cast<uint8_t>(bits));
    }
  }

  template <typename T> static inline __attribute__((always_inline)) void ct_setc_scalar(T* out, T src, bool cond) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "ct_setc_scalar only supports integral scalar types");
    uint64_t dst_ext = load_scalar_bits(out);
    const uint64_t src_ext = value_bits(src);
    const uint64_t blended = blend_scalar_bits(src_ext, dst_ext, cond);
    store_scalar_bits(out, blended);
  }

  template <typename T> static inline __attribute__((always_inline)) T ct_select_scalar(T a, T b, bool cond) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "ct_select_scalar only supports integral scalar types");
    const uint64_t a_ext = value_bits(a);
    const uint64_t b_ext = value_bits(b);
    const uint64_t blended = blend_scalar_bits(a_ext, b_ext, cond);
    return make_scalar_from_bits<T>(blended);
  }

  template <typename T> static inline __attribute__((always_inline)) void ct_swap_scalar(T* a, T* b, bool cond) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "ct_swap_scalar only supports integral scalar types");
    uint64_t ext_a = load_scalar_bits(a);
    uint64_t ext_b = load_scalar_bits(b);
    const uint64_t next_a = blend_scalar_bits(ext_b, ext_a, cond);
    const uint64_t next_b = blend_scalar_bits(ext_a, ext_b, cond);
    store_scalar_bits(a, next_a);
    store_scalar_bits(b, next_b);
  }

  static inline __attribute__((always_inline)) void ct_set_u64_asm(
      uint8_t* dst_bytes, const uint8_t* src_bytes, uint64_t mask64
  ) {
    uint64_t dst_word;
    uint64_t src_word;
    uint64_t diff;
    asm volatile("mov %[dst_word], qword ptr [%[dst]]\n\t"
                 "mov %[src_word], qword ptr [%[src]]\n\t"
                 "mov %[diff], %[src_word]\n\t"
                 "xor %[diff], %[dst_word]\n\t"
                 "and %[diff], %[mask]\n\t"
                 "xor %[dst_word], %[diff]\n\t"
                 "mov qword ptr [%[dst]], %[dst_word]\n\t"
                 : [dst_word] "=&r"(dst_word), [src_word] "=&r"(src_word), [diff] "=&r"(diff)
                 : [dst] "r"(dst_bytes), [src] "r"(src_bytes), [mask] "r"(mask64)
                 : "cc", "memory");
  }

  static inline __attribute__((always_inline)) void ct_select_u64_asm(
      uint8_t* out_bytes, const uint8_t* a_bytes, const uint8_t* b_bytes, uint64_t mask64
  ) {
    uint64_t a_word;
    uint64_t b_word;
    uint64_t diff;
    uint64_t out_word;
    asm volatile("mov %[a_word], qword ptr [%[a]]\n\t"
                 "mov %[b_word], qword ptr [%[b]]\n\t"
                 "mov %[diff], %[a_word]\n\t"
                 "xor %[diff], %[b_word]\n\t"
                 "and %[diff], %[mask]\n\t"
                 "mov %[out_word], %[b_word]\n\t"
                 "xor %[out_word], %[diff]\n\t"
                 "mov qword ptr [%[out]], %[out_word]\n\t"
                 : [a_word] "=&r"(a_word), [b_word] "=&r"(b_word), [diff] "=&r"(diff), [out_word] "=&r"(out_word)
                 : [a] "r"(a_bytes), [b] "r"(b_bytes), [mask] "r"(mask64), [out] "r"(out_bytes)
                 : "cc", "memory");
  }

  static inline __attribute__((always_inline)) void ct_swap_u64_asm(
      uint8_t* a_bytes, uint8_t* b_bytes, uint64_t mask64
  ) {
    uint64_t a_word;
    uint64_t b_word;
    uint64_t diff;
    asm volatile("mov %[a_word], qword ptr [%[a]]\n\t"
                 "mov %[b_word], qword ptr [%[b]]\n\t"
                 "mov %[diff], %[a_word]\n\t"
                 "xor %[diff], %[b_word]\n\t"
                 "and %[diff], %[mask]\n\t"
                 "xor %[a_word], %[diff]\n\t"
                 "xor %[b_word], %[diff]\n\t"
                 "mov qword ptr [%[a]], %[a_word]\n\t"
                 "mov qword ptr [%[b]], %[b_word]\n\t"
                 : [a_word] "=&r"(a_word), [b_word] "=&r"(b_word), [diff] "=&r"(diff)
                 : [a] "r"(a_bytes), [b] "r"(b_bytes), [mask] "r"(mask64)
                 : "cc", "memory");
  }

#ifdef SN_OBLIV_CT_SUPPORTS_UINT128

  static inline void ct_setc_u128(uint128_t* out, uint128_t src, bool cond) {
    const int64_t mask_scalar = -static_cast<int64_t>(cond);
    const __m128i mask = _mm_set1_epi64x(mask_scalar);
    const __m128i val_out = _mm_load_si128(out);
#if defined(__SSE4_1__)

    const __m128i result = _mm_blendv_epi8(val_out, src, mask);
#else

    const __m128i result = _mm_or_si128(_mm_and_si128(mask, src), _mm_andnot_si128(mask, val_out));
#endif
    _mm_store_si128(out, result);
  }
#endif

#ifdef SN_OBLIV_CT_SUPPORTS_UINT256

  static inline void ct_setc_u256(uint256_t* out, uint256_t src, bool cond) {
    const int64_t mask_scalar = -static_cast<int64_t>(cond);
    const __m256i mask = _mm256_set1_epi64x(mask_scalar);
    const __m256i val_out = _mm256_load_si256(out);
    const __m256i result = _mm256_blendv_epi8(val_out, src, mask);
    _mm256_store_si256(out, result);
  }
#endif

#ifdef SN_OBLIV_CT_SUPPORTS_UINT512

  static inline void ct_setc_u512(uint512_t* out, uint512_t src, bool cond) {
    const __mmask8 k = bool_to_mask8(cond);
    const __m512i val_out = _mm512_load_si512(out);

    const __m512i result = _mm512_mask_mov_epi64(val_out, k, src);
    _mm512_store_si512(out, result);
  }
#endif


  template <typename T> static inline void ct_setc_prim(T* out, T src, bool cond) {
    if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
      ct_setc_scalar(out, src, cond);
    }
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    else if constexpr (sizeof(T) == 16 && alignof(T) >= 16 && !std::is_integral_v<T>) {
      ct_setc_u128(reinterpret_cast<uint128_t*>(out), reinterpret_cast<const uint128_t&>(src), cond);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    else if constexpr (sizeof(T) == 32 && alignof(T) >= 32 && !std::is_integral_v<T>) {
      ct_setc_u256(reinterpret_cast<uint256_t*>(out), reinterpret_cast<const uint256_t&>(src), cond);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    else if constexpr (sizeof(T) == 64 && alignof(T) >= 64 && !std::is_integral_v<T>) {
      ct_setc_u512(reinterpret_cast<uint512_t*>(out), reinterpret_cast<const uint512_t&>(src), cond);
    }
#endif
    else {
      static_assert(sizeof(T) < 0, "unsupported type for ct_setc_prim: must be integral or SIMD vector type");
    }
  }


  template <typename T> static inline void ct_swapc_prim(T* a, T* b, bool cond) {
    if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
      ct_swap_scalar(a, b, cond);
    }
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    else if constexpr (sizeof(T) == 16 && alignof(T) >= 16 && !std::is_integral_v<T>) {

      const __m128i mask = _mm_set1_epi64x(-static_cast<int64_t>(cond));
      const __m128i val_a = _mm_load_si128(reinterpret_cast<const uint128_t*>(a));
      const __m128i val_b = _mm_load_si128(reinterpret_cast<const uint128_t*>(b));
      const __m128i diff = _mm_xor_si128(val_a, val_b);
      const __m128i masked_diff = _mm_and_si128(diff, mask);
      _mm_store_si128(reinterpret_cast<uint128_t*>(a), _mm_xor_si128(val_a, masked_diff));
      _mm_store_si128(reinterpret_cast<uint128_t*>(b), _mm_xor_si128(val_b, masked_diff));
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    else if constexpr (sizeof(T) == 32 && alignof(T) >= 32 && !std::is_integral_v<T>) {

      const __m256i mask = _mm256_set1_epi64x(-static_cast<int64_t>(cond));
      const __m256i val_a = _mm256_load_si256(reinterpret_cast<const uint256_t*>(a));
      const __m256i val_b = _mm256_load_si256(reinterpret_cast<const uint256_t*>(b));
      const __m256i diff = _mm256_xor_si256(val_a, val_b);
      const __m256i masked_diff = _mm256_and_si256(diff, mask);
      _mm256_store_si256(reinterpret_cast<uint256_t*>(a), _mm256_xor_si256(val_a, masked_diff));
      _mm256_store_si256(reinterpret_cast<uint256_t*>(b), _mm256_xor_si256(val_b, masked_diff));
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    else if constexpr (sizeof(T) == 64 && alignof(T) >= 64 && !std::is_integral_v<T>) {

      const __m512i mask = _mm512_set1_epi64(-static_cast<long long>(cond));
      const __m512i val_a = _mm512_load_si512(reinterpret_cast<const uint512_t*>(a));
      const __m512i val_b = _mm512_load_si512(reinterpret_cast<const uint512_t*>(b));
      const __m512i diff = _mm512_xor_si512(val_a, val_b);
      const __m512i masked_diff = _mm512_and_si512(mask, diff);
      _mm512_store_si512(reinterpret_cast<uint512_t*>(a), _mm512_xor_si512(val_a, masked_diff));
      _mm512_store_si512(reinterpret_cast<uint512_t*>(b), _mm512_xor_si512(val_b, masked_diff));
    }
#endif
    else {
      static_assert(sizeof(T) < 0, "unsupported type for ct_swapc_prim: must be integral or SIMD vector type");
    }
  }


  template <typename T> static inline __attribute__((always_inline, const)) bool ct_gt_prim(const T a, const T b) {
    static_assert(std::is_integral_v<T>, "ct_gt_prim: T must be an integral type");
    uint8_t result;
    if constexpr (std::is_signed_v<T>) {

      __asm__("cmp %[a], %[b]\n\t"
              "setg %[result]\n\t"
              : [result] "=q"(result)
              : [a] "r"(a), [b] "r"(b)
              : "cc");
    } else {

      __asm__("cmp %[a], %[b]\n\t"
              "seta %[result]\n\t"
              : [result] "=q"(result)
              : [a] "r"(a), [b] "r"(b)
              : "cc");
    }
    return static_cast<bool>(result);
  }


  template <typename T> static inline __attribute__((always_inline, const)) bool ct_eq_prim(const T a, const T b) {
    static_assert(std::is_integral_v<T>, "ct_eq_prim: T must be an integral type");
    uint8_t result;

    __asm__("cmp %[a], %[b]\n\t"
            "sete %[result]\n\t"
            : [result] "=q"(result)
            : [a] "r"(a), [b] "r"(b)
            : "cc");
    return static_cast<bool>(result);
  }


  template <typename T> static inline __attribute__((always_inline, const)) int ct_cmp_prim(const T a, const T b) {
    static_assert(std::is_integral_v<T>, "ct_cmp_prim: T must be an integral type");
    int64_t res = 0;
    const int64_t one = 1;
    const int64_t neg_one = -1;

    if constexpr (std::is_signed_v<T>) {

      __asm__("cmp %[a], %[b]\n\t"
              "cmovg %[res], %[one]\n\t"
              "cmovl %[res], %[neg]\n\t"
              : [res] "+r"(res)
              : [a] "r"(a), [b] "r"(b), [one] "r"(one), [neg] "r"(neg_one)
              : "cc");
    } else {

      __asm__("cmp %[a], %[b]\n\t"
              "cmova %[res], %[one]\n\t"
              "cmovb %[res], %[neg]\n\t"
              : [res] "+r"(res)
              : [a] "r"(a), [b] "r"(b), [one] "r"(one), [neg] "r"(neg_one)
              : "cc");
    }
    return (int) res;
  }


  template <typename T> static inline __attribute__((always_inline, const)) bool ct_lt_prim(const T a, const T b) {
    static_assert(std::is_integral_v<T>, "ct_lt_prim: T must be an integral type");
    uint8_t result;
    if constexpr (std::is_signed_v<T>) {
      __asm__("cmp %[a], %[b]\n\t"
              "setl %[result]\n\t"
              : [result] "=q"(result)
              : [a] "r"(a), [b] "r"(b)
              : "cc");
    } else {
      __asm__("cmp %[a], %[b]\n\t"
              "setb %[result]\n\t"
              : [result] "=q"(result)
              : [a] "r"(a), [b] "r"(b)
              : "cc");
    }
    return static_cast<bool>(result);
  }


  template <typename T> static inline __attribute__((always_inline, const)) bool ct_le_prim(const T a, const T b) {
    static_assert(std::is_integral_v<T>, "ct_le_prim: T must be an integral type");
    uint8_t result;
    if constexpr (std::is_signed_v<T>) {
      __asm__("cmp %[a], %[b]\n\t"
              "setle %[result]\n\t"
              : [result] "=q"(result)
              : [a] "r"(a), [b] "r"(b)
              : "cc");
    } else {
      __asm__("cmp %[a], %[b]\n\t"
              "setbe %[result]\n\t"
              : [result] "=q"(result)
              : [a] "r"(a), [b] "r"(b)
              : "cc");
    }
    return static_cast<bool>(result);
  }

  template <typename T> static inline __attribute__((always_inline, const)) bool ct_ge_prim(const T a, const T b) {
    static_assert(std::is_integral_v<T>, "ct_ge_prim: T must be an integral type");
    uint8_t result;
    if constexpr (std::is_signed_v<T>) {
      __asm__("cmp %[a], %[b]\n\t"
              "setge %[result]\n\t"
              : [result] "=q"(result)
              : [a] "r"(a), [b] "r"(b)
              : "cc");
    } else {
      __asm__("cmp %[a], %[b]\n\t"
              "setae %[result]\n\t"
              : [result] "=q"(result)
              : [a] "r"(a), [b] "r"(b)
              : "cc");
    }
    return static_cast<bool>(result);
  }

  template <typename T>
  static inline __attribute__((always_inline, const)) bool ct_eq_array_prim(const T* lhs, const T* rhs, size_t count) {
    static_assert(std::is_integral_v<T>, "ct_eq_array_prim: T must be an integral type");
    const auto* lhs_bytes = reinterpret_cast<const uint8_t*>(lhs);
    const auto* rhs_bytes = reinterpret_cast<const uint8_t*>(rhs);
    const size_t total_bytes = count * sizeof(T);
    size_t offset = 0;
    uint64_t diff_mask = 0;

#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    __mmask8 mismatch512 = 0;
    for (; offset + 64 <= total_bytes; offset += 64) {
      const __m512i lhs_vec = _mm512_loadu_si512(reinterpret_cast<const void*>(lhs_bytes + offset));
      const __m512i rhs_vec = _mm512_loadu_si512(reinterpret_cast<const void*>(rhs_bytes + offset));
      mismatch512 |= _mm512_cmpneq_epi64_mask(lhs_vec, rhs_vec);
    }
    diff_mask |= static_cast<uint64_t>(mismatch512);
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    __m256i accum256 = _mm256_setzero_si256();
    for (; offset + 32 <= total_bytes; offset += 32) {
      const __m256i lhs_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs_bytes + offset));
      const __m256i rhs_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs_bytes + offset));
      const __m256i xor_vec = _mm256_xor_si256(lhs_vec, rhs_vec);
      accum256 = _mm256_or_si256(accum256, xor_vec);
    }
    diff_mask |= static_cast<uint64_t>(_mm256_movemask_epi8(accum256));
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    __m128i accum128 = _mm_setzero_si128();
    for (; offset + 16 <= total_bytes; offset += 16) {
      const __m128i lhs_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs_bytes + offset));
      const __m128i rhs_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs_bytes + offset));
      const __m128i xor_vec = _mm_xor_si128(lhs_vec, rhs_vec);
      accum128 = _mm_or_si128(accum128, xor_vec);
    }
    diff_mask |= static_cast<uint64_t>(_mm_movemask_epi8(accum128));
#endif

    for (; offset + 8 <= total_bytes; offset += 8) {
      uint64_t lhs_word = 0;
      uint64_t rhs_word = 0;
      std::memcpy(&lhs_word, lhs_bytes + offset, sizeof(lhs_word));
      std::memcpy(&rhs_word, rhs_bytes + offset, sizeof(rhs_word));
      diff_mask |= lhs_word ^ rhs_word;
    }

    if (offset + 4 <= total_bytes) {
      uint32_t lhs_word = 0;
      uint32_t rhs_word = 0;
      std::memcpy(&lhs_word, lhs_bytes + offset, sizeof(lhs_word));
      std::memcpy(&rhs_word, rhs_bytes + offset, sizeof(rhs_word));
      diff_mask |= static_cast<uint64_t>(lhs_word ^ rhs_word);
      offset += 4;
    }

    for (; offset < total_bytes; ++offset) {
      diff_mask |= static_cast<uint64_t>(lhs_bytes[offset] ^ rhs_bytes[offset]);
    }

    return diff_mask == 0;
  }

  template <typename T>
  static inline __attribute__((always_inline)) void ct_set_array_scalar(T* dst, const T* src, size_t count, bool cond) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "ct_set_array_scalar: T must be integral <= 64-bit");
    auto* dst_bytes = reinterpret_cast<uint8_t*>(dst);
    const auto* src_bytes = reinterpret_cast<const uint8_t*>(src);
    const size_t total_bytes = count * sizeof(T);
    size_t offset = 0;
    const auto masks = make_condition_masks(cond);
#ifdef __x86_64__
    if (total_bytes == 8) {
      ct_set_u64_asm(dst_bytes, src_bytes, masks.mask64);
      return;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128

    if (total_bytes == 16) {
      const __m128i dst_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst_bytes));
      const __m128i src_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_bytes));
#if defined(__SSE4_1__)
      const __m128i blended = _mm_blendv_epi8(dst_vec, src_vec, masks.vec128);
#else
      const __m128i blended =
          _mm_or_si128(_mm_and_si128(masks.vec128, src_vec), _mm_andnot_si128(masks.vec128, dst_vec));
#endif
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_bytes), blended);
      return;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    if (total_bytes == 32) {
      const __m256i dst_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst_bytes));
      const __m256i src_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_bytes));
      const __m256i blended = _mm256_blendv_epi8(dst_vec, src_vec, masks.vec256);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_bytes), blended);
      return;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    for (; offset + 64 <= total_bytes; offset += 64) {
      const __m512i dst_vec = _mm512_loadu_si512(reinterpret_cast<const void*>(dst_bytes + offset));
      const __m512i src_vec = _mm512_loadu_si512(reinterpret_cast<const void*>(src_bytes + offset));
      const __m512i blended = _mm512_mask_blend_epi64(masks.blend64, dst_vec, src_vec);
      _mm512_storeu_si512(reinterpret_cast<void*>(dst_bytes + offset), blended);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    for (; offset + 32 <= total_bytes; offset += 32) {
      const __m256i dst_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst_bytes + offset));
      const __m256i src_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_bytes + offset));
      const __m256i blended = _mm256_blendv_epi8(dst_vec, src_vec, masks.vec256);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_bytes + offset), blended);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    for (; offset + 16 <= total_bytes; offset += 16) {
      const __m128i dst_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst_bytes + offset));
      const __m128i src_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_bytes + offset));
#if defined(__SSE4_1__)
      const __m128i blended = _mm_blendv_epi8(dst_vec, src_vec, masks.vec128);
#else
      const __m128i blended =
          _mm_or_si128(_mm_and_si128(masks.vec128, src_vec), _mm_andnot_si128(masks.vec128, dst_vec));
#endif
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_bytes + offset), blended);
    }
#endif
#if defined(SN_OBLIV_CT_SUPPORTS_UINT512) && defined(__AVX512BW__)
    const std::size_t remaining_bytes = total_bytes - offset;
    if (remaining_bytes > 0) {
      const __mmask64 tail_mask = byte_mask_upto(remaining_bytes);
      const __mmask64 select_mask = masks.cond ? tail_mask : 0;
      const __m512i dst_tail = _mm512_maskz_loadu_epi8(tail_mask, dst_bytes + offset);
      const __m512i src_tail = _mm512_maskz_loadu_epi8(tail_mask, src_bytes + offset);
      const __m512i blended_tail = _mm512_mask_blend_epi8(select_mask, dst_tail, src_tail);
      _mm512_mask_storeu_epi8(dst_bytes + offset, tail_mask, blended_tail);
      offset = total_bytes;
    }
#endif
    for (; offset + 8 <= total_bytes; offset += 8) {
      uint64_t dst_word = 0;
      uint64_t src_word = 0;
      std::memcpy(&dst_word, dst_bytes + offset, sizeof(dst_word));
      std::memcpy(&src_word, src_bytes + offset, sizeof(src_word));
      const uint64_t diff = (dst_word ^ src_word) & masks.mask64;
      dst_word ^= diff;
      std::memcpy(dst_bytes + offset, &dst_word, sizeof(dst_word));
    }
    if (offset + 4 <= total_bytes) {
      uint32_t dst_word = 0;
      uint32_t src_word = 0;
      std::memcpy(&dst_word, dst_bytes + offset, sizeof(dst_word));
      std::memcpy(&src_word, src_bytes + offset, sizeof(src_word));
      const uint32_t diff = (dst_word ^ src_word) & masks.mask32;
      dst_word ^= diff;
      std::memcpy(dst_bytes + offset, &dst_word, sizeof(dst_word));
      offset += 4;
    }
    for (; offset < total_bytes; ++offset) {
      const uint8_t dst_byte = dst_bytes[offset];
      const uint8_t src_byte = src_bytes[offset];
      dst_bytes[offset] = static_cast<uint8_t>(dst_byte ^ ((dst_byte ^ src_byte) & masks.mask8));
    }
  }

  template <typename T>
  static inline __attribute__((always_inline)) void ct_select_array_scalar(
      T* out, const T* a, const T* b, size_t count, bool cond
  ) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "ct_select_array_scalar: T must be integral <= 64-bit");
    auto* out_bytes = reinterpret_cast<uint8_t*>(out);
    const auto* a_bytes = reinterpret_cast<const uint8_t*>(a);
    const auto* b_bytes = reinterpret_cast<const uint8_t*>(b);
    const size_t total_bytes = count * sizeof(T);
    size_t offset = 0;
    const auto masks = make_condition_masks(cond);
#ifdef __x86_64__
    if (total_bytes == 8) {
      ct_select_u64_asm(out_bytes, a_bytes, b_bytes, masks.mask64);
      return;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128

    if (total_bytes == 16) {
      const __m128i a_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a_bytes));
      const __m128i b_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b_bytes));
#if defined(__SSE4_1__)
      const __m128i blended = _mm_blendv_epi8(b_vec, a_vec, masks.vec128);
#else
      const __m128i blended = _mm_or_si128(_mm_and_si128(masks.vec128, a_vec), _mm_andnot_si128(masks.vec128, b_vec));
#endif
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out_bytes), blended);
      return;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    if (total_bytes == 32) {
      const __m256i a_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a_bytes));
      const __m256i b_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b_bytes));
      const __m256i blended = _mm256_blendv_epi8(b_vec, a_vec, masks.vec256);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(out_bytes), blended);
      return;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    for (; offset + 64 <= total_bytes; offset += 64) {
      const __m512i a_vec = _mm512_loadu_si512(reinterpret_cast<const void*>(a_bytes + offset));
      const __m512i b_vec = _mm512_loadu_si512(reinterpret_cast<const void*>(b_bytes + offset));
      const __m512i blended = _mm512_mask_blend_epi64(masks.blend64, b_vec, a_vec);
      _mm512_storeu_si512(reinterpret_cast<void*>(out_bytes + offset), blended);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    for (; offset + 32 <= total_bytes; offset += 32) {
      const __m256i a_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a_bytes + offset));
      const __m256i b_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b_bytes + offset));
      const __m256i blended = _mm256_blendv_epi8(b_vec, a_vec, masks.vec256);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(out_bytes + offset), blended);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    for (; offset + 16 <= total_bytes; offset += 16) {
      const __m128i a_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a_bytes + offset));
      const __m128i b_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b_bytes + offset));
#if defined(__SSE4_1__)
      const __m128i blended = _mm_blendv_epi8(b_vec, a_vec, masks.vec128);
#else
      const __m128i blended = _mm_or_si128(_mm_and_si128(masks.vec128, a_vec), _mm_andnot_si128(masks.vec128, b_vec));
#endif
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out_bytes + offset), blended);
    }
#endif
#if defined(SN_OBLIV_CT_SUPPORTS_UINT512) && defined(__AVX512BW__)
    const std::size_t remaining_bytes = total_bytes - offset;
    if (remaining_bytes > 0) {
      const __mmask64 tail_mask = byte_mask_upto(remaining_bytes);
      const __mmask64 select_mask = masks.cond ? tail_mask : 0;
      const __m512i a_tail = _mm512_maskz_loadu_epi8(tail_mask, a_bytes + offset);
      const __m512i b_tail = _mm512_maskz_loadu_epi8(tail_mask, b_bytes + offset);
      const __m512i blended_tail = _mm512_mask_blend_epi8(select_mask, b_tail, a_tail);
      _mm512_mask_storeu_epi8(out_bytes + offset, tail_mask, blended_tail);
      offset = total_bytes;
    }
#endif
    for (; offset + 8 <= total_bytes; offset += 8) {
      uint64_t a_word = 0;
      uint64_t b_word = 0;
      std::memcpy(&a_word, a_bytes + offset, sizeof(a_word));
      std::memcpy(&b_word, b_bytes + offset, sizeof(b_word));
      const uint64_t result = b_word ^ ((a_word ^ b_word) & masks.mask64);
      std::memcpy(out_bytes + offset, &result, sizeof(result));
    }
    if (offset + 4 <= total_bytes) {
      uint32_t a_word = 0;
      uint32_t b_word = 0;
      std::memcpy(&a_word, a_bytes + offset, sizeof(a_word));
      std::memcpy(&b_word, b_bytes + offset, sizeof(b_word));
      const uint32_t result = b_word ^ ((a_word ^ b_word) & masks.mask32);
      std::memcpy(out_bytes + offset, &result, sizeof(result));
      offset += 4;
    }
    for (; offset < total_bytes; ++offset) {
      const uint8_t a_byte = a_bytes[offset];
      const uint8_t b_byte = b_bytes[offset];
      out_bytes[offset] = static_cast<uint8_t>(b_byte ^ ((a_byte ^ b_byte) & masks.mask8));
    }
  }

  template <typename T>
  static inline __attribute__((always_inline)) void ct_swap_array_scalar(T* a, T* b, size_t count, bool cond) {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8, "ct_swap_array_scalar: T must be integral <= 64-bit");
    auto* a_bytes = reinterpret_cast<uint8_t*>(a);
    auto* b_bytes = reinterpret_cast<uint8_t*>(b);
    const size_t total_bytes = count * sizeof(T);
    size_t offset = 0;
    const auto masks = make_condition_masks(cond);
#ifdef __x86_64__
    if (total_bytes == 8) {
      ct_swap_u64_asm(a_bytes, b_bytes, masks.mask64);
      return;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128

    if (total_bytes == 16) {
      const __m128i a_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a_bytes));
      const __m128i b_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b_bytes));
      const __m128i diff = _mm_xor_si128(a_vec, b_vec);
      const __m128i masked = _mm_and_si128(masks.vec128, diff);
      const __m128i next_a = _mm_xor_si128(a_vec, masked);
      const __m128i next_b = _mm_xor_si128(b_vec, masked);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(a_bytes), next_a);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(b_bytes), next_b);
      return;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    if (total_bytes == 32) {
      const __m256i a_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a_bytes));
      const __m256i b_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b_bytes));
      const __m256i diff = _mm256_xor_si256(a_vec, b_vec);
      const __m256i masked = _mm256_and_si256(masks.vec256, diff);
      const __m256i next_a = _mm256_xor_si256(a_vec, masked);
      const __m256i next_b = _mm256_xor_si256(b_vec, masked);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(a_bytes), next_a);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(b_bytes), next_b);
      return;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    for (; offset + 64 <= total_bytes; offset += 64) {
      const __m512i a_vec = _mm512_loadu_si512(reinterpret_cast<const void*>(a_bytes + offset));
      const __m512i b_vec = _mm512_loadu_si512(reinterpret_cast<const void*>(b_bytes + offset));
      const __m512i diff = _mm512_xor_si512(a_vec, b_vec);
      const __m512i masked = _mm512_and_si512(masks.vec512, diff);
      const __m512i next_a = _mm512_xor_si512(a_vec, masked);
      const __m512i next_b = _mm512_xor_si512(b_vec, masked);
      _mm512_storeu_si512(reinterpret_cast<void*>(a_bytes + offset), next_a);
      _mm512_storeu_si512(reinterpret_cast<void*>(b_bytes + offset), next_b);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    for (; offset + 32 <= total_bytes; offset += 32) {
      const __m256i a_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a_bytes + offset));
      const __m256i b_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b_bytes + offset));
      const __m256i diff = _mm256_xor_si256(a_vec, b_vec);
      const __m256i masked = _mm256_and_si256(masks.vec256, diff);
      const __m256i next_a = _mm256_xor_si256(a_vec, masked);
      const __m256i next_b = _mm256_xor_si256(b_vec, masked);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(a_bytes + offset), next_a);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(b_bytes + offset), next_b);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    for (; offset + 16 <= total_bytes; offset += 16) {
      const __m128i a_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a_bytes + offset));
      const __m128i b_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b_bytes + offset));
      const __m128i diff = _mm_xor_si128(a_vec, b_vec);
      const __m128i masked = _mm_and_si128(masks.vec128, diff);
      const __m128i next_a = _mm_xor_si128(a_vec, masked);
      const __m128i next_b = _mm_xor_si128(b_vec, masked);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(a_bytes + offset), next_a);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(b_bytes + offset), next_b);
    }
#endif
#if defined(SN_OBLIV_CT_SUPPORTS_UINT512) && defined(__AVX512BW__)
    const std::size_t remaining_bytes = total_bytes - offset;
    if (remaining_bytes > 0) {
      const __mmask64 tail_mask = byte_mask_upto(remaining_bytes);
      const __mmask64 select_mask = masks.cond ? tail_mask : 0;
      const __m512i a_tail = _mm512_maskz_loadu_epi8(tail_mask, a_bytes + offset);
      const __m512i b_tail = _mm512_maskz_loadu_epi8(tail_mask, b_bytes + offset);
      const __m512i next_a = _mm512_mask_blend_epi8(select_mask, a_tail, b_tail);
      const __m512i next_b = _mm512_mask_blend_epi8(select_mask, b_tail, a_tail);
      _mm512_mask_storeu_epi8(a_bytes + offset, tail_mask, next_a);
      _mm512_mask_storeu_epi8(b_bytes + offset, tail_mask, next_b);
      offset = total_bytes;
    }
#endif
    for (; offset + 8 <= total_bytes; offset += 8) {
      uint64_t a_word = 0;
      uint64_t b_word = 0;
      std::memcpy(&a_word, a_bytes + offset, sizeof(a_word));
      std::memcpy(&b_word, b_bytes + offset, sizeof(b_word));
      const uint64_t diff = (a_word ^ b_word) & masks.mask64;
      a_word ^= diff;
      b_word ^= diff;
      std::memcpy(a_bytes + offset, &a_word, sizeof(a_word));
      std::memcpy(b_bytes + offset, &b_word, sizeof(b_word));
    }
    if (offset + 4 <= total_bytes) {
      uint32_t a_word = 0;
      uint32_t b_word = 0;
      std::memcpy(&a_word, a_bytes + offset, sizeof(a_word));
      std::memcpy(&b_word, b_bytes + offset, sizeof(b_word));
      const uint32_t diff = (a_word ^ b_word) & masks.mask32;
      a_word ^= diff;
      b_word ^= diff;
      std::memcpy(a_bytes + offset, &a_word, sizeof(a_word));
      std::memcpy(b_bytes + offset, &b_word, sizeof(b_word));
      offset += 4;
    }
    for (; offset < total_bytes; ++offset) {
      const uint8_t diff = static_cast<uint8_t>((a_bytes[offset] ^ b_bytes[offset]) & masks.mask8);
      a_bytes[offset] ^= diff;
      b_bytes[offset] ^= diff;
    }
  }

#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
  static inline __attribute__((always_inline)) void ct_set_array_u128(
      uint128_t* __restrict__ dst, const uint128_t* __restrict__ src, size_t count, bool cond
  ) {
#if defined(__clang__) || defined(__GNUC__)
    dst = reinterpret_cast<uint128_t*>(__builtin_assume_aligned(dst, alignof(uint128_t)));
    src = reinterpret_cast<const uint128_t*>(__builtin_assume_aligned(src, alignof(uint128_t)));
#endif
    const auto masks = make_condition_masks(cond);
    for (size_t i = 0; i < count; ++i) {
      const __m128i dst_vec = _mm_load_si128(dst + i);
      const __m128i src_vec = _mm_load_si128(src + i);
#if defined(__SSE4_1__)
      const __m128i blended = _mm_blendv_epi8(dst_vec, src_vec, masks.vec128);
#else
      const __m128i blended =
          _mm_or_si128(_mm_and_si128(masks.vec128, src_vec), _mm_andnot_si128(masks.vec128, dst_vec));
#endif
      _mm_store_si128(dst + i, blended);
    }
  }

  static inline __attribute__((always_inline)) void ct_select_array_u128(
      uint128_t* __restrict__ out, const uint128_t* __restrict__ a, const uint128_t* __restrict__ b, size_t count,
      bool cond
  ) {
#if defined(__clang__) || defined(__GNUC__)
    out = reinterpret_cast<uint128_t*>(__builtin_assume_aligned(out, alignof(uint128_t)));
    a = reinterpret_cast<const uint128_t*>(__builtin_assume_aligned(a, alignof(uint128_t)));
    b = reinterpret_cast<const uint128_t*>(__builtin_assume_aligned(b, alignof(uint128_t)));
#endif
    const auto masks = make_condition_masks(cond);
    for (size_t i = 0; i < count; ++i) {
      const __m128i a_vec = _mm_load_si128(a + i);
      const __m128i b_vec = _mm_load_si128(b + i);
#if defined(__SSE4_1__)
      const __m128i blended = _mm_blendv_epi8(b_vec, a_vec, masks.vec128);
#else
      const __m128i blended = _mm_or_si128(_mm_and_si128(masks.vec128, a_vec), _mm_andnot_si128(masks.vec128, b_vec));
#endif
      _mm_store_si128(out + i, blended);
    }
  }

  static inline __attribute__((always_inline)) void ct_swap_array_u128(
      uint128_t* __restrict__ a, uint128_t* __restrict__ b, size_t count, bool cond
  ) {
#if defined(__clang__) || defined(__GNUC__)
    a = reinterpret_cast<uint128_t*>(__builtin_assume_aligned(a, alignof(uint128_t)));
    b = reinterpret_cast<uint128_t*>(__builtin_assume_aligned(b, alignof(uint128_t)));
#endif
    const auto masks = make_condition_masks(cond);
    for (size_t i = 0; i < count; ++i) {
      const __m128i a_vec = _mm_load_si128(a + i);
      const __m128i b_vec = _mm_load_si128(b + i);
      const __m128i diff = _mm_and_si128(_mm_xor_si128(a_vec, b_vec), masks.vec128);
      _mm_store_si128(a + i, _mm_xor_si128(a_vec, diff));
      _mm_store_si128(b + i, _mm_xor_si128(b_vec, diff));
    }
  }
#endif

#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
  static inline __attribute__((always_inline)) void ct_set_array_u256(
      uint256_t* __restrict__ dst, const uint256_t* __restrict__ src, size_t count, bool cond
  ) {
#if defined(__clang__) || defined(__GNUC__)
    dst = reinterpret_cast<uint256_t*>(__builtin_assume_aligned(dst, alignof(uint256_t)));
    src = reinterpret_cast<const uint256_t*>(__builtin_assume_aligned(src, alignof(uint256_t)));
#endif
    const auto masks = make_condition_masks(cond);
    for (size_t i = 0; i < count; ++i) {
      const __m256i dst_vec = _mm256_load_si256(dst + i);
      const __m256i src_vec = _mm256_load_si256(src + i);
      const __m256i blended = _mm256_blendv_epi8(dst_vec, src_vec, masks.vec256);
      _mm256_store_si256(dst + i, blended);
    }
  }

  static inline __attribute__((always_inline)) void ct_select_array_u256(
      uint256_t* __restrict__ out, const uint256_t* __restrict__ a, const uint256_t* __restrict__ b, size_t count,
      bool cond
  ) {
#if defined(__clang__) || defined(__GNUC__)
    out = reinterpret_cast<uint256_t*>(__builtin_assume_aligned(out, alignof(uint256_t)));
    a = reinterpret_cast<const uint256_t*>(__builtin_assume_aligned(a, alignof(uint256_t)));
    b = reinterpret_cast<const uint256_t*>(__builtin_assume_aligned(b, alignof(uint256_t)));
#endif
    const auto masks = make_condition_masks(cond);
    for (size_t i = 0; i < count; ++i) {
      const __m256i a_vec = _mm256_load_si256(a + i);
      const __m256i b_vec = _mm256_load_si256(b + i);
      const __m256i blended = _mm256_blendv_epi8(b_vec, a_vec, masks.vec256);
      _mm256_store_si256(out + i, blended);
    }
  }

  static inline __attribute__((always_inline)) void ct_swap_array_u256(
      uint256_t* __restrict__ a, uint256_t* __restrict__ b, size_t count, bool cond
  ) {
#if defined(__clang__) || defined(__GNUC__)
    a = reinterpret_cast<uint256_t*>(__builtin_assume_aligned(a, alignof(uint256_t)));
    b = reinterpret_cast<uint256_t*>(__builtin_assume_aligned(b, alignof(uint256_t)));
#endif
    const auto masks = make_condition_masks(cond);
    for (size_t i = 0; i < count; ++i) {
      const __m256i a_vec = _mm256_load_si256(a + i);
      const __m256i b_vec = _mm256_load_si256(b + i);
      const __m256i diff = _mm256_and_si256(_mm256_xor_si256(a_vec, b_vec), masks.vec256);
      _mm256_store_si256(a + i, _mm256_xor_si256(a_vec, diff));
      _mm256_store_si256(b + i, _mm256_xor_si256(b_vec, diff));
    }
  }
#endif

#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
  static inline __attribute__((always_inline)) void ct_set_array_u512(
      uint512_t* __restrict__ dst, const uint512_t* __restrict__ src, size_t count, bool cond
  ) {
#if defined(__clang__) || defined(__GNUC__)
    dst = reinterpret_cast<uint512_t*>(__builtin_assume_aligned(dst, alignof(uint512_t)));
    src = reinterpret_cast<const uint512_t*>(__builtin_assume_aligned(src, alignof(uint512_t)));
#endif
    const auto masks = make_condition_masks(cond);
    for (size_t i = 0; i < count; ++i) {
      const __m512i dst_vec = _mm512_load_si512(dst + i);
      const __m512i src_vec = _mm512_load_si512(src + i);
      const __m512i blended = _mm512_mask_blend_epi64(masks.blend64, dst_vec, src_vec);
      _mm512_store_si512(dst + i, blended);
    }
  }

  static inline __attribute__((always_inline)) void ct_select_array_u512(
      uint512_t* __restrict__ out, const uint512_t* __restrict__ a, const uint512_t* __restrict__ b, size_t count,
      bool cond
  ) {
#if defined(__clang__) || defined(__GNUC__)
    out = reinterpret_cast<uint512_t*>(__builtin_assume_aligned(out, alignof(uint512_t)));
    a = reinterpret_cast<const uint512_t*>(__builtin_assume_aligned(a, alignof(uint512_t)));
    b = reinterpret_cast<const uint512_t*>(__builtin_assume_aligned(b, alignof(uint512_t)));
#endif
    const auto masks = make_condition_masks(cond);
    for (size_t i = 0; i < count; ++i) {
      const __m512i a_vec = _mm512_load_si512(a + i);
      const __m512i b_vec = _mm512_load_si512(b + i);
      const __m512i blended = _mm512_mask_blend_epi64(masks.blend64, b_vec, a_vec);
      _mm512_store_si512(out + i, blended);
    }
  }

  static inline __attribute__((always_inline)) void ct_swap_array_u512(
      uint512_t* __restrict__ a, uint512_t* __restrict__ b, size_t count, bool cond
  ) {
#if defined(__clang__) || defined(__GNUC__)
    a = reinterpret_cast<uint512_t*>(__builtin_assume_aligned(a, alignof(uint512_t)));
    b = reinterpret_cast<uint512_t*>(__builtin_assume_aligned(b, alignof(uint512_t)));
#endif
    const auto masks = make_condition_masks(cond);
    for (size_t i = 0; i < count; ++i) {
      const __m512i a_vec = _mm512_load_si512(a + i);
      const __m512i b_vec = _mm512_load_si512(b + i);
      const __m512i diff = _mm512_xor_si512(a_vec, b_vec);
      const __m512i masked = _mm512_and_si512(masks.vec512, diff);
      _mm512_store_si512(a + i, _mm512_xor_si512(a_vec, masked));
      _mm512_store_si512(b + i, _mm512_xor_si512(b_vec, masked));
    }
  }
#endif

  template <typename T> static inline void ct_set_array(T* dst, const T* src, size_t count, bool cond) {
    if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
      ct_set_array_scalar(dst, src, count, cond);
    }
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    else if constexpr (std::is_same_v<T, uint128_t>) {
      ct_set_array_u128(reinterpret_cast<uint128_t*>(dst), reinterpret_cast<const uint128_t*>(src), count, cond);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    else if constexpr (std::is_same_v<T, uint256_t>) {
      ct_set_array_u256(reinterpret_cast<uint256_t*>(dst), reinterpret_cast<const uint256_t*>(src), count, cond);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    else if constexpr (std::is_same_v<T, uint512_t>) {
      ct_set_array_u512(reinterpret_cast<uint512_t*>(dst), reinterpret_cast<const uint512_t*>(src), count, cond);
    }
#endif
    else {
      static_assert(sizeof(T) < 0, "ct_set_array requires integral or supported SIMD vector types");
    }
  }

  template <typename T> static inline void ct_select_array(T* out, const T* a, const T* b, size_t count, bool cond) {
    if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
      ct_select_array_scalar(out, a, b, count, cond);
    }
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    else if constexpr (std::is_same_v<T, uint128_t>) {
      ct_select_array_u128(
          reinterpret_cast<uint128_t*>(out), reinterpret_cast<const uint128_t*>(a),
          reinterpret_cast<const uint128_t*>(b), count, cond
      );
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    else if constexpr (std::is_same_v<T, uint256_t>) {
      ct_select_array_u256(
          reinterpret_cast<uint256_t*>(out), reinterpret_cast<const uint256_t*>(a),
          reinterpret_cast<const uint256_t*>(b), count, cond
      );
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    else if constexpr (std::is_same_v<T, uint512_t>) {
      ct_select_array_u512(
          reinterpret_cast<uint512_t*>(out), reinterpret_cast<const uint512_t*>(a),
          reinterpret_cast<const uint512_t*>(b), count, cond
      );
    }
#endif
    else {
      static_assert(sizeof(T) < 0, "ct_select_array requires integral or supported SIMD vector types");
    }
  }

  template <typename T> static inline void ct_swap_array(T* a, T* b, size_t count, bool cond) {
    if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
      ct_swap_array_scalar(a, b, count, cond);
    }
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    else if constexpr (std::is_same_v<T, uint128_t>) {
      ct_swap_array_u128(reinterpret_cast<uint128_t*>(a), reinterpret_cast<uint128_t*>(b), count, cond);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    else if constexpr (std::is_same_v<T, uint256_t>) {
      ct_swap_array_u256(reinterpret_cast<uint256_t*>(a), reinterpret_cast<uint256_t*>(b), count, cond);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    else if constexpr (std::is_same_v<T, uint512_t>) {
      ct_swap_array_u512(reinterpret_cast<uint512_t*>(a), reinterpret_cast<uint512_t*>(b), count, cond);
    }
#endif
    else {
      static_assert(sizeof(T) < 0, "ct_swap_array requires integral or supported SIMD vector types");
    }
  }

#ifdef SN_OBLIV_CT_SUPPORTS_UINT128

  static inline uint128_t ct_select_u128(uint128_t a, uint128_t b, bool cond) {
    const int64_t mask_scalar = -static_cast<int64_t>(cond);
    const __m128i mask = _mm_set1_epi64x(mask_scalar);
#if defined(__SSE4_1__)

    return _mm_blendv_epi8(b, a, mask);
#else

    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
#endif
  }
#endif

#ifdef SN_OBLIV_CT_SUPPORTS_UINT256

  static inline uint256_t ct_select_u256(uint256_t a, uint256_t b, bool cond) {
    const int64_t mask_scalar = -static_cast<int64_t>(cond);
    const __m256i mask = _mm256_set1_epi64x(mask_scalar);
    return _mm256_blendv_epi8(b, a, mask);
  }
#endif

#ifdef SN_OBLIV_CT_SUPPORTS_UINT512

  static inline uint512_t ct_select_u512(uint512_t a, uint512_t b, bool cond) {
    const __mmask8 k = cond ? 0xFF : 0x00;

    return _mm512_mask_blend_epi64(k, b, a);
  }
#endif


  template <typename T> static inline T ct_select_prim(const T a, const T b, bool cond) {
    if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
      return ct_select_scalar(a, b, cond);
    }
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    else if constexpr (sizeof(T) == 16 && alignof(T) >= 16 && !std::is_integral_v<T>) {
      const auto selected =
          ct_select_u128(reinterpret_cast<const uint128_t&>(a), reinterpret_cast<const uint128_t&>(b), cond);
      T out;
      std::memcpy(&out, &selected, sizeof(T));
      return out;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    else if constexpr (sizeof(T) == 32 && alignof(T) >= 32 && !std::is_integral_v<T>) {
      const auto selected =
          ct_select_u256(reinterpret_cast<const uint256_t&>(a), reinterpret_cast<const uint256_t&>(b), cond);
      T out;
      std::memcpy(&out, &selected, sizeof(T));
      return out;
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    else if constexpr (sizeof(T) == 64 && alignof(T) >= 64 && !std::is_integral_v<T>) {
      const auto selected =
          ct_select_u512(reinterpret_cast<const uint512_t&>(a), reinterpret_cast<const uint512_t&>(b), cond);
      T out;
      std::memcpy(&out, &selected, sizeof(T));
      return out;
    }
#endif
    else {
      static_assert(sizeof(T) < 0, "unsupported type for ct_select_prim: must be integral or SIMD vector type");
    }
  }


  static inline uint64_t ct_count_leading_zeros(uint64_t x) {

#if defined(__LZCNT__)
    return _lzcnt_u64(x);
#else
    if (x == 0) {
      return 64;
    }
#if defined(__clang__) || defined(__GNUC__)
    return static_cast<uint64_t>(__builtin_clzll(x));
#else
#error "backend_x64 requires __builtin_clzll when LZCNT is unavailable"
#endif
#endif
  }


  static inline int ct_log2(const uint64_t value) {
    if (value == 0) {

      return -1;
    }

    return 63 - (int) ct_count_leading_zeros(value);
  }


  template <typename T> static inline T ct_div_pow2(const T a, const uint8_t log2_divisor) {
    static_assert(std::is_integral_v<T>, "ct_div_pow2: T must be an integral type");
    return a >> log2_divisor;
  }


  template <typename T> static inline T ct_mod_pow2(const T a, const uint8_t log2_divisor) {
    static_assert(std::is_integral_v<T>, "ct_mod_pow2: T must be an integral type");

    const T mask = (T(1) << log2_divisor) - 1;
    return a & mask;
  }


  template <typename T> static inline T ct_mul_pow2(const T a, const uint8_t log2_multiplier) {
    static_assert(std::is_integral_v<T>, "ct_mul_pow2: T must be an integral type");
    return a << log2_multiplier;
  }


  template <typename T> static inline T ct_pow2(const uint8_t exponent) {
    static_assert(std::is_integral_v<T>, "ct_pow2: T must be an integral type");
    return T(1) << exponent;
  }

  template <typename T> static inline void ct_set_array(T* __restrict__ out, const T* __restrict__ src, bool cond) {
    if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
      const T value = *src;
      ct_set(out, value, cond);
    }
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    else if constexpr (std::is_same_v<T, uint128_t>) {
      auto* out_vec = reinterpret_cast<uint128_t*>(out);
      auto const* src_vec = reinterpret_cast<const uint128_t*>(src);
#if defined(__clang__) || defined(__GNUC__)
      out_vec = reinterpret_cast<uint128_t*>(__builtin_assume_aligned(out_vec, alignof(uint128_t)));
      src_vec = reinterpret_cast<const uint128_t*>(__builtin_assume_aligned(src_vec, alignof(uint128_t)));
#endif
      const __m128i dst_val = _mm_load_si128(out_vec);
      const __m128i src_val = _mm_load_si128(src_vec);
      const __m128i mask = _mm_set1_epi64x(-static_cast<long long>(cond));
#if defined(__SSE4_1__)
      const __m128i result = _mm_blendv_epi8(dst_val, src_val, mask);
#else
      const __m128i result = _mm_or_si128(_mm_and_si128(mask, src_val), _mm_andnot_si128(mask, dst_val));
#endif
      _mm_store_si128(out_vec, result);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    else if constexpr (std::is_same_v<T, uint256_t>) {
      auto* out_vec = reinterpret_cast<uint256_t*>(out);
      auto const* src_vec = reinterpret_cast<const uint256_t*>(src);
#if defined(__clang__) || defined(__GNUC__)
      out_vec = reinterpret_cast<uint256_t*>(__builtin_assume_aligned(out_vec, alignof(uint256_t)));
      src_vec = reinterpret_cast<const uint256_t*>(__builtin_assume_aligned(src_vec, alignof(uint256_t)));
#endif
      const __m256i dst_val = _mm256_load_si256(out_vec);
      const __m256i src_val = _mm256_load_si256(src_vec);
      const __m256i mask = _mm256_set1_epi64x(-static_cast<long long>(cond));
      const __m256i result = _mm256_blendv_epi8(dst_val, src_val, mask);
      _mm256_store_si256(out_vec, result);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    else if constexpr (std::is_same_v<T, uint512_t>) {
      auto* out_vec = reinterpret_cast<uint512_t*>(out);
      auto const* src_vec = reinterpret_cast<const uint512_t*>(src);
#if defined(__clang__) || defined(__GNUC__)
      out_vec = reinterpret_cast<uint512_t*>(__builtin_assume_aligned(out_vec, alignof(uint512_t)));
      src_vec = reinterpret_cast<const uint512_t*>(__builtin_assume_aligned(src_vec, alignof(uint512_t)));
#endif
      const __m512i dst_val = _mm512_load_si512(out_vec);
      const __m512i src_val = _mm512_load_si512(src_vec);
      const __mmask8 mask = bool_to_mask8(cond);
      const __m512i result = _mm512_mask_blend_epi64(mask, dst_val, src_val);
      _mm512_store_si512(out_vec, result);
    }
#endif
    else {
      const T value = *src;
      ct_set(out, value, cond);
    }
  }

  template <typename T>
  static inline void ct_select_array(T* __restrict__ out, const T* __restrict__ a, const T* __restrict__ b, bool cond) {
    if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
      const T val_a = *a;
      const T val_b = *b;
      *out = ct_select(val_a, val_b, cond);
    }
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    else if constexpr (std::is_same_v<T, uint128_t>) {
      auto* out_vec = reinterpret_cast<uint128_t*>(out);
      auto const* a_vec = reinterpret_cast<const uint128_t*>(a);
      auto const* b_vec = reinterpret_cast<const uint128_t*>(b);
#if defined(__clang__) || defined(__GNUC__)
      out_vec = reinterpret_cast<uint128_t*>(__builtin_assume_aligned(out_vec, alignof(uint128_t)));
      a_vec = reinterpret_cast<const uint128_t*>(__builtin_assume_aligned(a_vec, alignof(uint128_t)));
      b_vec = reinterpret_cast<const uint128_t*>(__builtin_assume_aligned(b_vec, alignof(uint128_t)));
#endif
      const __m128i val_a = _mm_load_si128(a_vec);
      const __m128i val_b = _mm_load_si128(b_vec);
      const __m128i mask = _mm_set1_epi64x(-static_cast<long long>(cond));
#if defined(__SSE4_1__)
      const __m128i result = _mm_blendv_epi8(val_b, val_a, mask);
#else
      const __m128i result = _mm_or_si128(_mm_and_si128(mask, val_a), _mm_andnot_si128(mask, val_b));
#endif
      _mm_store_si128(out_vec, result);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    else if constexpr (std::is_same_v<T, uint256_t>) {
      auto* out_vec = reinterpret_cast<uint256_t*>(out);
      auto const* a_vec = reinterpret_cast<const uint256_t*>(a);
      auto const* b_vec = reinterpret_cast<const uint256_t*>(b);
#if defined(__clang__) || defined(__GNUC__)
      out_vec = reinterpret_cast<uint256_t*>(__builtin_assume_aligned(out_vec, alignof(uint256_t)));
      a_vec = reinterpret_cast<const uint256_t*>(__builtin_assume_aligned(a_vec, alignof(uint256_t)));
      b_vec = reinterpret_cast<const uint256_t*>(__builtin_assume_aligned(b_vec, alignof(uint256_t)));
#endif
      const __m256i val_a = _mm256_load_si256(a_vec);
      const __m256i val_b = _mm256_load_si256(b_vec);
      const __m256i mask = _mm256_set1_epi64x(-static_cast<long long>(cond));
      const __m256i result = _mm256_blendv_epi8(val_b, val_a, mask);
      _mm256_store_si256(out_vec, result);
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    else if constexpr (std::is_same_v<T, uint512_t>) {
      auto* __restrict__ out_vec = reinterpret_cast<uint512_t*>(out);
      auto const* __restrict__ a_vec = reinterpret_cast<const uint512_t*>(a);
      auto const* __restrict__ b_vec = reinterpret_cast<const uint512_t*>(b);
#if defined(__clang__) || defined(__GNUC__)
      out_vec = reinterpret_cast<uint512_t*>(__builtin_assume_aligned(out_vec, alignof(uint512_t)));
      a_vec = reinterpret_cast<const uint512_t*>(__builtin_assume_aligned(a_vec, alignof(uint512_t)));
      b_vec = reinterpret_cast<const uint512_t*>(__builtin_assume_aligned(b_vec, alignof(uint512_t)));
#endif
      const __m512i val_a = _mm512_load_si512(a_vec);
      const __m512i val_b = _mm512_load_si512(b_vec);
      const __mmask8 mask = bool_to_mask8(cond);
      const __m512i result = _mm512_mask_blend_epi64(mask, val_b, val_a);
      _mm512_store_si512(out_vec, result);
    }
#endif
    else {
      const T val_a = *a;
      const T val_b = *b;
      *out = ct_select(val_a, val_b, cond);
    }
  }

  template <typename T> static inline void ct_swap_array(T* a, T* b, bool cond) {
    if constexpr (std::is_integral_v<T> && sizeof(T) <= 8) {
      ct_swap(a, b, cond);
    }
#ifdef SN_OBLIV_CT_SUPPORTS_UINT128
    else if constexpr (std::is_same_v<T, uint128_t>) {
      auto* a_vec = reinterpret_cast<uint128_t*>(a);
      auto* b_vec = reinterpret_cast<uint128_t*>(b);
#if defined(__clang__) || defined(__GNUC__)
      a_vec = reinterpret_cast<uint128_t*>(__builtin_assume_aligned(a_vec, alignof(uint128_t)));
      b_vec = reinterpret_cast<uint128_t*>(__builtin_assume_aligned(b_vec, alignof(uint128_t)));
#endif
      const __m128i val_a = _mm_load_si128(a_vec);
      const __m128i val_b = _mm_load_si128(b_vec);
      const __m128i mask = _mm_set1_epi64x(-static_cast<long long>(cond));
      const __m128i diff = _mm_xor_si128(val_a, val_b);
      const __m128i masked_diff = _mm_and_si128(diff, mask);
      _mm_store_si128(a_vec, _mm_xor_si128(val_a, masked_diff));
      _mm_store_si128(b_vec, _mm_xor_si128(val_b, masked_diff));
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT256
    else if constexpr (std::is_same_v<T, uint256_t>) {
      auto* a_vec = reinterpret_cast<uint256_t*>(a);
      auto* b_vec = reinterpret_cast<uint256_t*>(b);
#if defined(__clang__) || defined(__GNUC__)
      a_vec = reinterpret_cast<uint256_t*>(__builtin_assume_aligned(a_vec, alignof(uint256_t)));
      b_vec = reinterpret_cast<uint256_t*>(__builtin_assume_aligned(b_vec, alignof(uint256_t)));
#endif
      const __m256i val_a = _mm256_load_si256(a_vec);
      const __m256i val_b = _mm256_load_si256(b_vec);
      const __m256i mask = _mm256_set1_epi64x(-static_cast<long long>(cond));
      const __m256i diff = _mm256_xor_si256(val_a, val_b);
      const __m256i masked_diff = _mm256_and_si256(diff, mask);
      _mm256_store_si256(a_vec, _mm256_xor_si256(val_a, masked_diff));
      _mm256_store_si256(b_vec, _mm256_xor_si256(val_b, masked_diff));
    }
#endif
#ifdef SN_OBLIV_CT_SUPPORTS_UINT512
    else if constexpr (std::is_same_v<T, uint512_t>) {
      auto* __restrict__ a_vec = reinterpret_cast<uint512_t*>(a);
      auto* __restrict__ b_vec = reinterpret_cast<uint512_t*>(b);
#if defined(__clang__) || defined(__GNUC__)
      a_vec = reinterpret_cast<uint512_t*>(__builtin_assume_aligned(a_vec, alignof(uint512_t)));
      b_vec = reinterpret_cast<uint512_t*>(__builtin_assume_aligned(b_vec, alignof(uint512_t)));
#endif
      const __m512i val_a = _mm512_load_si512(a_vec);
      const __m512i val_b = _mm512_load_si512(b_vec);
      const __m512i mask = _mm512_set1_epi64(-static_cast<long long>(cond));
      const __m512i diff = _mm512_xor_si512(val_a, val_b);
      const __m512i masked_diff = _mm512_and_si512(diff, mask);
      _mm512_store_si512(a_vec, _mm512_xor_si512(val_a, masked_diff));
      _mm512_store_si512(b_vec, _mm512_xor_si512(val_b, masked_diff));
    }
#endif
    else {
      ct_swap(a, b, cond);
    }
  }

  template <typename T> static inline void ct_set(T* out, T src, bool cond) { ct_setc_prim(out, src, cond); }

  template <typename T> static inline void ct_swap(T* a, T* b, bool cond) { ct_swapc_prim(a, b, cond); }

  template <typename T> static inline bool ct_eq(const T a, const T b) { return ct_eq_prim(a, b); }

  template <typename T> static inline bool ct_gt(const T a, const T b) { return ct_gt_prim(a, b); }

  template <typename T> static inline bool ct_lt(const T a, const T b) { return ct_lt_prim(a, b); }

  template <typename T> static inline bool ct_le(const T a, const T b) { return ct_le_prim(a, b); }

  template <typename T> static inline bool ct_ge(const T a, const T b) { return ct_ge_prim(a, b); }

  template <typename T> static inline bool ct_eq_array(const T* a, const T* b, size_t n) {
    return ct_eq_array_prim(a, b, n);
  }

  template <typename T> static inline int ct_cmp(const T a, const T b) { return ct_cmp_prim(a, b); }

  template <typename T> static inline T ct_select(const T a, const T b, bool cond) {
    return ct_select_prim(a, b, cond);
  }


  static inline uint64_t ct_madd(const uint64_t a, const uint64_t b, const uint64_t c) {

    return (a * b) + c;
  }
};

}
}
}
