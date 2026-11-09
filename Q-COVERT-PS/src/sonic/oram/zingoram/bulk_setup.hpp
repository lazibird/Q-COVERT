#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

#include "sonic/obliv/ops/bulk_primitives.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/oram/zingoram/state.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::zingoram {

template <std::size_t BlockBytes> struct bulk_record {
  std::int64_t address = -1;
  std::int64_t leaf = -1;
  std::array<std::uint8_t, BlockBytes> data{};
};

namespace detail {

struct alignas(8) bulk_metadata {
  std::uint64_t source_index = 0;
  std::uint64_t leaf_node_id = 0;
  std::uint64_t bucket_id = 0;
  std::uint64_t subtree_id = 0;
  std::uint64_t target_position = std::numeric_limits<std::uint64_t>::max();
};

static_assert(std::is_trivially_copyable_v<bulk_metadata>);

struct ct_u64_less {
  bool operator()(std::uint64_t lhs, std::uint64_t rhs) const noexcept {
    return sn::obliv::ct_lt<std::uint64_t>(lhs, rhs);
  }
};

template <typename Traits>
void bulk_setup(state<Traits>& st, std::span<const bulk_record<Traits::block_bytes>> records) {
  sn_prof_zone("zingoram.bulk_setup");
  using block_t = typename Traits::block_t;
  const auto& geom = st.shape();
  const auto& opts = st.options();
  const std::size_t count = records.size();
  const std::uint64_t bucket_real = opts.bucket_real_size;
  const std::uint64_t stash_capacity = st.derived().stash_bound;
  const std::uint64_t treetop_capacity = static_cast<std::uint64_t>(geom.routing_depth) * bucket_real;
  const std::uint64_t per_subtree_stash_capacity = treetop_capacity + stash_capacity;
  const std::uint64_t invalid_position = std::numeric_limits<std::uint64_t>::max();

  sn::util::log::ensure(count <= opts.block_count, "bulk_setup: too many records");
  sn::util::log::ensure(per_subtree_stash_capacity >= stash_capacity, "bulk_setup: stash capacity overflow");
  sn::util::log::ensure(
      per_subtree_stash_capacity <= invalid_position / geom.subtree_count,
      "bulk_setup: total subtree stash capacity overflow"
  );
  const std::uint64_t total_stash_slots = per_subtree_stash_capacity * geom.subtree_count;
  sn::util::log::ensure(geom.node_count <= (invalid_position - total_stash_slots) / bucket_real,
                        "bulk_setup: tree slot count overflow");
  const std::uint64_t tree_slots = geom.node_count * bucket_real;
  const std::uint64_t output_slots = tree_slots + total_stash_slots;
  sn::util::log::ensure(output_slots <= std::numeric_limits<std::size_t>::max() / 2,
                        "bulk_setup: output too large");
  sn::util::log::ensure(count <= output_slots, "bulk_setup: input exceeds tree and stash capacity");

  // Validate the contract without a secret-dependent early exit. The final
  // error bit is only observable for malformed input. Address uniqueness is
  // checked with a separate oblivious sort of small address words.
  std::uint64_t invalid = 0;
  std::vector<std::uint64_t> addresses(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& rec = records[i];
    invalid |= static_cast<std::uint64_t>(rec.address < 0 ||
                                          static_cast<std::uint64_t>(rec.address) >= opts.block_count);
    invalid |= static_cast<std::uint64_t>(rec.leaf < 0 ||
                                          static_cast<std::uint64_t>(rec.leaf) >= geom.leaf_count);
    addresses[i] = static_cast<std::uint64_t>(rec.address);
  }
  sn::sortshuffle::ser::bitonic::bitonic_sort(addresses.data(), addresses.size(),
                                              sn::sortshuffle::detail::default_key<std::uint64_t>{}, ct_u64_less{});
  for (std::size_t i = 1; i < addresses.size(); ++i) {
    invalid |= static_cast<std::uint64_t>(sn::obliv::ct_eq(addresses[i], addresses[i - 1]));
  }
  sn::util::log::ensure(invalid == 0, "bulk_setup: address/leaf invalid or duplicate address");

  // Both the original empty initializer and the bulk loader share bucket
  // allocation and metadata initialization. All later writes remain private
  // to this fresh client until the method returns successfully.
  st.initialize();
  if (count == 0) {
    return;
  }

  const std::size_t padded = std::bit_ceil(static_cast<std::size_t>(output_slots));
  sn::util::log::ensure(padded >= count && padded >= output_slots, "bulk_setup: padded size overflow");
  std::vector<bulk_metadata> metadata(padded);
  std::vector<block_t> blocks(padded);
  auto& uid = st.uid_gen();
  for (std::size_t i = 0; i < padded; ++i) {
    blocks[i].set_dummy(uid);
    metadata[i].source_index = i;
    metadata[i].target_position = invalid_position;
  }
  const std::uint64_t first_leaf_node = geom.leaf_count;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& rec = records[i];
    block_t& block = blocks[i];
    block.address = rec.address;
    block.leaf_ix = rec.leaf;
    block.data = rec.data;
    metadata[i].leaf_node_id = first_leaf_node + static_cast<std::uint64_t>(rec.leaf);
    metadata[i].bucket_id = metadata[i].leaf_node_id;
    metadata[i].subtree_id = static_cast<std::uint64_t>(rec.leaf) >> geom.subtree_height;
  }

