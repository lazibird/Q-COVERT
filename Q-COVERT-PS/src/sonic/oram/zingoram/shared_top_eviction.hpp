#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "sonic/obliv/ops/bulk_primitives.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/oram/stash/forestzing/pipeline.hpp"
#include "sonic/oram/zingoram/state.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"
#include "sonic/sortshuffle/ser/orshuffle.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::zingoram::detail {

struct shared_top_u64_less {
  bool operator()(std::uint64_t lhs, std::uint64_t rhs) const noexcept {
    return sn::obliv::ct_lt<std::uint64_t>(lhs, rhs);
  }
};

inline std::uint64_t reverse_low_bits(std::uint64_t input, std::uint32_t bits) noexcept {
  std::uint64_t result = 0;
  for (std::uint32_t bit = 0; bit < bits; ++bit) {
    result = (result << 1U) | ((input >> bit) & 1ULL);
  }
  return result;
}

template <typename Block> struct alignas(8) suffix_item {
  Block block{};
  std::uint64_t chain = 0;
  std::uint64_t depth = 0;
  std::uint64_t key = 0;
  std::uint8_t kind = 2; // 0=real, 1=filler, 2=discardable dummy
};

template <typename Block> struct alignas(8) top_item {
  Block block{};
  std::uint64_t leaf_node_id = 0;
  std::uint64_t bucket_id = 0;
  std::uint64_t target_position = std::numeric_limits<std::uint64_t>::max();
  std::uint8_t active = 0;
};

