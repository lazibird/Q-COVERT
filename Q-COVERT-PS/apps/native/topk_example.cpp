#include "oblivious_topk/oblivious_topk.hpp"

#include <iostream>
#include <string>
#include <vector>

int main() {
  const std::vector<int> keys{8, 3, 5, 1, 7, 2};
  const std::vector<std::string> values{"eight", "three", "five",
                                        "one", "seven", "two"};

  oblivious_topk::Options options;
  options.thread_count = 0;  // Choose threads from input size and CPU cores.
  auto [top_keys, top_values] =
      oblivious_topk::smallest_topk(keys, values, 3, options);

  for (std::size_t index = 0; index < top_keys.size(); ++index) {
    std::cout << top_keys[index] << ": " << top_values[index] << '\n';
  }
}
