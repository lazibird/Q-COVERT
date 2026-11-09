#ifdef NDEBUG
#undef NDEBUG
#endif

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ringdoram/oram.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"

namespace {

constexpr std::size_t block_bytes = 64;
constexpr std::size_t batch_size = 4;
using traits_type = ringdoram::traits<block_bytes, ringdoram::epoch_mode::default_epoch>;
using client_type = ringdoram::client<traits_type>;
using buffer_type = std::array<std::uint8_t, block_bytes>;

std::vector<ringdoram::access_request> requests_for(
    const std::array<std::int64_t, batch_size>& addresses, const std::array<std::uint64_t, batch_size>& current,
    const std::array<std::uint64_t, batch_size>& next, bool write, std::array<buffer_type, batch_size>& input,
    std::array<buffer_type, batch_size>& output
) {
  std::vector<ringdoram::access_request> requests(batch_size);
  for (std::size_t i = 0; i < batch_size; ++i) {
    requests[i] = ringdoram::access_request{
        .address = addresses[i],
        .cur_leaf = static_cast<std::int64_t>(current[i]),
        .new_leaf = static_cast<std::int64_t>(next[i]),
        .is_write = write,
        .in = sn::util::span<std::uint8_t>(input[i].data(), input[i].size()),
        .out = sn::util::span<std::uint8_t>(output[i].data(), output[i].size()),
    };
  }
  return requests;
}

} // namespace

int main() {
  ringdoram::options opts{};
  opts.block_count = 32;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 12;
  opts.eviction_rate = 1;
  opts.evict_batch = 1;
  opts.routing_depth = 1;
  opts.access_concurrency = batch_size;

  sn::threads::pthread_thread_pool eviction_pool(1);
  sn::threads::thread_team eviction_workers(eviction_pool.pool(), 2);
  client_type oram(opts, std::move(eviction_workers));
  oram.initialize();

  sn::threads::pthread_thread_pool access_pool(batch_size - 1);
  sn::threads::thread_team access_workers(access_pool.pool(), batch_size);
  ringdoram::batch_workspace<client_type> workspace;
  workspace.configure(oram, batch_size);

  const std::array<std::int64_t, batch_size> addresses{{0, 4, 8, 12}};
  std::array<std::uint64_t, batch_size> leaves{{1, 5, 9, 13}};
  std::array<std::uint64_t, batch_size> remapped{{17, 21, 25, 29}};
  std::array<buffer_type, batch_size> input{};
  std::array<buffer_type, batch_size> output{};
  for (std::size_t i = 0; i < batch_size; ++i) {
    input[i].fill(static_cast<std::uint8_t>(0x70U + i));
  }

  auto writes = requests_for(addresses, leaves, remapped, true, input, output);
  ringdoram::AccessBatch(oram, writes, workspace, access_workers);

  leaves = remapped;
  remapped = {{3, 7, 11, 15}};
  std::array<buffer_type, batch_size> read_input{};
  std::array<buffer_type, batch_size> read_output{};
  auto reads = requests_for(addresses, leaves, remapped, false, read_input, read_output);
  ringdoram::AccessBatch(oram, reads, workspace, access_workers);
  for (std::size_t i = 0; i < batch_size; ++i) {
    assert(read_output[i] == input[i]);
  }

  ringdoram::batch_workspace<client_type> too_small;
  too_small.configure(oram, batch_size - 1);
  bool rejected = false;
  try {
    ringdoram::AccessBatch(oram, reads, too_small, access_workers);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);

  bool flush_rejected = false;
  try {
    ringdoram::AccessBatch(oram, reads, workspace, access_workers, ringdoram::batch_close::flush);
  } catch (const std::logic_error&) {
    flush_rejected = true;
  }
  assert(flush_rejected);
  return 0;
}
