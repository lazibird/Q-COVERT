#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <vector>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/util/log.hpp"

namespace sn::obliv {

namespace detail {

struct bitset_constants {
  static constexpr std::size_t word_bits = 64;
  static constexpr std::size_t word_shift = 6;
  static constexpr std::size_t word_mask = word_bits - 1;

  static constexpr std::size_t words_for_bits(std::size_t bit_count) noexcept {
    return (bit_count + word_mask) >> word_shift;
  }

  static constexpr std::uint64_t last_word_mask(std::size_t bit_count, std::size_t word_count) noexcept {
    if (word_count == 0U || bit_count == 0U) {
      return 0ULL;
    }
    const std::size_t total_bits = word_count << word_shift;
    const std::size_t excess = total_bits - bit_count;
    if (excess == 0U) {
      return ~0ULL;
    }
    return ~0ULL >> excess;
  }
};

}

class bitset {
public:
  bitset() = default;

  void resize(std::size_t count) {
    bit_count_ = count;
    words_.assign(detail::bitset_constants::words_for_bits(count), 0ULL);
    sanitize_tail_bits();
  }

  void fill(bool value) {
    const std::uint64_t word = value ? ~0ULL : 0ULL;
    std::fill(words_.begin(), words_.end(), word);
    if (value) {
      sanitize_tail_bits();
    }
  }

  void set(std::size_t index, bool value) {
    sn::util::log::ensure(index < bit_count_, "obliv::bitset::set: index out of range");
    const std::size_t word_index = index >> detail::bitset_constants::word_shift;
    const std::uint64_t mask = std::uint64_t{1} << (index & detail::bitset_constants::word_mask);
    const std::uint64_t neg = static_cast<std::uint64_t>(-static_cast<std::int64_t>(value));
    auto& word = words_[word_index];
    word = (word & ~mask) | (neg & mask);
  }

  [[nodiscard]] bool get(std::size_t index) const {
    sn::util::log::ensure(index < bit_count_, "obliv::bitset::get: index out of range");
    const std::size_t word_index = index >> detail::bitset_constants::word_shift;
    const std::uint64_t mask = std::uint64_t{1} << (index & detail::bitset_constants::word_mask);
    return (words_[word_index] & mask) != 0ULL;
  }

  [[nodiscard]] choice get_ct(std::size_t index) const {
    sn::util::log::ensure(index < bit_count_, "obliv::bitset::get_ct: index out of range");
    const std::size_t target_word = index >> detail::bitset_constants::word_shift;
    const std::uint32_t bit_offset = static_cast<std::uint32_t>(index & detail::bitset_constants::word_mask);

    if (words_.size() == 1U) {
      const std::uint64_t bit = (words_[0] >> bit_offset) & 0x1ULL;
      return choice(static_cast<bool>(bit));
    }

    std::uint64_t selected = 0ULL;
    for (std::size_t word_ix = 0; word_ix < words_.size(); ++word_ix) {
      const bool is_target = sn::obliv::ct_eq<std::size_t>(word_ix, target_word);
      const std::uint64_t word_value = words_[word_ix];
      selected = sn::obliv::ct_select<std::uint64_t>(word_value, selected, is_target);
    }

    const std::uint64_t shifted = selected >> bit_offset;
    return choice(static_cast<bool>(shifted & 0x1ULL));
  }

  void set_ct(std::size_t index, bool value) {
    sn::util::log::ensure(index < bit_count_, "obliv::bitset::set_ct: index out of range");
    const std::size_t target_word = index >> detail::bitset_constants::word_shift;
    const std::uint64_t mask = std::uint64_t{1} << (index & detail::bitset_constants::word_mask);
    const std::uint64_t value_mask = static_cast<std::uint64_t>(-static_cast<std::int64_t>(value));
    const std::uint64_t set_mask = mask & value_mask;

    for (std::size_t word_ix = 0; word_ix < words_.size(); ++word_ix) {
      const bool is_target = sn::obliv::ct_eq<std::size_t>(word_ix, target_word);
      const std::uint64_t current = words_[word_ix];
      const std::uint64_t cleared = current & ~mask;
      const std::uint64_t updated = cleared | set_mask;
      const std::uint64_t result = sn::obliv::ct_select<std::uint64_t>(updated, current, is_target);
      words_[word_ix] = result;
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return bit_count_; }
  [[nodiscard]] std::size_t bit_count() const noexcept { return bit_count_; }

private:
  void sanitize_tail_bits() {
    if (words_.empty()) {
      return;
    }
    const std::size_t word_count = words_.size();
    const std::uint64_t mask = detail::bitset_constants::last_word_mask(bit_count_, word_count);
    if (mask == ~0ULL) {
      return;
    }
    words_.back() &= mask;
  }

  std::vector<std::uint64_t> words_{};
  std::size_t bit_count_ = 0;
};

class concurrent_bitset {
public:
#if !defined(SONIC_NO_OS)
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "concurrent_bitset requires lock-free uint64 atomics");
#endif

