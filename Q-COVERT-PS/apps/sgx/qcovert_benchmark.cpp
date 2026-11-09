#include "qcovert/benchmark.hpp"
#include "snapshot_transport.hpp"
#include "ringdoram_sgx_t.h"
#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/sgxbridge/enclave/threadpool_portal.hpp"
#include "sonic/sgxbridge/enclave/threadpool_provider.hpp"
#include "sonic/sgxbridge/enclave/threadpool_registry.hpp"
#include <sgx_tseal.h>
namespace {
std::uint64_t benchmark_clock() {
  std::uint64_t ns = 0;
  if (qcovert_benchmark_time_ns(&ns) != SGX_SUCCESS)
    throw std::runtime_error("benchmark clock OCALL failed");
  return ns;
}
void report_progress(void *host, const char *stage, std::uint64_t completed, std::uint64_t total) {
  if (qcovert_benchmark_progress(host, stage, completed, total) != SGX_SUCCESS)
    throw std::runtime_error("progress OCALL failed");
}
std::size_t read_snapshot(void *host, void *out, std::size_t size) {
  std::size_t got = 0;
  if (qcovert_snapshot_read(&got, host, static_cast<std::uint8_t *>(out), size) != SGX_SUCCESS)
    throw std::runtime_error("snapshot read OCALL failed");
  return got;
}
void write_snapshot(void *host, const void *data, std::size_t size) {
  std::uint32_t ok = 0;
  if (qcovert_snapshot_write(&ok, host, static_cast<const std::uint8_t *>(data), size) != SGX_SUCCESS || !ok)
    throw std::runtime_error("snapshot write OCALL failed");
}
void commit_snapshot(void *host) {
  std::uint32_t ok = 0;
  if (qcovert_snapshot_commit(&ok, host) != SGX_SUCCESS || !ok)
    throw std::runtime_error("snapshot commit OCALL failed");
}
void stream_report(void *host, const std::uint8_t *bytes, std::size_t size) {
  std::uint32_t ok = 0;
  if (qcovert_report_write(&ok, host, bytes, size) != SGX_SUCCESS || !ok)
    throw std::runtime_error("report stream OCALL failed");
}
std::vector<std::uint8_t> seal_key(const qcovert::key128 &key) {
  const auto size = sgx_calc_sealed_data_size(0, key.size());
  if (size == UINT32_MAX || size > 4096) throw std::runtime_error("invalid sealed key size");
  std::vector<std::uint8_t> blob(size);
  if (sgx_seal_data(0, nullptr, key.size(), key.data(), size,
                    reinterpret_cast<sgx_sealed_data_t *>(blob.data())) != SGX_SUCCESS)
    throw std::runtime_error("SGX snapshot key sealing failed");
  return blob;
}
qcovert::key128 unseal_key(std::span<const std::uint8_t> blob) {
  if (blob.size() == sizeof(qcovert::key128)) {
    // Cross-mode experiment images: native snapshots wrap the stream key raw,
    // so accept it as-is. Sealed blobs are always larger than the raw key.
    qcovert::key128 key{};
    std::memcpy(key.data(), blob.data(), key.size());
    return key;
  }
  if (blob.size() < sizeof(sgx_sealed_data_t))
    throw std::invalid_argument("invalid sealed snapshot key");
  const auto *sealed = reinterpret_cast<const sgx_sealed_data_t *>(blob.data());
  const auto encrypted_size = sgx_get_encrypt_txt_len(sealed);
  const auto mac_size_expected = sgx_get_add_mac_txt_len(sealed);
  if (encrypted_size != 16 || mac_size_expected != 0 ||
      sgx_calc_sealed_data_size(mac_size_expected, encrypted_size) != blob.size())
    throw std::invalid_argument("invalid sealed snapshot key length");
  qcovert::key128 key{};
  std::uint32_t mac_size = 0, key_size = key.size();
  if (sgx_unseal_data(sealed, nullptr, &mac_size,
                      key.data(), &key_size) != SGX_SUCCESS || key_size != key.size())
    throw std::runtime_error("SGX snapshot key unsealing failed");
  return key;
}
} // namespace
qcovert::snapshot::transport qcovert::snapshot::sgx_transport(void *host) {
  return {host, read_snapshot, write_snapshot, commit_snapshot, seal_key, unseal_key};
}
extern "C" std::uint32_t qcovert_sgx_benchmark(void *host, const std::uint8_t *bytes, std::size_t size,
                                               std::uint8_t *output, std::size_t capacity,
                                               std::size_t *written, std::uint32_t *snapshot_status) {
  try {
    *written = 0;
    *snapshot_status = 0;
    (void)output;
    (void)capacity;
    sn::sgxbridge::common::enclave_execution_context context{};
    sn::sgxbridge::tp::registry registry;
    context.registry = &registry;
    context.host_cookie = host;
    sn::sgxbridge::enclave::execution_context_guard guard(context);
    sn::sgxbridge::enclave::threadpool_portal portal(&context);
    const auto provider = sn::sgxbridge::enclave::make_threadpool_provider(portal);
    auto req = qcovert::benchmark::parse({bytes, size});
    sn::sgxbridge::tp::session session;
    if (!session
             .open(provider,
                   sn::sgxbridge::tp::make_request(req.threads - 1, req.threads * 2, {}, "qcovert-benchmark"))
             .succeeded())
      return 2;
    sn::threads::thread_team team(session.pool_ref(), req.threads);
    auto image_io = qcovert::snapshot::sgx_transport(host);
    qcovert::benchmark::snapshot_status image_result = qcovert::benchmark::snapshot_status::none;
    // Each finished job's report block is streamed to the host report file, so
    // no full-report copyback is needed (or possible) at the end.
    qcovert::benchmark::run({bytes, size}, team, benchmark_clock, report_progress, host,
                            &image_io, &image_result, stream_report, host);
    *written = 0;
    *snapshot_status = static_cast<std::uint32_t>(image_result);
    return 0;
  } catch (const std::exception &e) {
    sgx_status_t ignored;
    sonic_sgxbridge_log_sink(&ignored, host, e.what());
    return 1;
  }
}