template <typename Traits>
void evict_shared_top_subtree(
    state<Traits>& st, std::uint32_t subtree_ix, sn::util::span<const std::uint64_t> leaves,
    sn::util::span<const typename Traits::block_t> snapshot, sn::crypto::buffered_prng<>& prng
) {
  using block_t = typename Traits::block_t;
  using main_item = suffix_item<block_t>;
  using small_item = top_item<block_t>;

  const auto& geom = st.shape();
  const auto& opts = st.options();
  auto& stash_state = st.stash().state;
  auto& subtree = stash_state.subtrees[subtree_ix];
  auto& uid = st.uid_gen();
  auto& storage = st.storage();
  auto& epochs = st.epoch_states();
  const std::uint64_t E = opts.evict_batch;
  const std::uint64_t L = geom.subtree_height;
  const std::uint64_t i = geom.overlap_depth;
  const std::uint64_t Z = opts.bucket_real_size;
  const std::uint64_t Cs = st.derived().stash_bound;
  auto treetop = subtree.treetop.span(subtree.storage);
  const std::uint64_t resident_slots = Cs + treetop.size();
  sn::util::log::ensure(E > 1 && i > 0, "shared-top evict: invalid geometry");
  sn::util::log::ensure(leaves.size() == E && i <= L, "shared-top evict: invalid leaf batch");
  const std::uint64_t suffix_levels = L - i + 1ULL;
  const std::uint64_t suffix_slots = E * suffix_levels * Z;
  const std::uint64_t top_slots = (E - 1ULL) * Z;
  const std::uint64_t top_size = top_slots + resident_slots;
  auto deferred = subtree.local_deferred.span(subtree.storage);
  sn::util::log::ensure(deferred.size() == Cs, "shared-top evict: deferred capacity mismatch");

#if defined(SONIC_ORAM_METRICS)
  sn::oram::stash::forestzing::pipeline::observe_subtree_stash_sections(stash_state, subtree);
#endif
  const std::size_t input_slots = snapshot.size() + deferred.size() + treetop.size() +
                                  static_cast<std::size_t>(top_slots + suffix_slots);
  const std::size_t main_size = input_slots + static_cast<std::size_t>(suffix_slots);
  std::vector<main_item> main(main_size);
  std::vector<std::uint8_t> marks(main_size, 0);
  std::vector<std::size_t> prefix(main_size + 1, 0);
  std::size_t used = 0;
  const std::uint64_t suffix_bits = L - i;
  const std::uint64_t suffix_mask = (1ULL << suffix_bits) - 1ULL;
  const std::uint64_t beta = leaves[0] & suffix_mask;

  auto append_input = [&](const block_t& block) {
    auto& item = main[used++];
    item.block = block;
    const bool real = block.is_real().unwrap();
    const std::uint64_t leaf = sn::obliv::ct_select<std::uint64_t>(
        static_cast<std::uint64_t>(block.leaf_ix) & (geom.subtree_leaf_count - 1ULL), 0ULL, real
    );
    const std::uint64_t leaf_prefix = leaf >> suffix_bits;
    const std::uint64_t chain = reverse_low_bits(leaf_prefix, static_cast<std::uint32_t>(i));
    const std::uint64_t difference = (leaf & suffix_mask) ^ beta;
    const std::uint64_t lcp = suffix_bits == 0 ? 0ULL :
        std::min<std::uint64_t>(suffix_bits, std::countl_zero(difference << (64U - suffix_bits)));
    item.chain = sn::obliv::ct_select<std::uint64_t>(chain, E, real);
    item.depth = sn::obliv::ct_select<std::uint64_t>(i + lcp, i, real);
    item.kind = sn::obliv::ct_select<std::uint8_t>(0, 2, real);
    item.key = ((item.chain * suffix_levels) + (L - item.depth)) * 3ULL + item.kind;
  };

  for (const auto& block : snapshot) {
    append_input(block);
  }
  for (const auto& block : deferred) {
    append_input(block);
  }
  for (const auto& block : treetop) {
    append_input(block);
  }

  std::vector<block_t> bucket_blocks(static_cast<std::size_t>(Z));
  std::vector<std::int64_t> bucket_offsets(static_cast<std::size_t>(Z));
  auto read_bucket = [&](std::uint64_t node) {
    storage[node].read_bucket_max(bucket_blocks.data(), bucket_offsets.data());
    for (const auto& block : bucket_blocks) {
      append_input(block);
    }
  };
  // Read each shared top bucket exactly once, followed by disjoint suffixes.
  for (std::uint64_t node = 1; node < E; ++node) {
    read_bucket(st.forest_topology().subtree_node_id_to_node_id(node, subtree_ix));
  }
  std::vector<std::uint64_t> path(static_cast<std::size_t>(geom.height + 1ULL));
  auto path_to_leaf = [&](std::uint64_t leaf) {
    const std::uint64_t global_leaf = (static_cast<std::uint64_t>(subtree_ix) << L) + leaf;
    st.topology().path_to_leaf(global_leaf, sn::util::span<std::uint64_t>(path.data(), path.size()));
  };
  for (std::uint64_t chain = 0; chain < E; ++chain) {
    path_to_leaf(leaves[static_cast<std::size_t>(chain)]);
    for (std::uint64_t depth = i; depth <= L; ++depth) {
      read_bucket(path[static_cast<std::size_t>(geom.routing_depth + depth)]);
    }
  }
  sn::util::log::ensure(used == input_slots, "shared-top evict: input size mismatch");

  for (std::uint64_t chain = 0; chain < E; ++chain) {
    for (std::uint64_t depth = L + 1ULL; depth-- > i;) {
      for (std::uint64_t slot = 0; slot < Z; ++slot) {
        auto& item = main[used++];
        item.block.set_dummy(uid);
        item.chain = chain;
        item.depth = depth;
        item.kind = 1;
        item.key = ((chain * suffix_levels) + (L - depth)) * 3ULL + 1ULL;
      }
    }
  }
  sn::util::log::ensure(used == main_size, "shared-top evict: filler size mismatch");
  auto main_key = [](const main_item& item) { return item.key; };
  {
#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
    sn::sortshuffle::ser::bitonic::counting::category_scope comparator_scope(
        sn::sortshuffle::ser::bitonic::counting::category::eviction_main
    );
#endif
    sn::sortshuffle::ser::bitonic::bitonic_sort(main.data(), main.size(), main_key, shared_top_u64_less{});
  }

  std::uint64_t current_chain = 0;
  std::int64_t waterline = static_cast<std::int64_t>(L);
  std::uint64_t fill = 0;
  std::uint64_t kept_suffix = 0;
  std::uint64_t overflow_count = 0;
  for (std::size_t index = 0; index < main.size(); ++index) {
    const auto& item = main[index];
    const bool changed = !sn::obliv::ct_eq(item.chain, current_chain);
    current_chain = sn::obliv::ct_select<std::uint64_t>(item.chain, current_chain, changed);
    waterline = sn::obliv::ct_select<std::int64_t>(static_cast<std::int64_t>(L), waterline, changed);
    fill = sn::obliv::ct_select<std::uint64_t>(0, fill, changed);
    const bool available = sn::obliv::ct_ge(waterline, static_cast<std::int64_t>(i));
    const bool real = sn::obliv::ct_eq<std::uint8_t>(item.kind, 0);
    const bool filler = sn::obliv::ct_eq<std::uint8_t>(item.kind, 1);
    const bool keep_real = real & available;
    const bool keep_filler = filler & available & sn::obliv::ct_lt(fill, Z) &
                             !sn::obliv::ct_gt(item.depth, static_cast<std::uint64_t>(waterline));
    const bool keep = keep_real | keep_filler;
    marks[index] = static_cast<std::uint8_t>(keep);
    kept_suffix += static_cast<std::uint64_t>(keep);
    overflow_count += static_cast<std::uint64_t>(real & !keep_real);
    const std::uint64_t next_fill = fill + static_cast<std::uint64_t>(keep);
    const bool full = keep & sn::obliv::ct_eq(next_fill, Z);
    fill = sn::obliv::ct_select<std::uint64_t>(0, next_fill, full);
    waterline = sn::obliv::ct_select<std::int64_t>(waterline - 1, waterline, full);
  }
  sn::util::log::ensure(kept_suffix == suffix_slots, "shared-top evict: suffix population mismatch");
  sn::util::log::ensure(overflow_count <= top_size, "shared-top evict: top/stash capacity exceeded");
  sn::sortshuffle::ser::orshuffle::orcompact(main.data(), main.size(), marks.data(), prefix.data());

  const std::size_t tail_size = main.size() - static_cast<std::size_t>(suffix_slots);
  for (std::size_t index = 0; index < tail_size; ++index) {
    marks[index] = static_cast<std::uint8_t>(main[static_cast<std::size_t>(suffix_slots) + index].kind == 0);
  }
  sn::sortshuffle::ser::orshuffle::orcompact(
      main.data() + static_cast<std::size_t>(suffix_slots), tail_size, marks.data(), prefix.data()
  );

  const std::size_t padded_top = std::bit_ceil(static_cast<std::size_t>(top_size));
  std::vector<small_item> top(padded_top);
  const std::uint64_t invalid = std::numeric_limits<std::uint64_t>::max();
  for (std::size_t index = 0; index < padded_top; ++index) {
    auto& item = top[index];
    item.block.set_dummy(uid);
    item.leaf_node_id = invalid;
    item.bucket_id = invalid;
    item.target_position = invalid;
    item.active = 0;
  }
  for (std::size_t index = 0; index < static_cast<std::size_t>(top_size); ++index) {
    auto& item = top[index];
    item.block = main[static_cast<std::size_t>(suffix_slots) + index].block;
    const bool real = sn::obliv::ct_eq<std::uint8_t>(
        main[static_cast<std::size_t>(suffix_slots) + index].kind, 0
    );
    const std::uint64_t leaf = sn::obliv::ct_select<std::uint64_t>(
        static_cast<std::uint64_t>(item.block.leaf_ix) & (geom.subtree_leaf_count - 1ULL), 0ULL, real
    );
    const std::uint64_t top_leaf = (1ULL << (i - 1ULL)) + (leaf >> (L - i + 1ULL));
    item.leaf_node_id = sn::obliv::ct_select<std::uint64_t>(top_leaf, invalid, real);
    item.bucket_id = item.leaf_node_id;
    item.active = static_cast<std::uint8_t>(real);
  }
  auto top_leaf_key = [](const small_item& item) { return item.leaf_node_id; };
  {
#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
    sn::sortshuffle::ser::bitonic::counting::category_scope comparator_scope(
        sn::sortshuffle::ser::bitonic::counting::category::eviction_top
    );
#endif
    sn::sortshuffle::ser::bitonic::bitonic_sort(top.data(), top.size(), top_leaf_key, shared_top_u64_less{});
  }
  const std::uint64_t stash_bucket = E;
  sn::obliv::oadjust_bucket_id(
      sn::util::span<small_item>(top.data(), top.size()), static_cast<std::uint32_t>(i - 1ULL),
      opts.bucket_real_size, stash_bucket
  );
  for (auto& item : top) {
    item.bucket_id = sn::obliv::ct_select<std::uint64_t>(item.bucket_id, invalid, item.active != 0);
  }
  auto top_bucket_key = [](const small_item& item) { return item.bucket_id; };
  {
#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
    sn::sortshuffle::ser::bitonic::counting::category_scope comparator_scope(
        sn::sortshuffle::ser::bitonic::counting::category::eviction_top
    );
#endif
    sn::sortshuffle::ser::bitonic::bitonic_sort(top.data(), top.size(), top_bucket_key, shared_top_u64_less{});
  }

  std::uint64_t previous_bucket = 0;
  std::uint64_t within_bucket = 0;
  std::uint64_t capacity_error = 0;
  for (auto& item : top) {
    const bool active = item.active != 0;
    const bool changed = !sn::obliv::ct_eq(item.bucket_id, previous_bucket);
    within_bucket = sn::obliv::ct_select<std::uint64_t>(0, within_bucket, changed);
    const bool is_stash = sn::obliv::ct_eq(item.bucket_id, stash_bucket);
    const std::uint64_t tree_bucket = sn::obliv::ct_select<std::uint64_t>(1, item.bucket_id, is_stash);
    const std::uint64_t tree_target = (tree_bucket - 1ULL) * Z + within_bucket;
    const std::uint64_t stash_target = top_slots + within_bucket;
    const std::uint64_t target = sn::obliv::ct_select<std::uint64_t>(stash_target, tree_target, is_stash);
    item.target_position = sn::obliv::ct_select<std::uint64_t>(target, invalid, active);
    const std::uint64_t limit = sn::obliv::ct_select<std::uint64_t>(resident_slots, Z, is_stash);
    capacity_error |= static_cast<std::uint64_t>(active & sn::obliv::ct_ge(within_bucket, limit));
    within_bucket += static_cast<std::uint64_t>(active);
    previous_bucket = item.bucket_id;
  }
  sn::util::log::ensure(capacity_error == 0, "shared-top evict: assigned top bucket overflow");
  auto top_target_key = [](const small_item& item) { return item.target_position; };
  sn::obliv::oplace(sn::util::span<small_item>(top.data(), top.size()), top_target_key, invalid);

  // Commit fixed public bucket positions only after all capacity checks.
  std::vector<block_t> rebuilt(static_cast<std::size_t>(Z));
  std::size_t suffix_offset = 0;
  for (std::uint64_t chain = 0; chain < E; ++chain) {
    path_to_leaf(leaves[static_cast<std::size_t>(chain)]);
    for (std::uint64_t depth = L + 1ULL; depth-- > i;) {
      for (std::size_t slot = 0; slot < rebuilt.size(); ++slot) {
        rebuilt[slot] = main[suffix_offset++].block;
      }
      const std::uint64_t node = path[static_cast<std::size_t>(geom.routing_depth + depth)];
      storage[node].rebuild(sn::util::span<const block_t>(rebuilt.data(), rebuilt.size()), uid, prng);
      epochs[node].publish_after_eviction_rebuild();
    }
  }
  for (std::uint64_t node = 1; node < E; ++node) {
    for (std::size_t slot = 0; slot < rebuilt.size(); ++slot) {
      rebuilt[slot] = top[static_cast<std::size_t>((node - 1ULL) * Z) + slot].block;
    }
    const auto global_node = st.forest_topology().subtree_node_id_to_node_id(node, subtree_ix);
    storage[global_node].rebuild(sn::util::span<const block_t>(rebuilt.data(), rebuilt.size()), uid, prng);
    epochs[global_node].publish_after_eviction_rebuild();
  }
  // Fill relocated ancestors before the true overflow stash, at fixed public offsets.
  for (std::size_t slot = 0; slot < treetop.size(); ++slot) {
    treetop[slot] = top[static_cast<std::size_t>(top_slots) + slot].block;
  }
  for (std::size_t slot = 0; slot < deferred.size(); ++slot) {
    deferred[slot] = top[static_cast<std::size_t>(top_slots) + treetop.size() + slot].block;
  }
#if defined(SONIC_ORAM_METRICS)
  sn::oram::stash::forestzing::pipeline::observe_subtree_stash_sections(stash_state, subtree);
#endif
}

