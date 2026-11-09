#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/obliv/ops/struct_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/oram/tree/block.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"
#include "sonic/sortshuffle/ser/orshuffle.hpp"
#include "sonic/util/picoformat.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::stash::core {

namespace obliv = sn::obliv;
namespace ser = sn::sortshuffle::ser;
namespace log = sn::util::log;

// lock-free block stash
//
// this is a lock-free block stash to eliminate stash lock contention
// made of two sections: (static (J), mods (K))
// it works by performing modifications in an append-only manner
// so that multiple readers and writers can access the stash concurrently
// as long as no logical dependency exists across concurrent operations
// all blocks in the stash are identified by their address
// so as long as no two concurrent operations are for the same address
// then they can be performed concurrently without conflicts
//
// the static section contains a set of real/dummy blocks that are their old states
// the mods section is appended by every single non-read-only operation
// eventually, "condense" is called to apply mods to the static and clear mods
//
// supported operation types:
// - ADD(blk) - add a block (W)
//    precondition: no other block with the same address exists
//    cost: O(1) by appending to mods (add a create mod)
//    add a block that doesn't currently exist to the stash
// - READ_AND_REMOVE(addr, should_remove) - read and optionally remove a block (R/W)
//    precondition: matching block must exist
//    cost: O(J+K) by scanning static and mods
//    read a block from the stash and possibly remove it (add a remove mod)
// - UPDATE(blk) - update a block (R/W)
//    precondition: matching block must exist
//    cost: O(J+K) by scanning static and mods
//
// how do operations work?
// - first, scan the static blocks, looking for a matching block
// - put that matching block, if any, in your hand
// - then, scan the mods, and apply each mod in order
// - if encountering a CREATE mod, put that block in your hand
// - if encountering a WRITE mod, update the block in your hand
// - if encountering a REMOVE mod, clear the block in your hand
//
// we can store mod types in a single byte
// since blocks already have an 8-byte extra_data field
// we can use byte 0 for mod type
//
// how does condense work?
// - since this has to be doubly oblivious
// - and we want to avoid an O(N^2) algorithm
// - we will do this via sort/scan/compact
// - concatenate static and mods (with padding if necessary)
// - sort by address, prioritizing static over mods
// - this will group blocks by address, with static before mods
// - now, we can linear scan across with one hand
// - every time we encounter a static or a CREATE mod, set our hand
// - apply all subsequent mods to our hand
// - overwrite the slot with the hand, and update compact mark whenever we drop it
// - run compaction, to put together all the updated blocks
// - we will now have a new set of up-to-date static blocks and empty mods
template <typename Block> class lock_free_block_storage {
public:
  enum class block_flags : std::uint8_t {
    block_static = 1U << 0,
    block_mod = 1U << 1,
    block_mod_create = 1U << 2,
    block_mod_write = 1U << 3,
    block_mod_remove = 1U << 4,
  };

private:
  sn::util::log::logger logger_;

  // the size of the static section
  std::size_t static_capacity_;

  // the size of the mods section
  std::size_t mods_capacity_;

  // the number of mods that have been published (visible to readers)
  std::atomic<std::size_t> mods_n_;

  // the number of mods that have been reserved (can exceed published if writers overlap)
  std::atomic<std::size_t> mods_reserved_;

  // the number of real blocks in the static section
  std::atomic<std::size_t> held_real_blocks_;

  // the block vector
  std::vector<Block> blocks_;

  // another block vector used for condense
  std::vector<Block> condense_blocks_;
  std::vector<std::uint8_t> condense_compact_marks_;
  std::vector<std::size_t> condense_compact_prefix_sum_;

public:
  lock_free_block_storage(std::size_t static_capacity, std::size_t mods_capacity, sn::util::log::logger logger) :
      logger_(std::move(logger)),
      static_capacity_(static_capacity),
      mods_capacity_(mods_capacity),
      mods_n_(0),
      mods_reserved_(0),
      held_real_blocks_(0),
      blocks_(static_capacity + mods_capacity),
      condense_blocks_(static_capacity + mods_capacity),
      condense_compact_marks_(static_capacity + mods_capacity),
      condense_compact_prefix_sum_(static_capacity + mods_capacity + 1) {
    log::ensure(mods_capacity_ > 0, "lock_free_block_storage: mods_capacity must be positive");
    log::ensure(static_capacity_ + mods_capacity_ > 0, "lock_free_block_storage: total capacity must be positive");
    block_make_dummy(blocks_.data(), blocks_.size());
    block_make_dummy(condense_blocks_.data(), condense_blocks_.size());
  }

  std::size_t static_capacity() const { return static_capacity_; }
  std::size_t mods_capacity() const { return mods_capacity_; }

  Block* get_block(std::size_t i) { return &blocks_[i]; }
  Block* get_mod_block(std::size_t i) { return &blocks_[static_capacity_ + i]; }

  std::vector<Block>& get_blocks_buf() { return blocks_; }
  std::vector<Block>& blocks_buffer() { return blocks_; }
  const std::vector<Block>& blocks_buffer() const { return blocks_; }

  std::uint8_t block_flags_value(const Block* block) const { return static_cast<std::uint8_t>(block->extra & 0xffU); }

  obliv::choice block_is_static(const Block* block) const {
    return obliv::choice(
        obliv::ct_eq<std::uint8_t>(block_flags_value(block), static_cast<std::uint8_t>(block_flags::block_static))
    );
  }

  obliv::choice block_is_mod(const Block* block) const {
    return obliv::choice(
        obliv::ct_eq<std::uint8_t>(block_flags_value(block), static_cast<std::uint8_t>(block_flags::block_mod))
    );
  }

  obliv::choice block_is_mod_create(const Block* block) const {
    return obliv::choice(
        obliv::ct_eq<std::uint8_t>(
            block_flags_value(block),
            static_cast<std::uint8_t>(block_flags::block_mod) | static_cast<std::uint8_t>(block_flags::block_mod_create)
        )
    );
  }

  obliv::choice block_is_mod_write(const Block* block) const {
    return obliv::choice(
        obliv::ct_eq<std::uint8_t>(
            block_flags_value(block),
            static_cast<std::uint8_t>(block_flags::block_mod) | static_cast<std::uint8_t>(block_flags::block_mod_write)
        )
    );
  }

  obliv::choice block_is_mod_remove(const Block* block) const {
    return obliv::choice(
        obliv::ct_eq<std::uint8_t>(
            block_flags_value(block),
            static_cast<std::uint8_t>(block_flags::block_mod) | static_cast<std::uint8_t>(block_flags::block_mod_remove)
        )
    );
  }

  // clear all held blocks in the stash
  void clear() {
#if defined(ORAM_DEBUG)
    logger_.dbg("clear: clearing all blocks in the stash");
    // ensure there are no mods
    log::ensure(mods_n_.load(std::memory_order_acquire) == 0, "clear: mods must be empty");
#endif

    mods_n_.store(0, std::memory_order_relaxed);
    mods_reserved_.store(0, std::memory_order_relaxed);
    held_real_blocks_.store(0, std::memory_order_relaxed);

    block_make_dummy(blocks_.data(), blocks_.size());
  }

  // rebuild from blocks
  void rebuild(const std::vector<Block>& blocks) {
    sn_prof_zone("lockfree_block_storage.rebuild");

#if defined(ORAM_DEBUG)
    logger_.dbg("rebuild: rebuilding stash from blocks");
    // ensure there are no mods
    log::ensure(mods_n_.load(std::memory_order_acquire) == 0, "rebuild: mods must be empty");
    // rebuild vec must have static size of blocks
    log::ensure(blocks.size() == static_capacity_, "rebuild: blocks size must match static capacity");
#endif

    for (std::size_t i = 0; i < static_capacity_; ++i) {
      Block* block = get_block(i);
      block_copy(block, &blocks[i]);
      // set extra data
      std::uint8_t flags = static_cast<std::uint8_t>(block_flags::block_static);
      std::uint32_t chrono = 0;
      block->extra = static_cast<std::uint64_t>(flags) | (static_cast<std::uint64_t>(chrono) << 8U);
    }
    for (std::size_t i = static_capacity_; i < blocks_.size(); ++i) {
      block_make_dummy(&blocks_[i]);
    }
    held_real_blocks_.store(0, std::memory_order_relaxed);
  }

  // add a block to the stash
  void add(const Block* block) {
    sn_prof_zone("lockfree_block_storage.add");

#if defined(ORAM_DEBUG)
    logger_.dbg(
        pfm::format(
            "add: adding block#%llu (address=$%08llx)", static_cast<std::uint64_t>(debug_uid(block)),
            static_cast<std::uint64_t>(block->address)
        )
    );

    if (block->address >= 0) {
      // ensure that the block doesn't already exist
      bool exists = false;
      for (std::size_t i = 0; i < static_capacity_; ++i) {
        if (blocks_[i].address == block->address) {
          log::ensure(!exists, "add: conflicting static block");
          exists = true;
        }
      }
      const std::size_t mods_count = mods_n_.load(std::memory_order_acquire);
      for (std::size_t i = 0; i < mods_count; ++i) {
        Block* mod_block = get_mod_block(i);
        if (mod_block->address == block->address) {
          if (block_is_mod_create(mod_block).unwrap()) {
            log::ensure(!exists, "add: conflicting CREATE mod");
            exists = true;
          }
          if (block_is_mod_remove(mod_block).unwrap()) {
            log::ensure(exists, "add: conflicting REMOVE mod");
            exists = false;
          }
        }
      }
      log::ensuref(
          !exists, "add: block already exists for address=$%08llx", static_cast<std::uint64_t>(block->address)
      );
    }
#endif

    const obliv::choice new_block_is_real(
        obliv::ct_gt<std::int64_t>(block->address, static_cast<std::int64_t>(Block::dummy_address))
    );

    // add a CREATE mod with the same data
    std::uint8_t flags =
        static_cast<std::uint8_t>(block_flags::block_mod) | static_cast<std::uint8_t>(block_flags::block_mod_create);
    add_mod_block(block, flags);

    // update real block count (if added)
    held_real_blocks_.fetch_add(static_cast<std::size_t>(new_block_is_real.unwrap()), std::memory_order_relaxed);
  }

  // read (and optionally remove) a matching block from the stash
  void read(Block* out_block, std::int64_t address, obliv::choice should_remove) {
    sn_prof_zone("lockfree_block_storage.read");

#if defined(ORAM_DEBUG)
    logger_.dbg(pfm::format("read: reading block (address=$%08llx)", static_cast<std::uint64_t>(address)));
    log::ensure(address >= Block::dummy_address, "read: address must be valid (>= dummy_address)");
#endif

    Block hand;
    block_make_dummy(&hand);

    obliv::choice found_in_static = obliv::choice::false_value();
    // scan through all static blocks
    // if we find a matching block, put it in our hand
    for (std::size_t i = 0; i < static_capacity_; ++i) {
      Block* block = get_block(i);
      obliv::choice block_is_dummy(
          obliv::ct_eq<std::int64_t>(block->address, static_cast<std::int64_t>(Block::dummy_address))
      );
      obliv::choice addr_eq = obliv::choice(obliv::ct_eq<std::int64_t>(block->address, address)) && !block_is_dummy;
      block_copy_cond(&hand, block, addr_eq.unwrap());
      found_in_static = found_in_static || addr_eq;
    }

    // scan through all existing mods and apply them to our hand
    scan_mods(&hand, address);

    // our hand holds the result
    block_copy(out_block, &hand);

    // conditionally insert a REMOVE mod if we need to remove the block
    std::uint8_t remove_flags =
        static_cast<std::uint8_t>(block_flags::block_mod) | static_cast<std::uint8_t>(block_flags::block_mod_remove);
    std::uint8_t dummy_flags = static_cast<std::uint8_t>(block_flags::block_mod);
    std::uint8_t mod_flags = obliv::ct_select<std::uint8_t>(remove_flags, dummy_flags, should_remove.unwrap());
    add_mod_block(&hand, mod_flags);

    // update real block count (if removed)
    held_real_blocks_.fetch_sub(static_cast<std::size_t>(should_remove.unwrap()), std::memory_order_relaxed);
  }

  // update a block in the stash
  void update(const Block* block) {
    sn_prof_zone("lockfree_block_storage.update");

#if defined(ORAM_DEBUG)
    logger_.dbg(
        pfm::format(
            "update: updating block#%llu (address=$%08llx)", static_cast<std::uint64_t>(debug_uid(block)),
            static_cast<std::uint64_t>(block->address)
        )
    );

    // update implies that the block exists in the stash
    // scan static
    bool found_existing = false;
    for (std::size_t i = 0; i < static_capacity_; ++i) {
      Block* static_block = get_block(i);
      if (static_block->address == block->address) {
        found_existing = true;
        break;
      }
    }
    // scan mods
    const std::size_t mods_count = mods_n_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < mods_count; ++i) {
      Block* mod_block = get_mod_block(i);
      if (mod_block->address == block->address) {
        if (block_is_mod_create(mod_block).unwrap()) {
          log::ensure(!found_existing, "update: found CREATE after block already exists");
          found_existing = true;
        }
        if (block_is_mod_remove(mod_block).unwrap()) {
          log::ensure(found_existing, "update: found REMOVE without existing block");
          found_existing = false;
        }
        if (block_is_mod_write(mod_block).unwrap()) {
          log::ensure(found_existing, "update: found WRITE without existing block");
        }
      }
    }
    // ensure something existed
    log::ensure(found_existing, "update: existing block not found");
#endif

    // insert a WRITE mod
    std::uint8_t write_flags =
        static_cast<std::uint8_t>(block_flags::block_mod) | static_cast<std::uint8_t>(block_flags::block_mod_write);
    add_mod_block(block, write_flags);
  }

  // scan through all mods and apply them to the hand
  void scan_mods(Block* hand, std::int64_t address) {
    sn_prof_zone("lockfree_block_storage.scan_mods");

    const std::size_t mods_count = mods_n_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < mods_count; ++i) {
      Block* mod_block = get_mod_block(i);
      obliv::choice hand_is_dummy(
          obliv::ct_eq<std::int64_t>(hand->address, static_cast<std::int64_t>(Block::dummy_address))
      );
      obliv::choice mod_addr_eq(obliv::ct_eq<std::int64_t>(mod_block->address, address));

      // get mod type (mutually exclusive)
      obliv::choice mod_is_create = block_is_mod_create(mod_block);
      obliv::choice mod_is_write = block_is_mod_write(mod_block);
      obliv::choice mod_is_remove = block_is_mod_remove(mod_block);

      // - CREATE MOD
      // if the hand is dummy, put the mod block in the hand
      obliv::choice apply_create_mod = hand_is_dummy && mod_addr_eq && mod_is_create;
      block_copy_cond(hand, mod_block, apply_create_mod.unwrap());

      // - WRITE MOD
      // if the address matches, update the hand with the mod block
      obliv::choice apply_write_mod = !hand_is_dummy && mod_addr_eq && mod_is_write;
      block_copy_cond(hand, mod_block, apply_write_mod.unwrap());

      // - REMOVE MOD
      // if the address matches, clear the hand
      obliv::choice apply_remove_mod = !hand_is_dummy && mod_addr_eq && mod_is_remove;
      block_make_dummy_cond(hand, apply_remove_mod.unwrap());
    }
  }

  // add a mod to the stash
  void add_mod_block(const Block* new_mod_block, std::uint8_t flags) {
    sn_prof_zone("lockfree_block_storage.add_mod_block");

    obliv::choice block_is_dummy(
        obliv::ct_eq<std::int64_t>(new_mod_block->address, static_cast<std::int64_t>(Block::dummy_address))
    );

    std::size_t mod_slot = reserve_mod_slot();
    Block* mod_block = get_mod_block(mod_slot);
    // add the mod block
    block_copy(mod_block, new_mod_block);
    std::uint32_t chrono = static_cast<std::uint32_t>(mod_slot) + 1U;
    // set the extra data (mod index + 1)
    std::uint64_t extra = static_cast<std::uint64_t>(flags) | (static_cast<std::uint64_t>(chrono) << 8U);
    mod_block->extra = obliv::ct_select<std::uint64_t>(0, extra, block_is_dummy.unwrap());

    publish_mod_slot(mod_slot);
  }

  // condense and apply all mods to the static section
  void condense() {
    sn_prof_zone("lockfree_block_storage.condense");

#if defined(ORAM_DEBUG)
    logger_.dbg("condense: condensing stash");
#endif

    sn::util::span<Block> static_span(blocks_.data(), static_capacity_);
    condense_impl(static_span, true);

#if defined(ORAM_DEBUG)
    log_dbg_dump();
#endif
  }

  // condense into an external buffer without populating the static section
  std::size_t condense_to(sn::util::span<Block> out) {
    sn_prof_zone("lockfree_block_storage.condense_to");

#if defined(ORAM_DEBUG)
    logger_.dbg(pfm::format("condense_to: condensing stash into buffer (capacity=%zu)", out.size()));
#endif

    return condense_impl(out, false);
  }

  std::size_t held_real_blocks() const { return held_real_blocks_.load(std::memory_order_relaxed); }

