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
using traits_type = ringdoram::traits<block_bytes, ringdoram::epoch_mode::disjoint_epoch>;
using client_type = ringdoram::client<traits_type>;
using buffer_type = std::array<std::uint8_t, block_bytes>;

std::vector<ringdoram::access_request> make_requests(
    const std::array<std::int64_t, batch_size>& addresses, const std::array<std::uint64_t, batch_size>& current_leaves,
    const std::array<std::uint64_t, batch_size>& new_leaves, bool write, std::array<buffer_type, batch_size>& inputs,
    std::array<buffer_type, batch_size>& outputs
) {
  std::vector<ringdoram::access_request> requests(batch_size);
  for (std::size_t i = 0; i < batch_size; ++i) {
    requests[i].address = addresses[i];
    requests[i].cur_leaf = static_cast<std::int64_t>(current_leaves[i]);
    requests[i].new_leaf = static_cast<std::int64_t>(new_leaves[i]);
    requests[i].is_write = write;
    requests[i].in = sn::util::span<std::uint8_t>(inputs[i].data(), inputs[i].size());
    requests[i].out = sn::util::span<std::uint8_t>(outputs[i].data(), outputs[i].size());
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
  // R=1 and E=2 exercise forest routing plus one-sort multi-path eviction.
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

  const std::array<std::int64_t, batch_size> addresses{{1, 5, 9, 13}};
  std::array<std::uint64_t, batch_size> leaves{{0, 3, 7, 11}};
  std::array<std::uint64_t, batch_size> remapped{{16, 19, 23, 27}};
  std::array<buffer_type, batch_size> inputs{};
  std::array<buffer_type, batch_size> outputs{};
  for (std::size_t i = 0; i < batch_size; ++i) {
    inputs[i].fill(static_cast<std::uint8_t>(0x20U + i));
  }

  auto writes = make_requests(addresses, leaves, remapped, true, inputs, outputs);
  ringdoram::AccessBatch(
      oram, std::span<const ringdoram::access_request>(writes), workspace, access_workers,
      ringdoram::batch_close::flush
  );
  assert(oram.pending_epoch_accesses() == 0);

  leaves = remapped;
  remapped = {{2, 6, 10, 14}};
  std::array<buffer_type, batch_size> read_inputs{};
  std::array<buffer_type, batch_size> read_outputs{};
  auto reads = make_requests(addresses, leaves, remapped, false, read_inputs, read_outputs);
  ringdoram::AccessBatch(
      oram, std::span<const ringdoram::access_request>(reads), workspace, access_workers,
      ringdoram::batch_close::flush
  );

  for (std::size_t i = 0; i < batch_size; ++i) {
    assert(read_outputs[i] == inputs[i]);
  }

  // Repeated dummy requests are permitted for public batch-size padding.
  auto padding = reads;
  for (auto& request : padding) {
    request.address = -1;
  }
  ringdoram::AccessBatch(
      oram, std::span<const ringdoram::access_request>(padding), workspace, access_workers,
      ringdoram::batch_close::flush
  );

#if !defined(NDEBUG) || defined(RINGDORAM_ENABLE_CONTRACT_CHECKS)
  auto duplicates = reads;
  duplicates[1].address = duplicates[0].address;
  bool duplicate_rejected = false;
  try {
    ringdoram::AccessBatch(oram, std::span<const ringdoram::access_request>(duplicates), workspace, access_workers);
  } catch (const std::invalid_argument&) {
    duplicate_rejected = true;
  }
  assert(duplicate_rejected);
#endif
  return 0;
}
