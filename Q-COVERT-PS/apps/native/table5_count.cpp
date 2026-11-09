#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "ringdoram/oram.hpp"
#include "sonic/oram/stash/forestzing/pipeline.hpp"
#include "sonic/oram/zingoram/shared_top_eviction.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"

#if !defined(RINGDORAM_COUNT_SORT_COMPARATORS)
#error "Table 5 count target requires comparator instrumentation"
#endif

namespace {

using traits_t = ringdoram::traits<64>;
using state_t = sn::oram::zingoram::state<traits_t>;
using block_t = traits_t::block_t;
namespace sort = sn::sortshuffle::ser::bitonic;
namespace pipeline = sn::oram::stash::forestzing::pipeline;
namespace shared = sn::oram::zingoram::detail;

std::uint64_t parse_positive(const char* value) {
  if (value[0] == '-') throw std::invalid_argument("expected positive integer");
  char* end = nullptr;
  const auto result = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0' || result == 0) throw std::invalid_argument("expected positive integer");
  return result;
}

struct result {
  std::uint64_t n = 0;
  std::uint64_t e = 0;
  std::uint64_t height = 0;
  std::uint64_t cs = 0;
  std::uint64_t sonic_size = 0;
  std::uint64_t shared_size = 0;
  std::uint64_t top_size = 0;
  std::uint64_t sonic = 0;
  std::uint64_t qcovert = 0;
};

result count(std::uint64_t n, std::uint32_t e, std::uint64_t stash_bound) {
  ringdoram::options opts{};
  opts.block_count = n;
  opts.bucket_real_size = 8;
  opts.bucket_dummy_size = 8;
  opts.eviction_rate = 8;
  opts.routing_depth = 0;
  opts.evict_batch = e;
  opts.access_concurrency = 1;
  opts.stash_bound = stash_bound; // 0 = lambda=80 auto-derivation

  state_t sonic_state(opts);
  const auto& geom = sonic_state.shape();
  if (e > geom.leaf_count) throw std::invalid_argument("E exceeds leaf count");
  const std::size_t sonic_size = static_cast<std::size_t>(
      sonic_state.stash().state.geom.section_sizes.total());
  std::vector<block_t> sonic_items(sonic_size);
  const pipeline::target_coord_key_extractor<block_t> sonic_key(geom.subtree_height);
  sort::counting::reset();
  sort::bitonic_sort(sonic_items.data(), sonic_items.size(), sonic_key,
                     pipeline::target_coord_key_compare{});
  const std::uint64_t sonic_comparators = sort::counting::total;

  opts.evict_backend = ringdoram::eviction_backend::shared_top;
  state_t shared_state(opts);
  const std::uint64_t suffix_levels = geom.height - geom.overlap_depth + 1;
  const std::uint64_t suffix_slots = static_cast<std::uint64_t>(e) * suffix_levels * opts.bucket_real_size;
  const std::uint64_t top_slots = (static_cast<std::uint64_t>(e) - 1) * opts.bucket_real_size;
  const std::uint64_t cs = shared_state.derived().stash_bound;
  const std::uint64_t main_size = shared_state.stash().state.runtime.snapshot_span().size() + cs +
                                  top_slots + 2 * suffix_slots;
  const std::uint64_t top_size = std::bit_ceil(static_cast<std::size_t>(top_slots + cs));
  std::vector<shared::suffix_item<block_t>> main(static_cast<std::size_t>(main_size));
  std::vector<shared::top_item<block_t>> top(static_cast<std::size_t>(top_size));
  sort::counting::reset();
  auto main_key = [](const auto& item) { return item.key; };
  auto top_leaf_key = [](const auto& item) { return item.leaf_node_id; };
  auto top_bucket_key = [](const auto& item) { return item.bucket_id; };
  sort::bitonic_sort(main.data(), main.size(), main_key, shared::shared_top_u64_less{});
  sort::bitonic_sort(top.data(), top.size(), top_leaf_key, shared::shared_top_u64_less{});
  sort::bitonic_sort(top.data(), top.size(), top_bucket_key, shared::shared_top_u64_less{});

  return {.n = n, .e = e, .height = geom.height, .cs = cs,
          .sonic_size = sonic_size, .shared_size = main_size, .top_size = top_size,
          .sonic = sonic_comparators, .qcovert = sort::counting::total};
}

void write_row(std::ostream& out, const result& row) {
  const double reduction = 100.0 * (1.0 - static_cast<double>(row.qcovert) /
                                               static_cast<double>(row.sonic));
  out << row.n << ',' << row.e << ',' << row.height << ',' << row.cs << ','
      << row.sonic_size << ',' << row.shared_size << ',' << row.top_size << ','
      << row.sonic << ',' << row.qcovert << ',' << reduction << '\n';
}

} // namespace

int main(int argc, char** argv) {
  try {
    std::uint64_t max_n = 8388608;
    std::uint64_t stash_bound = 0;
    std::string csv_path;
    for (int index = 1; index < argc; ++index) {
      const std::string arg(argv[index]);
      if ((arg == "--max-n" || arg == "--csv" || arg == "--stash-bound") && index + 1 >= argc) {
        throw std::invalid_argument("missing option value");
      }
      if (arg == "--max-n") max_n = parse_positive(argv[++index]);
      else if (arg == "--csv") csv_path = argv[++index];
      else if (arg == "--stash-bound") stash_bound = parse_positive(argv[++index]);
      else throw std::invalid_argument(
          "usage: ringdoram_table5_count [--max-n N] [--csv FILE] [--stash-bound CS]");
    }
    const std::string header = "N,E,L,Cs,sonic_sort_items,qcovert_main_items,qcovert_top_items,"
                               "sonic_comparators,qcovert_comparators,reduction_percent\n";
    std::ofstream csv;
    if (!csv_path.empty()) {
      csv.open(csv_path, std::ios::trunc);
      if (!csv) throw std::runtime_error("cannot open CSV output");
      csv << header;
    }
    std::cout << header;
    for (const std::uint64_t n : {4096ULL, 65536ULL, 1048576ULL, 8388608ULL, 16777216ULL}) {
      if (n > max_n) continue;
      for (const std::uint32_t e : {4U, 8U, 32U, 64U}) {
        const result row = count(n, e, stash_bound);
        write_row(std::cout, row);
        if (csv) write_row(csv, row);
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "table5 count: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