template <typename Traits>
void evict_shared_top(state<Traits>& st, typename state<Traits>::eviction_plan_buffers& plan,
                      sn::threads::thread_team& workers) {
  namespace pipeline = sn::oram::stash::forestzing::pipeline;
  auto& stash_state = st.stash().state;
  auto& runtime = stash_state.runtime;
  const auto& geom = st.shape();
  const std::size_t E = st.options().evict_batch;
  pipeline::observe_forest_stash_sections(stash_state);
  // Take one immutable snapshot before starting subtree workers.
  if (!stash_state.disjoint_epoch_mode) {
    pipeline::snapshot_global_stash(stash_state);
  } else {
    sn::util::log::ensure(runtime.snapshot_prefilled, "shared-top evict: missing disjoint snapshot");
    runtime.snapshot_prefilled = false;
  }
#if defined(SONIC_ORAM_METRICS)
  stash_state.metrics.observe_global_snapshot_real(pipeline::count_real_blocks(std::as_const(runtime).snapshot_span()));
#endif
  if (geom.routing_depth == 0) {
    evict_shared_top_subtree<Traits>(st, 0, plan.subtree_slot(0, E), runtime.snapshot_span(), st.prng());
    pipeline::observe_forest_stash_sections(stash_state);
    return;
  }
  runtime.ensure_worker_capacity(workers.logical_threads(), stash_state.geom, st.uid_gen());
  workers.parallel_work([&](std::size_t logical) noexcept {
    auto& scratch = runtime.worker_buffers[logical];
    const auto [begin, end] = sn::threads::partition_evenly(logical, geom.subtree_count, workers.logical_threads());
    for (std::size_t ix = begin; ix < end; ++ix) {
      const auto subtree_ix = static_cast<std::uint32_t>(ix);
      auto& subtree = stash_state.subtrees[ix];
      const auto [leaf_begin, leaf_end] = st.forest_topology().leaf_range_for_subtree(ix);
      pipeline::route_gstash_to_subtree(stash_state, subtree, subtree_ix,
          {leaf_begin, leaf_end}, scratch);
      evict_shared_top_subtree<Traits>(st, subtree_ix, plan.subtree_slot(ix, E),
                                      subtree.routed_pathreads.span(subtree.storage), *scratch.prng);
    }
  });
  pipeline::observe_forest_stash_sections(stash_state);
}

} // namespace sn::oram::zingoram::detail
