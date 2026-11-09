#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/util/log.hpp"

namespace sn::oram::tree {

namespace log = sn::util::log;

inline std::size_t floor_log2(std::size_t value) {
  log::ensure(value > 0, "tree::overlap: floor_log2 input must be positive");
  std::size_t result = 0;
  while ((static_cast<std::size_t>(1) << (result + 1)) <= value) {
    ++result;
  }
  return result;
}

/**
 * utility class for calculating tree overlap
 */
class tree_overlap {
public:
  /** zero-indexed tree height (root=0, leaves=H) */
  std::size_t height;
  /** depth of tree overlap (0: no overlap, 1: root overlap, etc.) */
  std::size_t overlap_depth;

private:
  std::size_t max_overlapping_paths_;
  std::vector<std::size_t> n_mask_lut_;
  std::vector<std::size_t> n_mask_epsum_;

public:
  tree_overlap(std::size_t tree_height, std::size_t tree_overlap_depth) :
      height(tree_height),
      overlap_depth(tree_overlap_depth),
      max_overlapping_paths_(0),
      n_mask_lut_(),
      n_mask_epsum_() {
    log::ensure(overlap_depth <= height, "overlap depth must be <= height");

    // the maximum number of paths that can overlap in the overlap region
    // e.g. with 0 overlap, only 1 path
    // with depth 1 overlap (just the root), two paths
    // so 2^overlap_depth
    max_overlapping_paths_ = std::size_t{1} << overlap_depth;

    // when we have overlapping paths, we need to mask nodes that are touched by multiple paths
    // so we define a function nMask(i) which gives how many nodes at the top of the path
    // are "masked", meaning they were covered by an existing path, so we can skip reading them out
    // as an example:
    // p0:  1  2  4  8
    // p1:  1* 3  6  12
    // p2:  1* 2* 5  10
    // p3:  1* 3* 7  14
    // p4:  1* 2* 4* 9
    // p5:  1* 3* 6* 13
    // p6:  1* 2* 5* 11
    // p7:  1* 3* 7* 15
    // nMask(i) is effectively given by floor(log2(i))+1
    // we will precompute this as a lut
    n_mask_lut_.resize(max_overlapping_paths_);
    if (!n_mask_lut_.empty()) {
      n_mask_lut_[0] = 0;
      for (std::size_t i = 1; i < max_overlapping_paths_; ++i) {
        n_mask_lut_[i] = 1 + floor_log2(i);
      }
    }
    // we will also compute the exclusive prefix sum of this function
    n_mask_epsum_.resize(max_overlapping_paths_ + 1);
    n_mask_epsum_[0] = 0;
    for (std::size_t i = 1; i <= max_overlapping_paths_; ++i) {
      n_mask_epsum_[i] = n_mask_epsum_[i - 1] + n_mask_lut_[i - 1];
    }
  }

  tree_overlap() : tree_overlap(0, 0) {}

  std::size_t n_max_overlapping_paths() const { return max_overlapping_paths_; }

  /** get the number of nodes that are masked by the overlapping paths */
  std::size_t n_mask_for_path_ix(std::size_t path_ix) const {
    log::ensure(path_ix < max_overlapping_paths_, "path_ix out of range");
    return n_mask_lut_[path_ix];
  }

  /** get the number of nodes that are masked by the overlapping paths up to a certain path */
  std::size_t n_mask_for_path_ix_epsum(std::size_t path_ix) const {
    log::ensure(path_ix < max_overlapping_paths_, "path_ix out of range");
    return n_mask_epsum_[path_ix];
  }

  /** get the total number of unique nodes in all overlapping paths */
  std::size_t n_unique_path_nodes() const {
    // on its own, each path has H+1 nodes
    // but due to overlap, we multi-count overlapping nodes
    // masking accounts for overlapped nodes
    // so we can subtract our prefix sum of the masking function

    // the base number of nodes across all paths, without deduplication
    std::ptrdiff_t n_total_nodes = static_cast<std::ptrdiff_t>(height + 1) * max_overlapping_paths_;

    // the number of multi-counted nodes (total number of masked nodes)
    std::ptrdiff_t n_masked_nodes = static_cast<std::ptrdiff_t>(n_mask_epsum_[max_overlapping_paths_]);
    log::ensure(n_masked_nodes <= n_total_nodes, "masked nodes exceed total nodes");

    // subtract the masked nodes from the total
    std::ptrdiff_t n_unique_nodes = n_total_nodes - n_masked_nodes;
    log::ensure(n_unique_nodes > 0, "n_unique_nodes must be positive");

    return static_cast<std::size_t>(n_unique_nodes);
  }

  /** get the total number of nodes in the overlap region of the tree */
  std::size_t n_overlap_region_nodes() const {
    // it's just the number of nodes above the overlap depth
    // overlap depth 0: no overlap
    // overlap depth 1: levels 0 (root) and above (1 node)
    // overlap depth 2: levels 0-1 and above (3 nodes)

    if (overlap_depth <= 0) {
      return 0;
    }

    // at overlap depth i, all levels <i are included
    // meaning there are 2^i - 1 nodes
    std::size_t overlap_region_n_nodes = (std::size_t{1} << overlap_depth) - 1;

    return overlap_region_n_nodes;
  }

  static std::size_t evict_batch_size_to_overlap_depth(std::size_t evict_batch_size) {
    // first, ensure it's a power of 2
    log::ensure((evict_batch_size & (evict_batch_size - 1)) == 0, "evict batch size must be a power of 2");
    // then, convert number of overlapping paths to our overlap depth
    return floor_log2(evict_batch_size);
  }
};

} // namespace sn::oram::tree
