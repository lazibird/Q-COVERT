#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

#if defined(__SSE2__) || defined(__AVX2__) || defined(__AVX512F__)
#include <immintrin.h>
#endif

#include "sonic/obliv/ops/core_ops.hpp"

namespace sn {
namespace obliv {

namespace detail {

template <typename Backend> struct struct_ops_backend {
private:
  using core = core_ops_backend<Backend>;

  template <typename T> static constexpr bool is_struct_type() {
    return std::is_trivially_copyable_v<T> && !std::is_integral_v<T>;
  }

  template <typename T> static constexpr void ensure_struct_constraints() {
    static_assert(is_struct_type<T>(), "struct_ops requires a trivially copyable, non-integral type");
    static_assert(sizeof(T) >= 8, "struct_ops requires structs to be at least 8 bytes");
    static_assert(alignof(T) >= alignof(std::uint64_t), "struct_ops requires structs to have >= 8-byte alignment");
  }

  template <typename Chunk, std::size_t Offset> static constexpr void ensure_chunk_offset_alignment() {
    if constexpr (alignof(Chunk) > 1) {
      static_assert(
          (Offset % alignof(Chunk)) == 0,
          "struct_ops chunk offsets must respect the alignment required by the chunk type"
      );
    }
  }

  template <typename Chunk> static inline Chunk load_chunk(const std::uint8_t* ptr) {
    Chunk value;
#if defined(__clang__) || defined(__GNUC__)

    if constexpr (alignof(Chunk) > 1) {
      ptr = reinterpret_cast<const std::uint8_t*>(__builtin_assume_aligned(ptr, alignof(Chunk)));
    }
#endif
#if defined(__AVX512F__)
    if constexpr (Backend::supports_uint512 && std::is_same_v<Chunk, typename Backend::uint512_t>) {
      return _mm512_load_si512(reinterpret_cast<const void*>(ptr));
    }
#endif
#if defined(__AVX2__)
    if constexpr (Backend::supports_uint256 && std::is_same_v<Chunk, typename Backend::uint256_t>) {
      return _mm256_load_si256(reinterpret_cast<const __m256i*>(ptr));
    }
#endif
#if defined(__SSE2__)
    if constexpr (Backend::supports_uint128 && std::is_same_v<Chunk, typename Backend::uint128_t>) {
      return _mm_load_si128(reinterpret_cast<const __m128i*>(ptr));
    }
#endif
    std::memcpy(&value, ptr, sizeof(Chunk));
    return value;
  }

  template <typename Chunk> static inline void store_chunk(std::uint8_t* ptr, const Chunk& value) {
#if defined(__clang__) || defined(__GNUC__)

    if constexpr (alignof(Chunk) > 1) {
      ptr = reinterpret_cast<std::uint8_t*>(__builtin_assume_aligned(ptr, alignof(Chunk)));
    }
#endif
#if defined(__AVX512F__)
    if constexpr (Backend::supports_uint512 && std::is_same_v<Chunk, typename Backend::uint512_t>) {
      _mm512_store_si512(reinterpret_cast<void*>(ptr), value);
      return;
    }
#endif
#if defined(__AVX2__)
    if constexpr (Backend::supports_uint256 && std::is_same_v<Chunk, typename Backend::uint256_t>) {
      _mm256_store_si256(reinterpret_cast<__m256i*>(ptr), value);
      return;
    }
#endif
#if defined(__SSE2__)
    if constexpr (Backend::supports_uint128 && std::is_same_v<Chunk, typename Backend::uint128_t>) {
      _mm_store_si128(reinterpret_cast<__m128i*>(ptr), value);
      return;
    }
#endif
    std::memcpy(ptr, &value, sizeof(Chunk));
  }

  template <typename T> struct chunk_layout {
    static constexpr std::size_t total = sizeof(T);

    static constexpr std::size_t align512 = []() constexpr -> std::size_t {
      if constexpr (Backend::supports_uint512) {
        return alignof(typename Backend::uint512_t);
      }
      return std::size_t{0};
    }();
    static constexpr bool use512 = Backend::supports_uint512 && (alignof(T) >= align512);
    static constexpr std::size_t count512 = use512 ? total / 64 : 0;
    static constexpr std::size_t offset512 = 0;
    static constexpr std::size_t rem_after_512 = total - (count512 * 64);

    static constexpr std::size_t align256 = []() constexpr -> std::size_t {
      if constexpr (Backend::supports_uint256) {
        return alignof(typename Backend::uint256_t);
      }
      return std::size_t{0};
    }();
    static constexpr bool use256 = Backend::supports_uint256 && (alignof(T) >= align256);
    static constexpr std::size_t count256 = use256 ? rem_after_512 / 32 : 0;
    static constexpr std::size_t offset256 = offset512 + (count512 * 64);
    static constexpr std::size_t rem_after_256 = rem_after_512 - (count256 * 32);

    static constexpr std::size_t align128 = []() constexpr -> std::size_t {
      if constexpr (Backend::supports_uint128) {
        return alignof(typename Backend::uint128_t);
      }
      return std::size_t{0};
    }();
    static constexpr bool use128 = Backend::supports_uint128 && (alignof(T) >= align128);
    static constexpr std::size_t count128 = use128 ? rem_after_256 / 16 : 0;
    static constexpr std::size_t offset128 = offset256 + (count256 * 32);
    static constexpr std::size_t rem_after_128 = rem_after_256 - (count128 * 16);

    static constexpr std::size_t count64 = rem_after_128 / 8;
    static constexpr std::size_t offset64 = offset128 + (count128 * 16);
    static constexpr std::size_t rem_after_64 = rem_after_128 - (count64 * 8);

    static constexpr std::size_t count32 = rem_after_64 / 4;
    static constexpr std::size_t offset32 = offset64 + (count64 * 8);
    static constexpr std::size_t rem_after_32 = rem_after_64 - (count32 * 4);

    static constexpr std::size_t count16 = rem_after_32 / 2;
    static constexpr std::size_t offset16 = offset32 + (count32 * 4);
    static constexpr std::size_t rem_after_16 = rem_after_32 - (count16 * 2);

    static constexpr std::size_t count8 = rem_after_16;
    static constexpr std::size_t offset8 = offset16 + (count16 * 2);

    static_assert(offset8 + count8 == total, "struct chunk layout must cover all bytes");
  };

  template <typename T> static constexpr void validate_struct_alignment() {

    if constexpr (Backend::supports_uint512 && chunk_layout<T>::count512 > 0) {
      static_assert(
          alignof(T) >= alignof(typename Backend::uint512_t),
          "struct_ops align512"
      );
    }
    if constexpr (Backend::supports_uint256 && chunk_layout<T>::count256 > 0) {
      static_assert(
          alignof(T) >= alignof(typename Backend::uint256_t),
          "struct_ops align256"
      );
    }
    if constexpr (Backend::supports_uint128 && chunk_layout<T>::count128 > 0) {
      static_assert(
          alignof(T) >= alignof(typename Backend::uint128_t),
          "struct_ops align128"
      );
    }
    if constexpr (chunk_layout<T>::count64 > 0) {
      static_assert(
          (chunk_layout<T>::offset64 % alignof(std::uint64_t)) == 0,
          "struct_ops offset64"
      );
    }
    if constexpr (chunk_layout<T>::count32 > 0) {
      static_assert(
          (chunk_layout<T>::offset32 % alignof(std::uint32_t)) == 0,
          "struct_ops requires 32-bit chunk offsets to respect 4-byte alignment"
      );
    }
    if constexpr (chunk_layout<T>::count16 > 0) {
      static_assert(
          (chunk_layout<T>::offset16 % alignof(std::uint16_t)) == 0,
          "struct_ops requires 16-bit chunk offsets to respect 2-byte alignment"
      );
    }
  }

  template <std::size_t Offset, std::size_t Count16, std::size_t Count8> struct trailing_scalar_layout {
    static constexpr std::size_t total_bytes = (Count16 * 2) + Count8;
    static constexpr std::size_t offset_mod8 = Offset % alignof(std::uint64_t);
    static constexpr std::size_t align_gap = offset_mod8 == 0 ? 0 : alignof(std::uint64_t) - offset_mod8;
    static constexpr std::size_t head_bytes = (total_bytes < align_gap) ? total_bytes : align_gap;
    static constexpr std::size_t aligned_offset = Offset + head_bytes;
    static constexpr std::size_t remaining_bytes = total_bytes - head_bytes;
    static constexpr std::size_t qword_count = remaining_bytes / sizeof(std::uint64_t);
    static constexpr std::size_t tail_bytes = remaining_bytes - (qword_count * sizeof(std::uint64_t));

    static_assert(
        qword_count == 0 || (aligned_offset % alignof(std::uint64_t)) == 0,
        "trailing_scalar_layout must align 64-bit spans"
    );
  };

  template <typename T, std::size_t Offset, std::size_t Count16, std::size_t Count8>
  static inline void set_trailing_scalars(std::uint8_t* dst_bytes, const std::uint8_t* src_bytes, bool cond) {
    using tail = trailing_scalar_layout<Offset, Count16, Count8>;
    if constexpr (tail::total_bytes == 0) {
      return;
    }

    if constexpr (tail::head_bytes > 0) {
      core::template ct_set_array<std::uint8_t>(
          reinterpret_cast<std::uint8_t*>(dst_bytes + Offset),
          reinterpret_cast<const std::uint8_t*>(src_bytes + Offset), tail::head_bytes, cond
      );
    }

    if constexpr (tail::qword_count > 0) {
      static_assert(
          alignof(T) >= alignof(std::uint64_t),
          "struct_ops tail64"
      );
      ensure_chunk_offset_alignment<std::uint64_t, tail::aligned_offset>();
      auto* dst_u64 = reinterpret_cast<std::uint64_t*>(dst_bytes + tail::aligned_offset);
      const auto* src_u64 = reinterpret_cast<const std::uint64_t*>(src_bytes + tail::aligned_offset);
#if defined(__clang__) || defined(__GNUC__)
      dst_u64 = reinterpret_cast<std::uint64_t*>(__builtin_assume_aligned(dst_u64, alignof(std::uint64_t)));
      src_u64 = reinterpret_cast<const std::uint64_t*>(__builtin_assume_aligned(src_u64, alignof(std::uint64_t)));
#endif
      core::template ct_set_array<std::uint64_t>(dst_u64, src_u64, tail::qword_count, cond);
    }

    if constexpr (tail::tail_bytes > 0) {
      constexpr std::size_t tail_offset = tail::aligned_offset + (tail::qword_count * sizeof(std::uint64_t));
      core::template ct_set_array<std::uint8_t>(
          reinterpret_cast<std::uint8_t*>(dst_bytes + tail_offset),
          reinterpret_cast<const std::uint8_t*>(src_bytes + tail_offset), tail::tail_bytes, cond
      );
    }
  }

  template <typename T, std::size_t Offset, std::size_t Count16, std::size_t Count8>
  static inline void select_trailing_scalars(
      std::uint8_t* out_bytes, const std::uint8_t* a_bytes, const std::uint8_t* b_bytes, bool cond
  ) {
    using tail = trailing_scalar_layout<Offset, Count16, Count8>;
    if constexpr (tail::total_bytes == 0) {
      return;
    }

    if constexpr (tail::head_bytes > 0) {
      core::template ct_select_array<std::uint8_t>(
          reinterpret_cast<std::uint8_t*>(out_bytes + Offset), reinterpret_cast<const std::uint8_t*>(a_bytes + Offset),
          reinterpret_cast<const std::uint8_t*>(b_bytes + Offset), tail::head_bytes, cond
      );
    }

    if constexpr (tail::qword_count > 0) {
      static_assert(
          alignof(T) >= alignof(std::uint64_t),
          "struct_ops tail64"
      );
      ensure_chunk_offset_alignment<std::uint64_t, tail::aligned_offset>();
      auto* out_u64 = reinterpret_cast<std::uint64_t*>(out_bytes + tail::aligned_offset);
      const auto* a_u64 = reinterpret_cast<const std::uint64_t*>(a_bytes + tail::aligned_offset);
      const auto* b_u64 = reinterpret_cast<const std::uint64_t*>(b_bytes + tail::aligned_offset);
#if defined(__clang__) || defined(__GNUC__)
      out_u64 = reinterpret_cast<std::uint64_t*>(__builtin_assume_aligned(out_u64, alignof(std::uint64_t)));
      a_u64 = reinterpret_cast<const std::uint64_t*>(__builtin_assume_aligned(a_u64, alignof(std::uint64_t)));
      b_u64 = reinterpret_cast<const std::uint64_t*>(__builtin_assume_aligned(b_u64, alignof(std::uint64_t)));
#endif
      core::template ct_select_array<std::uint64_t>(out_u64, a_u64, b_u64, tail::qword_count, cond);
    }

    if constexpr (tail::tail_bytes > 0) {
      constexpr std::size_t tail_offset = tail::aligned_offset + (tail::qword_count * sizeof(std::uint64_t));
      core::template ct_select_array<std::uint8_t>(
          reinterpret_cast<std::uint8_t*>(out_bytes + tail_offset),
          reinterpret_cast<const std::uint8_t*>(a_bytes + tail_offset),
          reinterpret_cast<const std::uint8_t*>(b_bytes + tail_offset), tail::tail_bytes, cond
      );
    }
  }

  template <typename T, std::size_t Offset, std::size_t Count16, std::size_t Count8>
  static inline void swap_trailing_scalars(std::uint8_t* a_bytes, std::uint8_t* b_bytes, bool cond) {
    using tail = trailing_scalar_layout<Offset, Count16, Count8>;
    if constexpr (tail::total_bytes == 0) {
      return;
    }

    if constexpr (tail::head_bytes > 0) {
      core::template ct_swap_array<std::uint8_t>(
          reinterpret_cast<std::uint8_t*>(a_bytes + Offset), reinterpret_cast<std::uint8_t*>(b_bytes + Offset),
          tail::head_bytes, cond
      );
    }

    if constexpr (tail::qword_count > 0) {
      static_assert(
          alignof(T) >= alignof(std::uint64_t),
          "struct_ops tail64"
      );
      ensure_chunk_offset_alignment<std::uint64_t, tail::aligned_offset>();
      auto* a_u64 = reinterpret_cast<std::uint64_t*>(a_bytes + tail::aligned_offset);
      auto* b_u64 = reinterpret_cast<std::uint64_t*>(b_bytes + tail::aligned_offset);
#if defined(__clang__) || defined(__GNUC__)
      a_u64 = reinterpret_cast<std::uint64_t*>(__builtin_assume_aligned(a_u64, alignof(std::uint64_t)));
      b_u64 = reinterpret_cast<std::uint64_t*>(__builtin_assume_aligned(b_u64, alignof(std::uint64_t)));
#endif
      core::template ct_swap_array<std::uint64_t>(a_u64, b_u64, tail::qword_count, cond);
    }

    if constexpr (tail::tail_bytes > 0) {
      constexpr std::size_t tail_offset = tail::aligned_offset + (tail::qword_count * sizeof(std::uint64_t));
      core::template ct_swap_array<std::uint8_t>(
          reinterpret_cast<std::uint8_t*>(a_bytes + tail_offset),
          reinterpret_cast<std::uint8_t*>(b_bytes + tail_offset), tail::tail_bytes, cond
      );
    }
  }

  template <typename T, std::size_t Offset, std::size_t Count16, std::size_t Count8>
  static inline void accumulate_trailing_scalars(
      const std::uint8_t* a_bytes, const std::uint8_t* b_bytes, std::uint64_t& diff
  ) {
    using tail = trailing_scalar_layout<Offset, Count16, Count8>;
    if constexpr (tail::total_bytes == 0) {
      return;
    }

    if constexpr (tail::head_bytes > 0) {
      accumulate_eq_chunks<std::uint8_t, Offset, tail::head_bytes>(a_bytes, b_bytes, diff);
    }

    if constexpr (tail::qword_count > 0) {
      static_assert(
          alignof(T) >= alignof(std::uint64_t),
          "struct_ops tail64"
      );
      accumulate_eq_chunks<std::uint64_t, tail::aligned_offset, tail::qword_count>(a_bytes, b_bytes, diff);
    }

    if constexpr (tail::tail_bytes > 0) {
      constexpr std::size_t tail_offset = tail::aligned_offset + (tail::qword_count * sizeof(std::uint64_t));
      accumulate_eq_chunks<std::uint8_t, tail_offset, tail::tail_bytes>(a_bytes, b_bytes, diff);
    }
  }

  template <typename Chunk, std::size_t Offset>
  static inline void set_chunk_at(std::uint8_t* dst_bytes, const std::uint8_t* src_bytes, bool cond) {
    ensure_chunk_offset_alignment<Chunk, Offset>();
    Chunk* __restrict__ dst_ptr = reinterpret_cast<Chunk*>(dst_bytes + Offset);
    const Chunk* __restrict__ src_ptr = reinterpret_cast<const Chunk*>(src_bytes + Offset);
#if defined(__clang__) || defined(__GNUC__)
    if constexpr (alignof(Chunk) > 1) {
      dst_ptr = reinterpret_cast<Chunk*>(__builtin_assume_aligned(dst_ptr, alignof(Chunk)));
      src_ptr = reinterpret_cast<const Chunk*>(__builtin_assume_aligned(src_ptr, alignof(Chunk)));
    }
#endif
    core::template ct_set_array<Chunk>(dst_ptr, src_ptr, cond);
  }

  template <typename Chunk, std::size_t Offset>
  static inline void select_chunk_at(
      std::uint8_t* out_bytes, const std::uint8_t* a_bytes, const std::uint8_t* b_bytes, bool cond
  ) {
    ensure_chunk_offset_alignment<Chunk, Offset>();
    Chunk* __restrict__ out_ptr = reinterpret_cast<Chunk*>(out_bytes + Offset);
    const Chunk* __restrict__ a_ptr = reinterpret_cast<const Chunk*>(a_bytes + Offset);
    const Chunk* __restrict__ b_ptr = reinterpret_cast<const Chunk*>(b_bytes + Offset);
#if defined(__clang__) || defined(__GNUC__)
    if constexpr (alignof(Chunk) > 1) {
      out_ptr = reinterpret_cast<Chunk*>(__builtin_assume_aligned(out_ptr, alignof(Chunk)));
      a_ptr = reinterpret_cast<const Chunk*>(__builtin_assume_aligned(a_ptr, alignof(Chunk)));
      b_ptr = reinterpret_cast<const Chunk*>(__builtin_assume_aligned(b_ptr, alignof(Chunk)));
    }
#endif
    core::template ct_select_array<Chunk>(out_ptr, a_ptr, b_ptr, cond);
  }

  template <typename Chunk, std::size_t Offset>
  static inline void swap_chunk_at(std::uint8_t* a_bytes, std::uint8_t* b_bytes, bool cond) {
    ensure_chunk_offset_alignment<Chunk, Offset>();
    Chunk* __restrict__ a_ptr = reinterpret_cast<Chunk*>(a_bytes + Offset);
    Chunk* __restrict__ b_ptr = reinterpret_cast<Chunk*>(b_bytes + Offset);
#if defined(__clang__) || defined(__GNUC__)
    if constexpr (alignof(Chunk) > 1) {
      a_ptr = reinterpret_cast<Chunk*>(__builtin_assume_aligned(a_ptr, alignof(Chunk)));
      b_ptr = reinterpret_cast<Chunk*>(__builtin_assume_aligned(b_ptr, alignof(Chunk)));
    }
#endif
    core::template ct_swap_array<Chunk>(a_ptr, b_ptr, cond);
  }

  template <typename Chunk, std::size_t BaseOffset, std::size_t... Indices>
  static inline void set_chunks_sequence(
      std::uint8_t* dst_bytes, const std::uint8_t* src_bytes, bool cond, std::index_sequence<Indices...>
  ) {
    [[maybe_unused]] int expand[] = {
        0, (set_chunk_at<Chunk, BaseOffset + (Indices * sizeof(Chunk))>(dst_bytes, src_bytes, cond), 0)...
    };
    (void) expand;
  }

  template <typename Chunk, std::size_t BaseOffset, std::size_t... Indices>
  static inline void select_chunks_sequence(
      std::uint8_t* out_bytes, const std::uint8_t* a_bytes, const std::uint8_t* b_bytes, bool cond,
      std::index_sequence<Indices...>
  ) {
    [[maybe_unused]] int expand[] = {
        0, (select_chunk_at<Chunk, BaseOffset + (Indices * sizeof(Chunk))>(out_bytes, a_bytes, b_bytes, cond), 0)...
    };
    (void) expand;
  }

  template <typename Chunk, std::size_t BaseOffset, std::size_t... Indices>
  static inline void swap_chunks_sequence(
      std::uint8_t* a_bytes, std::uint8_t* b_bytes, bool cond, std::index_sequence<Indices...>
  ) {
    [[maybe_unused]] int expand[] = {
        0, (swap_chunk_at<Chunk, BaseOffset + (Indices * sizeof(Chunk))>(a_bytes, b_bytes, cond), 0)...
    };
    (void) expand;
  }

  template <typename Chunk, std::size_t BaseOffset, std::size_t Count>
  static inline void set_chunks(std::uint8_t* dst_bytes, const std::uint8_t* src_bytes, bool cond) {
    if constexpr (Count == 0) {
      return;
    } else if constexpr (std::is_integral_v<Chunk> && sizeof(Chunk) <= 8) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      Chunk* dst_ptr = reinterpret_cast<Chunk*>(dst_bytes + BaseOffset);
      const Chunk* src_ptr = reinterpret_cast<const Chunk*>(src_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      if constexpr (alignof(Chunk) > 1) {
        dst_ptr = reinterpret_cast<Chunk*>(__builtin_assume_aligned(dst_ptr, alignof(Chunk)));
        src_ptr = reinterpret_cast<const Chunk*>(__builtin_assume_aligned(src_ptr, alignof(Chunk)));
      }
#endif
      core::template ct_set_array<Chunk>(dst_ptr, src_ptr, Count, cond);
    } else if constexpr (Backend::supports_uint512 && std::is_same_v<Chunk, typename Backend::uint512_t>) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      auto* dst_ptr = reinterpret_cast<typename Backend::uint512_t*>(dst_bytes + BaseOffset);
      auto const* src_ptr = reinterpret_cast<const typename Backend::uint512_t*>(src_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      dst_ptr = reinterpret_cast<typename Backend::uint512_t*>(__builtin_assume_aligned(dst_ptr, alignof(Chunk)));
      src_ptr = reinterpret_cast<const typename Backend::uint512_t*>(__builtin_assume_aligned(src_ptr, alignof(Chunk)));
#endif
      core::template ct_set_array<typename Backend::uint512_t>(dst_ptr, src_ptr, Count, cond);
    } else if constexpr (Backend::supports_uint256 && std::is_same_v<Chunk, typename Backend::uint256_t>) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      auto* dst_ptr = reinterpret_cast<typename Backend::uint256_t*>(dst_bytes + BaseOffset);
      auto const* src_ptr = reinterpret_cast<const typename Backend::uint256_t*>(src_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      dst_ptr = reinterpret_cast<typename Backend::uint256_t*>(__builtin_assume_aligned(dst_ptr, alignof(Chunk)));
      src_ptr = reinterpret_cast<const typename Backend::uint256_t*>(__builtin_assume_aligned(src_ptr, alignof(Chunk)));
#endif
      core::template ct_set_array<typename Backend::uint256_t>(dst_ptr, src_ptr, Count, cond);
    } else if constexpr (Backend::supports_uint128 && std::is_same_v<Chunk, typename Backend::uint128_t>) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      auto* dst_ptr = reinterpret_cast<typename Backend::uint128_t*>(dst_bytes + BaseOffset);
      auto const* src_ptr = reinterpret_cast<const typename Backend::uint128_t*>(src_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      dst_ptr = reinterpret_cast<typename Backend::uint128_t*>(__builtin_assume_aligned(dst_ptr, alignof(Chunk)));
      src_ptr = reinterpret_cast<const typename Backend::uint128_t*>(__builtin_assume_aligned(src_ptr, alignof(Chunk)));
#endif
      core::template ct_set_array<typename Backend::uint128_t>(dst_ptr, src_ptr, Count, cond);
    } else {
      set_chunks_sequence<Chunk, BaseOffset>(dst_bytes, src_bytes, cond, std::make_index_sequence<Count>{});
    }
  }

  template <typename Chunk, std::size_t BaseOffset, std::size_t Count>
  static inline void select_chunks(
      std::uint8_t* out_bytes, const std::uint8_t* a_bytes, const std::uint8_t* b_bytes, bool cond
  ) {
    if constexpr (Count == 0) {
      return;
    } else if constexpr (std::is_integral_v<Chunk> && sizeof(Chunk) <= 8) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      Chunk* out_ptr = reinterpret_cast<Chunk*>(out_bytes + BaseOffset);
      const Chunk* a_ptr = reinterpret_cast<const Chunk*>(a_bytes + BaseOffset);
      const Chunk* b_ptr = reinterpret_cast<const Chunk*>(b_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      if constexpr (alignof(Chunk) > 1) {
        out_ptr = reinterpret_cast<Chunk*>(__builtin_assume_aligned(out_ptr, alignof(Chunk)));
        a_ptr = reinterpret_cast<const Chunk*>(__builtin_assume_aligned(a_ptr, alignof(Chunk)));
        b_ptr = reinterpret_cast<const Chunk*>(__builtin_assume_aligned(b_ptr, alignof(Chunk)));
      }
#endif
      core::template ct_select_array<Chunk>(out_ptr, a_ptr, b_ptr, Count, cond);
    } else if constexpr (Backend::supports_uint512 && std::is_same_v<Chunk, typename Backend::uint512_t>) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      auto* out_ptr = reinterpret_cast<typename Backend::uint512_t*>(out_bytes + BaseOffset);
      auto const* a_ptr = reinterpret_cast<const typename Backend::uint512_t*>(a_bytes + BaseOffset);
      auto const* b_ptr = reinterpret_cast<const typename Backend::uint512_t*>(b_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      out_ptr = reinterpret_cast<typename Backend::uint512_t*>(__builtin_assume_aligned(out_ptr, alignof(Chunk)));
      a_ptr = reinterpret_cast<const typename Backend::uint512_t*>(__builtin_assume_aligned(a_ptr, alignof(Chunk)));
      b_ptr = reinterpret_cast<const typename Backend::uint512_t*>(__builtin_assume_aligned(b_ptr, alignof(Chunk)));
#endif
      core::template ct_select_array<typename Backend::uint512_t>(out_ptr, a_ptr, b_ptr, Count, cond);
    } else if constexpr (Backend::supports_uint256 && std::is_same_v<Chunk, typename Backend::uint256_t>) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      auto* out_ptr = reinterpret_cast<typename Backend::uint256_t*>(out_bytes + BaseOffset);
      auto const* a_ptr = reinterpret_cast<const typename Backend::uint256_t*>(a_bytes + BaseOffset);
      auto const* b_ptr = reinterpret_cast<const typename Backend::uint256_t*>(b_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      out_ptr = reinterpret_cast<typename Backend::uint256_t*>(__builtin_assume_aligned(out_ptr, alignof(Chunk)));
      a_ptr = reinterpret_cast<const typename Backend::uint256_t*>(__builtin_assume_aligned(a_ptr, alignof(Chunk)));
      b_ptr = reinterpret_cast<const typename Backend::uint256_t*>(__builtin_assume_aligned(b_ptr, alignof(Chunk)));
#endif
      core::template ct_select_array<typename Backend::uint256_t>(out_ptr, a_ptr, b_ptr, Count, cond);
    } else if constexpr (Backend::supports_uint128 && std::is_same_v<Chunk, typename Backend::uint128_t>) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      auto* out_ptr = reinterpret_cast<typename Backend::uint128_t*>(out_bytes + BaseOffset);
      auto const* a_ptr = reinterpret_cast<const typename Backend::uint128_t*>(a_bytes + BaseOffset);
      auto const* b_ptr = reinterpret_cast<const typename Backend::uint128_t*>(b_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      out_ptr = reinterpret_cast<typename Backend::uint128_t*>(__builtin_assume_aligned(out_ptr, alignof(Chunk)));
      a_ptr = reinterpret_cast<const typename Backend::uint128_t*>(__builtin_assume_aligned(a_ptr, alignof(Chunk)));
      b_ptr = reinterpret_cast<const typename Backend::uint128_t*>(__builtin_assume_aligned(b_ptr, alignof(Chunk)));
#endif
      core::template ct_select_array<typename Backend::uint128_t>(out_ptr, a_ptr, b_ptr, Count, cond);
    } else {
      select_chunks_sequence<Chunk, BaseOffset>(out_bytes, a_bytes, b_bytes, cond, std::make_index_sequence<Count>{});
    }
  }

  template <typename Chunk, std::size_t BaseOffset, std::size_t Count>
  static inline void swap_chunks(std::uint8_t* a_bytes, std::uint8_t* b_bytes, bool cond) {
    if constexpr (Count == 0) {
      return;
    } else if constexpr (std::is_integral_v<Chunk> && sizeof(Chunk) <= 8) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      Chunk* a_ptr = reinterpret_cast<Chunk*>(a_bytes + BaseOffset);
      Chunk* b_ptr = reinterpret_cast<Chunk*>(b_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      if constexpr (alignof(Chunk) > 1) {
        a_ptr = reinterpret_cast<Chunk*>(__builtin_assume_aligned(a_ptr, alignof(Chunk)));
        b_ptr = reinterpret_cast<Chunk*>(__builtin_assume_aligned(b_ptr, alignof(Chunk)));
      }
#endif
      core::template ct_swap_array<Chunk>(a_ptr, b_ptr, Count, cond);
    } else if constexpr (Backend::supports_uint512 && std::is_same_v<Chunk, typename Backend::uint512_t>) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      auto* a_ptr = reinterpret_cast<typename Backend::uint512_t*>(a_bytes + BaseOffset);
      auto* b_ptr = reinterpret_cast<typename Backend::uint512_t*>(b_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      a_ptr = reinterpret_cast<typename Backend::uint512_t*>(__builtin_assume_aligned(a_ptr, alignof(Chunk)));
      b_ptr = reinterpret_cast<typename Backend::uint512_t*>(__builtin_assume_aligned(b_ptr, alignof(Chunk)));
#endif
      core::template ct_swap_array<typename Backend::uint512_t>(a_ptr, b_ptr, Count, cond);
    } else if constexpr (Backend::supports_uint256 && std::is_same_v<Chunk, typename Backend::uint256_t>) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      auto* a_ptr = reinterpret_cast<typename Backend::uint256_t*>(a_bytes + BaseOffset);
      auto* b_ptr = reinterpret_cast<typename Backend::uint256_t*>(b_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      a_ptr = reinterpret_cast<typename Backend::uint256_t*>(__builtin_assume_aligned(a_ptr, alignof(Chunk)));
      b_ptr = reinterpret_cast<typename Backend::uint256_t*>(__builtin_assume_aligned(b_ptr, alignof(Chunk)));
#endif
      core::template ct_swap_array<typename Backend::uint256_t>(a_ptr, b_ptr, Count, cond);
    } else if constexpr (Backend::supports_uint128 && std::is_same_v<Chunk, typename Backend::uint128_t>) {
      ensure_chunk_offset_alignment<Chunk, BaseOffset>();
      auto* a_ptr = reinterpret_cast<typename Backend::uint128_t*>(a_bytes + BaseOffset);
      auto* b_ptr = reinterpret_cast<typename Backend::uint128_t*>(b_bytes + BaseOffset);
#if defined(__clang__) || defined(__GNUC__)
      a_ptr = reinterpret_cast<typename Backend::uint128_t*>(__builtin_assume_aligned(a_ptr, alignof(Chunk)));
      b_ptr = reinterpret_cast<typename Backend::uint128_t*>(__builtin_assume_aligned(b_ptr, alignof(Chunk)));
#endif
      core::template ct_swap_array<typename Backend::uint128_t>(a_ptr, b_ptr, Count, cond);
    } else {
      swap_chunks_sequence<Chunk, BaseOffset>(a_bytes, b_bytes, cond, std::make_index_sequence<Count>{});
    }
  }

  template <typename T> static inline void set_struct_impl(T* dst, const T& src, bool cond) {
    validate_struct_alignment<T>();
    auto* dst_bytes = reinterpret_cast<std::uint8_t*>(dst);
    const auto* src_bytes = reinterpret_cast<const std::uint8_t*>(&src);
    const std::size_t total_bytes = sizeof(T);
    core::template ct_set_array<std::uint8_t>(dst_bytes, src_bytes, total_bytes, cond);
  }

  template <typename T> static inline T select_struct_impl(const T& a, const T& b, bool cond) {
    T out{};
    validate_struct_alignment<T>();
    auto* out_bytes = reinterpret_cast<std::uint8_t*>(&out);
    const auto* a_bytes = reinterpret_cast<const std::uint8_t*>(&a);
    const auto* b_bytes = reinterpret_cast<const std::uint8_t*>(&b);
    const std::size_t total_bytes = sizeof(T);
    core::template ct_select_array<std::uint8_t>(out_bytes, a_bytes, b_bytes, total_bytes, cond);
    return out;
  }

  template <typename T> static inline void swap_struct_impl(T* a, T* b, bool cond) {
    validate_struct_alignment<T>();
    auto* a_bytes = reinterpret_cast<std::uint8_t*>(a);
    auto* b_bytes = reinterpret_cast<std::uint8_t*>(b);
    const std::size_t total_bytes = sizeof(T);
    core::template ct_swap_array<std::uint8_t>(a_bytes, b_bytes, total_bytes, cond);
  }

  template <typename T> static inline void set_struct_array_impl(T* dst, const T* src, std::size_t n, bool cond) {
    if (n == 0) {
      return;
    }
    validate_struct_alignment<T>();
    auto* dst_bytes = reinterpret_cast<std::uint8_t*>(dst);
    const auto* src_bytes = reinterpret_cast<const std::uint8_t*>(src);
    const std::size_t total_bytes = n * sizeof(T);
    core::template ct_set_array<std::uint8_t>(dst_bytes, src_bytes, total_bytes, cond);
  }

  template <typename T>
  static inline void select_struct_array_impl(T* out, const T* a, const T* b, std::size_t n, bool cond) {
    if (n == 0) {
      return;
    }
    validate_struct_alignment<T>();
    auto* out_bytes = reinterpret_cast<std::uint8_t*>(out);
    const auto* a_bytes = reinterpret_cast<const std::uint8_t*>(a);
    const auto* b_bytes = reinterpret_cast<const std::uint8_t*>(b);
    const std::size_t total_bytes = n * sizeof(T);
    core::template ct_select_array<std::uint8_t>(out_bytes, a_bytes, b_bytes, total_bytes, cond);
  }

  template <typename T> static inline void swap_struct_array_impl(T* a, T* b, std::size_t n, bool cond) {
    if (n == 0) {
      return;
    }
    validate_struct_alignment<T>();
    auto* a_bytes = reinterpret_cast<std::uint8_t*>(a);
    auto* b_bytes = reinterpret_cast<std::uint8_t*>(b);
    const std::size_t total_bytes = n * sizeof(T);
    core::template ct_swap_array<std::uint8_t>(a_bytes, b_bytes, total_bytes, cond);
  }

  template <typename T> static inline bool eq_struct_array_impl(const T* a, const T* b, std::size_t n) {
    if (n == 0) {
      return true;
    }
    validate_struct_alignment<T>();
    const auto* a_bytes = reinterpret_cast<const std::uint8_t*>(a);
    const auto* b_bytes = reinterpret_cast<const std::uint8_t*>(b);
    const std::size_t total_bytes = n * sizeof(T);
    return core::template ct_eq_array<std::uint8_t>(a_bytes, b_bytes, total_bytes);
  }

  template <typename Chunk, std::size_t Offset>
  static inline void accumulate_eq_chunk(
      const std::uint8_t* a_bytes, const std::uint8_t* b_bytes, std::uint64_t& diff
  ) {
    constexpr std::size_t chunk_bytes = sizeof(Chunk);
    if constexpr (std::is_integral_v<Chunk> && chunk_bytes <= sizeof(std::uint64_t)) {
      const auto a_chunk = static_cast<std::make_unsigned_t<Chunk>>(load_chunk<Chunk>(a_bytes + Offset));
      const auto b_chunk = static_cast<std::make_unsigned_t<Chunk>>(load_chunk<Chunk>(b_bytes + Offset));
      diff |= static_cast<std::uint64_t>(a_chunk ^ b_chunk);
    } else {

      for (std::size_t lane = 0; lane < chunk_bytes; lane += sizeof(std::uint64_t)) {
        const auto lane_a = load_chunk<std::uint64_t>(a_bytes + Offset + lane);
        const auto lane_b = load_chunk<std::uint64_t>(b_bytes + Offset + lane);
        diff |= lane_a ^ lane_b;
      }
    }
  }

  template <typename Chunk, std::size_t BaseOffset, std::size_t... Indices>
  static inline void accumulate_chunks_sequence(
      const std::uint8_t* a_bytes, const std::uint8_t* b_bytes, std::uint64_t& diff, std::index_sequence<Indices...>
  ) {
    [[maybe_unused]] int expand[] = {
        0, (accumulate_eq_chunk<Chunk, BaseOffset + (Indices * sizeof(Chunk))>(a_bytes, b_bytes, diff), 0)...
    };
    (void) expand;
  }

  template <typename Chunk, std::size_t BaseOffset, std::size_t Count>
  static inline void accumulate_eq_chunks(
      const std::uint8_t* a_bytes, const std::uint8_t* b_bytes, std::uint64_t& diff
  ) {
    if constexpr (Count > 0) {

      accumulate_chunks_sequence<Chunk, BaseOffset>(a_bytes, b_bytes, diff, std::make_index_sequence<Count>{});
    }
  }

  template <typename T> static inline bool eq_struct_impl(const T& a, const T& b) {
    validate_struct_alignment<T>();
    const auto* a_bytes = reinterpret_cast<const std::uint8_t*>(&a);
    const auto* b_bytes = reinterpret_cast<const std::uint8_t*>(&b);
    const std::size_t total_bytes = sizeof(T);
    return core::template ct_eq_array<std::uint8_t>(a_bytes, b_bytes, total_bytes);
  }

public:
  static constexpr bool supports_uint128 = Backend::supports_uint128;
  static constexpr bool supports_uint256 = Backend::supports_uint256;
  static constexpr bool supports_uint512 = Backend::supports_uint512;

  template <typename T> static inline void ct_set(T* dst, const T& src, bool cond) {
    if constexpr (is_struct_type<T>()) {
      ensure_struct_constraints<T>();
      set_struct_impl(dst, src, cond);
    } else {
      core::template ct_set<T>(dst, src, cond);
    }
  }

  template <typename T> static inline T ct_select(const T& a, const T& b, bool cond) {
    if constexpr (is_struct_type<T>()) {
      ensure_struct_constraints<T>();
      return select_struct_impl(a, b, cond);
    } else {
      return core::template ct_select<T>(a, b, cond);
    }
  }

  template <typename T> static inline void ct_swap(T* a, T* b, bool cond) {
    if constexpr (is_struct_type<T>()) {
      ensure_struct_constraints<T>();
      swap_struct_impl(a, b, cond);
    } else {
      core::template ct_swap<T>(a, b, cond);
    }
  }

  template <typename T> static inline bool ct_eq(const T& a, const T& b) {
    if constexpr (is_struct_type<T>()) {
      ensure_struct_constraints<T>();
      return eq_struct_impl(a, b);
    } else {
      return core::template ct_eq<T>(a, b);
    }
  }

  template <typename T> static inline void ct_set_array(T* dst, const T* src, std::size_t n, bool cond) {
    if constexpr (is_struct_type<T>()) {
      ensure_struct_constraints<T>();
      set_struct_array_impl(dst, src, n, cond);
    } else {
      for (std::size_t i = 0; i < n; ++i) {
        ct_set(&dst[i], src[i], cond);
      }
    }
  }

  template <typename T> static inline void ct_select_array(T* out, const T* a, const T* b, std::size_t n, bool cond) {
    if constexpr (is_struct_type<T>()) {
      ensure_struct_constraints<T>();
      select_struct_array_impl(out, a, b, n, cond);
    } else {
      for (std::size_t i = 0; i < n; ++i) {
        out[i] = ct_select(a[i], b[i], cond);
      }
    }
  }

  template <typename T> static inline void ct_swap_array(T* a, T* b, std::size_t n, bool cond) {
    if constexpr (is_struct_type<T>()) {
      ensure_struct_constraints<T>();
      swap_struct_array_impl(a, b, n, cond);
    } else {
      for (std::size_t i = 0; i < n; ++i) {
        ct_swap(&a[i], &b[i], cond);
      }
    }
  }

  template <typename T> static inline bool ct_eq_array(const T* a, const T* b, std::size_t n) {
    if constexpr (is_struct_type<T>()) {
      ensure_struct_constraints<T>();
      return eq_struct_array_impl(a, b, n);
    } else {
      return core::template ct_eq_array<T>(a, b, n);
    }
  }
};

}

template <typename Backend = detail::default_backend> using struct_ops = detail::struct_ops_backend<Backend>;

namespace detail {
using default_struct_ops = struct_ops_backend<default_backend>;
}

template <typename T> inline void ct_set_data(T* dst, const T& src, bool cond) {
  detail::default_struct_ops::template ct_set<T>(dst, src, cond);
}

template <typename T> inline T ct_select_data(const T& a, const T& b, bool cond) {
  return detail::default_struct_ops::template ct_select<T>(a, b, cond);
}

template <typename T> inline void ct_swap_data(T* a, T* b, bool cond) {
  detail::default_struct_ops::template ct_swap<T>(a, b, cond);
}

template <typename T> inline bool ct_eq_data(const T& a, const T& b) {
  return detail::default_struct_ops::template ct_eq<T>(a, b);
}

template <typename T> inline void ct_set_data_array(T* dst, const T* src, std::size_t n, bool cond) {
  detail::default_struct_ops::template ct_set_array<T>(dst, src, n, cond);
}

template <typename T> inline void ct_select_data_array(T* out, const T* a, const T* b, std::size_t n, bool cond) {
  detail::default_struct_ops::template ct_select_array<T>(out, a, b, n, cond);
}

template <typename T> inline void ct_swap_data_array(T* a, T* b, std::size_t n, bool cond) {
  detail::default_struct_ops::template ct_swap_array<T>(a, b, n, cond);
}

template <typename T> inline bool ct_eq_data_array(const T* a, const T* b, std::size_t n) {
  return detail::default_struct_ops::template ct_eq_array<T>(a, b, n);
}

}
}
