#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace oblivious_topk {

struct Options {
  // Zero selects a workload-aware count bounded by
  // std::thread::hardware_concurrency(). The caller is included in this count,
  // so one selects the sequential path.
  std::size_t thread_count = 0;

  // Avoid synchronization overhead for small padded inputs.
  std::size_t parallel_threshold = 1U << 14U;
};

namespace detail {

inline std::size_t next_power_of_two(std::size_t value) {
  if (value <= 1) {
    return value;
  }
  std::size_t result = 1;
  while (result < value) {
    if (result > std::numeric_limits<std::size_t>::max() / 2) {
      throw std::length_error("input is too large for a Top-K network");
    }
    result *= 2;
  }
  return result;
}

inline std::size_t checked_multiply(std::size_t left, std::size_t right) {
  if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::length_error("padded Top-K workspace is too large");
  }
  return left * right;
}

// A reusable thread team is important because sorting/selection networks have
// many short stages separated by barriers. Work is statically partitioned, so
// scheduling and array ranges depend only on public n and k.
class ParallelExecutor {
 public:
  explicit ParallelExecutor(std::size_t thread_count)
      : thread_count_(std::max<std::size_t>(thread_count, 1)) {
    workers_.reserve(thread_count_ - 1);
    try {
      for (std::size_t worker = 1; worker < thread_count_; ++worker) {
        workers_.emplace_back([this, worker] { worker_loop(worker); });
      }
    } catch (...) {
      stop_and_join();
      throw;
    }
  }

  ParallelExecutor(const ParallelExecutor&) = delete;
  ParallelExecutor& operator=(const ParallelExecutor&) = delete;

  ~ParallelExecutor() { stop_and_join(); }

  template <class Function>
  void run(std::size_t count, Function&& function) {
    if (workers_.empty()) {
      function(0, count);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_ = std::forward<Function>(function);
      task_size_ = count;
      unfinished_workers_ = workers_.size();
      exception_ = nullptr;
      ++generation_;
    }
    start_cv_.notify_all();

    try {
      task_(0, count / thread_count_);
    } catch (...) {
      record_exception(std::current_exception());
    }

    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock, [this] { return unfinished_workers_ == 0; });
    std::exception_ptr exception = exception_;
    lock.unlock();
    if (exception) {
      std::rethrow_exception(exception);
    }
  }

 private:
  void worker_loop(std::size_t worker_index) {
    std::size_t observed_generation = 0;
    for (;;) {
      std::unique_lock<std::mutex> lock(mutex_);
      start_cv_.wait(lock, [this, observed_generation] {
        return stopping_ || generation_ != observed_generation;
      });
      if (stopping_) {
        return;
      }

      observed_generation = generation_;
      const std::size_t begin = task_size_ * worker_index / thread_count_;
      const std::size_t end =
          task_size_ * (worker_index + 1) / thread_count_;
      auto task = task_;
      lock.unlock();

      try {
        task(begin, end);
      } catch (...) {
        record_exception(std::current_exception());
      }

      lock.lock();
      if (--unfinished_workers_ == 0) {
        done_cv_.notify_one();
      }
    }
  }

  void record_exception(std::exception_ptr exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!exception_) {
      exception_ = std::move(exception);
    }
  }

  void stop_and_join() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      ++generation_;
    }
    start_cv_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  const std::size_t thread_count_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable start_cv_;
  std::condition_variable done_cv_;
  std::function<void(std::size_t, std::size_t)> task_;
  std::size_t task_size_ = 0;
  std::size_t unfinished_workers_ = 0;
  std::size_t generation_ = 0;
  std::exception_ptr exception_;
  bool stopping_ = false;
};

inline std::size_t select_thread_count(const Options& options,
                                       std::size_t padded_size) {
  if (padded_size < options.parallel_threshold) {
    return 1;
  }
  std::size_t count = options.thread_count;
  if (count == 0) {
    const std::size_t hardware = std::thread::hardware_concurrency();
    constexpr std::size_t records_per_worker = 1U << 17U;
    const std::size_t useful_workers =
        padded_size / records_per_worker +
        (padded_size % records_per_worker != 0);
    count = std::min(hardware, std::max<std::size_t>(useful_workers, 1));
  }
  if (count == 0) {
    count = 1;
  }
  return std::max<std::size_t>(1, std::min(count, padded_size / 2));
}

}  // namespace detail

