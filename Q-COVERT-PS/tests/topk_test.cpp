#include "oblivious_topk/oblivious_topk.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* expression) {
  if (!condition) {
    throw std::runtime_error(std::string("check failed: ") + expression);
  }
}

#define CHECK(expression) check((expression), #expression)

void verify_result(const std::vector<int>& input_keys,
                   const std::vector<std::size_t>& input_values,
                   std::size_t k,
                   const std::vector<int>& top_keys,
                   const std::vector<std::size_t>& top_values) {
  CHECK(top_keys.size() == k);
  CHECK(top_values.size() == k);
  CHECK(std::is_sorted(top_keys.begin(), top_keys.end()));

  std::vector<int> expected = input_keys;
  std::sort(expected.begin(), expected.end());
  expected.resize(k);
  CHECK(top_keys == expected);

  std::vector<bool> seen(input_keys.size(), false);
  for (std::size_t index = 0; index < k; ++index) {
    const std::size_t original = top_values[index];
    CHECK(original < input_values.size());
    CHECK(!seen[original]);
    CHECK(input_values[original] == original);
    CHECK(input_keys[original] == top_keys[index]);
    seen[original] = true;
  }
}

void test_invalid_arguments_and_edges() {
  std::vector<int> keys{3, 1};
  std::vector<int> short_values{0};
  bool mismatched = false;
  try {
    oblivious_topk::smallest_topk_inplace(keys, short_values, 1);
  } catch (const std::invalid_argument&) {
    mismatched = true;
  }
  CHECK(mismatched);

  std::vector<int> values{0, 1};
  bool oversized = false;
  try {
    oblivious_topk::smallest_topk(keys, values, 3);
  } catch (const std::invalid_argument&) {
    oversized = true;
  }
  CHECK(oversized);

  auto empty = oblivious_topk::smallest_topk(keys, values, 0);
  CHECK(empty.first.empty());
  CHECK(empty.second.empty());

  auto all = oblivious_topk::smallest_topk(keys, values, keys.size());
  CHECK((all.first == std::vector<int>{1, 3}));
  CHECK((all.second == std::vector<int>{1, 0}));
}

void test_all_small_n_and_k() {
  std::mt19937 random(0x5E1EC7U);
  for (std::size_t n = 1; n <= 129; ++n) {
    std::vector<int> keys(n);
    std::vector<std::size_t> values(n);
    for (int& key : keys) {
      key = static_cast<int>(random() % 23);
    }
    std::iota(values.begin(), values.end(), 0);

    for (std::size_t k = 1; k <= n; ++k) {
      oblivious_topk::Options options;
      options.thread_count = 1;
      auto result =
          oblivious_topk::smallest_topk(keys, values, k, options);
      verify_result(keys, values, k, result.first, result.second);
    }
  }
}

void test_parallel_large_cases() {
  std::mt19937 random(0xC0FFEEU);
  const std::vector<std::pair<std::size_t, std::size_t>> cases{
      {1000, 1}, {1000, 7}, {1000, 128}, {4099, 257}, {16387, 511}};
  for (const auto& [n, k] : cases) {
    std::vector<int> keys(n);
    std::vector<std::size_t> values(n);
    for (int& key : keys) {
      key = static_cast<int>(random() % 2001) - 1000;
    }
    std::iota(values.begin(), values.end(), 0);

    oblivious_topk::Options options;
    options.thread_count = 4;
    options.parallel_threshold = 0;
    auto result = oblivious_topk::smallest_topk(keys, values, k, options);
    verify_result(keys, values, k, result.first, result.second);
  }
}

struct UserKey {
  explicit UserKey(int input) : value(input) {}
  UserKey(const UserKey&) = default;
  UserKey& operator=(const UserKey&) = default;
  UserKey(UserKey&&) = default;
  UserKey& operator=(UserKey&&) = default;

  int value;
};

void test_custom_key_without_sentinel_or_default_constructor() {
  std::vector<UserKey> keys{UserKey(8), UserKey(-4), UserKey(3), UserKey(0)};
  std::vector<std::string> values{"eight", "minus four", "three", "zero"};
  auto result = oblivious_topk::smallest_topk(
      keys, values, 3, {}, [](const UserKey& left, const UserKey& right) {
        return left.value < right.value;
      });
  CHECK(result.first[0].value == -4);
  CHECK(result.first[1].value == 0);
  CHECK(result.first[2].value == 3);
  CHECK((result.second ==
         std::vector<std::string>{"minus four", "zero", "three"}));
}

struct CountingLess {
  std::size_t* calls;

  bool operator()(int left, int right) const {
    ++*calls;
    return left < right;
  }
};

void test_data_independent_comparison_count() {
  std::vector<int> ascending(37);
  std::iota(ascending.begin(), ascending.end(), 0);
  std::vector<int> descending = ascending;
  std::reverse(descending.begin(), descending.end());
  std::vector<int> values(37);
  std::iota(values.begin(), values.end(), 0);

  oblivious_topk::Options options;
  options.thread_count = 1;
  std::size_t ascending_calls = 0;
  std::size_t descending_calls = 0;
  oblivious_topk::smallest_topk(ascending, values, 5, options,
                                CountingLess{&ascending_calls});
  oblivious_topk::smallest_topk(descending, values, 5, options,
                                CountingLess{&descending_calls});
  CHECK(ascending_calls == descending_calls);

  // k=5 -> P=8; 37 records -> 8 padded blocks. Block sorting takes
  // 8*24 gates and the seven selection merges take 7*20 gates.
  CHECK(ascending_calls == 332);
}

}  // namespace

int main() {
  test_invalid_arguments_and_edges();
  test_all_small_n_and_k();
  test_parallel_large_cases();
  test_custom_key_without_sentinel_or_default_constructor();
  test_data_independent_comparison_count();
  std::cout << "all oblivious smallest-Top-K tests passed\n";
}
