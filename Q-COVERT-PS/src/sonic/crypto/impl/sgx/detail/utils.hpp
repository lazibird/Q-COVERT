#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace sn::crypto::detail {

struct sgx_prng_state {
  std::array<std::uint8_t, 16> key{};
  std::array<std::uint8_t, 16> counter{};
  std::array<std::uint8_t, 16> keystream{};
  std::size_t keystream_used{16};
};

inline void secure_zero(void* ptr, std::size_t len) {
  auto* cursor = static_cast<volatile std::uint8_t*>(ptr);
  while (len-- != 0) {
    *cursor++ = 0;
  }
}

template <typename T> inline T load_trivial(const std::uint8_t* src) {
  static_assert(std::is_trivially_copyable<T>::value, "load_trivial requires trivially copyable type");
  T value;
  std::memcpy(&value, src, sizeof(T));
  return value;
}

template <typename T> inline void store_trivial(std::uint8_t* dst, const T& value) {
  static_assert(std::is_trivially_copyable<T>::value, "store_trivial requires trivially copyable type");
  std::memcpy(dst, &value, sizeof(T));
}

constexpr std::size_t max_chunk_size = 1u << 20;

} // namespace sn::crypto::detail
