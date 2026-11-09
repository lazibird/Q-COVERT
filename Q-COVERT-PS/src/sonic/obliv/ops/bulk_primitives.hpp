#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/struct_ops.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::obliv {

// BULKOR's bucket adjustment. Input records must be sorted by leaf_node_id.
// first_bucket_depth excludes the nonexistent global top levels of a forest;
// occupancy resets when sorted leaves enter the next subtree. Only public
// loop bounds and sequential occupancy accesses are used.
template <typename Item>
std::size_t oadjust_bucket_id(
    sn::util::span<Item> items, std::uint32_t height, std::uint32_t capacity, std::uint64_t stash_bucket_id,
    std::uint32_t first_bucket_depth = 0
) {
  static_assert(std::is_trivially_copyable_v<Item>, "oadjust_bucket_id requires trivially copyable items");
  sn::util::log::ensure(capacity > 0, "oadjust_bucket_id: capacity must be positive");
  sn::util::log::ensure(height < 63, "oadjust_bucket_id: height is too large");
  sn::util::log::ensure(first_bucket_depth <= height, "oadjust_bucket_id: first depth exceeds height");

  // L is public and bounded by the address width; a fixed local array avoids
  // allocation or secret-indexed memory access inside the scan.
  std::uint64_t occupancy[64]{};
  std::uint64_t previous_leaf = 0;
  std::size_t stash_count = 0;
  for (Item& item : items) {
    const std::uint64_t leaf = item.leaf_node_id;
    const bool active = [&]() {
      if constexpr (requires { item.active; }) {
        return static_cast<bool>(item.active);
      } else {
        return true;
      }
    }();
    // Reset exactly those path levels that changed since the prior leaf.
    for (std::uint32_t depth = 0; depth <= height; ++depth) {
      const std::uint32_t shift = height - depth;
      const bool changed = !ct_eq<std::uint64_t>(leaf >> shift, previous_leaf >> shift);
      occupancy[depth] = ct_select<std::uint64_t>(0, occupancy[depth], changed);
    }

    bool assigned = false;
    std::uint64_t destination = stash_bucket_id;
    for (std::int64_t depth = static_cast<std::int64_t>(height);
         depth >= static_cast<std::int64_t>(first_bucket_depth); --depth) {
      const std::size_t level = static_cast<std::size_t>(depth);
      const bool can_place = active & (!assigned) & ct_lt<std::uint64_t>(occupancy[level], capacity);
      const std::uint64_t candidate = leaf >> (height - level);
      destination = ct_select<std::uint64_t>(candidate, destination, can_place);
      occupancy[level] = ct_select<std::uint64_t>(occupancy[level] + 1, occupancy[level], can_place);
      assigned |= can_place;
    }
    item.bucket_id = destination;
    stash_count += static_cast<std::size_t>(active & !assigned);
    previous_leaf = leaf;
  }
  return stash_count;
}

// BULKOR's oblivious placement network. The array size must be a power of two.
// Valid keys are unique target indices in [0, items.size()); invalid_key
// identifies dummies. Items are initially sorted by key, with dummies last.
template <typename Item, typename KeyExtractor>
void oplace(sn::util::span<Item> items, KeyExtractor key, std::uint64_t invalid_key) {
  static_assert(std::is_trivially_copyable_v<Item>, "oplace requires trivially copyable items");
  const std::size_t n = items.size();
  sn::util::log::ensure(n == 0 || (n & (n - 1)) == 0, "oplace: size must be a power of two");
  sn::util::log::ensure(invalid_key >= n, "oplace: invalid key overlaps target positions");
  if (n <= 1) {
    return;
  }

  for (std::size_t stride = n >> 1U; stride > 0; stride >>= 1U) {
    const std::size_t group_size = stride << 1U;
    for (std::size_t base = 0; base < n; base += group_size) {
      const std::uint64_t split = static_cast<std::uint64_t>(base + stride);
      for (std::size_t offset = 0; offset < stride; ++offset) {
        Item& left = items[base + offset];
        Item& right = items[base + stride + offset];
        const std::uint64_t left_key = static_cast<std::uint64_t>(key(left));
        const std::uint64_t right_key = static_cast<std::uint64_t>(key(right));
        const bool left_valid = !ct_eq<std::uint64_t>(left_key, invalid_key);
        const bool right_valid = !ct_eq<std::uint64_t>(right_key, invalid_key);
        const bool left_misplaced = left_valid & ct_ge<std::uint64_t>(left_key, split);
        const bool right_misplaced = right_valid & ct_lt<std::uint64_t>(right_key, split);
        ct_swap_data(&left, &right, left_misplaced | right_misplaced);
      }
    }
  }
}

} // namespace sn::obliv
