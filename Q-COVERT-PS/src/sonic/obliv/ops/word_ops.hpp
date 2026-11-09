#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::obliv {
namespace detail {

constexpr std::size_t native_word_bytes = sizeof(std::uint64_t);
constexpr std::size_t native_word_alignment = alignof(std::uint64_t);

template <std::size_t ByteCount> struct word_buffer_traits {
  static_assert(ByteCount % native_word_bytes == 0, "word_ops: byte count must be divisible by 8 bytes");
  static constexpr std::size_t word_count = ByteCount / native_word_bytes;
  static constexpr std::size_t byte_count = ByteCount;
};

struct word_alignment_guard {
  static inline void ensure(const void* ptr, const char* label) {
    if constexpr (native_word_alignment > 1) {
      const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
      sn::util::log::ensure((addr % native_word_alignment) == 0, label);
    } else {
      (void) ptr;
      (void) label;
    }
  }

  template <typename BytePtr> static inline BytePtr assume(BytePtr ptr) {
#if defined(__clang__) || defined(__GNUC__)
    if constexpr (native_word_alignment > 1) {
      ptr = reinterpret_cast<BytePtr>(__builtin_assume_aligned(ptr, native_word_alignment));
    }
#endif
    return ptr;
  }
};

inline std::uint64_t* cast_word_bytes(std::uint8_t* bytes, const char* label) {
  word_alignment_guard::ensure(bytes, label);
  auto* aligned = word_alignment_guard::assume(bytes);
  return reinterpret_cast<std::uint64_t*>(aligned);
}

inline const std::uint64_t* cast_word_bytes(const std::uint8_t* bytes, const char* label) {
  word_alignment_guard::ensure(bytes, label);
  auto* aligned = word_alignment_guard::assume(bytes);
  return reinterpret_cast<const std::uint64_t*>(aligned);
}

template <typename Buffer> inline void ensure_byte_size(Buffer&& buffer, std::size_t expected, const char* label) {
  sn::util::log::ensure(buffer.size() == expected, label);
  (void) buffer;
  (void) expected;
  (void) label;
}

inline void ensure_runtime_size(std::size_t bytes, const char* label) {
  sn::util::log::ensure(bytes % native_word_bytes == 0, label);
}

}

inline bool is_word_aligned(const void* ptr) {
  if constexpr (detail::native_word_alignment <= 1) {
    (void) ptr;
    return true;
  }
  const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
  return (addr % detail::native_word_alignment) == 0;
}

inline void ensure_word_aligned(const void* ptr, const char* label) {
  detail::word_alignment_guard::ensure(ptr, label);
}

template <std::size_t ByteCount>
inline constexpr std::size_t word_count_for_bytes = detail::word_buffer_traits<ByteCount>::word_count;

template <std::size_t ByteCount>
inline void ct_set_words(std::uint8_t* dst_bytes, const std::uint8_t* src_bytes, bool cond) {
  (void) sizeof(detail::word_buffer_traits<ByteCount>);
  auto* dst = detail::cast_word_bytes(dst_bytes, "ct_set_words: destination alignment requirement");
  const auto* src = detail::cast_word_bytes(src_bytes, "ct_set_words: source alignment requirement");
  sn::obliv::ct_set_array<std::uint64_t>(dst, src, detail::word_buffer_traits<ByteCount>::word_count, cond);
}

template <std::size_t ByteCount>
inline void ct_set_words(
    sn::util::span<std::uint8_t> dst_bytes, sn::util::span<const std::uint8_t> src_bytes, bool cond
) {
  detail::ensure_byte_size(
      dst_bytes, detail::word_buffer_traits<ByteCount>::byte_count, "ct_set_words: destination span size mismatch"
  );
  detail::ensure_byte_size(
      src_bytes, detail::word_buffer_traits<ByteCount>::byte_count, "ct_set_words: source span size mismatch"
  );
  ct_set_words<ByteCount>(dst_bytes.data(), src_bytes.data(), cond);
}

inline void ct_set_words(
    sn::util::span<std::uint8_t> dst_bytes, sn::util::span<const std::uint8_t> src_bytes, bool cond
) {
  sn::util::log::ensure(dst_bytes.size() == src_bytes.size(), "ct_set_words: span size mismatch");
  detail::ensure_runtime_size(dst_bytes.size(), "ct_set_words: byte count must be multiple of 8");
  auto* dst = detail::cast_word_bytes(dst_bytes.data(), "ct_set_words: destination alignment requirement");
  const auto* src = detail::cast_word_bytes(src_bytes.data(), "ct_set_words: source alignment requirement");
  sn::obliv::ct_set_array<std::uint64_t>(dst, src, dst_bytes.size() / detail::native_word_bytes, cond);
}

