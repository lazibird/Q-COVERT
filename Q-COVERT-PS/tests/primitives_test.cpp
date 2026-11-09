#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

#include "ringdoram/obliv.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"

namespace {

struct record {
  std::uint64_t key = 0;
  std::uint64_t payload = 0;
};

static_assert(std::is_trivially_copyable_v<record>);

struct record_key {
  std::uint64_t operator()(const record& value) const noexcept { return value.key; }
};

ringdoram::obliv::secure_random make_random() {
  sn::crypto::prng::seed_material seed{};
  for (std::size_t i = 0; i < seed.key.size(); ++i) {
    seed.key[i] = static_cast<std::uint8_t>(i + 1);
  }
  for (std::size_t i = 0; i < seed.iv.size(); ++i) {
    seed.iv[i] = static_cast<std::uint8_t>(0xa0U + i);
  }
  return ringdoram::obliv::secure_random(seed);
}

void test_select_and_swap() {
  assert(ringdoram::obliv::OSelect<std::uint64_t>(7, 9, true) == 7);
  assert(ringdoram::obliv::OSelect<std::uint64_t>(7, 9, false) == 9);

  record a{1, 11};
  record b{2, 22};
  const auto chosen = ringdoram::obliv::OSelect(a, b, true);
  assert(chosen.key == 1 && chosen.payload == 11);
  ringdoram::obliv::OSwap(a, b, false);
  assert(a.key == 1 && b.key == 2);
  ringdoram::obliv::OSwap(a, b, true);
  assert(a.key == 2 && b.key == 1);
}

void test_sort() {
  std::span<std::uint64_t> empty;
  ringdoram::obliv::OSort<std::uint64_t>(empty);
  std::array<std::uint64_t, 1> singleton{{42}};
  ringdoram::obliv::OSort<std::uint64_t>(singleton);
  assert(singleton[0] == 42);

  std::array<record, 7> values{{{9, 0}, {2, 1}, {7, 2}, {2, 3}, {5, 4}, {1, 5}, {8, 6}}};
  ringdoram::obliv::OSort<record>(values, record_key{}, std::less<std::uint64_t>{});
  assert(std::is_sorted(values.begin(), values.end(), [](const record& lhs, const record& rhs) {
    return lhs.key < rhs.key;
  }));

  sn::threads::pthread_thread_pool pool(3);
  sn::threads::thread_team workers(pool.pool(), 4);
  std::array<std::uint64_t, 9> parallel{{8, 4, 3, 9, 1, 7, 6, 2, 5}};
  ringdoram::obliv::OSortParallel<std::uint64_t>(parallel, workers);
  assert(std::is_sorted(parallel.begin(), parallel.end()));
}

void test_compact() {
  std::array<std::uint64_t, 8> values{{10, 11, 12, 13, 14, 15, 16, 17}};
  const std::array<std::uint8_t, 8> marks{{0, 1, 0, 1, 1, 0, 0, 1}};
  ringdoram::obliv::compact_scratch scratch;
  ringdoram::obliv::OCompact<std::uint64_t>(values, marks, scratch);
  const std::array<std::uint64_t, 4> expected{{11, 13, 14, 17}};
  assert(std::equal(expected.begin(), expected.end(), values.begin()));

  sn::threads::pthread_thread_pool pool(3);
  sn::threads::thread_team workers(pool.pool(), 4);
  std::array<std::uint64_t, 9> parallel{{0, 1, 2, 3, 4, 5, 6, 7, 8}};
  const std::array<std::uint8_t, 9> parallel_marks{{1, 0, 1, 0, 0, 1, 1, 0, 1}};
  ringdoram::obliv::OCompactParallel<std::uint64_t>(parallel, parallel_marks, scratch, workers);
  const std::array<std::uint64_t, 5> parallel_expected{{0, 2, 5, 6, 8}};
  assert(std::equal(parallel_expected.begin(), parallel_expected.end(), parallel.begin()));

  bool rejected = false;
  try {
    ringdoram::obliv::OCompact<std::uint64_t>(
        std::span<std::uint64_t>(parallel), std::span<const std::uint8_t>(parallel_marks).first(8), scratch
    );
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
}

template <typename T> void assert_permutation(const std::vector<T>& values) {
  std::vector<T> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    assert(sorted[i] == static_cast<T>(i));
  }
}

void test_shuffle() {
  auto random = make_random();
  ringdoram::obliv::shuffle_scratch scratch;

  std::vector<std::uint64_t> values(16);
  std::iota(values.begin(), values.end(), 0);
  ringdoram::obliv::OShuffle<std::uint64_t>(values, scratch, random);
  assert_permutation(values);

  std::iota(values.begin(), values.end(), 0);
  ringdoram::obliv::OShuffleBitonic<std::uint64_t>(values, scratch, random);
  assert_permutation(values);

  sn::threads::pthread_thread_pool pool(3);
  sn::threads::thread_team workers(pool.pool(), 4);
  std::iota(values.begin(), values.end(), 0);
  ringdoram::obliv::OShuffleBitonicParallel<std::uint64_t>(values, scratch, random, workers);
  assert_permutation(values);
}

struct alignas(8) bucket_candidate {
  std::uint64_t leaf_node_id = 0;
  std::uint64_t bucket_id = 0;
};

void test_adjust_bucket_id() {
  constexpr std::uint32_t height = 3;
  constexpr std::uint32_t capacity = 2;
  constexpr std::uint64_t stash_id = 16;
  const std::array<std::uint64_t, 18> leaves{
      8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 10, 10, 11, 13, 14, 15
  };
  std::array<bucket_candidate, leaves.size()> values{};
  std::array<std::uint64_t, leaves.size()> expected_buckets{};
  std::array<std::uint32_t, stash_id> reference_occupancy{};
  std::size_t expected_stash = 0;
  for (std::size_t i = 0; i < values.size(); ++i) {
    values[i].leaf_node_id = leaves[i];
    std::uint64_t destination = stash_id;
    for (std::int64_t depth = height; depth >= 0; --depth) {
      const std::uint64_t node = leaves[i] >> (height - depth);
      if (reference_occupancy[node] < capacity) {
        ++reference_occupancy[node];
        destination = node;
        break;
      }
    }
    if (destination == stash_id) {
      ++expected_stash;
    }
    expected_buckets[i] = destination;
  }

  for (auto& item : values) {
    item.bucket_id = 0;
  }
  const auto actual_stash = ringdoram::obliv::OAdjustBucketID(
      std::span<bucket_candidate>(values), height, capacity, stash_id
  );
  assert(actual_stash == expected_stash);
  std::fill(reference_occupancy.begin(), reference_occupancy.end(), 0);
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto& item = values[i];
    assert(item.bucket_id == expected_buckets[i]);
    if (item.bucket_id != stash_id) {
      assert((item.leaf_node_id >> (height - (std::bit_width(item.bucket_id) - 1))) == item.bucket_id);
      ++reference_occupancy[item.bucket_id];
      assert(reference_occupancy[item.bucket_id] <= capacity);
    }
  }
}

