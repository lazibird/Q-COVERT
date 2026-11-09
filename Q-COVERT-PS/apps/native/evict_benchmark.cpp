#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/resource.h>
#include <vector>

#include "ringdoram/oram.hpp"
#include "sonic/oram/zingoram/eviction.hpp"
#include "sonic/oram/zingoram/schedule.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"
#include "sonic/sortshuffle/ser/orshuffle.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"

namespace {

constexpr std::size_t block_bytes = 64;
using traits_t = ringdoram::traits<block_bytes>;
using client_t = ringdoram::client<traits_t>;
using record_t = ringdoram::bulk_record<block_bytes>;
using timer = std::chrono::steady_clock;

std::uint64_t parse(const char* arg) {
  char* end = nullptr;
  const std::uint64_t value = std::strtoull(arg, &end, 10);
  if (end == arg || *end != '\0') {
    throw std::invalid_argument("expected unsigned integer");
  }
  return value;
}

std::uint64_t step(std::uint64_t& state) {
  state ^= state << 13U;
  state ^= state >> 7U;
  state ^= state << 17U;
  return state;
}

double ms(timer::time_point start, timer::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

double p95(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[((values.size() * 95U + 99U) / 100U) - 1U];
}

void print_samples(std::string_view name, const std::vector<double>& samples) {
  std::cout << name << '=';
  for (std::size_t index = 0; index < samples.size(); ++index) {
    std::cout << (index == 0 ? "" : ",") << std::fixed << std::setprecision(3) << samples[index];
  }
  std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "usage: ringdoram_evict_{benchmark|count} N E sonic|shared repetitions"
                 " [--pure-only] [--stash-bound CS]\n";
    return 2;
  }
  bool pure_only = false;
  std::uint64_t stash_bound = 0;
  for (int index = 5; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--pure-only") {
      pure_only = true;
    } else if (arg == "--stash-bound" && index + 1 < argc) {
      stash_bound = parse(argv[++index]);
    } else {
      std::cerr << "usage: ringdoram_evict_{benchmark|count} N E sonic|shared repetitions"
                   " [--pure-only] [--stash-bound CS]\n";
      return 2;
    }
  }
  const std::uint64_t N = parse(argv[1]);
  const std::uint32_t E = static_cast<std::uint32_t>(parse(argv[2]));
  const std::string_view backend_name(argv[3]);
  const std::size_t repetitions = static_cast<std::size_t>(parse(argv[4]));
  if (N == 0 || E == 0 || repetitions == 0 || (backend_name != "sonic" && backend_name != "shared")) {
    throw std::invalid_argument("invalid benchmark arguments");
  }

  ringdoram::options opts{};
  opts.block_count = N;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 8;
  opts.eviction_rate = 8;
  opts.routing_depth = 0;
  opts.evict_batch = E;
  opts.access_concurrency = 1;
  opts.evict_backend = backend_name == "shared" ? ringdoram::eviction_backend::shared_top :
                                                  ringdoram::eviction_backend::sonic;
  opts.stash_bound = stash_bound;

  sn::threads::pthread_thread_pool pool(0);
  sn::threads::thread_team client_workers(pool.pool(), 1);
  client_t oram(opts, std::move(client_workers));
  sn::threads::thread_team direct_workers(pool.pool(), 1);
  const std::uint64_t leaves = oram.shape().leaf_count;
  const std::size_t loaded = static_cast<std::size_t>(std::min<std::uint64_t>(N, 4096));
  std::vector<record_t> records(loaded);
  std::vector<std::uint64_t> position(loaded);
  std::uint64_t random = 0x243f6a8885a308d3ULL;
  for (std::size_t index = 0; index < loaded; ++index) {
    auto& record = records[index];
    record.address = static_cast<std::int64_t>(index);
    record.leaf = static_cast<std::int64_t>(step(random) % leaves);
    record.data.fill(static_cast<std::uint8_t>(index + 1));
    position[index] = static_cast<std::uint64_t>(record.leaf);
  }
  oram.bulk_setup(records);
  sn::oram::zingoram::schedule direct_schedule(oram.state_ref().topology(), E);

