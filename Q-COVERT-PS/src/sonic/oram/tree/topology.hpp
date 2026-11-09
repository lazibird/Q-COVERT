#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::tree {

using u64 = std::uint64_t;
using s64 = std::int64_t;

class topology {
public:
  topology() = default;

  topology(u64 height, u64 fanout = 2) { reset(height, fanout); }

  // initialize and precompute auxiliary data
  void reset(u64 height, u64 fanout = 2) {
    sn::util::log::ensure(fanout > 0, "tree::topology: fanout must be positive");
    height_ = height;
    fanout_ = fanout;

    fanout_powers_.assign(height_ + 1, 1);
    for (u64 level = 1; level <= height_; ++level) {
      fanout_powers_[level] = fanout_powers_[level - 1] * fanout_;
    }

    leaf_count_cached_ = fanout_powers_[height_];

    level_offsets_.assign(height_ + 1, 1);
    for (u64 level = 1; level <= height_; ++level) {
      level_offsets_[level] = level_offsets_[level - 1] + fanout_powers_[level - 1];
    }

    total_nodes_ = level_offsets_[height_] + leaf_count_cached_ - 1;
    first_leaf_node_id_ = level_offsets_[height_];
  }

  [[nodiscard]] u64 height() const noexcept { return height_; }
  [[nodiscard]] u64 fanout() const noexcept { return fanout_; }
  [[nodiscard]] u64 leaf_count() const noexcept { return leaf_count_cached_; }
  [[nodiscard]] u64 node_count() const noexcept { return total_nodes_; }
  [[nodiscard]] u64 first_leaf_node_id() const noexcept { return first_leaf_node_id_; }

  // get tree node id from leaf index
  [[nodiscard]] u64 leaf_index_to_node_id(s64 leaf_ix) const {
    sn::util::log::ensure(
        leaf_ix >= 0 && leaf_ix < static_cast<s64>(leaf_count_cached_), "tree::topology: leaf_index out of range"
    );
    return first_leaf_node_id_ + leaf_ix;
  }

  [[nodiscard]] u64 leaf_node_id_to_index(u64 leaf_node_id) const {
    sn::util::log::ensure(
        leaf_node_id >= first_leaf_node_id_ && leaf_node_id < first_leaf_node_id_ + leaf_count_cached_,
        "tree::topology: leaf_node_id out of range"
    );
    return leaf_node_id - first_leaf_node_id_;
  }

  // get the depth in the tree of a given node id
  [[nodiscard]] u64 node_depth(u64 node_id) const {
    sn::util::log::ensure(node_id != 0, "tree::topology: node_id must be positive");
    sn::util::log::ensure(node_id <= total_nodes_, "tree::topology: node_id exceeds node count");

    if (fanout_ == 2) {
      const int leading = sn::obliv::ct_count_leading_zeros(node_id);
      return static_cast<u64>(63 - leading);
    }

    const auto it = std::upper_bound(level_offsets_.begin(), level_offsets_.end(), node_id);
    return static_cast<u64>(std::distance(level_offsets_.begin(), it) - 1);
  }

  // constant-time depth in the tree of a given node id
  // depth is the position of the highest set bet in the node id
  [[nodiscard]] u64 ct_node_depth(u64 node_id) const {
    sn::util::log::ensure(fanout_ == 2, "tree::topology::ct_node_depth requires binary tree");
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(node_id != 0, "tree::topology::ct_node_depth: node_id must be positive");
    sn::util::log::ensure(node_id <= total_nodes_, "tree::topology::ct_node_depth: node_id exceeds node count");
#endif
    const std::uint64_t leading = sn::obliv::ct_count_leading_zeros(node_id);
    return static_cast<u64>(63U - leading);
  }

  // determine whether the eviction node lies on the mapped leaf's path
  // thus, whether a block mapped to a certain leaf can reside at a particular node
  [[nodiscard]] bool ct_is_evictable_to_node(u64 mapped_leaf_node_id, u64 evict_node_id) const {
    sn::util::log::ensure(fanout_ == 2, "tree::topology::ct_is_evictable_to_node requires binary tree");
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(
        mapped_leaf_node_id >= first_leaf_node_id_ && mapped_leaf_node_id < first_leaf_node_id_ + leaf_count_cached_,
        "tree::topology::ct_is_evictable_to_node: mapped leaf out of range"
    );
    sn::util::log::ensure(
        evict_node_id != 0 && evict_node_id <= total_nodes_,
        "tree::topology::ct_is_evictable_to_node: evict node out of range"
    );
#endif

    // mapped_leaf_node_id is a leaf node, and is guaranteed to have bits set for every level
    // evict_node_id can be any node, so it may have less bits
    // a block is evictable to a node if the node is along the path to the leaf node

    // get the level of the evict node (call it i)
    const u64 evict_level = ct_node_depth(evict_node_id);
    const u64 height_diff = height_ - evict_level;

    // shift the evict node bits to be aligned with the mapped leaf node bits
    // a node id at level i has i+1 bits (because the first bit for level 0 is always 1)
    const u64 aligned_evict_node = evict_node_id << height_diff;

    // create a mask to check only the bits that are set in the mapped leaf node
    // we do this by masking exactly the top i+1 bits of the node ids
    // shift the mask to be aligned with the upper bits
    const u64 prefix_mask = ((static_cast<u64>(1) << (evict_level + 1U)) - 1U) << height_diff;
    const u64 mapped_prefix = mapped_leaf_node_id & prefix_mask;
    // if the aligned evict node matches the mapped prefix, then the evict node is on the path
    const u64 diff = mapped_prefix ^ aligned_evict_node;
    return sn::obliv::ct_eq(diff, static_cast<u64>(0));
  }