  concurrent_bitset() = default;
  concurrent_bitset(const concurrent_bitset&) = delete;
  concurrent_bitset& operator=(const concurrent_bitset&) = delete;
  concurrent_bitset(concurrent_bitset&&) noexcept = default;
  concurrent_bitset& operator=(concurrent_bitset&&) noexcept = default;

  void resize(std::size_t count) {
    bit_count_ = count;
    const std::size_t words_needed = detail::bitset_constants::words_for_bits(count);
    if (words_needed == 0U) {
      owned_.reset();
      words_ = nullptr;
      word_count_ = 0U;
      return;
    }

    owned_.reset(new std::atomic<std::uint64_t>[words_needed]);
    words_ = owned_.get();
    word_count_ = words_needed;
    for (std::size_t i = 0; i < word_count_; ++i) {
      words_[i].store(0ULL, std::memory_order_relaxed);
    }
  }

  void adopt(std::atomic<std::uint64_t>* words, std::size_t bit_count) {
    const std::size_t words_needed = detail::bitset_constants::words_for_bits(bit_count);
    bit_count_ = bit_count;
    word_count_ = words_needed;
    owned_.reset();
    words_ = words;
  }

  void fill(bool value) {
    if (word_count_ == 0U) {
      return;
    }

    const std::uint64_t fill_word = value ? ~0ULL : 0ULL;
    const std::size_t last = word_count_ - 1U;
    for (std::size_t i = 0; i < last; ++i) {
      words_[i].store(fill_word, std::memory_order_relaxed);
    }

    const std::uint64_t tail_mask = detail::bitset_constants::last_word_mask(bit_count_, word_count_);
    const std::uint64_t tail_word = value ? tail_mask : 0ULL;
    words_[last].store(tail_word, std::memory_order_relaxed);
  }