  std::cout << "backend=" << backend_name << " N=" << N << " d=" << loaded << " E=" << E
            << " Z=8 A=8 S=8 B=64 R=0 T=1 L=" << oram.shape().height
            << " Cs=" << oram.state_ref().derived().stash_bound
            << " sizeof_block=" << sizeof(client_t::block_t) << " repetitions=" << repetitions << '\n';

#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
  sn::sortshuffle::ser::bitonic::counting::reset();
  sn::sortshuffle::ser::orshuffle::counting::reset();
  sn::oram::zingoram::evict<traits_t>(oram.state_ref(), direct_schedule, direct_workers);
  const auto main_sort = sn::sortshuffle::ser::bitonic::counting::eviction_main;
  const auto top_sort = sn::sortshuffle::ser::bitonic::counting::eviction_top;
  const auto bucket_shuffle = sn::sortshuffle::ser::bitonic::counting::bucket_shuffle;
  const auto osort = main_sort + top_sort;
  const auto total = sn::sortshuffle::ser::bitonic::counting::total;
  std::cout << "evict_osort_comparators=" << osort << " evict_all_sort_comparators=" << total
            << " main_sort_comparators=" << main_sort << " top_sort_comparators=" << top_sort
            << " bucket_shuffle_comparators=" << bucket_shuffle
            << " other_sort_comparators=" << total - osort - bucket_shuffle
            << " compact_conditional_swaps="
            << sn::sortshuffle::ser::orshuffle::counting::compact_conditional_swaps << '\n';
#else
  if (pure_only) {
    // Exclude the first eviction after BulkLoad from the timed samples.
    sn::oram::zingoram::evict<traits_t>(oram.state_ref(), direct_schedule, direct_workers);
  }
  std::vector<double> pure_times;
  pure_times.reserve(repetitions);
  for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
    const auto start = timer::now();
    sn::oram::zingoram::evict<traits_t>(oram.state_ref(), direct_schedule, direct_workers);
    pure_times.push_back(ms(start, timer::now()));
  }
  if (pure_only) {
    std::cout << std::fixed << std::setprecision(6)
              << "pure_evict_median_ms=" << median(pure_times)
              << " pure_evict_p95_ms=" << p95(pure_times) << '\n';
    return 0;
  }

  client_t::access_scratch scratch;
  oram.configure_access_scratch(scratch);
  std::array<std::uint8_t, block_bytes> input{};
  std::array<std::uint8_t, block_bytes> output{};
  const std::size_t accesses_per_evict = static_cast<std::size_t>(8U * E);
  if (accesses_per_evict > loaded) {
    throw std::invalid_argument("loaded sample too small for one distinct-address eviction epoch");
  }
  std::vector<double> epoch_times;
  epoch_times.reserve(repetitions);
  for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
    const auto start = timer::now();
    for (std::size_t step_index = 0; step_index < accesses_per_evict; ++step_index) {
      const std::size_t address = (repetition * accesses_per_evict + step_index) % loaded;
      const std::uint64_t next_leaf = step(random) % leaves;
      ringdoram::access_request request{
          .address = static_cast<std::int64_t>(address),
          .cur_leaf = static_cast<std::int64_t>(position[address]),
          .new_leaf = static_cast<std::int64_t>(next_leaf),
          .is_write = false,
          .in = sn::util::span<std::uint8_t>(input.data(), input.size()),
          .out = sn::util::span<std::uint8_t>(output.data(), output.size()),
      };
      oram.access(request, scratch);
      position[address] = next_leaf;
      if (output != records[address].data) {
        throw std::runtime_error("benchmark access returned wrong data");
      }
    }
    epoch_times.push_back(ms(start, timer::now()));
  }
  struct rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  std::cout << std::fixed << std::setprecision(3)
            << "pure_evict_median_ms=" << median(pure_times) << " pure_evict_p95_ms=" << p95(pure_times)
            << " epoch_access_plus_evict_median_ms=" << median(epoch_times)
            << " epoch_access_plus_evict_p95_ms=" << p95(epoch_times)
            << " peak_rss_kb=" << usage.ru_maxrss << '\n';
  print_samples("pure_evict_samples_ms", pure_times);
  print_samples("epoch_access_plus_evict_samples_ms", epoch_times);
#endif
  return 0;
}