  // Sort only the real metadata by their initial leaf bucket and run the
  // BULKOR bottom-up greedy adjustment. Real data remain in source order.
  auto leaf_key = [](const bulk_metadata& x) { return x.leaf_node_id; };
  sn::sortshuffle::ser::bitonic::bitonic_sort(metadata.data(), count, leaf_key, ct_u64_less{});
  const std::uint64_t stash_bucket_id = geom.node_count + 1ULL;
  const std::size_t stash_used = sn::obliv::oadjust_bucket_id(
      sn::util::span<bulk_metadata>(metadata.data(), count), static_cast<std::uint32_t>(geom.height),
      opts.bucket_real_size, stash_bucket_id, geom.routing_depth
  );
  sn::util::log::ensure(stash_used <= total_stash_slots, "bulk_setup: total stash capacity exceeded");

  // The forest has a separate stash per subtree. Route overflow by the
  // pre-sampled leaf, without indexing a subtree through a secret value.
  for (std::size_t i = 0; i < count; ++i) {
    auto& item = metadata[i];
    const bool is_stash = sn::obliv::ct_eq(item.bucket_id, stash_bucket_id);
    item.bucket_id = sn::obliv::ct_select<std::uint64_t>(
        stash_bucket_id + item.subtree_id, item.bucket_id, is_stash
    );
  }

  auto bucket_key = [](const bulk_metadata& x) { return x.bucket_id; };
  sn::sortshuffle::ser::bitonic::bitonic_sort(metadata.data(), count, bucket_key, ct_u64_less{});

  // Sorted bucket IDs receive consecutive slot numbers. A stash bucket uses
  // the range after all tree slots. All scans and writes are sequential.
  std::uint64_t previous_bucket = 0;
  std::uint64_t within_bucket = 0;
  std::uint64_t capacity_error = 0;
  for (std::size_t i = 0; i < count; ++i) {
    bulk_metadata& item = metadata[i];
    const bool new_bucket = !sn::obliv::ct_eq(item.bucket_id, previous_bucket);
    within_bucket = sn::obliv::ct_select<std::uint64_t>(0, within_bucket, new_bucket);
    const bool is_stash = sn::obliv::ct_ge(item.bucket_id, stash_bucket_id);
    const std::uint64_t tree_bucket = sn::obliv::ct_select<std::uint64_t>(1, item.bucket_id, is_stash);
    const std::uint64_t tree_position = (tree_bucket - 1ULL) * bucket_real + within_bucket;
    const std::uint64_t stash_position = tree_slots + item.subtree_id * per_subtree_stash_capacity + within_bucket;
    item.target_position = sn::obliv::ct_select<std::uint64_t>(stash_position, tree_position, is_stash);
    const std::uint64_t limit =
        sn::obliv::ct_select<std::uint64_t>(per_subtree_stash_capacity, bucket_real, is_stash);
    capacity_error |= static_cast<std::uint64_t>(sn::obliv::ct_ge<std::uint64_t>(within_bucket, limit));
    ++within_bucket;
    previous_bucket = item.bucket_id;
  }
  sn::util::log::ensure(capacity_error == 0, "bulk_setup: assigned bucket capacity exceeded");

  // Append dummy metadata with invalid keys and obliviously fill every
  // physical output position. Padding beyond output_slots is also dummy.
  auto target_key = [](const bulk_metadata& x) { return x.target_position; };
  sn::obliv::oplace(sn::util::span<bulk_metadata>(metadata.data(), metadata.size()), target_key, invalid_position);
  for (std::size_t i = 0; i < padded; ++i) {
    metadata[i].target_position = i;
  }