#if defined(ORAM_DEBUG)
  std::string log_dbg_format_block(Block* block) {
    // format block with flags
    std::uint8_t flags = static_cast<std::uint8_t>(block->extra & 0xffU);
    std::uint32_t chrono = static_cast<std::uint32_t>((block->extra >> 8U) & 0xffffffffU);
    std::string flags_str;
    if (flags & static_cast<std::uint8_t>(block_flags::block_static)) {
      flags_str += "STA|";
    }
    if (flags & static_cast<std::uint8_t>(block_flags::block_mod)) {
      flags_str += "MOD|";
    }
    if (flags & static_cast<std::uint8_t>(block_flags::block_mod_create)) {
      flags_str += "CREATE";
    }
    if (flags & static_cast<std::uint8_t>(block_flags::block_mod_write)) {
      flags_str += "WRITE_";
    }
    if (flags & static_cast<std::uint8_t>(block_flags::block_mod_remove)) {
      flags_str += "REMOVE";
    }

    if (block->address >= 0) {
      return pfm::format(
          "block#%llu (address=$%08llx, leaf_ix=%lld) [%s] t=%u", static_cast<std::uint64_t>(debug_uid(block)),
          static_cast<std::uint64_t>(block->address), static_cast<std::int64_t>(block->leaf_ix), flags_str, chrono
      );
    }
    return pfm::format("block#%llu (dummy) [%s]", static_cast<std::uint64_t>(debug_uid(block)), flags_str);
  }

  void log_dbg_dump() {
    std::ostringstream oss;
    oss << "stash dump:\n";
    oss << pfm::format("  static[%zu]:\n", static_capacity_);
    for (std::size_t i = 0; i < static_capacity_; ++i) {
      Block* block = get_block(i);
      oss << "    " << log_dbg_format_block(block) << "\n";
    }
    std::size_t mods_count = mods_n_.load(std::memory_order_acquire);
    oss << pfm::format("  mods[%zu]:\n", mods_count);
    for (std::size_t i = 0; i < mods_count; ++i) {
      Block* block = get_mod_block(i);
      oss << "    " << log_dbg_format_block(block) << "\n";
    }
    logger_.dbg(oss.str());
  }

  void log_condense_dump(const std::string& msg) {
    std::ostringstream oss;
    oss << "condense dump: " << msg << "\n";
    for (std::size_t i = 0; i < condense_blocks_.size(); ++i) {
      Block* block = &condense_blocks_[i];
      oss << pfm::format("    i=%zu: %s\n", i, log_dbg_format_block(block));
    }
    logger_.dbg(oss.str());
  }
