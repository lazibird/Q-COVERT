#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "sonic/crypto/detail/backend_utils.hpp"
#include "sonic/crypto/random.hpp"
#include "sonic/util/span.hpp"

namespace sn::crypto {

namespace detail {

template <typename Next>
inline std::uint64_t sample_range_with_u64(std::uint64_t begin, std::uint64_t end, Next&& next) {
  if (end <= begin) {
    return begin;
  }

  const std::uint64_t range = end - begin;
  const std::uint64_t threshold = (-range) % range;
  auto&& generator = std::forward<Next>(next);

  while (true) {
    __uint128_t product = static_cast<__uint128_t>(generator()) * static_cast<__uint128_t>(range);
    const std::uint64_t lower = static_cast<std::uint64_t>(product);
    if (lower >= threshold) {
      return begin + static_cast<std::uint64_t>(product >> 64);
    }
  }
}

}

class prng {
public:
  static constexpr std::size_t key_size = 32;
  static constexpr std::size_t nonce_size = 16;
  static constexpr std::size_t block_size = 16;
  static constexpr std::uint64_t max_blocks_per_key = 1ull << 32;

  struct seed_material {
    std::array<std::uint8_t, key_size> key{};
    std::array<std::uint8_t, nonce_size> iv{};
  };

  prng();
  explicit prng(const seed_material& seed);
  ~prng();

  prng(const prng&) = delete;
  prng& operator=(const prng&) = delete;

  prng(prng&&) noexcept = default;
  prng& operator=(prng&&) noexcept = default;

  void reseed(const seed_material& seed);

  static seed_material make_seed(random_device& rd);
  static seed_material make_seed();

  void random_bytes(sn::util::span<std::uint8_t> out);

  void random_bytes(std::uint8_t* dst, std::size_t len);

  std::uint64_t random_u64();

  std::uint64_t random_u64(std::uint64_t begin, std::uint64_t end);

private:
  using prng_state_type =
#if defined(RINGDORAM_CRYPTO_BACKEND_SGX)
      detail::sgx_prng_state
#else
      detail::evp_cipher_ctx
#endif
      ;

  prng_state_type ctx_{};
  seed_material seed_{};
  std::uint64_t blocks_generated_ = 0;
  bool allow_auto_reseed_ = true;
  random_device seeder_{};
  alignas(16) std::array<std::uint8_t, block_size> u64_cache_{};
  std::size_t u64_cache_used_ = block_size;
};

}
