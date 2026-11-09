#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#ifndef RINGDORAM_SGX_HAS_BISON
extern "C" std::uint32_t bison_sgx_benchmark(void *, const std::uint8_t *, std::size_t,
    std::uint8_t *, std::size_t, std::size_t *, std::uint32_t *) { return 254; }
#endif
#ifndef RINGDORAM_SGX_HAS_COMPASS
// Keep the generated EDL bridge linkable when the optional baseline is off.
extern "C" std::uint32_t compass_sgx_benchmark(void *, const std::uint8_t *, std::size_t,
    std::uint8_t *, std::size_t, std::size_t *written, std::uint32_t *snapshot_status) {
  *written = 0;
  *snapshot_status = 0;
  return 1;
}
#endif

#include "ringdoram_sgx_t.h"
#include "qcovert_smoke.hpp"

#include "ringdoram/oram.hpp"
#include "sonic/crypto/prng.hpp"
#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/sgxbridge/enclave/threadpool_portal.hpp"
#include "sonic/sgxbridge/enclave/threadpool_provider.hpp"
#include "sonic/sgxbridge/enclave/threadpool_registry.hpp"

namespace {

constexpr std::size_t block_bytes = 64;
constexpr std::size_t batch_size = 4;
using traits_type = ringdoram::traits<block_bytes, ringdoram::epoch_mode::disjoint_epoch>;
using client_type = ringdoram::client<traits_type>;
using buffer_type = std::array<std::uint8_t, block_bytes>;

struct enclave_environment {
  sn::sgxbridge::common::enclave_execution_context context{};
  sn::sgxbridge::tp::registry registry{};
};

enclave_environment& environment() {
  static enclave_environment value;
  value.context.registry = &value.registry;
  return value;
}

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

std::uint32_t run_smoke_test(void* host_state) {
  if (host_state == nullptr) {
    return 1;
  }

  auto& env = environment();
  env.context.host_cookie = host_state;
  sn::sgxbridge::enclave::execution_context_guard context_guard(env.context);

  // Verify that the enclave AES-CTR backend preserves explicit-seed semantics.
  sn::crypto::prng::seed_material seed{};
  for (std::size_t i = 0; i < seed.key.size(); ++i) {
    seed.key[i] = static_cast<std::uint8_t>(i + 1);
  }
  for (std::size_t i = 0; i < seed.iv.size(); ++i) {
    seed.iv[i] = static_cast<std::uint8_t>(0xa0U + i);
  }
  sn::crypto::prng first(seed);
  sn::crypto::prng second(seed);
  std::array<std::uint8_t, 37> first_stream{};
  std::array<std::uint8_t, 37> second_stream{};
  first.random_bytes(first_stream.data(), first_stream.size());
  second.random_bytes(second_stream.data(), second_stream.size());
  if (first_stream != second_stream) {
    return 4;
  }

  sn::sgxbridge::enclave::threadpool_portal portal(&env.context);
  const auto provider = sn::sgxbridge::enclave::make_threadpool_provider(portal);

  sn::sgxbridge::tp::session access_session;
  if (!access_session.open(provider, sn::sgxbridge::tp::make_request(batch_size - 1, batch_size * 2, {}, "access"))
           .succeeded()) {
    return 2;
  }
  sn::sgxbridge::tp::session eviction_session;
  if (!eviction_session.open(provider, sn::sgxbridge::tp::make_request(1, 4, {}, "eviction")).succeeded()) {
    return 3;
  }

  ringdoram::options opts{};
  opts.block_count = 32;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 12;
  opts.eviction_rate = 1;
  opts.evict_batch = 1;
  opts.routing_depth = 1;
  opts.access_concurrency = batch_size;
  opts.disjoint_epoch_window = batch_size;

  sn::threads::thread_team eviction_workers(eviction_session.pool_ref(), 2);
  client_type oram(opts, std::move(eviction_workers));
  oram.initialize();
  sn::threads::thread_team access_workers(access_session.pool_ref(), batch_size);
  ringdoram::batch_workspace<client_type> workspace;
  workspace.configure(oram, batch_size);

  const std::array<std::int64_t, batch_size> addresses{{1, 5, 9, 13}};
  std::array<std::uint64_t, batch_size> leaves{{0, 3, 7, 11}};
  std::array<std::uint64_t, batch_size> remapped{{16, 19, 23, 27}};
  std::array<buffer_type, batch_size> inputs{};
  std::array<buffer_type, batch_size> outputs{};
  for (std::size_t i = 0; i < batch_size; ++i) {
    inputs[i].fill(static_cast<std::uint8_t>(0x40U + i));
  }

  auto writes = make_requests(addresses, leaves, remapped, true, inputs, outputs);
  ringdoram::AccessBatch(oram, std::span<const ringdoram::access_request>(writes), workspace, access_workers,
                         ringdoram::batch_close::flush);

  leaves = remapped;
  remapped = {{2, 6, 10, 14}};
  std::array<buffer_type, batch_size> read_inputs{};
  std::array<buffer_type, batch_size> read_outputs{};
  auto reads = make_requests(addresses, leaves, remapped, false, read_inputs, read_outputs);
  ringdoram::AccessBatch(oram, std::span<const ringdoram::access_request>(reads), workspace, access_workers,
                         ringdoram::batch_close::flush);

  for (std::size_t i = 0; i < batch_size; ++i) {
    if (read_outputs[i] != inputs[i]) {
      return static_cast<std::uint32_t>(10 + i);
    }
  }

  // Exercise both eviction backends and the original R=1 forest in the enclave,
  // including E>1 overlap regions, concurrent reads and a subsequent flush.
  const std::array<std::int64_t, batch_size> bulk_addresses{{0, 1, 2, 3}};
  std::array<std::uint64_t, batch_size> bulk_leaves{{0, 1, 2, 3}};
  std::array<std::uint64_t, batch_size> bulk_remapped{{4, 5, 6, 7}};
  std::array<ringdoram::bulk_record<block_bytes>, batch_size> bulk_records{};
  for (std::size_t i = 0; i < batch_size; ++i) {
    bulk_records[i].address = bulk_addresses[i];
    bulk_records[i].leaf = static_cast<std::int64_t>(bulk_leaves[i]);
    bulk_records[i].data = inputs[i];
  }
  for (std::uint32_t scenario = 0; scenario < 3; ++scenario) {
    const std::uint32_t routing_depth = scenario == 1 ? 1U : 0U;
    ringdoram::options bulk_opts = opts;
    bulk_opts.routing_depth = routing_depth;
    bulk_opts.evict_batch = 4;
    bulk_opts.disjoint_epoch_window = batch_size * bulk_opts.evict_batch * (1U << routing_depth);
    bulk_opts.evict_backend = scenario == 2 ? ringdoram::eviction_backend::shared_top :
                                              ringdoram::eviction_backend::sonic;
    sn::threads::thread_team bulk_eviction_workers(eviction_session.pool_ref(), 1);
    client_type bulk_oram(bulk_opts, std::move(bulk_eviction_workers));
    bulk_oram.bulk_setup(std::span<const ringdoram::bulk_record<block_bytes>>(bulk_records));
    ringdoram::batch_workspace<client_type> bulk_workspace;
    bulk_workspace.configure(bulk_oram, batch_size);
    std::array<buffer_type, batch_size> bulk_read_inputs{};
    std::array<buffer_type, batch_size> bulk_read_outputs{};
    auto bulk_reads = make_requests(bulk_addresses, bulk_leaves, bulk_remapped, false, bulk_read_inputs,
                                    bulk_read_outputs);
    ringdoram::AccessBatch(bulk_oram, std::span<const ringdoram::access_request>(bulk_reads), bulk_workspace,
                           access_workers, ringdoram::batch_close::flush);
    for (std::size_t i = 0; i < batch_size; ++i) {
      if (bulk_read_outputs[i] != inputs[i]) {
        return static_cast<std::uint32_t>(20 + i + scenario * batch_size);
      }
    }
  }
  if (!qcovert_sgx_smoke(access_workers)) return 50;
  return 0;
}

} // namespace

extern "C" std::uint32_t ringdoram_sgx_run(void* host_state) {
  try {
    return run_smoke_test(host_state);
  } catch (...) {
    return 255;
  }
}