template <std::size_t ByteCount>
inline void ct_select_words(
    std::uint8_t* out_bytes, const std::uint8_t* a_bytes, const std::uint8_t* b_bytes, bool cond
) {
  (void) sizeof(detail::word_buffer_traits<ByteCount>);
  auto* out = detail::cast_word_bytes(out_bytes, "ct_select_words: output alignment requirement");
  const auto* a = detail::cast_word_bytes(a_bytes, "ct_select_words: lhs alignment requirement");
  const auto* b = detail::cast_word_bytes(b_bytes, "ct_select_words: rhs alignment requirement");
  sn::obliv::ct_select_array<std::uint64_t>(out, a, b, detail::word_buffer_traits<ByteCount>::word_count, cond);
}

template <std::size_t ByteCount>
inline void ct_select_words(
    sn::util::span<std::uint8_t> out_bytes, sn::util::span<const std::uint8_t> a_bytes,
    sn::util::span<const std::uint8_t> b_bytes, bool cond
) {
  detail::ensure_byte_size(
      out_bytes, detail::word_buffer_traits<ByteCount>::byte_count, "ct_select_words: out span size mismatch"
  );
  detail::ensure_byte_size(
      a_bytes, detail::word_buffer_traits<ByteCount>::byte_count, "ct_select_words: lhs span size mismatch"
  );
  detail::ensure_byte_size(
      b_bytes, detail::word_buffer_traits<ByteCount>::byte_count, "ct_select_words: rhs span size mismatch"
  );
  ct_select_words<ByteCount>(out_bytes.data(), a_bytes.data(), b_bytes.data(), cond);
}

inline void ct_select_words(
    sn::util::span<std::uint8_t> out_bytes, sn::util::span<const std::uint8_t> a_bytes,
    sn::util::span<const std::uint8_t> b_bytes, bool cond
) {
  sn::util::log::ensure(out_bytes.size() == a_bytes.size(), "ct_select_words: span size mismatch");
  sn::util::log::ensure(out_bytes.size() == b_bytes.size(), "ct_select_words: span size mismatch");
  detail::ensure_runtime_size(out_bytes.size(), "ct_select_words: byte count must be multiple of 8");
  auto* out = detail::cast_word_bytes(out_bytes.data(), "ct_select_words: output alignment requirement");
  const auto* a = detail::cast_word_bytes(a_bytes.data(), "ct_select_words: lhs alignment requirement");
  const auto* b = detail::cast_word_bytes(b_bytes.data(), "ct_select_words: rhs alignment requirement");
  sn::obliv::ct_select_array<std::uint64_t>(out, a, b, out_bytes.size() / detail::native_word_bytes, cond);
}

template <std::size_t ByteCount> inline void ct_swap_words(std::uint8_t* a_bytes, std::uint8_t* b_bytes, bool cond) {
  (void) sizeof(detail::word_buffer_traits<ByteCount>);
  auto* a = detail::cast_word_bytes(a_bytes, "ct_swap_words: lhs alignment requirement");
  auto* b = detail::cast_word_bytes(b_bytes, "ct_swap_words: rhs alignment requirement");
  sn::obliv::ct_swap_array<std::uint64_t>(a, b, detail::word_buffer_traits<ByteCount>::word_count, cond);
}

template <std::size_t ByteCount>
inline void ct_swap_words(sn::util::span<std::uint8_t> a_bytes, sn::util::span<std::uint8_t> b_bytes, bool cond) {
  detail::ensure_byte_size(
      a_bytes, detail::word_buffer_traits<ByteCount>::byte_count, "ct_swap_words: lhs span size mismatch"
  );
  detail::ensure_byte_size(
      b_bytes, detail::word_buffer_traits<ByteCount>::byte_count, "ct_swap_words: rhs span size mismatch"
  );
  ct_swap_words<ByteCount>(a_bytes.data(), b_bytes.data(), cond);
}

inline void ct_swap_words(sn::util::span<std::uint8_t> a_bytes, sn::util::span<std::uint8_t> b_bytes, bool cond) {
  sn::util::log::ensure(a_bytes.size() == b_bytes.size(), "ct_swap_words: span size mismatch");
  detail::ensure_runtime_size(a_bytes.size(), "ct_swap_words: byte count must be multiple of 8");
  auto* a = detail::cast_word_bytes(a_bytes.data(), "ct_swap_words: lhs alignment requirement");
  auto* b = detail::cast_word_bytes(b_bytes.data(), "ct_swap_words: rhs alignment requirement");
  sn::obliv::ct_swap_array<std::uint64_t>(a, b, a_bytes.size() / detail::native_word_bytes, cond);
}

}
