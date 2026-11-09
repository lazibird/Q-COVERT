#include "sonic/crypto/prng.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <sgx_tcrypto.h>

#include "sonic/crypto/error.hpp"

namespace sn::crypto {

namespace {

void generate_block(detail::sgx_prng_state& state) {
  const std::array<std::uint8_t, prng::block_size> zeros{};
  const auto* key = reinterpret_cast<const sgx_aes_ctr_128bit_key_t*>(state.key.data());
  const sgx_status_t status = sgx_aes_ctr_encrypt(
      key, zeros.data(), static_cast<std::uint32_t>(zeros.size()), state.counter.data(), 128,
      state.keystream.data()
  );
  if (status != SGX_SUCCESS) {
    throw error("sgx_aes_ctr_encrypt failed");
  }
  state.keystream_used = 0;
}

} // namespace

prng::prng() {
  allow_auto_reseed_ = true;
  reseed(make_seed(seeder_));
}

prng::prng(const seed_material& seed) {
  allow_auto_reseed_ = false;
  reseed(seed);
}

prng::~prng() {
  detail::secure_zero(&ctx_, sizeof(ctx_));
  detail::secure_zero(&seed_, sizeof(seed_));
  detail::secure_zero(u64_cache_.data(), u64_cache_.size());
}

void prng::reseed(const seed_material& seed) {
  seed_ = seed;
  for (std::size_t i = 0; i < ctx_.key.size(); ++i) {
    ctx_.key[i] = static_cast<std::uint8_t>(seed.key[i] ^ seed.key[i + ctx_.key.size()]);
  }
  std::copy(seed.iv.begin(), seed.iv.end(), ctx_.counter.begin());
  detail::secure_zero(ctx_.keystream.data(), ctx_.keystream.size());
  ctx_.keystream_used = ctx_.keystream.size();
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
  if (dst == nullptr) {
    throw error("prng: null destination");
  }

  while (len != 0) {
    if (ctx_.keystream_used == ctx_.keystream.size()) {
      if (blocks_generated_ == max_blocks_per_key) {
        if (!allow_auto_reseed_) {
          throw error("prng block budget exceeded for deterministic stream");
        }
        reseed(make_seed(seeder_));
      }
      generate_block(ctx_);
      ++blocks_generated_;
    }

    const std::size_t available = ctx_.keystream.size() - ctx_.keystream_used;
    const std::size_t step = std::min(available, len);
    std::memcpy(dst, ctx_.keystream.data() + ctx_.keystream_used, step);
    ctx_.keystream_used += step;
    dst += step;
    len -= step;
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

} // namespace sn::crypto
