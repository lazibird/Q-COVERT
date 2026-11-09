#pragma once

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

#include "ringdoram/oram.hpp"

namespace setup_benchmark {

// Match upstream sonic's ordinary zingoram: bounded access windows, periodic
// reverse-lexicographic eviction, and slab storage (inside the enclave on SGX).
template <std::size_t Bytes> using traits = ringdoram::traits<Bytes>;

inline ringdoram::options options(std::uint64_t n, std::uint64_t z, std::uint64_t s,
                                  std::uint64_t a, std::uint64_t r, std::uint64_t e,
                                  std::uint64_t threads) {
  constexpr auto max32 = std::numeric_limits<std::uint32_t>::max();
  if (!n || n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      !z || z > max32 || !s || s > max32 - z || !a || a > max32 ||
      r >= 32 || !std::has_single_bit(e) || e > max32 || !threads || threads > 256)
    throw std::invalid_argument("invalid setup parameters");
  const auto paths = e * (std::uint64_t{1} << r);
  // Upstream's access gate encodes the remaining window in 16 bits.
  if (paths > sn::oram::zingoram::detail::access_gate::max_supported_window() / a)
    throw std::invalid_argument("A * E * 2^R exceeds Sonic's access window limit (65535)");
  ringdoram::options opts{};
  opts.block_count = std::max<std::uint64_t>(n, 2);
  opts.bucket_real_size = static_cast<std::uint32_t>(z);
  opts.bucket_dummy_size = static_cast<std::uint32_t>(s);
  opts.eviction_rate = static_cast<std::uint32_t>(a);
  opts.routing_depth = static_cast<std::uint32_t>(r);
  opts.evict_batch = static_cast<std::uint32_t>(e);
  opts.access_concurrency = static_cast<std::uint32_t>(threads);
  opts.evict_backend = ringdoram::eviction_backend::sonic;
  return opts;
}

// Every address is new, so its old leaf is independent of the final leaf in
// records. Sample outside the timed insertion loop, as with the input leaves.
inline std::vector<std::int64_t> initial_leaves(std::size_t count, std::uint64_t leaves) {
  sn::crypto::buffered_prng<> random;
  std::vector<std::int64_t> result(count);
  for (auto& leaf : result) leaf = static_cast<std::int64_t>(random.random_u64(0, leaves));
  return result;
}

// Match upstream's stream scheduler with one scratch buffer per live worker.
// The client's eviction team MUST use a different pool from access_workers:
// eviction runs within access(), while other access workers may wait on its gate.
template <typename Client, typename Record, typename Progress>
void insert(Client& client, std::span<Record> records, std::span<const std::int64_t> old_leaves,
            sn::threads::thread_team& access_workers, Progress&& progress) {
  static_assert(!Client::disjoint_epoch_mode);
  if (old_leaves.size() != records.size()) throw std::invalid_argument("initial leaf count mismatch");
  const auto threads = access_workers.logical_threads();
  std::vector<typename Client::access_scratch> scratch(threads);
  std::vector<std::array<std::uint8_t, Client::block_t::byte_size>> output(threads);
  for (auto& slot : scratch) client.configure_access_scratch(slot);
  constexpr std::size_t chunk_size = 4096;
  for (std::size_t base = 0; base < records.size(); base += chunk_size) {
    const auto end = std::min(base + chunk_size, records.size());
    std::atomic<std::size_t> next{base};
    access_workers.parallel_work([&](std::size_t worker) noexcept {
      for (;;) {
        const auto index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= end) break;
        auto& record = records[index];
        ringdoram::access_request request{
            .address = record.address, .cur_leaf = old_leaves[index], .new_leaf = record.leaf,
            .is_write = true, .in = {record.data.data(), record.data.size()},
            .out = {output[worker].data(), output[worker].size()}};
        client.access(request, scratch[worker]);
      }
    });
    progress(end, records.size());
  }
  // A partial final window is a valid ordinary Sonic state: those blocks live
  // in pathreads. Do not add a disjoint flush or discard its pending inserts.
#if defined(SONIC_ORAM_METRICS)
  const auto measured = client.state_ref().metrics_snapshot();
  const auto& derived = client.state_ref().derived();
  const auto expected = (records.size() / derived.adjusted_eviction_rate) * derived.batch_eviction_factor;
  if (measured.access_ops != records.size() || measured.evict_ops != expected)
    throw std::runtime_error("Sonic setup eviction cadence mismatch");
#endif
}

