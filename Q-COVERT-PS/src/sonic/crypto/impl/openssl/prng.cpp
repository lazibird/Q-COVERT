#include "sonic/crypto/prng.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include <openssl/evp.h>

#include "sonic/crypto/error.hpp"

namespace sn::crypto {

namespace {
constexpr std::size_t kEvpMaxRequest = static_cast<std::size_t>(std::numeric_limits<int>::max());
constexpr std::size_t kZeroBufferSize = detail::max_chunk_size;
alignas(16) const std::array<std::uint8_t, kZeroBufferSize> kZeroBuffer{};

std::size_t clamp_step(std::size_t remaining, std::size_t bytes_available_this_key) {
  return std::min<std::size_t>(
      {remaining, bytes_available_this_key, kZeroBufferSize, detail::max_chunk_size, kEvpMaxRequest}
  );
}
}

prng::prng() {
  allow_auto_reseed_ = true;
  u64_cache_used_ = block_size;
  const auto seed = make_seed(seeder_);
  reseed(seed);
}

prng::prng(const seed_material& seed) {
  allow_auto_reseed_ = false;
  u64_cache_used_ = block_size;
  reseed(seed);
}

prng::~prng() {
  detail::secure_zero(seed_.key.data(), seed_.key.size());
  detail::secure_zero(seed_.iv.data(), seed_.iv.size());
  detail::secure_zero(u64_cache_.data(), u64_cache_.size());
  if (ctx_.get() != nullptr) {
    EVP_CIPHER_CTX_reset(ctx_.get());
  }
}

void prng::reseed(const seed_material& seed) {
  seed_ = seed;
  detail::check_evp(
      EVP_EncryptInit_ex(ctx_.get(), EVP_aes_256_ctr(), nullptr, seed_.key.data(), seed_.iv.data()),
      "EVP_EncryptInit_ex"
  );
  detail::check_evp(EVP_CIPHER_CTX_set_padding(ctx_.get(), 0), "EVP_CIPHER_CTX_set_padding");
  blocks_generated_ = 0;
  u64_cache_used_ = block_size;
  detail::secure_zero(u64_cache_.data(), u64_cache_.size());
}

prng::seed_material prng::make_seed(random_device& rd) {
  seed_material seed;
  rd.fill(seed.key.data(), seed.key.size());
  rd.fill(seed.iv.data(), seed.iv.size());
  return seed;
}

prng::seed_material prng::make_seed() {
  random_device rd;
  return make_seed(rd);
}

void prng::random_bytes(sn::util::span<std::uint8_t> out) { random_bytes(out.data(), out.size()); }

void prng::random_bytes(std::uint8_t* dst, std::size_t len) {
  if (len == 0) {
    return;
  }

  if (!allow_auto_reseed_) {
    const std::uint64_t blocks_remaining = max_blocks_per_key - blocks_generated_;
    const std::uint64_t bytes_remaining = blocks_remaining * block_size;
    if (static_cast<std::uint64_t>(len) > bytes_remaining) {
      throw error("prng block budget exceeded for deterministic stream");
    }
  }

  std::size_t remaining = len;
  std::uint8_t* cursor = dst;

  while (remaining > 0) {
    if (allow_auto_reseed_ && blocks_generated_ >= max_blocks_per_key) {
      reseed(make_seed(seeder_));
    } else if (!allow_auto_reseed_ && blocks_generated_ >= max_blocks_per_key) {
      throw error("prng block budget exceeded for deterministic stream");
    }

    const std::uint64_t blocks_available = max_blocks_per_key - blocks_generated_;
    const std::uint64_t raw_bytes_available = blocks_available * block_size;
    const std::size_t bytes_available_this_key = static_cast<std::size_t>(std::min<std::uint64_t>(
        raw_bytes_available, static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
    ));

    if (bytes_available_this_key == 0) {
      if (allow_auto_reseed_) {
        reseed(make_seed(seeder_));
        continue;
      }
      throw error("prng block budget exhausted");
    }

    const std::size_t step = clamp_step(remaining, bytes_available_this_key);
    if (step == 0) {
      break;
    }

    int produced = 0;
    detail::check_evp(
        EVP_EncryptUpdate(ctx_.get(), cursor, &produced, kZeroBuffer.data(), static_cast<int>(step)),
        "EVP_EncryptUpdate"
    );
    if (produced != static_cast<int>(step)) {
      detail::throw_openssl_error("EVP_EncryptUpdate short output");
    }

    cursor += static_cast<std::size_t>(produced);
    remaining -= static_cast<std::size_t>(produced);

    const std::uint64_t used_blocks = (static_cast<std::uint64_t>(produced) + block_size - 1) / block_size;
    blocks_generated_ += used_blocks;
  }
}

std::uint64_t prng::random_u64() {
  if (u64_cache_used_ + sizeof(std::uint64_t) <= u64_cache_.size()) {
    const auto value = detail::load_trivial<std::uint64_t>(u64_cache_.data() + u64_cache_used_);
    u64_cache_used_ += sizeof(std::uint64_t);
    return value;
  }

  random_bytes(u64_cache_.data(), u64_cache_.size());
  u64_cache_used_ = sizeof(std::uint64_t);
  return detail::load_trivial<std::uint64_t>(u64_cache_.data());
}

std::uint64_t prng::random_u64(std::uint64_t begin, std::uint64_t end) {
  return detail::sample_range_with_u64(begin, end, [this]() { return random_u64(); });
}

}
