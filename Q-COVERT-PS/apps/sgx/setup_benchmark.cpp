// In-enclave ORAM Setup benchmark: oblivious BulkOR bulk_setup and
// empty-initialize + ordinary Sonic writes, timed inside the enclave (SIM/HW).
// Request: u64 magic, config_count, z, s, a, evict_batch, threads, then per
// config {block_bytes, n, routing_depth, path_mask(bit0=bulk, bit1=sequential),
// label[64] zero-padded}. Each finished config×path streams one CSV row to the
// host report stream via the qcovert_report_write OCALL, so a stopped run
// still keeps every completed measurement.
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

#include "../common/setup_support.hpp"
#include "ringdoram/oram.hpp"
#include "ringdoram_sgx_t.h"
#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/sgxbridge/enclave/threadpool_portal.hpp"
#include "sonic/sgxbridge/enclave/threadpool_provider.hpp"
#include "sonic/sgxbridge/enclave/threadpool_registry.hpp"

namespace {

constexpr std::uint64_t request_magic = 0x5143554552545301ULL; // "QCUETS" tag
constexpr std::size_t header_words = 7;
constexpr std::size_t config_words = 12; // 4 scalars + 64-byte label

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

void emit_row(void* host, const char* label, const char* path, std::uint64_t n, std::uint64_t block_bytes,
              std::uint64_t z, std::uint64_t s, std::uint64_t a, std::uint64_t routing_depth,
              std::uint64_t evict_batch, std::uint64_t threads, std::uint64_t ns, std::uint64_t leaves,
              std::uint64_t stash_bound) {
  row_writer row;
  row.text(label);
  row.text(",sgx-" RINGDORAM_SETUP_SGX_MODE ",");
  row.text(path);
  const std::uint64_t fields[] = {n, block_bytes, z, s, a, routing_depth, evict_batch, threads};
  for (auto f : fields) {
    row.buf[row.len++] = ',';
    row.u64(f);
  }
  row.buf[row.len++] = ',';
  row.ms(ns);
  row.buf[row.len++] = ',';
  row.u64(leaves);
  row.buf[row.len++] = ',';
  row.u64(stash_bound);
  row.text(",slab,default,sonic\n");
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
    if (pos + 8 > size) throw std::invalid_argument("truncated setup request");
    std::uint64_t v;
    std::memcpy(&v, p + pos, 8);
    pos += 8;
    return v;
  }
  void label(char* out, std::size_t capacity) {
    if (pos + 64 > size) throw std::invalid_argument("truncated setup label");
    std::size_t n = 0;
    while (n < 64 && p[pos + n]) ++n;
    if (n >= capacity) throw std::invalid_argument("setup label too long");
    std::memcpy(out, p + pos, n);
    out[n] = 0;
    pos += 64;
  }
};

template <std::size_t BlockBytes>
void measure_config(reader& in, void* host, std::uint64_t z, std::uint64_t s, std::uint64_t a,
                    std::uint64_t evict_batch, std::uint64_t threads, sn::threads::thread_team& workers,
                    sn::threads::thread_team& eviction_workers) {
  using traits_t = setup_benchmark::traits<BlockBytes>;
  using client_t = ringdoram::client<traits_t>;
  using record_t = ringdoram::bulk_record<BlockBytes>;

  const std::uint64_t n = in.get();
  const std::uint64_t routing_depth = in.get();
  const std::uint64_t path_mask = in.get();
  char label[65];
  in.label(label, sizeof(label));
  if (!n || n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) || !z || !s || !a ||
      !evict_batch || routing_depth >= 32 || !threads || (path_mask == 0 || path_mask > 3) || !label[0])
    throw std::invalid_argument("invalid setup config");

  const auto opts = setup_benchmark::options(n, z, s, a, routing_depth, evict_batch, threads);

  std::uint64_t leaves = 0, stash_bound = 0;
  {
    sn::threads::thread_team one(workers.pool(), 1);
    client_t geometry_client(opts, std::move(one));
    leaves = geometry_client.shape().leaf_count;
    stash_bound = geometry_client.state_ref().derived().stash_bound;
  }
  // The client constructor requires subtree_count (2^R) to be divisible by the
  // eviction worker count; mirror qcovert's eviction_team and cap it, while
  // access batches use the full team.
  const auto evict_threads =
      std::bit_floor(std::min<std::uint64_t>(std::uint64_t{1} << routing_depth, threads));
  std::vector<record_t> records(static_cast<std::size_t>(n));
  sn::crypto::buffered_prng<> random;
  for (std::size_t i = 0; i < records.size(); ++i) {
    records[i].address = static_cast<std::int64_t>(i);
    records[i].leaf = static_cast<std::int64_t>(random.random_u64(0, leaves));
    for (std::size_t j = 0; j < BlockBytes; ++j)
      records[i].data[j] = static_cast<std::uint8_t>(i >> ((j % 8) * 8));
  }

  auto verify = [&](client_t& client, const char* path) {
    typename client_t::access_scratch scratch;
    client.configure_access_scratch(scratch);
    std::array<std::uint8_t, BlockBytes> input{};
    std::array<std::uint8_t, BlockBytes> output{};
    const auto samples = std::array<record_t, 3>{records.front(), records[records.size() / 2], records.back()};
    for (std::size_t index = 0; index < samples.size(); ++index) {
      const auto& record = samples[index];
      bool duplicate = false;
      for (std::size_t j = 0; j < index; ++j) duplicate |= samples[j].address == record.address;
      if (duplicate) continue;
      ringdoram::access_request request{
          .address = record.address,
          .cur_leaf = record.leaf,
          .new_leaf = static_cast<std::int64_t>(random.random_u64(0, leaves)),
          .is_write = false,
          .in = sn::util::span<std::uint8_t>(input.data(), input.size()),
          .out = sn::util::span<std::uint8_t>(output.data(), output.size()),
      };
      client.access(request, scratch);
      if (output != record.data)
        throw std::runtime_error(path);
    }
  };

  if (path_mask & 1) {
    char stage[96];
    std::snprintf(stage, sizeof(stage), "setup/%s/bulk", label);
    report_progress(host, stage, 0, 1);
    client_t client(opts, eviction_workers.limited_to(evict_threads));
    const auto start = benchmark_clock();
    client.bulk_setup(records);
    const std::uint64_t bulk_ns = benchmark_clock() - start;
    verify(client, "bulk_setup: sample read mismatch");
    emit_row(host, label, "bulk", n, BlockBytes, z, s, a, routing_depth, evict_batch, threads, bulk_ns,
             leaves, stash_bound);
    report_progress(host, stage, 1, 1);
  }
  if (path_mask & 2) {
    char stage[96];
    std::snprintf(stage, sizeof(stage), "setup/%s/sequential", label);
    report_progress(host, stage, 0, 1);
    const auto old_leaves = setup_benchmark::initial_leaves(records.size(), leaves);
    client_t client(opts, eviction_workers.limited_to(evict_threads));
    const auto start = benchmark_clock();
    client.initialize();
    const auto empty_end = benchmark_clock();
    setup_benchmark::insert(client, std::span<record_t>(records), old_leaves, workers,
        [&](std::size_t done, std::size_t total) { report_progress(host, stage, done, total); });
    const auto finish = benchmark_clock();
    const auto sequential_ns = finish - start;
    const auto stats = client.state_ref().metrics_snapshot();
    char note[256];
    std::snprintf(note, sizeof(note),
        "setup/%s: empty_ns=%llu insert_ns=%llu accesses=%llu evict_paths=%llu", label,
        static_cast<unsigned long long>(empty_end - start),
        static_cast<unsigned long long>(finish - empty_end),
        static_cast<unsigned long long>(stats.access_ops), static_cast<unsigned long long>(stats.evict_ops));
    report_progress(host, note, records.size(), records.size());
    verify(client, "sequential_setup: sample read mismatch");
    emit_row(host, label, "sequential", n, BlockBytes, z, s, a, routing_depth, evict_batch, threads,
             sequential_ns, leaves, stash_bound);
    report_progress(host, stage, records.size(), records.size());
  }
}

} // namespace