  void set(std::size_t index, bool value) {
    sn::util::log::ensure(index < bit_count_, "obliv::concurrent_bitset::set: index out of range");
    const std::size_t word_index = index >> detail::bitset_constants::word_shift;
    const std::uint64_t mask = std::uint64_t{1} << (index & detail::bitset_constants::word_mask);
    auto& word = words_[word_index];

    if (value) {
      word.fetch_or(mask, std::memory_order_relaxed);
    } else {
      word.fetch_and(~mask, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] bool get(std::size_t index) const {
    sn::util::log::ensure(index < bit_count_, "obliv::concurrent_bitset::get: index out of range");
    if (word_count_ == 0U) {
      return false;
    }
    const std::size_t word_index = index >> detail::bitset_constants::word_shift;
    const std::uint64_t mask = std::uint64_t{1} << (index & detail::bitset_constants::word_mask);
    return (words_[word_index].load(std::memory_order_relaxed) & mask) != 0ULL;
  }

  [[nodiscard]] choice get_ct(std::size_t index) const {
    sn::util::log::ensure(index < bit_count_, "obliv::concurrent_bitset::get_ct: index out of range");
    if (word_count_ == 0U) {
      return choice(false);
    }

    const std::size_t target_word = index >> detail::bitset_constants::word_shift;
    const std::uint32_t bit_offset = static_cast<std::uint32_t>(index & detail::bitset_constants::word_mask);

    if (word_count_ == 1U) {
      const std::uint64_t bit = (words_[0].load(std::memory_order_relaxed) >> bit_offset) & 0x1ULL;
      return choice(static_cast<bool>(bit));
    }

    std::uint64_t selected = 0ULL;
    for (std::size_t word_ix = 0; word_ix < word_count_; ++word_ix) {
      const std::uint64_t word_value = words_[word_ix].load(std::memory_order_relaxed);
      const bool is_target = sn::obliv::ct_eq<std::size_t>(word_ix, target_word);
      selected = sn::obliv::ct_select<std::uint64_t>(word_value, selected, is_target);
    }

    const std::uint64_t bit = (selected >> bit_offset) & 0x1ULL;
    return choice(static_cast<bool>(bit));
  }

  void set_ct(std::size_t index, bool value) {
    sn::util::log::ensure(index < bit_count_, "obliv::concurrent_bitset::set_ct: index out of range");
    const std::size_t target_word = index >> detail::bitset_constants::word_shift;
    const std::uint64_t bit = std::uint64_t{1} << (index & detail::bitset_constants::word_mask);
    const std::uint64_t value_mask = static_cast<std::uint64_t>(-static_cast<std::int64_t>(value));

    for (std::size_t word_ix = 0; word_ix < word_count_; ++word_ix) {
      const bool is_target = sn::obliv::ct_eq<std::size_t>(word_ix, target_word);
      auto& word = words_[word_ix];
      const std::uint64_t target_mask = sn::obliv::ct_select<std::uint64_t>(~0ULL, 0ULL, is_target);
      const std::uint64_t masked_bit = bit & target_mask;
      const std::uint64_t set_mask = masked_bit & value_mask;
      const std::uint64_t clear_mask = masked_bit & ~value_mask;

      word.fetch_and(~clear_mask, std::memory_order_relaxed);
      word.fetch_or(set_mask, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return bit_count_; }
  [[nodiscard]] std::size_t bit_count() const noexcept { return bit_count_; }
  [[nodiscard]] std::size_t word_count() const noexcept { return word_count_; }
  [[nodiscard]] const std::atomic<std::uint64_t>* words_data() const noexcept { return words_; }
  [[nodiscard]] std::uint64_t load_word(std::size_t index) const {
    sn::util::log::ensure(index < word_count_, "obliv::concurrent_bitset::load_word: index out of range");
    return words_[index].load(std::memory_order_relaxed);
  }

private:
  void sanitize_tail_bits() {
    if (word_count_ == 0U || bit_count_ == 0U) {
      return;
    }
    const std::uint64_t mask = detail::bitset_constants::last_word_mask(bit_count_, word_count_);
    if (mask == ~0ULL) {
      return;
    }
    const std::size_t last = word_count_ - 1U;
    std::uint64_t word = words_[last].load(std::memory_order_relaxed) & mask;
    words_[last].store(word, std::memory_order_relaxed);
  }

  std::unique_ptr<std::atomic<std::uint64_t>[]> owned_{};
  std::atomic<std::uint64_t>* words_ = nullptr;
  std::size_t bit_count_ = 0;
  std::size_t word_count_ = 0;
};

template <std::size_t WordCapacity> class bitset_snapshot {
public:
  void load(const concurrent_bitset& source) {
    word_count_ = source.word_count();
    bit_count_ = source.bit_count();
    sn::util::log::ensure(word_count_ <= WordCapacity, "obliv::bitset_snapshot: word capacity exceeded");
    for (std::size_t ix = 0; ix < word_count_; ++ix) {
      words_[ix] = source.load_word(ix);
    }
  }

  [[nodiscard]] choice get_ct(std::size_t index) const {
    sn::util::log::ensure(index < bit_count_, "obliv::bitset_snapshot::get_ct: index out of range");
    const std::size_t target_word = index >> detail::bitset_constants::word_shift;
    const std::uint32_t bit_offset = static_cast<std::uint32_t>(index & detail::bitset_constants::word_mask);

    if (word_count_ == 1U) {
      const std::uint64_t bit = (words_[0] >> bit_offset) & 0x1ULL;
      return choice(static_cast<bool>(bit));
    }

    std::uint64_t selected = 0ULL;
    for (std::size_t word_ix = 0; word_ix < word_count_; ++word_ix) {
      const bool is_target = sn::obliv::ct_eq<std::size_t>(word_ix, target_word);
      const std::uint64_t word_value = words_[word_ix];
      selected = sn::obliv::ct_select<std::uint64_t>(word_value, selected, is_target);
    }

    const std::uint64_t bit = (selected >> bit_offset) & 0x1ULL;
    return choice(static_cast<bool>(bit));
  }

private:
  std::array<std::uint64_t, WordCapacity> words_{};
  std::size_t word_count_ = 0;
  std::size_t bit_count_ = 0;
};

}