#endif

private:
  std::size_t condense_impl(sn::util::span<Block> out, bool update_static) {
    const std::size_t capacity_limit = out.size();

    // reset work buffers
    block_make_dummy(condense_blocks_.data(), condense_blocks_.size());

    const std::size_t mods_published = mods_n_.load(std::memory_order_acquire);

    // concatenate all static blocks and mods
    // copy all static blocks
    sn::obliv::copy(blocks_.begin(), blocks_.begin() + static_capacity_, condense_blocks_.begin());
    // copy only the published mods so readers never observe partially written slots
    sn::obliv::copy(
        blocks_.begin() + static_capacity_, blocks_.begin() + static_capacity_ + mods_published,
        condense_blocks_.begin() + static_capacity_
    );

    // group blocks by address, prioritizing static over mods
    auto key_extractor = [](const Block& block) {
      // read chrono counter from extra_data
      std::uint8_t is_dummy = static_cast<std::uint8_t>(block.address == Block::dummy_address);
      std::int64_t addr = block.address;
      std::uint32_t chrono = static_cast<std::uint32_t>((block.extra >> 8U) & 0xffffffffU);
      // sort by: is_dummy (real blocks first), then address, then chrono (static before mods)
      return std::tuple(is_dummy, addr, chrono);
    };
    ser::bitonic::bitonic_sort(condense_blocks_.data(), condense_blocks_.size(), key_extractor);

#if defined(ORAM_DEBUG)
    if (logger_.verbosity() >= sn::util::log::level::pedantic) {
      log_condense_dump("grouped");
    }
#endif

    // scan through the grouped blocks, applying mods
    std::size_t n_marked_blocks = 0;
    Block hand;
    block_make_dummy(&hand);

    for (std::size_t i = 0; i < condense_blocks_.size(); ++i) {
      Block* scan_block = &condense_blocks_[i];
      obliv::choice block_is_dummy(
          obliv::ct_eq<std::int64_t>(scan_block->address, static_cast<std::int64_t>(Block::dummy_address))
      );
      obliv::choice block_is_static_flag = block_is_static(scan_block);

      // whether the next block is a different segment
      obliv::choice block_is_seg_end = !block_is_dummy;
      if (i + 1 < condense_blocks_.size()) {
        Block* next_block = &condense_blocks_[i + 1];
        // this means the next block is a different address
        // if the next block has a different address, this is the end of the segment
        obliv::choice next_addr_eq(obliv::ct_eq<std::int64_t>(scan_block->address, next_block->address));
        block_is_seg_end = (!next_addr_eq) && !block_is_dummy;
      } else {
        // if at the last index, this is the end of the segment
        block_is_seg_end = !block_is_dummy;
      }

      obliv::choice hand_is_dummy(
          obliv::ct_eq<std::int64_t>(hand.address, static_cast<std::int64_t>(Block::dummy_address))
      );
      obliv::choice hand_addr_eq =
          obliv::choice(obliv::ct_eq<std::int64_t>(scan_block->address, hand.address)) && !hand_is_dummy;

      // track whether to mark this block for compaction
      std::uint8_t mark_val = 0;

      // get mod type (mutually exclusive)
      obliv::choice mod_is_create = block_is_mod_create(scan_block);
      obliv::choice mod_is_write = block_is_mod_write(scan_block);
      obliv::choice mod_is_remove = block_is_mod_remove(scan_block);

      // - STATIC BLOCK
      // if the hand is empty and this is a static block, pick it up
      obliv::choice apply_static = block_is_static_flag && hand_is_dummy;
      block_copy_cond(&hand, scan_block, apply_static.unwrap());

      // - CREATE MOD
      obliv::choice apply_create_mod = mod_is_create;
#if defined(ORAM_DEBUG)
      if (apply_create_mod.unwrap()) {
        log::ensuref(hand_is_dummy.unwrap(), "condense i=%zu: CREATE mod with non-empty hand", i);
      }
#endif
      // hand takes the block value
      block_copy_cond(&hand, scan_block, apply_create_mod.unwrap());

      // - WRITE MOD
      obliv::choice apply_write_mod = hand_addr_eq && mod_is_write;
#if defined(ORAM_DEBUG)
      if (apply_write_mod.unwrap()) {
        log::ensuref(!block_is_dummy.unwrap(), "condense i=%zu: WRITE mod with dummy block", i);
      }
#endif
      // hand takes the block value
      block_copy_cond(&hand, scan_block, apply_write_mod.unwrap());

      // - REMOVE MOD
      obliv::choice apply_remove_mod = hand_addr_eq && mod_is_remove;
#if defined(ORAM_DEBUG)
      if (apply_remove_mod.unwrap()) {
        log::ensuref(!block_is_dummy.unwrap(), "condense i=%zu: REMOVE mod with dummy block", i);
      }
#endif
      // hand is cleared
      block_make_dummy_cond(&hand, apply_remove_mod.unwrap());

      // check if the new hand is real
      obliv::choice new_hand_is_dummy(
          obliv::ct_eq<std::int64_t>(hand.address, static_cast<std::int64_t>(Block::dummy_address))
      );

      // if this is a segment end and the hand holds a real block,
      // drop the hand here (store final value) and mark for compaction
      obliv::choice drop_hand_here = block_is_seg_end && !new_hand_is_dummy;
      // conditionally copy hand to block
      block_copy_cond(scan_block, &hand, drop_hand_here.unwrap());
      // conditionally set mark value
      mark_val = drop_hand_here.unwrap();
      n_marked_blocks += mark_val;
      // otherwise clear the block
      block_make_dummy_cond(scan_block, !drop_hand_here.unwrap());
      scan_block->extra = obliv::ct_select<std::uint64_t>(0, scan_block->extra, !drop_hand_here.unwrap());
      // if we drop the hand also clear it
      block_make_dummy_cond(&hand, drop_hand_here.unwrap());
      hand.extra = obliv::ct_select<std::uint64_t>(0, hand.extra, !drop_hand_here.unwrap());
      // set marks for compaction
      condense_compact_marks_[i] = mark_val;
    }

    // check the number of marked blocks
    // ensure it will fit in the output capacity
    log::ensuref(
        n_marked_blocks <= capacity_limit, "condense: n_marked_blocks (%zu) exceeds output capacity (%zu)",
        n_marked_blocks, capacity_limit
    );
    const std::size_t retained_static = update_static ? n_marked_blocks : 0;
    held_real_blocks_.store(retained_static, std::memory_order_relaxed);

#if defined(ORAM_DEBUG)
    logger_.dbg(pfm::format("condense: n_marked_blocks=%zu", n_marked_blocks));
    if (logger_.verbosity() >= sn::util::log::level::pedantic) {
      log_condense_dump("after scan");
    }
#endif

    // compact to build a new static section
    ser::orshuffle::orcompact(
        condense_blocks_.data(), condense_blocks_.size(), condense_compact_marks_.data(),
        condense_compact_prefix_sum_.data()
    );

#if defined(ORAM_DEBUG)
    if (logger_.verbosity() >= sn::util::log::level::pedantic) {
      log_condense_dump("after compact");
    }
#endif

    // copy the compacted blocks to the output buffer
    const std::size_t copy_count = std::min<std::size_t>(n_marked_blocks, out.size());
    sn::obliv::copy(condense_blocks_.begin(), condense_blocks_.begin() + copy_count, out.begin());
    for (std::size_t i = copy_count; i < out.size(); ++i) {
      block_make_dummy(&out[i]);
    }

    // reset extra data flags, clear mods
    if (update_static) {
      // copy the static from the compacted condensed blocks (already in out buffer)
      // initialize the static blocks with correct meta
      for (std::size_t i = 0; i < static_capacity_; ++i) {
        Block* static_block = get_block(i);
        std::uint8_t static_flags = static_cast<std::uint8_t>(block_flags::block_static);
        std::uint32_t static_chrono = 0;
        static_block->extra =
            static_cast<std::uint64_t>(static_flags) | (static_cast<std::uint64_t>(static_chrono) << 8U);
      }
    } else {
      // clear the static section without updating from compaction
      for (std::size_t i = 0; i < static_capacity_; ++i) {
        Block* static_block = get_block(i);
        block_make_dummy(static_block);
        std::uint8_t static_flags = static_cast<std::uint8_t>(block_flags::block_static);
        static_block->extra = static_cast<std::uint64_t>(static_flags);
      }
    }

    // zero out the mods
    for (std::size_t i = static_capacity_; i < blocks_.size(); ++i) {
      block_make_dummy(&blocks_[i]);
    }
    mods_n_.store(0, std::memory_order_relaxed);
    mods_reserved_.store(0, std::memory_order_relaxed);

    return n_marked_blocks;
  }
  static void block_copy(Block* dst, const Block* src) { *dst = *src; }

  static void block_copy(Block* dst, const Block* src, std::size_t count) { sn::obliv::copy(src, src + count, dst); }

  static void block_copy_cond(Block* dst, const Block* src, bool cond) { obliv::ct_set_data(dst, *src, cond); }

  static void block_swap_cond(Block* a, Block* b, bool cond) { obliv::ct_swap_data(a, b, cond); }

  // reserve a mod slot for the caller; publish done separately
  std::size_t reserve_mod_slot() {
    std::size_t mod_slot = mods_reserved_.fetch_add(1, std::memory_order_relaxed);
    log::ensure(mod_slot < mods_capacity_, "add_mod: out of mod slots");
    return mod_slot;
  }

  // publish a reserved slot so readers can observe it
  // we advance mods_n_ in order so readers never touch partially written blocks
  void publish_mod_slot(std::size_t mod_slot) {
    const std::size_t published_target = mod_slot + 1U;
    std::size_t expected = mod_slot;
    while (
        !mods_n_.compare_exchange_weak(expected, published_target, std::memory_order_release, std::memory_order_relaxed)
    ) {
      expected = mod_slot;
    }
  }

  static void block_make_dummy(Block* block) {
    block->address = Block::dummy_address;
    block->leaf_ix = Block::dummy_address;
    block->extra = 0;
#if defined(ORAM_DEBUG)
    block->uid = 0;
#endif
  }

  static void block_make_dummy(Block* blocks, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      block_make_dummy(&blocks[i]);
    }
  }

  static void block_make_dummy_cond(Block* block, bool cond) {
    block->address = obliv::ct_select<std::int64_t>(Block::dummy_address, block->address, cond);
    block->leaf_ix = obliv::ct_select<std::int64_t>(Block::dummy_address, block->leaf_ix, cond);
    block->extra = obliv::ct_select<std::uint64_t>(0, block->extra, cond);
#if defined(ORAM_DEBUG)
    block->uid = obliv::ct_select<std::uint64_t>(0, block->uid, cond);
#endif
  }

#if defined(ORAM_DEBUG)
  static std::uint64_t debug_uid(const Block* block) { return block->uid; }
#else
  static std::uint64_t debug_uid([[maybe_unused]] const Block* block) { return 0; }
#endif
};

} // namespace sn::oram::stash::core