extern "C" std::uint32_t qcovert_sgx_setup(void* host, const std::uint8_t* bytes, std::size_t size,
                                           std::uint8_t* output, std::size_t capacity) {
  try {
    (void)output;
    (void)capacity;
    reader in{bytes, size};
    if (in.get() != request_magic) return 2;
    const std::uint64_t count = in.get();
    const std::uint64_t z = in.get(), s = in.get(), a = in.get(), evict_batch = in.get(), threads = in.get();
    if (!count || count > 64 || !z || !s || !a || !evict_batch || !threads || threads > 256 ||
        in.pos + count * config_words * 8 != size)
      return 2;

    // Validate all configurations before starting pools and avoid reserving
    // idle eviction workers when R is small.
    std::uint64_t max_eviction_threads = 1;
    auto configs = in;
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto block = configs.get(), n = configs.get(), r = configs.get(), paths = configs.get();
      char label[65];
      configs.label(label, sizeof(label));
      (void)setup_benchmark::options(n, z, s, a, r, evict_batch, threads);
      if ((block != 64 && block != 128 && block != 1280) || !paths || paths > 3 || !label[0] ||
          std::strpbrk(label, ",\r\n")) return 2;
      max_eviction_threads = std::max(max_eviction_threads,
          std::bit_floor(std::min(std::uint64_t{1} << r, threads)));
    }
    sn::sgxbridge::common::enclave_execution_context context{};
    sn::sgxbridge::tp::registry registry;
    context.registry = &registry;
    context.host_cookie = host;
    sn::sgxbridge::enclave::execution_context_guard guard(context);
    sn::sgxbridge::enclave::threadpool_portal portal(&context);
    const auto provider = sn::sgxbridge::enclave::make_threadpool_provider(portal);
    sn::sgxbridge::tp::session session;
    if (!session
             .open(provider,
                   sn::sgxbridge::tp::make_request(threads - 1, threads * 2, {}, "qcovert-setup"))
             .succeeded())
      return 2;
    sn::sgxbridge::tp::session eviction_session;
    if (!eviction_session.open(provider, sn::sgxbridge::tp::make_request(
            max_eviction_threads - 1, max_eviction_threads * 2, {}, "setup-eviction")).succeeded())
      return 2;
    sn::threads::thread_team eviction_team(eviction_session.pool_ref(), max_eviction_threads);
    sn::threads::thread_team team(session.pool_ref(), threads);

    for (std::uint64_t i = 0; i < count; ++i) {
      const std::uint64_t block_bytes = in.get();
      switch (block_bytes) {
        case 64:
          measure_config<64>(in, host, z, s, a, evict_batch, threads, team, eviction_team);
          break;
        case 128:
          measure_config<128>(in, host, z, s, a, evict_batch, threads, team, eviction_team);
          break;
        case 1280:
          measure_config<1280>(in, host, z, s, a, evict_batch, threads, team, eviction_team);
          break;
        default:
          return 2;
      }
    }
    return 0;
  } catch (const std::exception& e) {
    sgx_status_t ignored;
    sonic_sgxbridge_log_sink(&ignored, host, e.what());
    return 1;
  }
}
