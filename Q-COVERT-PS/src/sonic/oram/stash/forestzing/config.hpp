#pragma once

#include <cstdint>

namespace sn::oram::stash::forestzing {

namespace pipeline {
struct shape {
  std::uint64_t subtree_count = 1;
};
} // namespace pipeline

struct config {
  struct tree_geometry {
    std::uint64_t height = 0;
    std::uint32_t routing_depth = 0;
    std::uint64_t subtree_height = 0;
    std::uint32_t subtree_count = 0;
    std::uint64_t subpath_real_block_count = 0;
    std::uint64_t subpath_nonexistent_real_block_count = 0;
    std::uint64_t overlap_depth = 0;
    std::uint64_t non_overlapping_subpath_height = 0;
  } tree{};

  struct bucket_geometry {
    std::uint64_t real = 0;
    std::uint64_t dummy = 0;
  } bucket{};

  struct limits {
    std::uint64_t stash_bound = 0;
    std::uint64_t num_pathreads = 0;
    std::uint64_t eviction_rate = 0;
    std::uint64_t evict_batch = 0;
    std::uint64_t evictslots_per_path = 0;
  } limits{};

  bool disjoint_epoch_mode = false;
  bool single_thread_access = false;
  bool shared_top_backend = false;

  std::uint32_t balls_and_bins_security = 80;
  pipeline::shape shape{};
};

} // namespace sn::oram::stash::forestzing
