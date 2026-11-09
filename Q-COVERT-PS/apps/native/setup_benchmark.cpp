#include <algorithm>
#include <bit>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../common/setup_support.hpp"
#include "ringdoram/oram.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"

namespace {

using clock_t = std::chrono::steady_clock;

// Default parameters match the standalone microbenchmark (Z=S=A=8, R=0, E=1,
// 64-byte blocks, one thread). Dataset runs override them with the configured
// ORAM profile (Z=32, S=58, A=46, R/E per layer, 128B Fu / 1280B Qu blocks).
struct config {
  std::uint64_t z = 8;
  std::uint64_t s = 8;
  std::uint64_t a = 8;
  std::uint64_t routing_depth = 0;
  std::uint64_t evict_batch = 1;
  std::uint64_t threads = 1;
  std::string label = "custom";
  unsigned paths = 7; // bulk, sequential, plain
};

double milliseconds(clock_t::time_point start, clock_t::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

std::uint64_t parse_integer(const char* value, bool allow_zero = false) {
  if (value[0] == '-') throw std::invalid_argument("expected a positive integer");
  errno = 0;
  char* end = nullptr;
  const auto result = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0' || errno == ERANGE || (!allow_zero && result == 0)) {
    throw std::invalid_argument("expected a positive integer");
  }
  return result;
}


template <std::size_t BlockBytes> struct runner {
  using traits_t = setup_benchmark::traits<BlockBytes>;
  using client_t = ringdoram::client<traits_t>;
  using block_t = typename traits_t::block_t;
  using record_t = ringdoram::bulk_record<BlockBytes>;

  static ringdoram::options make_options(std::uint64_t n, const config& cfg) {
    return setup_benchmark::options(n, cfg.z, cfg.s, cfg.a, cfg.routing_depth, cfg.evict_batch, cfg.threads);
  }

  static void verify_samples(client_t& client, const std::vector<record_t>& records, const char* path) {
    try {
      typename client_t::access_scratch scratch;
      client.configure_access_scratch(scratch);
      std::array<std::uint8_t, BlockBytes> input{};
      std::array<std::uint8_t, BlockBytes> output{};
      const auto samples = std::array<record_t, 3>{records.front(), records[records.size() / 2], records.back()};
      sn::crypto::buffered_prng<> random;
      for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto& record = samples[index];
        bool duplicate = false;
        for (std::size_t j = 0; j < index; ++j) duplicate |= samples[j].address == record.address;
        if (duplicate) continue;
        ringdoram::access_request request{
            .address = record.address,
            .cur_leaf = record.leaf,
            .new_leaf = static_cast<std::int64_t>(random.random_u64(0, client.shape().leaf_count)),
            .is_write = false,
            .in = sn::util::span<std::uint8_t>(input.data(), input.size()),
            .out = sn::util::span<std::uint8_t>(output.data(), output.size()),
        };
        client.access(request, scratch);
        if (output != record.data) throw std::runtime_error("post-setup sample read differs from input");
      }
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string(path) + ": " + error.what());
    }
  }


  struct row {
    std::uint64_t n = 0;
    std::uint64_t leaves = 0;
    std::uint64_t stash_bound = 0;
    double bulk_ms = std::numeric_limits<double>::quiet_NaN();
    double sequential_ms = std::numeric_limits<double>::quiet_NaN();
    double plain_ms = std::numeric_limits<double>::quiet_NaN();
  };

  static row measure(std::uint64_t n, const config& cfg) {
    if (n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::invalid_argument("N exceeds address range");
    }
    const auto opts = make_options(n, cfg);
    row result{.n = n};
    std::uint64_t leaves = 0;
    {
      sn::threads::pthread_thread_pool geometry_pool(0);
      sn::threads::thread_team geometry_workers(geometry_pool.pool(), 1);
      client_t geometry_client(opts, std::move(geometry_workers));
      leaves = geometry_client.shape().leaf_count;
      result.leaves = leaves;
      result.stash_bound = geometry_client.state_ref().derived().stash_bound;
    }
    std::vector<record_t> records(static_cast<std::size_t>(n));
    sn::crypto::buffered_prng<> random;
    for (std::size_t i = 0; i < records.size(); ++i) {
      records[i].address = static_cast<std::int64_t>(i);
      records[i].leaf = static_cast<std::int64_t>(random.random_u64(0, leaves));
      for (std::size_t j = 0; j < BlockBytes; ++j)
        records[i].data[j] = static_cast<std::uint8_t>(i >> ((j % 8) * 8));
    }
    if (cfg.paths & 1) {
      sn::threads::pthread_thread_pool pool(cfg.threads > 0 ? cfg.threads - 1 : 0);
      sn::threads::thread_team workers(pool.pool(), cfg.threads);
      const auto evict_threads = std::bit_floor(std::min<std::uint64_t>(std::uint64_t{1} << cfg.routing_depth,
                                                                        cfg.threads));
      client_t client(opts, workers.limited_to(evict_threads));
      const auto start = clock_t::now();
      client.bulk_setup(records);
      result.bulk_ms = milliseconds(start, clock_t::now());
      verify_samples(client, records, "bulk_setup");
    }
    if (cfg.paths & 2) {
      // Empty initialization folded into the sequential insert measurement.
      sn::threads::pthread_thread_pool pool(cfg.threads > 0 ? cfg.threads - 1 : 0);
      sn::threads::thread_team workers(pool.pool(), cfg.threads);
      const auto evict_threads = std::bit_floor(std::min<std::uint64_t>(std::uint64_t{1} << cfg.routing_depth,
                                                                        cfg.threads));
      client_t client(opts, workers.limited_to(evict_threads));
      const auto old_leaves = setup_benchmark::initial_leaves(records.size(), leaves);
      sn::threads::pthread_thread_pool access_pool(cfg.threads - 1);
      sn::threads::thread_team access_workers(access_pool.pool(), cfg.threads);
      const auto start = clock_t::now();
      client.initialize();
      const auto empty_end = clock_t::now();
      setup_benchmark::insert(client, std::span<record_t>(records), old_leaves, access_workers,
          [&](std::size_t done, std::size_t total) {
            std::cerr << "setup/sequential " << done << '/' << total << '\n';
          });
      const auto finish = clock_t::now();
      result.sequential_ms = milliseconds(start, finish);
      const auto stats = client.state_ref().metrics_snapshot();
      std::cerr << "setup/sonic empty_ms=" << milliseconds(start, empty_end)
                << " insert_ms=" << milliseconds(empty_end, finish)
                << " accesses=" << stats.access_ops << " evict_paths=" << stats.evict_ops << '\n';
      verify_samples(client, records, "sequential_setup");
    }
    if (cfg.paths & 4) {
      sn::threads::pthread_thread_pool pool(cfg.threads > 0 ? cfg.threads - 1 : 0);
      sn::threads::thread_team workers(pool.pool(), cfg.threads);
      const auto evict_threads = std::bit_floor(std::min<std::uint64_t>(std::uint64_t{1} << cfg.routing_depth,
                                                                        cfg.threads));
      client_t client(opts, workers.limited_to(evict_threads));
      const auto start = clock_t::now();
      setup_benchmark::plain_setup(client, std::span<const record_t>(records));
      result.plain_ms = milliseconds(start, clock_t::now());
      verify_samples(client, records, "plain_setup");
    }
    return result;
  }
};