// Benchmark-only, non-oblivious counterpart to BulkOR's bottom-up placement.
// Data-dependent leaves, path nodes and bucket slots are indexed directly.
// Ring ORAM's bucket code still inserts dummies and creates the permutation.
template <typename Client, typename Record>
void plain_setup(Client& client, std::span<const Record> records) {
  using block_t = typename Client::block_t;
  client.initialize();
  auto& state = client.state_ref();
  const auto& shape = state.shape();
  const std::size_t z = state.options().bucket_real_size;
  const std::uint64_t node_count = shape.node_count;
  // Buckets exist only at and below the subtree roots (depth >= routing_depth).
  const std::uint64_t first_physical = std::uint64_t{1} << shape.routing_depth;
  std::vector<std::uint32_t> occupancy(static_cast<std::size_t>(node_count) + 1);
  std::vector<block_t> slots(static_cast<std::size_t>(node_count) * z);
  std::vector<std::size_t> order(records.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
    return records[lhs].leaf < records[rhs].leaf;
  });
  std::vector<std::size_t> stash_used(static_cast<std::size_t>(shape.subtree_count));

  for (const auto index : order) {
    const auto& record = records[index];
    block_t block{};
    block.address = record.address;
    block.leaf_ix = record.leaf;
    block.data = record.data;
#if defined(ORAM_DEBUG)
    block.uid = state.uid_gen().next();
    state.assigned_blocks().record(static_cast<std::uint64_t>(record.address), block.uid);
#endif
    std::uint64_t node = shape.leaf_count + static_cast<std::uint64_t>(record.leaf);
    while (node >= first_physical && occupancy[static_cast<std::size_t>(node)] == z) node >>= 1;
    if (node < first_physical) {
      // Above the subtree root: spill into the subtree's treetop, then deferred.
      const auto subtree_ix = static_cast<std::size_t>(record.leaf >> shape.subtree_height);
      auto& subtree = state.stash().state.subtrees[subtree_ix];
      auto treetop = subtree.treetop.span(subtree.storage);
      auto deferred = subtree.local_deferred.span(subtree.storage);
      const std::size_t pos = stash_used[subtree_ix]++;
      if (pos < treetop.size()) {
        treetop[pos] = block;
      } else if (pos - treetop.size() < deferred.size()) {
        deferred[pos - treetop.size()] = block;
      } else {
        throw std::runtime_error("plain setup stash overflow");
      }
    } else {
      const std::size_t offset = (static_cast<std::size_t>(node) - 1) * z;
      slots[offset + occupancy[static_cast<std::size_t>(node)]++] = block;
    }
  }

  const auto overlap_nodes = (std::uint64_t{1} << shape.overlap_depth) - 1;
  for (std::uint64_t subtree_ix = 0; subtree_ix < shape.subtree_count; ++subtree_ix) {
    auto& subtree = state.stash().state.subtrees[subtree_ix];
    auto overlap = subtree.overlap_region.span(subtree.storage);
    for (std::uint64_t local = 1; local <= overlap_nodes; ++local) {
      const auto node = state.forest_topology().subtree_node_id_to_node_id(local, subtree_ix);
      std::copy_n(slots.data() + (node - 1) * z, z, overlap.begin() + (local - 1) * z);
    }
  }
  const auto first_bucket = std::uint64_t{1} << (shape.routing_depth + shape.overlap_depth);
  for (std::uint64_t node = first_bucket; node <= node_count; ++node) {
    state.storage()[node].rebuild(
        sn::util::span<const block_t>(slots.data() + (static_cast<std::size_t>(node) - 1) * z, z),
        state.uid_gen(), state.prng());
  }
}

} // namespace setup_benchmark
