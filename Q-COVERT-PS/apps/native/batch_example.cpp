#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

#include "ringdoram/ringdoram.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"

int main() {
  constexpr std::size_t block_bytes = 64;
  constexpr std::size_t batch_size = 4;
  using traits_type = ringdoram::traits<block_bytes, ringdoram::epoch_mode::disjoint_epoch>;
  using client_type = ringdoram::client<traits_type>;

  ringdoram::options opts{};
  opts.block_count = 32;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 12;
  opts.eviction_rate = 1;
  opts.routing_depth = 1;
  opts.evict_batch = 1;
  opts.access_concurrency = batch_size;
  opts.disjoint_epoch_window = batch_size;

  sn::threads::pthread_thread_pool eviction_pool(1);
  sn::threads::thread_team eviction_workers(eviction_pool.pool(), 2);
  client_type oram(opts, std::move(eviction_workers));
  oram.initialize();

  sn::threads::pthread_thread_pool access_pool(batch_size - 1);
  sn::threads::thread_team access_workers(access_pool.pool(), batch_size);
  ringdoram::batch_workspace<client_type> workspace;
  workspace.configure(oram, batch_size);

  // The caller owns this position map. A production caller must access its
  // position map obliviously; direct indexing here is only for the example.
  std::array<std::uint64_t, 32> position_map{};
  for (std::size_t i = 0; i < position_map.size(); ++i) {
    position_map[i] = i;
  }

  std::array<std::array<std::uint8_t, block_bytes>, batch_size> input{};
  std::array<std::array<std::uint8_t, block_bytes>, batch_size> output{};
  std::vector<ringdoram::access_request> requests(batch_size);
  for (std::size_t i = 0; i < batch_size; ++i) {
    const std::int64_t address = static_cast<std::int64_t>(1 + i * 4);
    input[i].fill(static_cast<std::uint8_t>(0x40U + i));
    requests[i] = ringdoram::access_request{
        .address = address,
        .cur_leaf = static_cast<std::int64_t>(position_map[static_cast<std::size_t>(address)]),
        .new_leaf = static_cast<std::int64_t>(16 + i),
        .is_write = true,
        .in = sn::util::span<std::uint8_t>(input[i].data(), input[i].size()),
        .out = sn::util::span<std::uint8_t>(output[i].data(), output[i].size()),
    };
  }

  ringdoram::AccessBatch(
      oram, std::span<const ringdoram::access_request>(requests), workspace, access_workers,
      ringdoram::batch_close::flush
  );
  for (const auto& request : requests) {
    position_map[static_cast<std::size_t>(request.address)] = static_cast<std::uint64_t>(request.new_leaf);
  }

  std::cout << "completed one concurrent batch of " << requests.size() << " distinct writes\n";
  return 0;
}
