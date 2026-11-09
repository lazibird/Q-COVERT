#include "compass/benchmark.hpp"
#include "snapshot_transport.hpp"
#include "ringdoram_sgx_t.h"
#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/sgxbridge/enclave/threadpool_portal.hpp"
#include "sonic/sgxbridge/enclave/threadpool_provider.hpp"
#include "sonic/sgxbridge/enclave/threadpool_registry.hpp"
namespace {
std::uint64_t clock_ns() {
  std::uint64_t ns = 0;
  if (qcovert_benchmark_time_ns(&ns) != SGX_SUCCESS) throw std::runtime_error("Compass clock OCALL failed");
  return ns;
}
void progress(void *host, const char *stage, std::uint64_t done, std::uint64_t total) {
  if (qcovert_benchmark_progress(host, stage, done, total) != SGX_SUCCESS)
    throw std::runtime_error("Compass progress OCALL failed");
}
}
// Public-data experiment entry, like qcovert_sgx_benchmark. Production callers
// establish a trusted session and retain index<L> within their enclave service.
extern "C" std::uint32_t compass_sgx_benchmark(void *host, const std::uint8_t *bytes, std::size_t size,
    std::uint8_t *output, std::size_t capacity, std::size_t *written, std::uint32_t *snapshot_status) {
  try {
    *written = 0;
    *snapshot_status = 0;
    sn::sgxbridge::common::enclave_execution_context context{};
    sn::sgxbridge::tp::registry registry;
    context.registry = &registry; context.host_cookie = host;
    sn::sgxbridge::enclave::execution_context_guard guard(context);
    sn::sgxbridge::enclave::threadpool_portal portal(&context);
    const auto provider = sn::sgxbridge::enclave::make_threadpool_provider(portal);
    const auto req = compass::benchmark::parse({bytes, size});
    sn::sgxbridge::tp::session session;
    if (!session.open(provider, sn::sgxbridge::tp::make_request(req.threads - 1, req.threads * 2, {}, "compass")).succeeded()) return 2;
    sn::threads::thread_team team(session.pool_ref(), req.threads);
    auto image_io = qcovert::snapshot::sgx_transport(host);
    compass::benchmark::snapshot_status image_result = compass::benchmark::snapshot_status::none;
    const auto report = compass::benchmark::run({bytes, size}, team, clock_ns, progress, host,
                                               &image_io, &image_result);
    if (report.size() > capacity) return 3;
    std::memcpy(output, report.data(), report.size()); *written = report.size();
    *snapshot_status = static_cast<std::uint32_t>(image_result);
    return 0;
  } catch (const std::exception &e) {
    sgx_status_t ignored; sonic_sgxbridge_log_sink(&ignored, host, e.what()); return 1;
  }
}