  // constant-time deepest eviction depth between two leaves
  // the block is mapped to a leaf, check how deep it can reside along the path to another leaf
  [[nodiscard]] u64 ct_evictable_depth(u64 mapped_leaf_node_id, u64 eviction_leaf_node_id) const {
    sn::util::log::ensure(fanout_ == 2, "tree::topology::ct_evictable_depth supports only binary trees");
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(
        mapped_leaf_node_id >= first_leaf_node_id_ && mapped_leaf_node_id < first_leaf_node_id_ + leaf_count_cached_,
        "tree::topology::ct_evictable_depth: mapped leaf out of range"
    );
    sn::util::log::ensure(
        eviction_leaf_node_id >= first_leaf_node_id_ &&
            eviction_leaf_node_id < first_leaf_node_id_ + leaf_count_cached_,
        "tree::topology::ct_evictable_depth: eviction leaf out of range"
    );
#endif
    // convert node ids to zero-based leaf indices
    const u64 mapped_leaf_index = mapped_leaf_node_id - first_leaf_node_id_;
    const u64 eviction_leaf_index = eviction_leaf_node_id - first_leaf_node_id_;
    const u64 xor_value = mapped_leaf_index ^ eviction_leaf_index;

    // locate the highest differing bit (if any) by counting leading zeros
    const u64 leading = sn::obliv::ct_count_leading_zeros(xor_value);
    // get the position of the MSB from the right
    const u64 msb_pos_from_right = 64U - leading;

    // get the evict level from the msb pos
    // evict level is H - msb_pos_from_right
    const u64 evict_level = height_ - msb_pos_from_right;
    const bool identical = sn::obliv::ct_eq(xor_value, static_cast<u64>(0));
    return sn::obliv::ct_select(height_, evict_level, identical);
  }

  void path_to_leaf(u64 leaf_ix, sn::util::span<u64> node_ids) const {
    sn::util::log::ensure(leaf_ix < leaf_count_cached_, "tree::topology::path_to_leaf: leaf index out of range");
    sn::util::log::ensure(node_ids.size() == height_ + 1, "tree::topology::path_to_leaf: node span size mismatch");

    node_ids[0] = 1;
    if (height_ == 0) {
      return;
    }

    u64 index_in_level = 0;
    if (fanout_ == 2) {
      for (u64 level = 0; level < height_; ++level) {
        const u64 shift = height_ - level - 1;
        const std::uint8_t digit = static_cast<std::uint8_t>((leaf_ix >> shift) & 1U);
        index_in_level = (index_in_level << 1U) | digit;
        node_ids[level + 1] = level_offsets_[level + 1] + index_in_level;
      }
    } else {
      u64 remainder = leaf_ix;
      for (u64 level = 0; level < height_; ++level) {
        const u64 divisor = fanout_powers_[height_ - level - 1];
        const std::uint8_t digit = static_cast<std::uint8_t>(divisor == 0 ? 0 : remainder / divisor);
        remainder -= static_cast<u64>(digit) * divisor;
        index_in_level = index_in_level * fanout_ + digit;
        node_ids[level + 1] = level_offsets_[level + 1] + index_in_level;
      }
    }
  }

  void paths_to_leaves(sn::util::span<const u64> leaf_ixs, sn::util::span<u64> out_node_ids) const {
    const std::size_t path_count = leaf_ixs.size();
    const std::size_t nodes_per_path = static_cast<std::size_t>(height_ + 1);

    sn::util::log::ensure(
        out_node_ids.size() == path_count * nodes_per_path, "tree::topology::paths_to_leaves: node span size mismatch"
    );

    for (std::size_t idx = 0; idx < path_count; ++idx) {
      const u64 leaf_ix = leaf_ixs[idx];
      auto* node_dest_ptr = out_node_ids.data() + idx * nodes_per_path;
      sn::util::span<u64> node_dest(node_dest_ptr, nodes_per_path);
      path_to_leaf(leaf_ix, node_dest);
    }
  }

  [[nodiscard]] u64 reverse_lex_leaf(u64 order_index) const {
    sn::util::log::ensure(fanout_ == 2, "tree::topology::reverse_lex_leaf supports only binary trees");
    sn::util::log::ensure(
        order_index < leaf_count_cached_, "tree::topology::reverse_lex_leaf: order_index out of range"
    );
    u64 value = order_index;
    u64 reversed = 0;
    for (u64 i = 0; i < height_; ++i) {
      reversed = (reversed << 1U) | (value & 1U);
      value >>= 1U;
    }
    return reversed;
  }

private:
  u64 height_ = 0;
  u64 fanout_ = 2;
  u64 leaf_count_cached_ = 1;
  u64 total_nodes_ = 1;
  u64 first_leaf_node_id_ = 1;
  std::vector<u64> fanout_powers_ = {1};
  std::vector<u64> level_offsets_ = {1};
};

} // namespace sn::oram::tree
