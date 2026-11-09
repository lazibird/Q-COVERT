#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ringdoram/oram.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"

namespace {

constexpr std::size_t block_bytes = 64;
constexpr std::size_t block_count = 32;
constexpr std::size_t batch_size = 4;
constexpr std::size_t rounds = 64;
using traits_type = ringdoram::traits<block_bytes, ringdoram::epoch_mode::disjoint_epoch>;
using client_type = ringdoram::client<traits_type>;
using buffer_type = std::array<std::uint8_t, block_bytes>;

std::uint64_t next_random(std::uint64_t& state) {
  state ^= state << 13U;
  state ^= state >> 7U;
  state ^= state << 17U;
  return state;
}

} // namespace

int main() {
  ringdoram::options opts{};
  opts.block_count = block_count;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 12;
  opts.eviction_rate = 1;
  opts.evict_batch = 2;
  opts.routing_depth = 1;
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

  // This is deliberately a plain test-harness position map. Production callers
  // must provide their own oblivious position-map implementation.
  std::array<std::uint64_t, block_count> position{};
  std::array<buffer_type, block_count> shadow{};
  std::uint64_t random_state = 0x243f6a8885a308d3ULL;
  for (auto& leaf : position) {
    leaf = next_random(random_state) % oram.shape().leaf_count;
  }

  for (std::size_t round = 0; round < rounds; ++round) {
    std::array<buffer_type, batch_size> inputs{};
    std::array<buffer_type, batch_size> outputs{};
    std::array<std::size_t, batch_size> addresses{};
    std::array<bool, batch_size> writes{};
    std::vector<ringdoram::batch_request> requests(batch_size);

    for (std::size_t lane = 0; lane < batch_size; ++lane) {
      const std::size_t address = (round * 7 + lane * 13) % block_count;
      const bool write = (next_random(random_state) & 3U) != 0;
      const std::uint64_t new_leaf = next_random(random_state) % oram.shape().leaf_count;
      addresses[lane] = address;
      writes[lane] = write;
      for (std::size_t byte = 0; byte < block_bytes; ++byte) {
        inputs[lane][byte] = static_cast<std::uint8_t>(next_random(random_state));
      }
      requests[lane] = ringdoram::batch_request{
          .address = static_cast<std::int64_t>(address),
          .cur_leaf = static_cast<std::int64_t>(position[address]),
          .new_leaf = static_cast<std::int64_t>(new_leaf),
          .is_write = write,
          .in = sn::util::span<std::uint8_t>(inputs[lane].data(), inputs[lane].size()),
          .out = sn::util::span<std::uint8_t>(outputs[lane].data(), outputs[lane].size()),
      };
      position[address] = new_leaf;
    }

    ringdoram::access_batch(oram, requests, workspace, access_workers, ringdoram::batch_close::flush);
    for (std::size_t lane = 0; lane < batch_size; ++lane) {
      const std::size_t address = addresses[lane];
      if (writes[lane]) {
        shadow[address] = inputs[lane];
      } else {
        assert(outputs[lane] == shadow[address]);
      }
    }
  }
  return 0;
}
