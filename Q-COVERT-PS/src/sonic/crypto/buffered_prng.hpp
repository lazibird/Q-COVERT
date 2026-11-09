#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sonic/crypto/detail/backend_utils.hpp"
#include "sonic/crypto/prng.hpp"
#include "sonic/util/span.hpp"

namespace sn::crypto {

inline constexpr std::size_t k_buffered_prng_default_size = 16 * 1024;

template <std::size_t BufferSize = k_buffered_prng_default_size> class buffered_prng {
public:
  static_assert(BufferSize > 0, "buffer size must be positive");

  buffered_prng() = default;

  explicit buffered_prng(const typename prng::seed_material& seed) : engine_(seed), cursor_(BufferSize) {}

  void reseed(const typename prng::seed_material& seed) {
    engine_.reseed(seed);
    cursor_ = BufferSize;
  }

  void random_bytes(sn::util::span<std::uint8_t> out) { random_bytes(out.data(), out.size()); }

  void random_bytes(std::uint8_t* dst, std::size_t len) {
    if (len == 0) {
      return;
    }

    std::size_t remaining = len;
    std::uint8_t* cursor = dst;

    while (remaining > 0) {
      if (cursor_ >= BufferSize) {
        refill();
      }

      const std::size_t available = BufferSize - cursor_;
      const std::size_t chunk = std::min<std::size_t>(available, remaining);
      std::memcpy(cursor, buffer_.data() + cursor_, chunk);

      cursor_ += chunk;
      cursor += chunk;
      remaining -= chunk;
    }
  }

  std::uint64_t random_u64() {
    std::array<std::uint8_t, sizeof(std::uint64_t)> buf{};
    random_bytes(buf.data(), buf.size());
    return detail::load_trivial<std::uint64_t>(buf.data());
  }

  std::uint64_t random_u64(std::uint64_t begin, std::uint64_t end) {
    return detail::sample_range_with_u64(begin, end, [this]() { return random_u64(); });
  }

  prng& engine() { return engine_; }
  const prng& engine() const { return engine_; }

private:
  void refill() {
    engine_.random_bytes(buffer_.data(), buffer_.size());
    cursor_ = 0;
  }

  prng engine_{};
  std::array<std::uint8_t, BufferSize> buffer_{};
  std::size_t cursor_ = BufferSize;
};

}
