#include "ringdoram/oram.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include <iostream>
#include <stdexcept>
int main() {
  try {
    using client = ringdoram::client<ringdoram::traits<64, ringdoram::epoch_mode::disjoint_epoch>>;
    constexpr std::size_t count = 160;
    for (const auto R : {0U, 1U, 2U}) {
      ringdoram::options opts{};
      opts.block_count = count;
      opts.bucket_real_size = 4;
      opts.bucket_dummy_size = 8;
      opts.eviction_rate = 1;
      opts.evict_batch = 2;
      opts.stash_bound = 128;
      opts.access_concurrency = 1U << R;
      opts.disjoint_epoch_window = count;
      opts.evict_backend = ringdoram::eviction_backend::shared_top;
      opts.routing_depth = R;
      sn::threads::pthread_thread_pool pool((1U << R) - 1);
      sn::threads::thread_team team(pool.pool(), 1U << R);
      client oram(opts, team.limited_to(1U << R));
      std::vector<ringdoram::bulk_record<64>> records(count);
      for (std::size_t i = 0; i < records.size(); ++i) {
        records[i].address = i;
        records[i].leaf = (i % (1U << R)) * oram.shape().subtree_leaf_count;
        records[i].data.fill(static_cast<std::uint8_t>(i));
      }
      oram.bulk_setup(records);
      std::vector<std::array<std::uint8_t, 64>> in(count), out(count);
      std::vector<ringdoram::access_request> requests(count);
      for (std::size_t i = 0; i < count; ++i)
        requests[i] = {static_cast<std::int64_t>(i), records[i].leaf, records[i].leaf, false,
                       {in[i].data(), 64}, {out[i].data(), 64}};
      ringdoram::batch_workspace<client> workspace;
      workspace.configure(oram, count);
      for (int round = 0; round < 2; ++round) {
        oram.reset_stash_metrics();
        ringdoram::AccessBatch(oram, requests, workspace, team, ringdoram::batch_close::flush);
        for (std::size_t i = 0; i < count; ++i)
          if (out[i] != records[i].data)
            throw std::runtime_error("stash test lost data");
        const auto stats = oram.stash_metrics_snapshot();
        std::uint64_t actual = 0;
        auto &state = oram.state_ref().stash().state;
        for (const auto &subtree : state.subtrees)
          for (const auto &b : subtree.local_deferred.span(subtree.storage))
            actual += b.address >= 0;
        // Same-leaf placement exceeds the real capacity of one path. A zero peak is a missing hook.
        if (!actual || stats.maxima.subtree_stash_deferred_real_max < actual ||
            stats.maxima.subtree_stash_deferred_real_max > 128 * (1U << R))
          throw std::runtime_error("shared_top stash peak missing or below observed occupancy");
        std::cout << "resident=" << actual << " peak=" << stats.maxima.subtree_stash_deferred_real_max << '\n';
      }
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
