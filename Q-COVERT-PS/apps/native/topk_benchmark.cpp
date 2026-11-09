#include "oblivious_topk/oblivious_topk.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

int main(int argc, char** argv) {
  const std::size_t n =
      argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10))
               : 1U << 20U;
  const std::size_t k =
      argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10))
               : 256;
  const std::size_t threads =
      argc > 3 ? static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10))
               : 0;
  if (k > n) {
    std::cerr << "k must not exceed n\n";
    return 2;
  }

  std::vector<std::uint64_t> keys(n);
  std::vector<std::size_t> values(n);
  std::mt19937_64 random(0xB17B17U);
  for (std::uint64_t& key : keys) {
    key = random();
  }
  std::iota(values.begin(), values.end(), 0);

  oblivious_topk::Options options;
  options.thread_count = threads;
  const auto begin = std::chrono::steady_clock::now();
  auto result = oblivious_topk::smallest_topk(keys, values, k, options);
  const auto end = std::chrono::steady_clock::now();
  const double milliseconds =
      std::chrono::duration<double, std::milli>(end - begin).count();

  std::cout << "n=" << n << " k=" << k << " threads=" << threads
            << " elapsed_ms=" << milliseconds << '\n';
  if (!result.first.empty()) {
    std::cout << "smallest=" << result.first.front()
              << " kth=" << result.first.back() << '\n';
  }
}
