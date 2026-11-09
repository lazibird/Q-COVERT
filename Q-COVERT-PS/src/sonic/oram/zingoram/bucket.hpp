#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#if defined(ORAM_DEBUG)
#include <sstream>
#include <string>
#endif

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/obliv/types/bitset.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/oram/tree/block.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"
#include "sonic/threads/sync.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/util/profiling.hpp"
#if defined(ORAM_DEBUG)
#include "sonic/util/picoformat.hpp"
#endif
#include "sonic/oram/storage/pin_mode.hpp"
#include "sonic/oram/storage/slab_store.hpp"
#include "sonic/oram/zingoram/detail/bucket_metadata_arena.hpp"

namespace sn::oram::zingoram {

struct bucket_epoch_span {
  std::uint64_t base = 0;
  std::uint32_t span = 0;
};

// view of bucket access rank
struct bucket_access_view {
  std::uint32_t span = 0;
  std::uint32_t rank = 0;
  [[nodiscard]] std::uint32_t rank_of() const noexcept { return rank; }
};

#if defined(ORAM_DEBUG)
namespace detail {
inline sn::util::log::logger& bucket_logger() {
  static sn::util::log::logger logger = sn::util::log::create("zingoram.bucket");
  return logger;
}
} // namespace detail
#endif

// a single bucket, which contains a mini-oram of blocks, and some metadata fields
template <typename Block, typename BlockStore = sn::oram::zingoram::storage::slab_store<Block>> class bucket {
public:
  using block_t = Block;
  using store_t = BlockStore;
  using uid_generator = sn::oram::uid_generator;
  using prng_t = sn::crypto::buffered_prng<>;
  using choice = sn::obliv::choice;
  using slot_ix = std::uint32_t;
  using offset_ix = std::uint32_t;
  using address_t = std::int64_t;

  bucket() = default;

  bucket(
      std::uint64_t node_id, std::uint32_t level, std::uint32_t real_slots, std::uint32_t dummy_slots, store_t store,
      const detail::bucket_metadata_view& meta
  ) {
    configure(node_id, level, real_slots, dummy_slots, std::move(store), meta);
  }

  bucket(const bucket&) = delete;
  bucket& operator=(const bucket&) = delete;
  bucket(bucket&& other) noexcept { move_from(std::move(other)); }
  bucket& operator=(bucket&& other) noexcept {
    if (this != &other) {
      move_from(std::move(other));
    }
    return *this;
  }

private:
#if defined(ORAM_DEBUG)
  std::string log_dbg_dump() const;
#endif

  static constexpr std::size_t k_max_bucket_slots = 255;
  static constexpr std::size_t k_valids_snapshot_words = (k_max_bucket_slots + 63U) >> 6U;
  using valids_snapshot_t = sn::obliv::bitset_snapshot<k_valids_snapshot_words>;

  void configure(
      std::uint64_t node_id, std::uint32_t level, std::uint32_t real_slots, std::uint32_t dummy_slots, store_t store,
      const detail::bucket_metadata_view& meta
  ) {
    sn::util::log::ensure(real_slots > 0, "zingoram::bucket: real_slots must be positive");
    sn::util::log::ensure(dummy_slots > 0, "zingoram::bucket: dummy_slots must be positive");
    sn::util::log::ensure(real_slots <= k_max_bucket_slots, "zingoram::bucket: real_slots exceeds max capacity");
    sn::util::log::ensure(dummy_slots <= k_max_bucket_slots, "zingoram::bucket: dummy_slots exceeds max capacity");

    node_id_ = node_id;
    level_ = level;
    real_slot_count_ = real_slots;
    dummy_slot_count_ = dummy_slots;
    slot_count_ = real_slot_count_ + dummy_slot_count_;
    sn::util::log::ensure(slot_count_ <= k_max_bucket_slots, "zingoram::bucket: slot_count exceeds snapshot capacity");

    store_ = std::move(store);

    // wire up metadata views from the arena
    const std::size_t words_needed = sn::obliv::detail::bitset_constants::words_for_bits(slot_count_);
    sn::util::log::ensure(meta.valids_words.size() == words_needed, "zingoram::bucket: valids_words size mismatch");
    sn::util::log::ensure(meta.permutation.size() == slot_count_, "zingoram::bucket: permutation size mismatch");
    sn::util::log::ensure(
        meta.real_addresses.size() == real_slot_count_, "zingoram::bucket: real_addresses size mismatch"
    );
    sn::util::log::ensure(meta.shuffle_words.size() == slot_count_, "zingoram::bucket: shuffle_words size mismatch");

    valids_.adopt(meta.valids_words.data(), slot_count_);
    permutation_ = meta.permutation;
    real_addresses_ = meta.real_addresses;
    shuffle_words_ = meta.shuffle_words;
  }

public:
  [[nodiscard]] bool configured() const noexcept { return slot_count_ != 0; }

  [[nodiscard]] std::uint64_t node_id() const noexcept { return node_id_; }
  [[nodiscard]] std::uint32_t level() const noexcept { return level_; }
  [[nodiscard]] std::uint32_t real_slots() const noexcept { return real_slot_count_; }
  [[nodiscard]] std::uint32_t dummy_slots() const noexcept { return dummy_slot_count_; }
  [[nodiscard]] std::uint32_t slot_count() const noexcept { return slot_count_; }

  template <class Writer> void save_initial(Writer& writer) const {
    for (std::size_t i = 0; i < valids_.word_count(); ++i) {
      const auto word = valids_.load_word(i);
      writer.write(&word, sizeof(word));
    }
    writer.write(permutation_.data(), permutation_.size() * sizeof(std::uint8_t));
    writer.write(real_addresses_.data(), real_addresses_.size() * sizeof(std::int64_t));
    writer.write(shuffle_words_.data(), shuffle_words_.size() * sizeof(std::uint16_t));
    store_.save_initial(writer);
  }

  template <class Reader> void load_initial(Reader& reader) {
    valids_.fill(false);
    for (std::size_t i = 0; i < valids_.word_count(); ++i) {
      std::uint64_t word = 0;
      reader.read(&word, sizeof(word));
      for (std::size_t bit = 0; bit < 64 && i * 64 + bit < slot_count_; ++bit)
        valids_.set(i * 64 + bit, ((word >> bit) & 1U) != 0);
    }
    reader.read(permutation_.data(), permutation_.size() * sizeof(std::uint8_t));
    reader.read(real_addresses_.data(), real_addresses_.size() * sizeof(std::int64_t));
    reader.read(shuffle_words_.data(), shuffle_words_.size() * sizeof(std::uint16_t));
    store_.load_initial(reader);
  }

  [[nodiscard]] std::uint32_t max_touch_count() const noexcept { return dummy_slot_count_; }

  void prefetch() const { store_.prefetch(); }
  // Integration hook for backend-specific instrumentation; call only while
  // the ORAM is idle, with no active bucket guards or worker accesses.
  store_t& block_store() noexcept { return store_; }

  // synchronization/epoch is external; bucket is sync-free.

  void initialize(uid_generator& uid_gen, prng_t& prng) {
    sn_prof_zone("zingoram.bucket.initialize");
    namespace log = sn::util::log;

#if defined(ORAM_DEBUG)
    log::ensure(configured(), "zingoram::bucket: initialize called before layout configured");
#endif

    auto guard = store_.pin(storage::pin_mode::exclusive);

    // at setup time, all blocks are dummy, so we can just make a completely random permutation
    fill_dummy_slots(guard.data(), uid_gen);
    sn::obliv::fill(real_addresses_.begin(), real_addresses_.end(), block_t::dummy_address);

    // shuffle offsets
    shuffle_offsets(prng);
    // reorder blocks to permutation
    reorder_blocks(guard.data());

    guard.mark_dirty();

    // mark all blocks valid
    valids_.fill(true);
  }

  void rebuild(sn::util::span<const block_t> real_blocks, uid_generator& uid_gen, prng_t& prng) {
    sn_prof_zone("zingoram.bucket.rebuild");
    namespace log = sn::util::log;

#if defined(ORAM_DEBUG)
    log::ensure(configured(), "zingoram::bucket: rebuild called before layout configured");
    log::ensure(real_blocks.size() == real_slot_count_, "zingoram::bucket: real block span must match real slot count");

    auto& logger = detail::bucket_logger();

    std::ostringstream ss;
    for (std::size_t i = 0; i < real_blocks.size(); ++i) {
      const block_t& blk = real_blocks[i];
      ss << pfm::format("    real[%03d]: block#%d addr=$%08x leaf_ix=%d\n", i, blk.uid, blk.address, blk.leaf_ix);
    }
    logger.dbgf("bucket::rebuild: bucket(id=%d, level=%d)", node_id_, level_);
    logger.pedf("new real blocks:\n%s", ss.str());
#endif

    { // Release a buffered store pin before any debug read-back.
      auto guard = store_.pin(storage::pin_mode::exclusive);
      block_t* data = guard.data();

      // copy new blocks into the bucket and update address metadata for the real slots
      {
        sn_prof_zone("zingoram.bucket.rebuild.copy_reals");
        for (std::uint32_t i = 0; i < real_slot_count_; ++i) {
          data[i] = real_blocks[i];
          real_addresses_[i] = real_blocks[i].address;
        }
      }

      // dummy out all dummy slots so reshuffles never leak which entries were real
      {
        sn_prof_zone("zingoram.bucket.rebuild.fill_dummies");
        for (std::uint32_t i = real_slot_count_; i < slot_count_; ++i) {
          data[i].set_dummy(uid_gen);
        }
      }

      // Count both oblivious sorts used by this bucket rebuild separately from Evict OSort.
#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
      sn::sortshuffle::ser::bitonic::counting::category_scope comparator_scope(
          sn::sortshuffle::ser::bitonic::counting::category::bucket_shuffle
      );
#endif
      shuffle_offsets(prng);
      reorder_blocks(data);

      guard.mark_dirty();
    }

    // mark all blocks valid
    {
      sn_prof_zone("zingoram.bucket.rebuild.valids_fill");
      valids_.fill(true);
    }

#if defined(ORAM_DEBUG)
    logger.pedf("after rebuild: %s", log_dbg_dump());
#endif
  }

  std::uint32_t get_block_offset(std::int64_t address, bool& is_real_block, const bucket_access_view& view) {
    sn_prof_zone("zingoram.bucket.get_block_offset");
    namespace log = sn::util::log;

#if defined(ORAM_DEBUG)
    log::ensure(configured(), "zingoram::bucket: get_block_offset before layout configured");
    log::ensure(address >= block_t::dummy_address, "zingoram::bucket: get_block_offset invalid address");
#endif

#if defined(ORAM_DEBUG)
    auto& logger = detail::bucket_logger();
    log::ensure(view.span > 0, "zingoram::bucket: access view span must be positive");
    const std::uint32_t debug_rank = view.rank;
    log::ensure(debug_rank < view.span, "zingoram::bucket: access view rank exceeds span");
    logger.dbgf(
        "  get_block_offset: node_id=%d, address=$%08x, rank=%u", node_id_, static_cast<unsigned long long>(address),
        debug_rank
    );
#endif

    // concurrent accessors will never conflict on real slots, snapshot
    valids_snapshot_t valids_snapshot{};
    valids_snapshot.load(valids_);

    // see if the block of interest is in this bucket
    // we check the first Z slots, which may contain real blocks
    // obliv: slot_i is public: values will be 0 -> Z-1
    choice found = choice::false_value();
    std::int64_t selected_real_offset = -1;
    {
      sn_prof_zone("zingoram.bucket.get_block_offset.real_scan");
      for (std::uint32_t slot = 0; slot < real_slot_count_; ++slot) {
        const std::uint32_t offset = permutation_[slot];
        // check if block is valid
        const choice slot_valid = valids_snapshot.get_ct(offset);
        // get the address of the real block in this slot
        const std::int64_t slot_address = real_addresses_[slot];
        const choice slot_real(sn::obliv::ct_gt(slot_address, static_cast<std::int64_t>(-1)));
        const choice address_match(sn::obliv::ct_eq(slot_address, address));

        // if this block is valid and the address matches, we found our block
        // if the address is not -1, this is by definition a real block
        const choice take = slot_valid && slot_real && address_match;
#if defined(ORAM_DEBUG)
        const bool previously_found = found.unwrap();
#endif
        selected_real_offset =
            sn::obliv::ct_select<std::int64_t>(static_cast<std::int64_t>(offset), selected_real_offset, take.unwrap());
        found = found || take;

#if defined(ORAM_DEBUG)
        if (previously_found && take.unwrap()) {
          logger.errf(
              "zingoram::bucket: multiple real blocks found for address=$%08x: previously selected (node_id=%d, "
              "block_of=%d [real]), conflicting (node_id=%d, block_of=%d [real])",
              address, node_id_, selected_real_offset, node_id_, offset
          );
          logger.errf("bucket state: %s", log_dbg_dump());
          sn::util::log::failf("zingoram::bucket: get_block_offset: multiple real blocks found");
        }
        const bool slot_valid_bool = slot_valid.unwrap();
        const bool slot_real_bool = slot_real.unwrap();
        if (take.unwrap()) {
          logger.dbgf(
              "    selected real block offset: (node_id=%d, block_of=%d [real]), address=$%08x", node_id_, offset,
              slot_address
          );
        } else {
          logger.aygf(
              "    skipping non-matching z_slot=%d: (node_id=%d, block_of=%d), is_valid=%d, is_real=%d, "
              "real_blocks_addrs[%d]=$%08x",
              slot, node_id_, offset, slot_valid_bool, slot_real_bool, slot, slot_address
          );
        }
#endif
      }
    }

    // in case a real block is not found, we must select a dummy offset
    // since we have S dummy blocks, and because each ticket admits a unique rank in [0, S)
    // we can directly index the permutation with rank to get a unique dummy offset
    const std::uint32_t slot_rank = view.rank;
    const std::uint32_t dummy_slot_index = real_slot_count_ + slot_rank;

    std::uint32_t dummy_offset = 0;
    bool found_real = false;
    std::int64_t chosen_offset = 0;
    {
      sn_prof_zone("zingoram.bucket.get_block_offset.dummy_select");
      // ensure validity of the dummy slot index
      if (dummy_slot_index >= slot_count_) {
        sn::util::log::failf(
            "zingoram::bucket: dummy slot exhausted (node_id=%llu level=%u real=%u dummy=%u rank=%u span=%u)",
            static_cast<unsigned long long>(node_id_), level_, real_slot_count_, dummy_slot_count_, slot_rank, view.span
        );
      }
      dummy_offset = permutation_[dummy_slot_index];

      found_real = found.unwrap();
      chosen_offset =
          sn::obliv::ct_select<std::int64_t>(selected_real_offset, static_cast<std::int64_t>(dummy_offset), found_real);
    }
    // update the flag of whether we found a matching real block
    is_real_block = found_real;

#if defined(ORAM_DEBUG)
    // check offset
    sn::util::log::ensure(
        chosen_offset >= 0 && static_cast<std::uint32_t>(chosen_offset) < slot_count_,
        "zingoram::bucket: selected offset out of range"
    );
    // check dummy validity
    const bool dummy_valid = valids_.get(dummy_offset);
    sn::util::log::ensure(dummy_valid, "zingoram::bucket: dummy slot already invalidated");
    if (!found_real) {
      logger.dbgf("    selected dummy block offset: (node_id=%d, block_of=%d [dummy])", node_id_, dummy_offset);
    }
#endif

    return static_cast<std::uint32_t>(chosen_offset);
  }

  block_t read_block_at_offset(std::uint32_t offset) {
    namespace log = sn::util::log;
    log::ensure(offset < slot_count_, "zingoram::bucket: read offset out of range");

#if defined(ORAM_DEBUG)
    detail::bucket_logger().pedf("  read_block_at_offset: node_id=%d, block_offset=%d", node_id_, offset);
#endif

    // read the block at the selected offset
    block_t copy = store_.read_block(offset);

    // burn the slot
    valids_.set(offset, false);
    return copy;
  }

  void mark_slot_valid(std::uint32_t offset, bool value) {
    namespace log = sn::util::log;

#if defined(ORAM_DEBUG)
    log::ensure(offset < slot_count_, "zingoram::bucket: mark offset out of range");
#endif

    // reset fields
    valids_.set(offset, value);
  }

  bool access_block(block_t& out_block, std::int64_t address, const bucket_access_view& view) {
    sn_prof_zone("zingoram.bucket.access_block");
#if defined(ORAM_DEBUG)
    auto& logger = detail::bucket_logger();
    logger.dbgf("bucket_access_block: node_id=%d, address=$%08x", node_id_, static_cast<unsigned long long>(address));
    logger.pedf("after access_block: %s", log_dbg_dump());
#endif

    bool is_real = false;
    const std::uint32_t offset = get_block_offset(address, is_real, view);
    out_block = read_block_at_offset(offset);
    return is_real;
  }

  void read_bucket_max_select_offsets(std::int64_t* selected_offsets) {
    sn_prof_zone("zingoram.bucket.read_max_offsets");
    namespace log = sn::util::log;
    log::ensure(configured(), "zingoram::bucket: read_bucket_max_select_offsets before layout configured");

    const std::uint64_t invalid = static_cast<std::uint64_t>(-1);

#if defined(ORAM_DEBUG)
    auto& logger = detail::bucket_logger();
    logger.dbgf("read_bucket_max_select_offsets: node_id=%d", node_id_);
    logger.pedf("before read_bucket_max_select_offsets: %s", log_dbg_dump());
#endif

    for (std::uint32_t i = 0; i < real_slot_count_; ++i) {
      selected_offsets[i] = static_cast<std::int64_t>(-1);
    }

    // owner has exclusive access, snapshot valids
    valids_snapshot_t valids_snapshot{};
    valids_snapshot.load(valids_);

    std::uint32_t real_selected = 0;
    {
      sn_prof_zone("zingoram.bucket.read_max_offsets.real_scan");
      for (std::uint32_t slot = 0; slot < real_slot_count_; ++slot) {
        const std::uint32_t offset = permutation_[slot];
        const choice slot_valid = valids_snapshot.get_ct(offset);
        // conditionally select this block
        selected_offsets[slot] = sn::obliv::ct_select<std::int64_t>(
            static_cast<std::int64_t>(offset), selected_offsets[slot], slot_valid.unwrap()
        );
        const std::uint32_t inc = real_selected + 1;
        real_selected = sn::obliv::ct_select<std::uint32_t>(inc, real_selected, slot_valid.unwrap());

#if defined(ORAM_DEBUG)
        const bool slot_valid_bool = slot_valid.unwrap();
        const std::int64_t slot_address = real_addresses_[slot];
        const bool slot_real_bool = slot_address >= 0;
        if (slot_valid_bool) {
          if (slot_real_bool) {
            logger.dbgf(
                "    selected z_slot=%d: (node_id=%d, block_of=%d [real]), address=$%08x", slot, node_id_, offset,
                static_cast<unsigned long long>(slot_address)
            );
          } else {
            logger.dbgf("    selected z_slot=%d: (node_id=%d, block_of=%d [dummy])", slot, node_id_, offset);
          }
        } else {
          logger.dbgf(
              "    skipping non-valid z_slot=%d: (node_id=%d, block_of=%d), is_valid=%d, is_real=%d, "
              "real_blocks_addrs[%d]=$%08x",
              slot, node_id_, offset, slot_valid_bool, slot_real_bool, slot,
              static_cast<unsigned long long>(slot_address)
          );
        }
#endif
      }
    }

    // figure out how many dummy slots we need to select
    const std::uint32_t needed_dummy = real_slot_count_ - real_selected;
    sn::util::log::ensure(needed_dummy <= dummy_slot_count_, "zingoram::bucket: insufficient dummy slots");
    std::uint32_t dummy_selected = 0;

    // obliv: slot_i is public: values will be Z -> Z+S-1
    {
      sn_prof_zone("zingoram.bucket.read_max_offsets.dummy_pad");
      for (std::uint32_t slot = real_slot_count_; slot < slot_count_; ++slot) {
        const std::uint32_t offset = permutation_[slot];
        // check if slot is valid
        const choice slot_valid = valids_snapshot.get_ct(offset);
        // check whether we need more dummy slots
        const choice need_dummy(sn::obliv::ct_lt(dummy_selected, needed_dummy));
        const choice take_dummy = slot_valid && need_dummy;

        // figure out which index in the selected slots this would go in
        choice inserted = choice::false_value();
        for (std::uint32_t i = 0; i < real_slot_count_; ++i) {
          // update the selected slot in our selected offsets vec
          const choice slot_empty(sn::obliv::ct_eq(static_cast<std::uint64_t>(selected_offsets[i]), invalid));
          const choice place_here = (!inserted) && slot_empty && take_dummy;
          selected_offsets[i] = sn::obliv::ct_select<std::int64_t>(
              static_cast<std::int64_t>(offset), selected_offsets[i], place_here.unwrap()
          );
          inserted = inserted || place_here;
        }

        const std::uint32_t inc = dummy_selected + 1;
        dummy_selected = sn::obliv::ct_select<std::uint32_t>(inc, dummy_selected, take_dummy.unwrap());

#if defined(ORAM_DEBUG)
        if (take_dummy.unwrap()) {
          sn::util::log::ensure(
              inserted.unwrap(), "zingoram::bucket: read_bucket_max_select_offsets failed to insert dummy slot"
          );
          logger.dbgf("    selected dummy_slot=%d: (node_id=%d, block_of=%d [dummy])", slot, node_id_, offset);
        } else {
          logger.pedf(
              "    skipping non-valid dummy_slot=%d: (node_id=%d, block_of=%d), is_valid=%d", slot, node_id_, offset,
              slot_valid.unwrap()
          );
        }
#endif
      }
    }

#if defined(ORAM_DEBUG)
    sn::util::log::ensuref(
        (dummy_selected + real_selected) == real_slot_count_,
        "zingoram::bucket: read_bucket_max_select_offsets total_selected=%d expected=%d",
        dummy_selected + real_selected, real_slot_count_
    );
#endif
  }

  void read_bucket_max(block_t* out_blocks, std::int64_t* selected_offsets) {
    sn_prof_zone("zingoram.bucket.read_max");
#if defined(ORAM_DEBUG)
    auto& logger = detail::bucket_logger();
    logger.dbgf("read_bucket_max: node_id=%d", node_id_);
    logger.pedf("before read_bucket_max: %s", log_dbg_dump());
#endif

    // select offsets
    read_bucket_max_select_offsets(selected_offsets);

    {
      sn_prof_zone("zingoram.bucket.read_max.read_blocks");
      for (std::uint32_t i = 0; i < real_slot_count_; ++i) {
        const std::uint32_t offset = static_cast<std::uint32_t>(selected_offsets[i]);
        out_blocks[i] = store_.read_block(offset);
        valids_.set(offset, false);
      }
    }

#if defined(ORAM_DEBUG)
    for (std::uint32_t i = 0; i < real_slot_count_; ++i) {
      const auto& block = out_blocks[i];
      if (block.is_real().unwrap()) {
        logger.dbgf(
            "    block#%d (address=$%08x, leaf_ix=%d)", block.uid, static_cast<unsigned long long>(block.address),
            block.leaf_ix
        );
      } else {
        logger.dbgf("    block#%d (dummy)", block.uid);
      }
    }
    logger.dbgf("  read_bucket_max: read blocks from bucket(id=%d)", node_id_);
    for (std::uint32_t i = 0; i < real_slot_count_; ++i) {
      const auto offset = static_cast<std::uint32_t>(selected_offsets[i]);
      const bool block_valid = valids_.get(offset);
      sn::util::log::ensuref(
          !block_valid, "zingoram::bucket: read_bucket_max real_slot_ix=%d was not invalidated", i
      );
    }
    logger.pedf("after read_bucket_max: %s", log_dbg_dump());
#endif
  }

  void fill_dummy(uid_generator& uid_gen) {
    auto guard = store_.pin(storage::pin_mode::exclusive);
    fill_dummy_slots(guard.data(), uid_gen);
    guard.mark_dirty();
  }

  [[nodiscard]] bool is_offset_valid(std::uint32_t offset) const { return valids_.get(offset); }

  [[nodiscard]] block_t slot(std::uint32_t offset) const {
    auto guard = store_.pin(storage::pin_mode::shared);
    return guard.data()[offset];
  }
  [[nodiscard]] sn::util::span<const std::uint8_t> permutation() const noexcept {
    return {permutation_.data(), permutation_.size()};
  }
  [[nodiscard]] sn::util::span<const std::int64_t> real_slot_addresses() const noexcept {
    return {real_addresses_.data(), real_addresses_.size()};
  }

private:
  void fill_dummy_slots(block_t* data, uid_generator& uid_gen) {
    for (std::uint32_t i = 0; i < slot_count_; ++i) {
      data[i].set_dummy(uid_gen);
    }
  }

  // shuffle offsets using random tags to create a new permutation
  void shuffle_offsets(prng_t& prng) {
    sn_prof_zone("zingoram.bucket.shuffle_offsets");

#if defined(ORAM_DEBUG)
    sn::util::log::ensure(
        slot_count_ > 1 && slot_count_ <= 0xFFFFu, "zingoram::bucket: shuffle_offsets requires 1 < slot_count <= 65535"
    );
#endif

    {
      sn_prof_zone("zingoram.bucket.shuffle_offsets.fill_prng");
      // fill shuffle words with random data
      prng.random_bytes(
          reinterpret_cast<std::uint8_t*>(shuffle_words_.data()),
          static_cast<std::size_t>(shuffle_words_.size()) * sizeof(std::uint16_t)
      );
    }

    {
      sn_prof_zone("zingoram.bucket.shuffle_offsets.pack_words");
      // pack each shuffle word: high 8 bits of random tag, and low 8 bits of index
      for (std::uint32_t i = 0; i < slot_count_; ++i) {
        // word = tag | index
        shuffle_words_[i] = static_cast<std::uint16_t>((shuffle_words_[i] & 0xFF00u) | (i & 0x00FFu));
      }
    }

    {
      sn_prof_zone("zingoram.bucket.shuffle_offsets.bitonic_shuffle");
      // sort packed (tag|index) pairs for new permutation
      sn::sortshuffle::ser::bitonic::bitonic_sort<std::uint16_t>(shuffle_words_.data(), slot_count_);
    }

    {
      sn_prof_zone("zingoram.bucket.shuffle_offsets.store_permutation");
      // write back permutation: low byte stores slot index
      for (std::uint32_t i = 0; i < slot_count_; ++i) {
        permutation_[i] = static_cast<std::uint8_t>(shuffle_words_[i] & 0x00FFu);
      }
    }
  }

  // reorder blocks to permutation
  void reorder_blocks(block_t* data) {
    sn_prof_zone("zingoram.bucket.reorder_blocks");
    struct extra_key {
      std::uint64_t operator()(const block_t& blk) const noexcept { return blk.extra; }
    };
    for (std::uint32_t i = 0; i < slot_count_; ++i) {
      data[i].extra = permutation_[i];
    }
    sn::sortshuffle::ser::bitonic::bitonic_sort(data, slot_count_, extra_key{}, std::less<std::uint64_t>{});
  }

  void move_from(bucket&& other) noexcept {
    node_id_ = other.node_id_;
    level_ = other.level_;
    real_slot_count_ = other.real_slot_count_;
    dummy_slot_count_ = other.dummy_slot_count_;
    slot_count_ = other.slot_count_;

    store_ = std::move(other.store_);
    valids_ = std::move(other.valids_);
    permutation_ = other.permutation_;
    real_addresses_ = other.real_addresses_;
    shuffle_words_ = other.shuffle_words_;

    other.node_id_ = 0;
    other.level_ = 0;
    other.real_slot_count_ = 0;
    other.dummy_slot_count_ = 0;
    other.slot_count_ = 0;

    other.store_ = store_t{};
    other.valids_.resize(0);
    other.permutation_ = {};
    other.real_addresses_ = {};
    other.shuffle_words_ = {};
  }

  std::uint64_t node_id_ = 0;
  std::uint32_t level_ = 0;
  std::uint32_t real_slot_count_ = 0;
  std::uint32_t dummy_slot_count_ = 0;
  std::uint32_t slot_count_ = 0;

  store_t store_;
  sn::obliv::concurrent_bitset valids_;
  sn::util::span<std::uint8_t> permutation_{};
  sn::util::span<std::int64_t> real_addresses_{};
  sn::util::span<std::uint16_t> shuffle_words_{};
#if defined(ORAM_DEBUG)
  sn::util::log::logger* log_ = nullptr;
#endif
};

#if defined(ORAM_DEBUG)
template <typename Block, typename BlockStore> std::string bucket<Block, BlockStore>::log_dbg_dump() const {
  std::ostringstream out;
  out << pfm::format("bucket(id=%d) (N=%d, S=%d):\n", node_id_, slot_count_, max_touch_count());
  out << "  slots:\n";

  auto guard = store_.pin(storage::pin_mode::shared);
  const block_t* data = guard.data();

  for (std::size_t i = 0; i < slot_count_; ++i) {
    std::int64_t slot_addr = -1;
    const bool is_real_slot = i < real_slot_count_;
    if (is_real_slot) {
      slot_addr = real_addresses_[i];
    }

    const std::uint32_t offset = permutation_[i];
    const bool slot_valid = valids_.get(offset);
    const auto& block = data[offset];

    std::ostringstream slot_info;
    if (is_real_slot) {
      slot_info << pfm::format("slot.r[%03d]->%d", i, offset);
    } else {
      slot_info << pfm::format("slot.d[%03d]->%d", i, offset);
    }

    if (block.is_real().unwrap()) {
      out << pfm::format(
          "    %s: block#%d (address=$%08x, leaf_ix=%d, valid=%d)\n", slot_info.str(), block.uid,
          static_cast<unsigned long long>(block.address), block.leaf_ix, slot_valid
      );
    } else {
      out << pfm::format("    %s: block#%d (dummy, valid=%d)\n", slot_info.str(), block.uid, slot_valid);
    }

    if (is_real_slot) {
      sn::util::log::ensuref(
          slot_addr == block.address,
          "zingoram::bucket: slot#%d address mismatch: slot_addr=$%08x block_addr=$%08x", i,
          static_cast<unsigned long long>(slot_addr), static_cast<unsigned long long>(block.address)
      );
    } else {
      sn::util::log::ensuref(
          slot_addr < 0, "zingoram::bucket: dummy slot#%d holds real block address=$%08x", i,
          static_cast<unsigned long long>(slot_addr)
      );
    }
  }

  return out.str();
}
#endif

} // namespace sn::oram::zingoram
