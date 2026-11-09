#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "ringdoram/oram.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"

namespace {

constexpr std::size_t block_bytes = 64;
constexpr std::size_t block_count = 32;
using record_t = ringdoram::bulk_record<block_bytes>;
using payload_t = std::array<std::uint8_t, block_bytes>;

std::uint64_t step(std::uint64_t& state) {
  state ^= state << 13U;
  state ^= state >> 7U;
  state ^= state << 17U;
  return state;
}

template <ringdoram::epoch_mode Mode>
void run_case(
    std::uint32_t evict_batch, std::size_t loaded, std::uint32_t routing_depth = 0,
    ringdoram::eviction_backend backend = ringdoram::eviction_backend::sonic
) {
  using traits_t = ringdoram::traits<block_bytes, Mode>;
  using client_t = ringdoram::client<traits_t>;
  ringdoram::options opts{};
  opts.block_count = block_count;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 12;
  opts.eviction_rate = 1;
  opts.routing_depth = routing_depth;
  opts.evict_batch = evict_batch;
  opts.access_concurrency = 4;
  opts.evict_backend = backend;
  if constexpr (Mode == ringdoram::epoch_mode::disjoint_epoch) {
    opts.disjoint_epoch_window = 4 * evict_batch * (1U << routing_depth);
  }

  sn::threads::pthread_thread_pool eviction_pool(0);
  sn::threads::thread_team eviction_workers(eviction_pool.pool(), 1);
  client_t oram(opts, std::move(eviction_workers));
  const std::uint64_t leaves = oram.shape().leaf_count;

  std::array<std::uint64_t, block_count> position{};
  std::array<payload_t, block_count> expected{};
  std::vector<record_t> records(loaded);
  std::uint64_t random = 0x243f6a8885a308d3ULL;
  for (std::size_t i = 0; i < block_count; ++i) {
    position[i] = step(random) % leaves;
    for (std::size_t b = 0; b < block_bytes; ++b) {
      expected[i][b] = static_cast<std::uint8_t>((i * 31 + b * 7) & 0xffU);
    }
    if (i < loaded) {
      records[i] = record_t{static_cast<std::int64_t>(i), static_cast<std::int64_t>(position[i]), expected[i]};
    }
  }
  oram.bulk_setup(records);

  sn::threads::pthread_thread_pool access_pool(3);
  sn::threads::thread_team access_workers(access_pool.pool(), 4);
  ringdoram::batch_workspace<client_t> workspace;
  workspace.configure(oram, 4);
  for (std::size_t round = 0; round < 5; ++round) {
    for (std::size_t start = 0; start < loaded; start += 4) {
      std::array<payload_t, 4> inputs{};
      std::array<payload_t, 4> outputs{};
      std::array<ringdoram::access_request, 4> requests{};
      const bool write = round % 2 == 1;
      for (std::size_t lane = 0; lane < 4; ++lane) {
        const std::size_t address = start + lane;
        inputs[lane].fill(static_cast<std::uint8_t>(address + round * 17));
        const std::uint64_t next_leaf = step(random) % leaves;
        requests[lane] = ringdoram::access_request{
            .address = static_cast<std::int64_t>(address),
            .cur_leaf = static_cast<std::int64_t>(position[address]),
            .new_leaf = static_cast<std::int64_t>(next_leaf),
            .is_write = write,
            .in = sn::util::span<std::uint8_t>(inputs[lane].data(), block_bytes),
            .out = sn::util::span<std::uint8_t>(outputs[lane].data(), block_bytes),
        };
        position[address] = next_leaf;
      }
      if constexpr (Mode == ringdoram::epoch_mode::disjoint_epoch) {
        ringdoram::access_batch(oram, requests, workspace, access_workers, ringdoram::batch_close::flush);
      } else {
        ringdoram::access_batch(oram, requests, workspace, access_workers);
      }
      for (std::size_t lane = 0; lane < 4; ++lane) {
        if (write)
          expected[start + lane] = inputs[lane];
        else
          assert(outputs[lane] == expected[start + lane]);
      }
    }
  }
  if (loaded < block_count) {
    const std::size_t address = loaded;
    payload_t input{};
    payload_t output{};
    auto run_one = [&](bool write) {
      const auto next_leaf = step(random) % leaves;
      const ringdoram::access_request request{
          .address = static_cast<std::int64_t>(address),
          .cur_leaf = static_cast<std::int64_t>(position[address]),
          .new_leaf = static_cast<std::int64_t>(next_leaf),
          .is_write = write,
          .in = sn::util::span<std::uint8_t>(input.data(), input.size()),
          .out = sn::util::span<std::uint8_t>(output.data(), output.size()),
      };
      if constexpr (Mode == ringdoram::epoch_mode::disjoint_epoch) {
        ringdoram::access_batch(oram, std::span<const ringdoram::access_request>(&request, 1), workspace,
                                access_workers, ringdoram::batch_close::flush);
      } else {
        ringdoram::access_batch(oram, std::span<const ringdoram::access_request>(&request, 1), workspace,
                                access_workers);
      }
      position[address] = next_leaf;
    };
    run_one(false);
    assert(output == payload_t{});
    input = expected[address];
    run_one(true);
    output.fill(0);
    run_one(false);
    assert(output == expected[address]);
  }
}

void test_reject_reinitialize() {
  using traits_t = ringdoram::traits<block_bytes>;
  ringdoram::options opts{};
  opts.block_count = block_count;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 12;
  opts.eviction_rate = 1;
  opts.routing_depth = 1;
  opts.evict_batch = 1;
  sn::threads::pthread_thread_pool pool(0);
  sn::threads::thread_team workers(pool.pool(), 1);
  ringdoram::client<traits_t> oram(opts, std::move(workers));
  oram.bulk_setup(std::span<const record_t>{});
  bool rejected = false;
  try {
    oram.bulk_setup(std::span<const record_t>{});
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  opts.evict_backend = ringdoram::eviction_backend::shared_top;
  opts.evict_batch = 32; // Each R=1 subtree has only 16 leaves.
  sn::threads::thread_team workers_rejected(pool.pool(), 1);
  rejected = false;
  try {
    ringdoram::client<traits_t> unsupported(opts, std::move(workers_rejected));
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);
}

void test_reject_duplicate_and_overflow() {
  using traits_t = ringdoram::traits<block_bytes>;
  ringdoram::options opts{};
  opts.block_count = 128;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 8;
  opts.eviction_rate = 8;
  opts.routing_depth = 0;
  opts.evict_batch = 1;
  opts.access_concurrency = 1;
  sn::threads::pthread_thread_pool pool(0);

  std::array<record_t, 2> duplicate{};
  duplicate[0].address = 0;
  duplicate[1].address = 0;
  duplicate[0].leaf = 0;
  duplicate[1].leaf = 1;
  sn::threads::thread_team workers_a(pool.pool(), 1);
  ringdoram::client<traits_t> duplicate_oram(opts, std::move(workers_a));
  bool rejected = false;
  try {
    duplicate_oram.bulk_setup(duplicate);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  sn::threads::thread_team workers_b(pool.pool(), 1);
  ringdoram::client<traits_t> overflow_oram(opts, std::move(workers_b));
  std::vector<record_t> clustered(opts.block_count);
  for (std::size_t i = 0; i < clustered.size(); ++i) {
    clustered[i].address = static_cast<std::int64_t>(i);
    clustered[i].leaf = 0;
  }
  rejected = false;
  try {
    overflow_oram.bulk_setup(clustered);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  opts.routing_depth = 2;
  sn::threads::thread_team workers_c(pool.pool(), 1);
  ringdoram::client<traits_t> forest_overflow_oram(opts, std::move(workers_c));
  rejected = false;
  try {
    forest_overflow_oram.bulk_setup(clustered);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);
}

void test_forest_treetop_and_deferred() {
  using traits_t = ringdoram::traits<block_bytes>;
  ringdoram::options opts{};
  opts.block_count = 128;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 12;
  opts.eviction_rate = 8;
  opts.routing_depth = 2;
  opts.evict_batch = 2;
  opts.access_concurrency = 1;
  sn::threads::pthread_thread_pool pool(0);
  sn::threads::thread_team workers(pool.pool(), 1);
  ringdoram::client<traits_t> oram(opts, std::move(workers));

  std::vector<record_t> clustered(48);
  for (std::size_t i = 0; i < clustered.size(); ++i) {
    clustered[i].address = static_cast<std::int64_t>(i);
    clustered[i].leaf = 0;
    clustered[i].data.fill(static_cast<std::uint8_t>(i + 1));
  }
  oram.bulk_setup(clustered);

  ringdoram::client<traits_t>::access_scratch scratch;
  oram.configure_access_scratch(scratch);
  payload_t input{};
  payload_t output{};
  for (const std::size_t index : {std::size_t{0}, std::size_t{24}, std::size_t{40}, std::size_t{47}}) {
    const ringdoram::access_request request{
        .address = static_cast<std::int64_t>(index),
        .cur_leaf = 0,
        .new_leaf = 0,
        .is_write = false,
        .in = sn::util::span<std::uint8_t>(input.data(), input.size()),
        .out = sn::util::span<std::uint8_t>(output.data(), output.size()),
    };
    oram.access(request, scratch);
    assert(output == clustered[index].data);
  }
}

void test_shared_top_empty_initialize(std::uint32_t R = 0) {
  using traits_t = ringdoram::traits<block_bytes>;
  ringdoram::options opts{};
  opts.block_count = block_count;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 12;
  opts.eviction_rate = 1;
  opts.routing_depth = R;
  opts.evict_batch = 4;
  opts.access_concurrency = 1;
  opts.evict_backend = ringdoram::eviction_backend::shared_top;
  sn::threads::pthread_thread_pool pool(0);
  sn::threads::thread_team workers(pool.pool(), 1);
  ringdoram::client<traits_t> oram(opts, std::move(workers));
  oram.initialize();
  ringdoram::client<traits_t>::access_scratch scratch;
  oram.configure_access_scratch(scratch);
  payload_t input{};
  payload_t output{};
  for (std::int64_t address = 0; address < 4; ++address) {
    input.fill(static_cast<std::uint8_t>(address + 1));
    const ringdoram::access_request request{
        .address = address,
        .cur_leaf = address,
        .new_leaf = address + 4,
        .is_write = true,
        .in = sn::util::span<std::uint8_t>(input.data(), input.size()),
        .out = sn::util::span<std::uint8_t>(output.data(), output.size()),
    };
    oram.access(request, scratch);
  }
  for (std::int64_t address = 0; address < 4; ++address) {
    const ringdoram::access_request request{
        .address = address,
        .cur_leaf = address + 4,
        .new_leaf = address + 8,
        .is_write = false,
        .in = sn::util::span<std::uint8_t>(input.data(), input.size()),
        .out = sn::util::span<std::uint8_t>(output.data(), output.size()),
    };
    oram.access(request, scratch);
    for (const auto byte : output) {
      assert(byte == static_cast<std::uint8_t>(address + 1));
    }
  }
}

} // namespace

int main() {
  run_case<ringdoram::epoch_mode::default_epoch>(1, block_count);
  run_case<ringdoram::epoch_mode::default_epoch>(4, block_count);
  run_case<ringdoram::epoch_mode::disjoint_epoch>(1, block_count);
  run_case<ringdoram::epoch_mode::disjoint_epoch>(4, block_count);
  run_case<ringdoram::epoch_mode::disjoint_epoch>(4, block_count / 2);
  run_case<ringdoram::epoch_mode::default_epoch>(1, block_count, 1);
  run_case<ringdoram::epoch_mode::default_epoch>(4, block_count, 1);
  run_case<ringdoram::epoch_mode::disjoint_epoch>(1, block_count, 1);
  run_case<ringdoram::epoch_mode::disjoint_epoch>(4, block_count / 2, 1);
  run_case<ringdoram::epoch_mode::disjoint_epoch>(2, block_count, 2);
  run_case<ringdoram::epoch_mode::default_epoch>(1, block_count, 0, ringdoram::eviction_backend::shared_top);
  run_case<ringdoram::epoch_mode::default_epoch>(2, block_count, 0, ringdoram::eviction_backend::shared_top);
  run_case<ringdoram::epoch_mode::default_epoch>(4, block_count, 0, ringdoram::eviction_backend::shared_top);
  run_case<ringdoram::epoch_mode::disjoint_epoch>(2, block_count, 0, ringdoram::eviction_backend::shared_top);
  run_case<ringdoram::epoch_mode::disjoint_epoch>(4, block_count / 2, 0,
                                                  ringdoram::eviction_backend::shared_top);
  test_reject_reinitialize();
  test_reject_duplicate_and_overflow();
  test_forest_treetop_and_deferred();
  test_shared_top_empty_initialize();
  test_shared_top_empty_initialize(1);
  test_shared_top_empty_initialize(2);
  for (const auto R : {1U, 2U, 3U}) {
    for (const auto E : {1U, 2U, 4U}) {
      run_case<ringdoram::epoch_mode::default_epoch>(E, block_count, R, ringdoram::eviction_backend::shared_top);
      run_case<ringdoram::epoch_mode::disjoint_epoch>(E, block_count / 2, R,
                                                   ringdoram::eviction_backend::shared_top);
    }
  }
  return 0;
}
