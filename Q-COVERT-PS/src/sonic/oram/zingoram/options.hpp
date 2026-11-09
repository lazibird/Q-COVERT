#pragma once

#include <cstdint>

namespace sn::oram::zingoram {

enum class eviction_backend : std::uint8_t {
  sonic,
  shared_top,
};

struct options {
  std::uint64_t block_count = 0;
  std::uint32_t bucket_real_size = 0;
  std::uint32_t bucket_dummy_size = 0;
  std::uint32_t eviction_rate = 0;
  std::uint64_t stash_bound = 0;
  std::uint32_t evict_batch = 1;
  std::uint32_t routing_depth = 0;
  std::uint32_t access_concurrency = 1;
  std::uint64_t disjoint_epoch_window = 0;
  eviction_backend evict_backend = eviction_backend::sonic;
};

} // namespace sn::oram::zingoram