void write_header(std::ostream& out) {
  out << "label,N,block_bytes,Z,S,A,routing_depth,evict_batch,threads,"
         "bulk_setup_ms,sequential_setup_ms,plain_bulk_setup_ms,leaves,stash_bound,repetitions,storage,epoch,eviction\n";
}

template <std::size_t BlockBytes>
int run_sizes(const config& cfg, std::uint64_t repetitions, const std::vector<std::uint64_t>& sizes,
              std::ostream* csv) {
  for (const auto n : sizes) {
    std::cerr << "setup benchmark: N=" << n << " block=" << BlockBytes << " Z=" << cfg.z << " S=" << cfg.s
              << " A=" << cfg.a << " R=" << cfg.routing_depth << " E=" << cfg.evict_batch
              << " T=" << cfg.threads << '\n';
    using run_t = runner<BlockBytes>;
    typename run_t::row total{};
    total.bulk_ms = total.sequential_ms = total.plain_ms = 0;
    for (std::uint64_t rep = 0; rep < repetitions; ++rep) {
      const auto result = run_t::measure(n, cfg);
      total.bulk_ms += result.bulk_ms;
      total.sequential_ms += result.sequential_ms;
      total.plain_ms += result.plain_ms;
      total.leaves = result.leaves;
      total.stash_bound = result.stash_bound;
    }
    const double scale = 1.0 / static_cast<double>(repetitions);
    std::ostream& out = csv ? *csv : std::cout;
    out << cfg.label << ',' << n << ',' << BlockBytes << ',' << cfg.z << ',' << cfg.s << ',' << cfg.a << ','
        << cfg.routing_depth << ',' << cfg.evict_batch << ',' << cfg.threads << ','
        << total.bulk_ms * scale << ',' << total.sequential_ms * scale << ',' << total.plain_ms * scale << ','
        << total.leaves << ',' << total.stash_bound << ',' << repetitions << ",slab,default,sonic\n";
    out.flush();
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  try {
    config cfg;
    std::uint64_t repetitions = 1;
    std::uint64_t block_bytes = 64;
    std::string csv_path;
    std::vector<std::uint64_t> sizes;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto take = [&](const char* name) -> const char* {
        if (i + 1 >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
        return argv[++i];
      };
      if (arg == "--repetitions") repetitions = parse_integer(take("--repetitions"));
      else if (arg == "--csv") csv_path = take("--csv");
      else if (arg == "--paths") {
        const std::string paths = take("--paths");
        cfg.paths = 0;
        std::size_t at = 0;
        do {
          const auto end = paths.find(',', at);
          const auto name = paths.substr(at, end == std::string::npos ? end : end - at);
          if (name == "bulk") cfg.paths |= 1;
          else if (name == "sequential") cfg.paths |= 2;
          else if (name == "plain") cfg.paths |= 4;
          else throw std::invalid_argument("--paths accepts bulk,sequential,plain");
          if (end == std::string::npos) break;
          at = end + 1;
        } while (at <= paths.size());
      }
      else if (arg == "--label") cfg.label = take("--label");
      else if (arg == "--z") cfg.z = parse_integer(take("--z"));
      else if (arg == "--s") cfg.s = parse_integer(take("--s"));
      else if (arg == "--a") cfg.a = parse_integer(take("--a"));
      else if (arg == "--routing-depth") cfg.routing_depth = parse_integer(take("--routing-depth"), true);
      else if (arg == "--evict-batch") cfg.evict_batch = parse_integer(take("--evict-batch"));
      else if (arg == "--threads") cfg.threads = parse_integer(take("--threads"));
      else if (arg == "--block-bytes") block_bytes = parse_integer(take("--block-bytes"));
      else if (arg == "--help") {
        std::cerr << "usage: ringdoram_setup_benchmark [--repetitions K] [--csv FILE] [--label L]\n"
                     "  [--z Z] [--s S] [--a A] [--routing-depth R] [--evict-batch E] [--threads T]\n"
                     "  [--paths bulk,sequential,plain] [--block-bytes 64|128|1280] N [N ...]\n";
        return 0;
      } else sizes.push_back(parse_integer(argv[i]));
    }
    if (sizes.empty()) {
      std::cerr << "usage: ringdoram_setup_benchmark [--repetitions K] [--csv FILE] [--label L] "
                   "[--z Z] [--s S] [--a A] [--routing-depth R] [--evict-batch E] [--threads T] "
                   "[--block-bytes B] N [N ...]\n";
      return 2;
    }
    if (cfg.label.empty() || cfg.label.find_first_of(",\r\n") != std::string::npos)
      throw std::invalid_argument("label must be nonempty without CSV separators");
    if (cfg.routing_depth >= 32) throw std::invalid_argument("routing depth exceeds subtree representation");
    std::ofstream csv;
    std::ostream* out = nullptr;
    if (!csv_path.empty()) {
      std::ifstream existing(csv_path);
      std::string old_header;
      const bool has_rows = static_cast<bool>(std::getline(existing, old_header));
      std::ostringstream expected;
      write_header(expected);
      if (has_rows && old_header + '\n' != expected.str())
        throw std::invalid_argument("setup CSV schema differs; use a new file");
      csv.open(csv_path, std::ios::app);
      if (!csv) throw std::runtime_error("cannot open CSV output");
      if (!has_rows) write_header(csv);
      out = &csv;
    } else {
      write_header(std::cout);
    }
    switch (block_bytes) {
      case 64: return run_sizes<64>(cfg, repetitions, sizes, out);
      case 128: return run_sizes<128>(cfg, repetitions, sizes, out);
      case 1280: return run_sizes<1280>(cfg, repetitions, sizes, out);
      default: throw std::invalid_argument("--block-bytes must be 64, 128 or 1280");
    }
  } catch (const std::exception& error) {
    std::cerr << "setup benchmark: " << error.what() << '\n';
    return 1;
  }
}
