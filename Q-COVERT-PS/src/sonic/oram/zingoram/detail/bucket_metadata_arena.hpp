#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "sonic/util/span.hpp"
#include "sonic/obliv/types/bitset.hpp"

namespace sn::oram::zingoram::detail {

struct bucket_metadata_view {
  sn::util::span<std::atomic<std::uint64_t>> valids_words;
  sn::util::span<std::uint8_t> permutation;
  sn::util::span<std::int64_t> real_addresses;
  sn::util::span<std::uint16_t> shuffle_words;
};

class bucket_metadata_arena {
public:
  bucket_metadata_arena() = default;

  void configure(std::uint64_t node_count, std::uint32_t real_slots, std::uint32_t dummy_slots) {
    node_count_ = node_count + 1; // root node is 1, 0 is sentinel
    real_slots_ = real_slots;
    slot_count_ = real_slots + dummy_slots;
    const std::size_t total_buckets = static_cast<std::size_t>(node_count_);

    words_per_bucket_ = sn::obliv::detail::bitset_constants::words_for_bits(slot_count_);

    const std::size_t total_valid_words = total_buckets * words_per_bucket_;
    // rebuild vector fresh to avoid moving atomics
    valids_words_ = std::vector<std::atomic<std::uint64_t>>(total_valid_words);
    for (std::size_t i = 0; i < total_valid_words; ++i) {
      valids_words_[i].store(0ULL, std::memory_order_relaxed);
    }
    permutation_.assign(total_buckets * slot_count_, 0);
    real_addresses_.assign(total_buckets * real_slots_, 0);
    shuffle_words_.assign(total_buckets * slot_count_, 0);
  }

  [[nodiscard]] bucket_metadata_view view(std::size_t node_id) {
    const std::size_t base_word = node_id * words_per_bucket_;
    const std::size_t base_perm = node_id * slot_count_;
    const std::size_t base_real = node_id * real_slots_;
    const std::size_t base_shuffle = node_id * slot_count_;

    return bucket_metadata_view{
        sn::util::span<std::atomic<std::uint64_t>>(valids_words_.data() + base_word, words_per_bucket_),
        sn::util::span<std::uint8_t>(permutation_.data() + base_perm, slot_count_),
        sn::util::span<std::int64_t>(real_addresses_.data() + base_real, real_slots_),
        sn::util::span<std::uint16_t>(shuffle_words_.data() + base_shuffle, slot_count_)
    };
  }

  [[nodiscard]] std::size_t bytes() const noexcept {
    return valids_words_.size() * sizeof(std::atomic<std::uint64_t>) + permutation_.size() * sizeof(std::uint8_t) +
           real_addresses_.size() * sizeof(std::int64_t) + shuffle_words_.size() * sizeof(std::uint16_t);
  }

  struct breakdown {
    std::size_t total = 0;
    std::size_t per_bucket = 0;
    std::size_t valids = 0;
    std::size_t permutation = 0;
    std::size_t real_addresses = 0;
    std::size_t shuffle = 0;
  };

  static breakdown planned(std::uint64_t node_count, std::uint32_t real_slots, std::uint32_t dummy_slots) noexcept {
    const std::size_t buckets = static_cast<std::size_t>(node_count + 1ULL);
    const std::uint32_t slot_count = real_slots + dummy_slots;
    const std::size_t words = sn::obliv::detail::bitset_constants::words_for_bits(slot_count);

    breakdown b{};
    b.valids = buckets * words * sizeof(std::atomic<std::uint64_t>);
    b.permutation = buckets * static_cast<std::size_t>(slot_count) * sizeof(std::uint8_t);
    b.real_addresses = buckets * static_cast<std::size_t>(real_slots) * sizeof(std::int64_t);
    b.shuffle = buckets * static_cast<std::size_t>(slot_count) * sizeof(std::uint16_t);
    b.total = b.valids + b.permutation + b.real_addresses + b.shuffle;
    b.per_bucket = (buckets > 0) ? (b.total / buckets) : 0;
    return b;
  }

  [[nodiscard]] std::uint32_t slot_count() const noexcept { return slot_count_; }
  [[nodiscard]] std::uint32_t real_slots() const noexcept { return real_slots_; }
  [[nodiscard]] std::size_t words_per_bucket() const noexcept { return words_per_bucket_; }

private:
  std::uint64_t node_count_ = 0;
  std::uint32_t real_slots_ = 0;
  std::uint32_t slot_count_ = 0;
  std::size_t words_per_bucket_ = 0;

  std::vector<std::atomic<std::uint64_t>> valids_words_{};
  std::vector<std::uint8_t> permutation_{};
  std::vector<std::int64_t> real_addresses_{};
  std::vector<std::uint16_t> shuffle_words_{};
};

} // namespace sn::oram::zingoram::detail
