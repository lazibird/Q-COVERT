#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace sn::mem {

namespace detail {

#if defined(SGX_TRUSTED)
#define SONIC_USE_CUSTOM_MEMCPY 1
#endif

#if defined(SONIC_USE_CUSTOM_MEMCPY) && SONIC_USE_CUSTOM_MEMCPY
constexpr bool kCustomMemcpyEnabled = true;
#else
constexpr bool kCustomMemcpyEnabled = false;
#endif

inline void bytewise_copy(std::uint8_t* dst, const std::uint8_t* src, std::size_t count) {
  std::size_t idx = 0;
  constexpr std::size_t kBlock = 32;
  for (; idx + kBlock <= count; idx += kBlock) {
    dst[idx + 0] = src[idx + 0];
    dst[idx + 1] = src[idx + 1];
    dst[idx + 2] = src[idx + 2];
    dst[idx + 3] = src[idx + 3];
    dst[idx + 4] = src[idx + 4];
    dst[idx + 5] = src[idx + 5];
    dst[idx + 6] = src[idx + 6];
    dst[idx + 7] = src[idx + 7];
    dst[idx + 8] = src[idx + 8];
    dst[idx + 9] = src[idx + 9];
    dst[idx + 10] = src[idx + 10];
    dst[idx + 11] = src[idx + 11];
    dst[idx + 12] = src[idx + 12];
    dst[idx + 13] = src[idx + 13];
    dst[idx + 14] = src[idx + 14];
    dst[idx + 15] = src[idx + 15];
    dst[idx + 16] = src[idx + 16];
    dst[idx + 17] = src[idx + 17];
    dst[idx + 18] = src[idx + 18];
    dst[idx + 19] = src[idx + 19];
    dst[idx + 20] = src[idx + 20];
    dst[idx + 21] = src[idx + 21];
    dst[idx + 22] = src[idx + 22];
    dst[idx + 23] = src[idx + 23];
    dst[idx + 24] = src[idx + 24];
    dst[idx + 25] = src[idx + 25];
    dst[idx + 26] = src[idx + 26];
    dst[idx + 27] = src[idx + 27];
    dst[idx + 28] = src[idx + 28];
    dst[idx + 29] = src[idx + 29];
    dst[idx + 30] = src[idx + 30];
    dst[idx + 31] = src[idx + 31];
  }

  if (idx + 16 <= count) {
    dst[idx + 0] = src[idx + 0];
    dst[idx + 1] = src[idx + 1];
    dst[idx + 2] = src[idx + 2];
    dst[idx + 3] = src[idx + 3];
    dst[idx + 4] = src[idx + 4];
    dst[idx + 5] = src[idx + 5];
    dst[idx + 6] = src[idx + 6];
    dst[idx + 7] = src[idx + 7];
    dst[idx + 8] = src[idx + 8];
    dst[idx + 9] = src[idx + 9];
    dst[idx + 10] = src[idx + 10];
    dst[idx + 11] = src[idx + 11];
    dst[idx + 12] = src[idx + 12];
    dst[idx + 13] = src[idx + 13];
    dst[idx + 14] = src[idx + 14];
    dst[idx + 15] = src[idx + 15];
    idx += 16;
  }

  if (idx + 8 <= count) {
    dst[idx + 0] = src[idx + 0];
    dst[idx + 1] = src[idx + 1];
    dst[idx + 2] = src[idx + 2];
    dst[idx + 3] = src[idx + 3];
    dst[idx + 4] = src[idx + 4];
    dst[idx + 5] = src[idx + 5];
    dst[idx + 6] = src[idx + 6];
    dst[idx + 7] = src[idx + 7];
    idx += 8;
  }

  if (idx + 4 <= count) {
    dst[idx + 0] = src[idx + 0];
    dst[idx + 1] = src[idx + 1];
    dst[idx + 2] = src[idx + 2];
    dst[idx + 3] = src[idx + 3];
    idx += 4;
  }

  for (; idx < count; ++idx) {
    dst[idx] = src[idx];
  }
}

inline void bytewise_fill(std::uint8_t* dst, std::uint8_t value, std::size_t count) {
  std::size_t idx = 0;
  constexpr std::size_t kBlock = 32;
  for (; idx + kBlock <= count; idx += kBlock) {
    dst[idx + 0] = value;
    dst[idx + 1] = value;
    dst[idx + 2] = value;
    dst[idx + 3] = value;
    dst[idx + 4] = value;
    dst[idx + 5] = value;
    dst[idx + 6] = value;
    dst[idx + 7] = value;
    dst[idx + 8] = value;
    dst[idx + 9] = value;
    dst[idx + 10] = value;
    dst[idx + 11] = value;
    dst[idx + 12] = value;
    dst[idx + 13] = value;
    dst[idx + 14] = value;
    dst[idx + 15] = value;
    dst[idx + 16] = value;
    dst[idx + 17] = value;
    dst[idx + 18] = value;
    dst[idx + 19] = value;
    dst[idx + 20] = value;
    dst[idx + 21] = value;
    dst[idx + 22] = value;
    dst[idx + 23] = value;
    dst[idx + 24] = value;
    dst[idx + 25] = value;
    dst[idx + 26] = value;
    dst[idx + 27] = value;
    dst[idx + 28] = value;
    dst[idx + 29] = value;
    dst[idx + 30] = value;
    dst[idx + 31] = value;
  }

  if (idx + 16 <= count) {
    dst[idx + 0] = value;
    dst[idx + 1] = value;
    dst[idx + 2] = value;
    dst[idx + 3] = value;
    dst[idx + 4] = value;
    dst[idx + 5] = value;
    dst[idx + 6] = value;
    dst[idx + 7] = value;
    dst[idx + 8] = value;
    dst[idx + 9] = value;
    dst[idx + 10] = value;
    dst[idx + 11] = value;
    dst[idx + 12] = value;
    dst[idx + 13] = value;
    dst[idx + 14] = value;
    dst[idx + 15] = value;
    idx += 16;
  }

  if (idx + 8 <= count) {
    dst[idx + 0] = value;
    dst[idx + 1] = value;
    dst[idx + 2] = value;
    dst[idx + 3] = value;
    dst[idx + 4] = value;
    dst[idx + 5] = value;
    dst[idx + 6] = value;
    dst[idx + 7] = value;
    idx += 8;
  }

  if (idx + 4 <= count) {
    dst[idx + 0] = value;
    dst[idx + 1] = value;
    dst[idx + 2] = value;
    dst[idx + 3] = value;
    idx += 4;
  }

  for (; idx < count; ++idx) {
    dst[idx] = value;
  }
}

inline void copy_aligned_words(std::uint8_t* dst_bytes, const std::uint8_t* src_bytes, std::size_t byte_count) {
  constexpr std::size_t kWord = sizeof(std::uint64_t);
  const std::size_t word_count = byte_count / kWord;
  auto* dst_words = reinterpret_cast<std::uint64_t*>(dst_bytes);
  const auto* src_words = reinterpret_cast<const std::uint64_t*>(src_bytes);

  std::size_t idx = 0;
  for (; idx + 8 <= word_count; idx += 8) {
    dst_words[idx + 0] = src_words[idx + 0];
    dst_words[idx + 1] = src_words[idx + 1];
    dst_words[idx + 2] = src_words[idx + 2];
    dst_words[idx + 3] = src_words[idx + 3];
    dst_words[idx + 4] = src_words[idx + 4];
    dst_words[idx + 5] = src_words[idx + 5];
    dst_words[idx + 6] = src_words[idx + 6];
    dst_words[idx + 7] = src_words[idx + 7];
  }

  if (idx + 4 <= word_count) {
    dst_words[idx + 0] = src_words[idx + 0];
    dst_words[idx + 1] = src_words[idx + 1];
    dst_words[idx + 2] = src_words[idx + 2];
    dst_words[idx + 3] = src_words[idx + 3];
    idx += 4;
  }

  for (; idx < word_count; ++idx) {
    dst_words[idx] = src_words[idx];
  }

  const std::size_t copied_bytes = word_count * kWord;
  bytewise_copy(dst_bytes + copied_bytes, src_bytes + copied_bytes, byte_count - copied_bytes);
}

template <typename T> inline void fill_trivial_impl(T* dst, std::size_t count, const T& value) {
  std::size_t idx = 0;
  for (; idx + 8 <= count; idx += 8) {
    dst[idx + 0] = value;
    dst[idx + 1] = value;
    dst[idx + 2] = value;
    dst[idx + 3] = value;
    dst[idx + 4] = value;
    dst[idx + 5] = value;
    dst[idx + 6] = value;
    dst[idx + 7] = value;
  }

  if (idx + 4 <= count) {
    dst[idx + 0] = value;
    dst[idx + 1] = value;
    dst[idx + 2] = value;
    dst[idx + 3] = value;
    idx += 4;
  }

  for (; idx < count; ++idx) {
    dst[idx] = value;
  }
}

}