struct alignas(8) placement_item {
  std::uint64_t target = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t value = 0;
};

void test_place() {
  constexpr std::uint64_t invalid = std::numeric_limits<std::uint64_t>::max();
  std::array<placement_item, 8> values{{
      {0, 10}, {2, 12}, {5, 15}, {7, 17}, {invalid, 100}, {invalid, 101}, {invalid, 102}, {invalid, 103}
  }};
  ringdoram::obliv::OPlace(std::span<placement_item>(values), [](const auto& x) { return x.target; }, invalid);
  for (const std::size_t target : {0U, 2U, 5U, 7U}) {
    assert(values[target].target == target);
    assert(values[target].value == target + 10);
  }
  for (const std::size_t target : {1U, 3U, 4U, 6U}) {
    assert(values[target].target == invalid);
  }

  // Exhaust every sparse placement pattern for a small network, including
  // the all-dummy and fully occupied cases.
  for (std::uint32_t mask = 0; mask < 256; ++mask) {
    std::array<placement_item, 8> pattern{};
    std::size_t used = 0;
    for (std::uint64_t target = 0; target < pattern.size(); ++target) {
      if ((mask & (1U << target)) != 0) {
        pattern[used++] = placement_item{target, target + 20};
      }
    }
    for (std::size_t i = used; i < pattern.size(); ++i) {
      pattern[i] = placement_item{invalid, 0};
    }
    ringdoram::obliv::OPlace(std::span<placement_item>(pattern), [](const auto& x) { return x.target; }, invalid);
    for (std::uint64_t target = 0; target < pattern.size(); ++target) {
      if ((mask & (1U << target)) != 0) {
        assert(pattern[target].target == target);
        assert(pattern[target].value == target + 20);
      } else {
        assert(pattern[target].target == invalid);
      }
    }
  }
}

} // namespace

int main() {
  test_select_and_swap();
  test_sort();
  test_compact();
  test_shuffle();
  test_adjust_bucket_id();
  test_place();
  return 0;
}