  // Restore source order to attach the computed physical targets to the
  // original payloads, then obliviously permute the payloads by target.
  auto source_key = [](const bulk_metadata& x) { return x.source_index; };
  sn::sortshuffle::ser::bitonic::bitonic_sort(metadata.data(), metadata.size(), source_key, ct_u64_less{});
  for (std::size_t i = 0; i < padded; ++i) {
    blocks[i].extra = metadata[i].target_position;
  }
  auto block_target_key = [](const block_t& x) { return x.extra; };
  sn::sortshuffle::ser::bitonic::bitonic_sort(blocks.data(), blocks.size(), block_target_key, ct_u64_less{});

  // Tree levels above routing_depth are nonexistent. Each subtree instead
  // receives a fixed treetop spill region, and the top overlap_depth levels
  // of its own physical tree live in its overlap section. All transfers use
  // public subtree/node/slot indices; the sort above already routed payloads.
  const std::uint64_t setup_overlap_depth =
      opts.evict_backend == eviction_backend::shared_top ? 0ULL : geom.overlap_depth;
  const std::uint64_t overlap_nodes_per_subtree = (1ULL << setup_overlap_depth) - 1ULL;
  const std::uint64_t overlap_slots_per_subtree = overlap_nodes_per_subtree * bucket_real;
  for (std::uint64_t subtree_ix = 0; subtree_ix < geom.subtree_count; ++subtree_ix) {
    auto& subtree = st.stash().state.subtrees[static_cast<std::size_t>(subtree_ix)];
    const std::size_t stash_start =
        static_cast<std::size_t>(tree_slots + subtree_ix * per_subtree_stash_capacity);

    auto treetop = subtree.treetop.span(subtree.storage);
    sn::util::log::ensure(treetop.size() == treetop_capacity, "bulk_setup: treetop capacity mismatch");
    sn::obliv::copy(blocks.begin() + stash_start, blocks.begin() + stash_start + treetop.size(), treetop.begin());

    auto deferred = subtree.local_deferred.span(subtree.storage);
    sn::util::log::ensure(deferred.size() == stash_capacity, "bulk_setup: deferred capacity mismatch");
    sn::obliv::copy(blocks.begin() + stash_start + treetop.size(),
                    blocks.begin() + stash_start + treetop.size() + deferred.size(), deferred.begin());

    auto overlap = subtree.overlap_region.span(subtree.storage);
    sn::util::log::ensure(overlap_slots_per_subtree <= overlap.size(), "bulk_setup: overlap stash too small");
    for (std::uint64_t local_node = 1; local_node <= overlap_nodes_per_subtree; ++local_node) {
      const std::uint64_t global_node = st.forest_topology().subtree_node_id_to_node_id(local_node, subtree_ix);
      const std::size_t source = static_cast<std::size_t>((global_node - 1ULL) * bucket_real);
      const std::size_t destination = static_cast<std::size_t>((local_node - 1ULL) * bucket_real);
      sn::obliv::copy(blocks.begin() + source, blocks.begin() + source + static_cast<std::size_t>(bucket_real),
                      overlap.begin() + destination);
    }
  }

  // The remaining physical buckets are rebuilt in public heap order. This
  // reuses RingDORAM's S-dummy insertion, random permutation and metadata.
  const std::uint64_t virtual_top_nodes = (1ULL << (geom.routing_depth + setup_overlap_depth)) - 1ULL;
  for (std::uint64_t node = virtual_top_nodes + 1ULL; node <= geom.node_count; ++node) {
    const std::size_t offset = static_cast<std::size_t>((node - 1ULL) * bucket_real);
    sn::util::span<const block_t> contents(blocks.data() + offset, static_cast<std::size_t>(bucket_real));
    st.storage()[node].rebuild(contents, uid, st.prng());
  }

#if defined(ORAM_DEBUG)
  for (std::size_t i = 0; i < count; ++i) {
    // The final source-order metadata identifies the payload at its assigned
    // output position, so record the actual post-sort UID for later checks.
    const std::size_t target = static_cast<std::size_t>(metadata[i].target_position);
    st.assigned_blocks().record(static_cast<std::uint64_t>(records[i].address), blocks[target].uid);
  }
#endif
}

} // namespace detail

} // namespace sn::oram::zingoram
