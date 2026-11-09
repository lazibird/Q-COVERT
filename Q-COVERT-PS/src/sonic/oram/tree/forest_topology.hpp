#pragma once

#include <cstdint>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::tree {

class forest_topology {
public:
  forest_topology() = default;

  forest_topology(std::uint64_t tree_height, std::uint32_t routing_depth) { reset(tree_height, routing_depth); }

  void reset(std::uint64_t tree_height, std::uint32_t routing_depth) {
    sn::util::log::ensure(tree_height >= routing_depth, "forest_topology: routing depth exceeds tree height");
    tree_height_ = tree_height;
    routing_depth_ = routing_depth;
    routing_subtree_count_ = 1ULL << routing_depth_;
    routing_subtree_height_ = tree_height_ - routing_depth_;
    subtree_leaf_count_ = 1ULL << routing_subtree_height_;
    subtree_leaf_mask_ = subtree_leaf_count_ - 1ULL;
    // largest node id below the routing depth
    largest_nonexistent_node_id_ = routing_depth_ == 0 ? 0ULL : ((1ULL << routing_depth_) - 1ULL);
  }

  [[nodiscard]] std::uint64_t tree_height() const noexcept { return tree_height_; }
  [[nodiscard]] std::uint32_t routing_depth() const noexcept { return routing_depth_; }
  [[nodiscard]] std::uint64_t routing_subtree_count() const noexcept { return routing_subtree_count_; }
  [[nodiscard]] std::uint64_t routing_subtree_height() const noexcept { return routing_subtree_height_; }
  [[nodiscard]] std::uint64_t routing_subtree_leaf_count() const noexcept { return subtree_leaf_count_; }

  // get the range of virtual tree leaves that belong to a subtree
  [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> leaf_range_for_subtree(std::uint64_t subtree_ix) const {
    sn::util::log::ensure(
        subtree_ix < routing_subtree_count_, "forest_topology::leaf_range_for_subtree: subtree_ix out of range"
    );
    const std::uint64_t begin = subtree_ix << routing_subtree_height_;
    return {begin, begin + subtree_leaf_count_};
  }

  // compute the routing subtree index that a virtual tree leaf index belongs to
  [[nodiscard]] std::uint64_t leaf_ix_to_subtree_ix(std::uint64_t leaf_ix) const {
    std::uint64_t subtree_ix = leaf_ix >> routing_subtree_height_;
    return subtree_ix;
  }

  // compute the subtree-local leaf index from a virtual tree leaf index
  [[nodiscard]] std::uint64_t leaf_ix_to_subtree_leaf_ix(std::uint64_t leaf_ix) const {
    std::uint64_t subtree_leaf_ix = leaf_ix & subtree_leaf_mask_;
    return subtree_leaf_ix;
  }

  // convert a virtual tree node id to a subtree-local node id
  [[nodiscard]] std::uint64_t node_id_to_subtree_node_id(std::uint64_t node_id, std::uint64_t subtree_ix) const {
    // if routing depth is 0, unchanged
    if (routing_depth_ == 0) {
      return node_id;
    }

    // ensure the virtual tree node id is not within a nonexistent level
    sn::util::log::ensure(
        node_id > largest_nonexistent_node_id_, "node_id_to_subtree_node_id: node_id within nonexistent level"
    );

    // determine node level within virtual tree
    std::uint64_t vtree_node_level = fast_log2(node_id);
    // determine number of nodes in level
    std::uint64_t vtree_level_width = 1ULL << vtree_node_level;
    // determine first vtree node id in vtree level
    std::uint64_t vtree_node_level_first_node_id = vtree_level_width;
    // determine node rank within level
    std::uint64_t vtree_node_rank_within_level = node_id - vtree_node_level_first_node_id;

    // ensure the node id is in the correct subtree
    std::uint64_t vtree_level_slice_width = vtree_level_width >> routing_depth_;
    // determine which slice of the level this node belongs to
    std::uint64_t slice_shift = vtree_node_level - routing_depth_;
    std::uint64_t vtree_level_slice_ix =
        slice_shift == 0 ? vtree_node_rank_within_level : (vtree_node_rank_within_level >> slice_shift);
    sn::util::log::ensure(
        vtree_level_slice_ix == subtree_ix, "node_id_to_subtree_node_id: node_id not in correct subtree"
    );

    // determine the node level within a subtree
    std::uint64_t subtree_node_level = vtree_node_level - routing_depth_;
    // determine the width of the level within the subtree
    std::uint64_t subtree_level_width = 1ULL << subtree_node_level;
    // determine the node rank within the subtree level
    std::uint64_t subtree_node_rank_within_level = vtree_node_rank_within_level & (subtree_level_width - 1ULL);

    // compute the subtree node id from level and rank coordinates
    std::uint64_t subtree_node_level_first_node_id = subtree_level_width;
    std::uint64_t subtree_node_id = subtree_node_level_first_node_id + subtree_node_rank_within_level;

    return subtree_node_id;
  }

  // convert a subtree-local node id to a virtual tree node id
  [[nodiscard]] std::uint64_t subtree_node_id_to_node_id(
      std::uint64_t subtree_node_id, std::uint64_t subtree_ix
  ) const {
    // if routing depth is 0, unchanged
    if (routing_depth_ == 0) {
      return subtree_node_id;
    }

    // determine the node level within the subtree
    std::uint64_t subtree_node_level = fast_log2(subtree_node_id);
    // determine the width of the level within the subtree
    std::uint64_t subtree_level_width = 1ULL << subtree_node_level;
    // determine the node rank within the subtree level
    std::uint64_t subtree_node_rank_within_level = subtree_node_id - subtree_level_width;

    // determine the node level within the virtual tree
    std::uint64_t vtree_node_level = subtree_node_level + routing_depth_;
    // determine the width of the level within the virtual tree
    std::uint64_t vtree_level_width = 1ULL << vtree_node_level;
    // determine the first node id in the vtree level
    std::uint64_t vtree_node_level_first_node_id = vtree_level_width;

    // determine the node rank within the vtree level
    std::uint64_t vtree_node_rank_within_level = (subtree_ix << subtree_node_level) + subtree_node_rank_within_level;

    // compute the node id from level and rank coordinates
    std::uint64_t node_id = vtree_node_level_first_node_id + vtree_node_rank_within_level;

    return node_id;
  }

private:
  static std::uint64_t fast_log2(std::uint64_t value) {
    const std::uint64_t leading = sn::obliv::ct_count_leading_zeros(value);
    return 63ULL - leading;
  }

  std::uint64_t tree_height_ = 0;
  std::uint32_t routing_depth_ = 0;
  std::uint64_t routing_subtree_count_ = 1;
  std::uint64_t routing_subtree_height_ = 0;
  std::uint64_t subtree_leaf_count_ = 1;
  std::uint64_t subtree_leaf_mask_ = 0;
  std::uint64_t largest_nonexistent_node_id_ = 0;
};

} // namespace sn::oram::tree
