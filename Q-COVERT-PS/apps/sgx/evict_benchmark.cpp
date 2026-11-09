// In-enclave batched-eviction benchmark: builds a Ring ORAM (bulk_setup with a
// small real sample) and times pure evict<>() calls for the sonic and
// shared-top backends, inside the enclave (SIM/HW). Mirrors
// apps/native/evict_benchmark.cpp so enclave timings line up with the native
// comparator-count pipeline. Request: u64 magic, config_count, z, s, a,
// stash_bound (0 = auto), then per config {n, evict_batch, backend
// (0=sonic,1=shared), repetitions, label[64] zero-padded}. Each finished
// config streams one CSV row to the host report stream via the
// qcovert_report_write OCALL, so a stopped run keeps every completed
// measurement.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "ringdoram/oram.hpp"
#include "ringdoram_sgx_t.h"
#include "sonic/oram/zingoram/eviction.hpp"
#include "sonic/oram/zingoram/schedule.hpp"
#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/sgxbridge/enclave/threadpool_portal.hpp"
#include "sonic/sgxbridge/enclave/threadpool_provider.hpp"
#include "sonic/sgxbridge/enclave/threadpool_registry.hpp"

namespace {

constexpr std::uint64_t request_magic = 0x5143455643540001ULL; // "QCEVCT" tag
constexpr std::size_t header_words = 6;
constexpr std::size_t config_words = 12; // 4 scalars + 64-byte label

constexpr std::size_t block_bytes = 64;
using traits_t = ringdoram::traits<block_bytes>;
using client_t = ringdoram::client<traits_t>;
using record_t = ringdoram::bulk_record<block_bytes>;

std::uint64_t benchmark_clock() {
  std::uint64_t ns = 0;
  if (qcovert_benchmark_time_ns(&ns) != SGX_SUCCESS)
    throw std::runtime_error("benchmark clock OCALL failed");
  return ns;
}

void report_progress(void* host, const char* stage, std::uint64_t completed, std::uint64_t total) {
  if (qcovert_benchmark_progress(host, stage, completed, total) != SGX_SUCCESS)
    throw std::runtime_error("progress OCALL failed");
}

// Integer-only row formatting (enclave printf lacks reliable %f).
struct row_writer {
  char buf[640];
  std::size_t len = 0;
  void text(const char* s) {
    while (*s && len + 1 < sizeof(buf)) buf[len++] = *s++;
  }
  void u64(std::uint64_t v) {
    char digits[20];
    std::size_t n = 0;
    do {
      digits[n++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v);
    while (n) buf[len++] = digits[--n];
  }
  void ms(std::uint64_t ns) {
    u64(ns / 1000000);
    buf[len++] = '.';
    const std::uint64_t frac = (ns / 1000) % 1000;
    buf[len++] = static_cast<char>('0' + frac / 100);
    buf[len++] = static_cast<char>('0' + (frac / 10) % 10);
    buf[len++] = static_cast<char>('0' + frac % 10);
  }
};

void emit_row(void* host, const char* label, const char* backend, std::uint64_t n, std::uint64_t e,
              std::uint64_t height, std::uint64_t stash_bound, std::uint64_t repetitions,
              std::uint64_t median_ns, std::uint64_t p95_ns, std::uint64_t min_ns) {
  row_writer row;
  row.text(label);
  row.text(",sgx-" RINGDORAM_SETUP_SGX_MODE ",");
  row.text(backend);
  const std::uint64_t fields[] = {n, e, height, stash_bound, repetitions};
  for (auto f : fields) {
    row.buf[row.len++] = ',';
    row.u64(f);
  }
  row.buf[row.len++] = ',';
  row.ms(median_ns);
  row.buf[row.len++] = ',';
  row.ms(p95_ns);
  row.buf[row.len++] = ',';
  row.ms(min_ns);
  row.text("\n");
  std::uint32_t ok = 0;
  if (qcovert_report_write(&ok, host, reinterpret_cast<const std::uint8_t*>(row.buf), row.len) !=
          SGX_SUCCESS ||
      !ok)
    throw std::runtime_error("report stream OCALL failed");
}

struct reader {
  const std::uint8_t* p;
  std::size_t size, pos = 0;
  std::uint64_t get() {
    if (pos + 8 > size) throw std::invalid_argument("truncated evict request");
    std::uint64_t v;
    std::memcpy(&v, p + pos, 8);
    pos += 8;
    return v;
  }
  void label(char* out, std::size_t capacity) {
    if (pos + 64 > size) throw std::invalid_argument("truncated evict label");
    std::size_t n = 0;
    while (n < 64 && p[pos + n]) ++n;
    if (n >= capacity) throw std::invalid_argument("evict label too long");
    std::memcpy(out, p + pos, n);
    out[n] = 0;
    pos += 64;
  }
};

std::uint64_t step(std::uint64_t& state) {
  state ^= state << 13U;
  state ^= state >> 7U;
  state ^= state << 17U;
  return state;
}

void measure_config(reader& in, void* host, std::uint64_t z, std::uint64_t s, std::uint64_t a,
                    std::uint64_t stash_bound, sn::threads::thread_team& workers) {
  const std::uint64_t n = in.get();
  const std::uint64_t evict_batch = in.get();
  const std::uint64_t backend = in.get();
  const std::uint64_t repetitions = in.get();
  char label[65];
  in.label(label, sizeof(label));
  if (!n || n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || !evict_batch ||
      backend > 1 || !repetitions || repetitions > 10000 || !z || !s || !a || !label[0] ||
      std::strpbrk(label, ",\r\n"))
    throw std::invalid_argument("invalid evict config");

  ringdoram::options opts{};
  opts.block_count = n;
  opts.bucket_real_size = static_cast<std::uint32_t>(z);
  opts.bucket_dummy_size = static_cast<std::uint32_t>(s);
  opts.eviction_rate = static_cast<std::uint32_t>(a);
  opts.routing_depth = 0;
  opts.evict_batch = static_cast<std::uint32_t>(evict_batch);
  opts.access_concurrency = 1;
  opts.evict_backend = backend == 1 ? ringdoram::eviction_backend::shared_top
                                    : ringdoram::eviction_backend::sonic;
  opts.stash_bound = stash_bound;

  sn::threads::thread_team one(workers.pool(), 1);
  client_t oram(opts, std::move(one));
  const std::uint64_t leaves = oram.shape().leaf_count;
  if (evict_batch > leaves) throw std::invalid_argument("evict batch exceeds leaf count");
  const std::uint64_t height = oram.shape().height;
  const std::uint64_t cs = oram.state_ref().derived().stash_bound;

  // Same real-block sample as the native benchmark: min(n, 4096) records with
  // xorshift leaves, placed by bulk_setup.
  const std::size_t loaded = static_cast<std::size_t>(std::min<std::uint64_t>(n, 4096));
  std::vector<record_t> records(loaded);
  std::uint64_t random = 0x243f6a8885a308d3ULL;
  for (std::size_t index = 0; index < loaded; ++index) {
    auto& record = records[index];
    record.address = static_cast<std::int64_t>(index);
    record.leaf = static_cast<std::int64_t>(step(random) % leaves);
    record.data.fill(static_cast<std::uint8_t>(index + 1));
  }
  char stage[96];
  std::snprintf(stage, sizeof(stage), "evict/%s", label);
  report_progress(host, stage, 0, repetitions + 1);
  oram.bulk_setup(records);
  sn::oram::zingoram::schedule direct_schedule(oram.state_ref().topology(),
                                               static_cast<std::uint32_t>(evict_batch));

  // Exclude the first eviction after bulk_setup from the timed samples.
  sn::oram::zingoram::evict<traits_t>(oram.state_ref(), direct_schedule, workers);
  report_progress(host, stage, 1, repetitions + 1);

  std::vector<std::uint64_t> samples;
  samples.reserve(static_cast<std::size_t>(repetitions));
  for (std::uint64_t repetition = 0; repetition < repetitions; ++repetition) {
    const auto start = benchmark_clock();
    sn::oram::zingoram::evict<traits_t>(oram.state_ref(), direct_schedule, workers);
    samples.push_back(benchmark_clock() - start);
    report_progress(host, stage, repetition + 2, repetitions + 1);
  }
  std::sort(samples.begin(), samples.end());
  const std::uint64_t median_ns = samples[samples.size() / 2];
  const std::uint64_t p95_ns = samples[((samples.size() * 95U + 99U) / 100U) - 1U];
  emit_row(host, label, backend == 1 ? "shared" : "sonic", n, evict_batch, height, cs, repetitions,
           median_ns, p95_ns, samples.front());
}

} // namespace

extern "C" std::uint32_t qcovert_sgx_evict(void* host, const std::uint8_t* bytes, std::size_t size,
                                           std::uint8_t* output, std::size_t capacity) {
  try {
    (void)output;
    (void)capacity;
    reader in{bytes, size};
    if (in.get() != request_magic) return 2;
    const std::uint64_t count = in.get();
    const std::uint64_t z = in.get(), s = in.get(), a = in.get(), stash_bound = in.get();
    if (!count || count > 64 || !z || !s || !a || in.pos + count * config_words * 8 != size)
      return 2;

    sn::sgxbridge::common::enclave_execution_context context{};
    sn::sgxbridge::tp::registry registry;
    context.registry = &registry;
    context.host_cookie = host;
    sn::sgxbridge::enclave::execution_context_guard guard(context);
    sn::sgxbridge::enclave::threadpool_portal portal(&context);
    const auto provider = sn::sgxbridge::enclave::make_threadpool_provider(portal);
    sn::sgxbridge::tp::session eviction_session;
    if (!eviction_session.open(provider,
                               sn::sgxbridge::tp::make_request(1, 4, {}, "evict-bench")).succeeded())
      return 2;
    sn::threads::thread_team eviction_team(eviction_session.pool_ref(), 2);
    sn::threads::thread_team workers = eviction_team.limited_to(1);

    for (std::uint64_t i = 0; i < count; ++i) {
      measure_config(in, host, z, s, a, stash_bound, workers);
    }
    return 0;
  } catch (const std::exception& e) {
    sgx_status_t ignored;
    sonic_sgxbridge_log_sink(&ignored, host, e.what());
    return 1;
  }
}