[[nodiscard]] inline constexpr bool custom_memcpy_enabled() { return detail::kCustomMemcpyEnabled; }

inline void copy_bytes(void* dst, const void* src, std::size_t byte_count) {
  if (byte_count == 0) {
    return;
  }

  if constexpr (!detail::kCustomMemcpyEnabled) {
    std::memcpy(dst, src, byte_count);
    return;
  }

  auto* dst_bytes = static_cast<std::uint8_t*>(dst);
  const auto* src_bytes = static_cast<const std::uint8_t*>(src);
  constexpr std::size_t kWord = sizeof(std::uint64_t);
  const bool both_aligned =
      ((reinterpret_cast<std::uintptr_t>(dst_bytes) | reinterpret_cast<std::uintptr_t>(src_bytes)) & (kWord - 1U)) == 0;

  if (both_aligned && byte_count >= kWord) {
    detail::copy_aligned_words(dst_bytes, src_bytes, byte_count);
  } else {
    detail::bytewise_copy(dst_bytes, src_bytes, byte_count);
  }
}

inline void fill_bytes(void* dst, std::uint8_t value, std::size_t byte_count) {
  if (byte_count == 0) {
    return;
  }

  if constexpr (!detail::kCustomMemcpyEnabled) {
    std::memset(dst, value, byte_count);
    return;
  }

  detail::bytewise_fill(static_cast<std::uint8_t*>(dst), value, byte_count);
}

template <typename T> inline void fill_trivial(T* dst, std::size_t count, const T& value) {
  if (count == 0) {
    return;
  }

  if constexpr (!detail::kCustomMemcpyEnabled) {
    for (std::size_t i = 0; i < count; ++i) {
      dst[i] = value;
    }
    return;
  }

  detail::fill_trivial_impl(dst, count, value);
}

}