// Selects the k smallest records, sorts them by key in ascending order, and
// applies the same fixed-address transformations to values. The compared index
// sequence and array address trace depend only on n and k, not on key data.
//
// Key and Value must be copy constructible and move assignable. Compare must
// provide a strict weak ordering and be safe for concurrent calls when more
// than one thread is used.
template <class Key, class Value, class Compare = std::less<Key>>
void smallest_topk_inplace(std::vector<Key>& keys,
                           std::vector<Value>& values,
                           std::size_t k,
                           Options options = {},
                           Compare compare = Compare{}) {
  if (keys.size() != values.size()) {
    throw std::invalid_argument("keys and values must have the same size");
  }
  if (k > keys.size()) {
    throw std::invalid_argument("k must not exceed the input size");
  }
  if (k == 0) {
    keys.clear();
    values.clear();
    return;
  }

  static_assert(std::is_copy_constructible<Key>::value,
                "Key must be copy constructible");
  static_assert(std::is_copy_constructible<Value>::value,
                "Value must be copy constructible");

  const std::size_t input_size = keys.size();
  const std::size_t block_size = detail::next_power_of_two(k);
  const std::size_t input_block_count =
      input_size / block_size + (input_size % block_size != 0);
  const std::size_t block_count =
      detail::next_power_of_two(input_block_count);
  const std::size_t padded_size =
      detail::checked_multiply(block_count, block_size);

  // Invalid records are ordered after valid records, so no maximum-key
  // sentinel or default constructor is required.
  keys.resize(padded_size, keys.front());
  values.resize(padded_size, values.front());
  std::vector<std::uint8_t> valid(padded_size, 0);
  for (std::size_t index = 0; index < input_size; ++index) {
    valid[index] = 1;
  }

  detail::ParallelExecutor executor(
      detail::select_thread_count(options, padded_size));

  auto item_less = [&](std::size_t left, std::size_t right) {
    // The key comparator is invoked even for padding records to keep the
    // comparator call schedule independent of data and validity placement.
    const bool key_less = compare(keys[left], keys[right]);
    if (valid[left] != valid[right]) {
      return valid[left] > valid[right];
    }
    return key_less;
  };

  auto compare_exchange = [&](std::size_t left, std::size_t right,
                              bool ascending) {
    const bool exchange =
        ascending ? item_less(right, left) : item_less(left, right);

    // Both records are loaded and both array locations are written for every
    // gate, including when their logical order does not change.
    Key left_key = std::move(keys[left]);
    Key right_key = std::move(keys[right]);
    Value left_value = std::move(values[left]);
    Value right_value = std::move(values[right]);
    const std::uint8_t left_valid = valid[left];
    const std::uint8_t right_valid = valid[right];

    if (exchange) {
      keys[left] = std::move(right_key);
      keys[right] = std::move(left_key);
      values[left] = std::move(right_value);
      values[right] = std::move(left_value);
      valid[left] = right_valid;
      valid[right] = left_valid;
    } else {
      keys[left] = std::move(left_key);
      keys[right] = std::move(right_key);
      values[left] = std::move(left_value);
      values[right] = std::move(right_value);
      valid[left] = left_valid;
      valid[right] = right_valid;
    }
  };

  auto keep_left_minimum = [&](std::size_t left, std::size_t right) {
    const bool take_right = item_less(right, left);
    Key left_key = std::move(keys[left]);
    Key right_key = std::move(keys[right]);
    Value left_value = std::move(values[left]);
    Value right_value = std::move(values[right]);
    const std::uint8_t left_valid = valid[left];
    const std::uint8_t right_valid = valid[right];

    if (take_right) {
      keys[left] = std::move(right_key);
      values[left] = std::move(right_value);
      valid[left] = right_valid;
    } else {
      keys[left] = std::move(left_key);
      values[left] = std::move(left_value);
      valid[left] = left_valid;
    }

    // Keep the complementary maximum on the right. Both array locations
    // receive a store regardless of the comparison result.
    keys[right] = std::move(take_right ? left_key : right_key);
    values[right] = std::move(take_right ? left_value : right_value);
    valid[right] = take_right ? left_valid : right_valid;
  };

  // Phase 1: independently sort blocks of P=ceil_pow2(k). This is the only
  // O(n log^2(k)) phase; it never sorts the complete n-element input.
  if (block_size > 1) {
    const std::size_t pairs_per_block = block_size / 2;
    const std::size_t all_block_pairs = block_count * pairs_per_block;
    for (std::size_t sequence_size = 2;; sequence_size *= 2) {
      for (std::size_t distance = sequence_size / 2; distance != 0;
           distance /= 2) {
        executor.run(all_block_pairs,
                     [&](std::size_t begin, std::size_t end) {
          for (std::size_t pair = begin; pair < end; ++pair) {
            const std::size_t block = pair / pairs_per_block;
            const std::size_t local_pair = pair % pairs_per_block;
            const std::size_t low = local_pair & (distance - 1);
            const std::size_t local_left =
                ((local_pair - low) * 2) + low;
            const std::size_t left = block * block_size + local_left;
            const std::size_t right = left + distance;
            const bool ascending = (local_left & sequence_size) == 0;
            compare_exchange(left, right, ascending);
          }
        });
      }
      if (sequence_size == block_size) {
        break;
      }
    }
  }

  // Phase 2: reduce pairs of sorted candidate blocks. Comparing A[i] with
  // B[P-1-i] creates the lower half of a bitonic merge; sorting just that half
  // retains the P smallest records and discards the rest.
  std::size_t active_block_count = block_count;
  std::size_t block_stride = 1;
  while (active_block_count > 1) {
    const std::size_t merge_count = active_block_count / 2;
    executor.run(merge_count * block_size,
                 [&](std::size_t begin, std::size_t end) {
      for (std::size_t item = begin; item < end; ++item) {
        const std::size_t merge = item / block_size;
        const std::size_t local = item % block_size;
        const std::size_t left_block = 2 * merge * block_stride;
        const std::size_t right_block = left_block + block_stride;
        const std::size_t left = left_block * block_size + local;
        const std::size_t right =
            right_block * block_size + (block_size - 1 - local);
        keep_left_minimum(left, right);
      }
    });

    if (block_size > 1) {
      const std::size_t pairs_per_merge = block_size / 2;
      for (std::size_t distance = block_size / 2; distance != 0;
           distance /= 2) {
        executor.run(merge_count * pairs_per_merge,
                     [&](std::size_t begin, std::size_t end) {
          for (std::size_t pair = begin; pair < end; ++pair) {
            const std::size_t merge = pair / pairs_per_merge;
            const std::size_t local_pair = pair % pairs_per_merge;
            const std::size_t low = local_pair & (distance - 1);
            const std::size_t local_left =
                ((local_pair - low) * 2) + low;
            const std::size_t left_block = 2 * merge * block_stride;
            const std::size_t left =
                left_block * block_size + local_left;
            compare_exchange(left, left + distance, true);
          }
        });
      }
    }

    active_block_count /= 2;
    block_stride *= 2;
  }

  keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(k), keys.end());
  values.erase(values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
}

// Copying convenience API:
// auto [top_keys, top_values] = smallest_topk(keys, values, k);
template <class Key, class Value, class Compare = std::less<Key>>
std::pair<std::vector<Key>, std::vector<Value>> smallest_topk(
    const std::vector<Key>& keys,
    const std::vector<Value>& values,
    std::size_t k,
    Options options = {},
    Compare compare = Compare{}) {
  std::vector<Key> top_keys = keys;
  std::vector<Value> top_values = values;
  smallest_topk_inplace(top_keys, top_values, k, options,
                        std::move(compare));
  return {std::move(top_keys), std::move(top_values)};
}

}  // namespace oblivious_topk
