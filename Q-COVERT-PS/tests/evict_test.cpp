#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ringdoram/oram.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"

namespace {

constexpr std::size_t block_bytes = 64;
constexpr std::size_t loaded = 128;
constexpr std::size_t lanes = 4;
using data_t = std::array<std::uint8_t, block_bytes>;

std::uint64_t next_random(std::uint64_t& state) {
  state ^= state << 13U;
  state ^= state >> 7U;
  state ^= state << 17U;
  return state;
}

template <ringdoram::epoch_mode Mode = ringdoram::epoch_mode::default_epoch>
void run_case(std::uint32_t E, ringdoram::eviction_backend backend, std::uint32_t R = 0,
              std::size_t eviction_threads = 1) {
  using client_t = ringdoram::client<ringdoram::traits<block_bytes, Mode>>;
  ringdoram::options opts{};
  opts.block_count = 512;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 8;
  opts.eviction_rate = 1;
  opts.evict_batch = E;
  opts.routing_depth = R;
  opts.access_concurrency = lanes;
  opts.evict_backend = backend;
  if constexpr (Mode == ringdoram::epoch_mode::disjoint_epoch) {
    const auto chunk = E * (1U << R);
    opts.disjoint_epoch_window = ((loaded + chunk - 1) / chunk) * chunk;
  }

  sn::threads::pthread_thread_pool eviction_pool(eviction_threads - 1);
  sn::threads::thread_team eviction_workers(eviction_pool.pool(), eviction_threads);
  client_t oram(opts, std::move(eviction_workers));
  sn::threads::pthread_thread_pool access_pool(lanes - 1);
  sn::threads::thread_team access_workers(access_pool.pool(), lanes);
  ringdoram::batch_workspace<client_t> workspace;
  workspace.configure(oram, lanes);

  std::array<std::uint64_t, loaded> position{};
  std::array<data_t, loaded> shadow{};
  std::uint64_t random = 0x243f6a8885a308d3ULL;
  std::vector<ringdoram::bulk_record<block_bytes>> records(loaded);
  for (std::size_t address = 0; address < loaded; ++address) {
    position[address] = next_random(random) % oram.shape().leaf_count;
    shadow[address].fill(static_cast<std::uint8_t>(address + 1));
    records[address] = {static_cast<std::int64_t>(address), static_cast<std::int64_t>(position[address]),
                        shadow[address]};
  }
  oram.bulk_setup(records);

  for (std::size_t round = 0; round < std::max<std::size_t>(8, 3U * E * (1U << R) / lanes); ++round) {
    if constexpr (Mode == ringdoram::epoch_mode::disjoint_epoch) {
      // Flush before wrapping addresses, even when the padded window is larger.
      if (round * lanes % loaded == 0)
        oram.flush_epoch();
    }
    std::array<data_t, lanes> inputs{};
    std::array<data_t, lanes> outputs{};
    std::array<std::size_t, lanes> addresses{};
    std::array<bool, lanes> writes{};
    std::array<ringdoram::access_request, lanes> requests{};
    for (std::size_t lane = 0; lane < lanes; ++lane) {
      const std::size_t address = (round * lanes + lane) % loaded;
      const bool write = ((round * lanes + lane) % 5U) == 0;
      const std::uint64_t new_leaf = next_random(random) % oram.shape().leaf_count;
      addresses[lane] = address;
      writes[lane] = write;
      inputs[lane].fill(static_cast<std::uint8_t>(round + lane + 17));
      requests[lane] = {
          .address = static_cast<std::int64_t>(address),
          .cur_leaf = static_cast<std::int64_t>(position[address]),
          .new_leaf = static_cast<std::int64_t>(new_leaf),
          .is_write = write,
          .in = sn::util::span<std::uint8_t>(inputs[lane].data(), inputs[lane].size()),
          .out = sn::util::span<std::uint8_t>(outputs[lane].data(), outputs[lane].size()),
      };
      position[address] = new_leaf;
    }
    ringdoram::access_batch(oram, std::span<const ringdoram::access_request>(requests), workspace, access_workers);
    for (std::size_t lane = 0; lane < lanes; ++lane) {
      if (writes[lane]) {
        shadow[addresses[lane]] = inputs[lane];
      } else {
        assert(outputs[lane] == shadow[addresses[lane]]);
      }
    }
  }
  if constexpr (Mode == ringdoram::epoch_mode::disjoint_epoch)
    oram.flush_epoch();
  typename client_t::access_scratch scratch;
  oram.configure_access_scratch(scratch);
  for (std::size_t address = 0; address < loaded; ++address) {
    data_t input{};
    data_t output{};
    const std::uint64_t next_leaf = next_random(random) % oram.shape().leaf_count;
    const ringdoram::access_request request{
        .address = static_cast<std::int64_t>(address),
        .cur_leaf = static_cast<std::int64_t>(position[address]),
        .new_leaf = static_cast<std::int64_t>(next_leaf),
        .is_write = false,
        .in = sn::util::span<std::uint8_t>(input.data(), input.size()),
        .out = sn::util::span<std::uint8_t>(output.data(), output.size()),
    };
    oram.access(request, scratch);
    assert(output == shadow[address]);
    position[address] = next_leaf;
  }
  if constexpr (Mode == ringdoram::epoch_mode::disjoint_epoch)
    oram.flush_epoch();
}

} // namespace

int main() {
  for (const auto E : {8U, 32U, 64U}) {
    run_case(E, ringdoram::eviction_backend::sonic);
    run_case(E, ringdoram::eviction_backend::shared_top);
  }
  for (const auto R : {1U, 3U, 6U}) {
    for (const auto E : {1U, 2U, 4U, 8U}) {
      for (const auto threads : {1U, 1U << R}) {
        run_case(E, ringdoram::eviction_backend::shared_top, R, threads);
      }
    }
  }
  for (const auto R : {3U, 6U}) {
    for (const auto E : {2U, 8U}) {
      run_case<ringdoram::epoch_mode::disjoint_epoch>(E, ringdoram::eviction_backend::shared_top, R, 1U << R);
    }
  }
}
