#include "../apps/common/setup_support.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"

#include <iostream>
#include <type_traits>

namespace {
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

void run(std::uint64_t r, std::uint64_t e, std::size_t threads) {
  using traits = setup_benchmark::traits<64>;
  using client = ringdoram::client<traits>;
  using record = ringdoram::bulk_record<64>;
  static_assert(!client::disjoint_epoch_mode);
  static_assert(std::is_same_v<traits::block_store_t,
                              sn::oram::zingoram::storage::slab_store<traits::block_t>>);
  constexpr std::size_t n = 4099; // several progress chunks + partial eviction window
  auto opts = setup_benchmark::options(n, 8, 8, 8, r, e, threads);
  require(opts.evict_backend == ringdoram::eviction_backend::sonic && !opts.disjoint_epoch_window,
          "baseline must use original Sonic scheduling");
  sn::threads::pthread_thread_pool access_pool(threads - 1);
  sn::threads::pthread_thread_pool eviction_pool(threads - 1);
  sn::threads::thread_team access_workers(access_pool.pool(), threads);
  sn::threads::thread_team eviction_workers(eviction_pool.pool(),
      std::bit_floor(std::min<std::size_t>(std::size_t{1} << r, threads)));
  client oram(opts, std::move(eviction_workers));
  oram.initialize();
  std::vector<record> records(n);
  // Force repeated old-path reads. This exercises just-in-time reshuffles in
  // buckets that are not on the current scheduled eviction path.
  std::vector<std::int64_t> old_leaves(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    records[i].address = i;
    records[i].leaf = (i * 73) % oram.shape().leaf_count;
    for (std::size_t j = 0; j < 64; ++j) records[i].data[j] = i >> ((j % 8) * 8);
  }
  const auto window = 8 * e * (std::uint64_t{1} << r);
  setup_benchmark::insert(oram, std::span<record>(records), old_leaves, access_workers,
      [&](std::size_t done, std::size_t) {
        const auto metrics = oram.state_ref().metrics_snapshot();
        require(metrics.access_ops == done && metrics.evict_ops == (done / window) * (window / 8),
                "eviction must occur during loading, at each complete window");
      });

  client::access_scratch scratch;
  oram.configure_access_scratch(scratch);
  std::array<std::uint8_t, 64> input{}, output{};
  // Read everything twice with remapping. Starting at the last record checks
  // the final partial window's stash before it is evicted by further reads.
  for (std::size_t round = 0; round < 2; ++round) {
    for (std::size_t i = n; i-- > 0;) {
      auto& item = records[i];
      const auto next_leaf = (item.leaf + 17) % oram.shape().leaf_count;
      ringdoram::access_request req{item.address, item.leaf, static_cast<std::int64_t>(next_leaf), false,
                                    {input.data(), input.size()}, {output.data(), output.size()}};
      oram.access(req, scratch);
      require(output == item.data, "Sonic setup lost or duplicated a block");
      item.leaf = req.new_leaf;
    }
  }
}
void plain_overlap() {
  using traits = setup_benchmark::traits<64>;
  using client = ringdoram::client<traits>;
  using record = ringdoram::bulk_record<64>;
  sn::threads::pthread_thread_pool pool(0);
  auto opts = setup_benchmark::options(512, 8, 8, 8, 2, 4, 1);
  client oram(opts, sn::threads::thread_team(pool.pool(), 1));
  std::vector<record> records(128);
  for (std::size_t i = 0; i < records.size(); ++i) {
    records[i].address = i;
    records[i].leaf = 0; // fills path, overlap, treetop, and deferred stash
    records[i].data.fill(static_cast<std::uint8_t>(i + 1));
  }
  setup_benchmark::plain_setup(oram, std::span<const record>(records));
  client::access_scratch scratch;
  oram.configure_access_scratch(scratch);
  std::array<std::uint8_t, 64> input{}, output{};
  for (const auto& item : records) {
    ringdoram::access_request req{item.address, item.leaf,
                                 static_cast<std::int64_t>(item.address % oram.shape().leaf_count), false,
                                 {input.data(), 64}, {output.data(), 64}};
    oram.access(req, scratch);
    require(output == item.data, "plain initialization lost overlap/stash blocks");
  }
}

} // namespace

int main() {
  try {
    plain_overlap();
    run(0, 1, 1);
    run(0, 1, 4); // access concurrency exceeds subtree count
    run(2, 1, 4);
    run(2, 4, 4); // parallel evictions with overlapping virtual top buckets
    bool rejected = false;
    try { (void)setup_benchmark::options(4096, 8, 8, 8, 31, 4, 4); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "reject overflowing access windows before narrowing");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
