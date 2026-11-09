#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/oram/tree/block.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/span.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/struct_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
namespace sn::oram::stash::core {

namespace obliv = sn::obliv;

namespace log = sn::util::log;

// contiguous storage for stash blocks, with optional section views
template <typename Block> class linear_block_storage {
public:
  struct section {
    std::size_t offset = 0;
    std::size_t length = 0;

    [[nodiscard]] bool empty() const noexcept { return length == 0; }

    [[nodiscard]] sn::util::span<Block> span(linear_block_storage& storage) const {
      return storage.slice(offset, length);
    }

    [[nodiscard]] sn::util::span<const Block> span(const linear_block_storage& storage) const {
      return storage.slice(offset, length);
    }
  };

  linear_block_storage(std::size_t total_slots, sn::oram::uid_generator& uid_gen, sn::util::log::logger logger) :
      uid_gen_(uid_gen), log_(std::move(logger)), slots_(total_slots) {
    log::ensure(total_slots > 0, "linear_block_storage: total_slots must be positive");
    fill_dummy();
  }

  [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

  section make_section(std::size_t offset, std::size_t length) const {
    log::ensure(offset + length <= slots_.size(), "linear_block_storage: section exceeds bounds");
    return section{offset, length};
  }

  [[nodiscard]] sn::util::span<Block> span() noexcept { return sn::util::span<Block>(slots_); }
  [[nodiscard]] sn::util::span<const Block> span() const noexcept { return sn::util::span<const Block>(slots_); }

  [[nodiscard]] sn::util::span<Block> slice(std::size_t offset, std::size_t count) {
    ensure_slice_bounds(offset, count);
    return {slots_.data() + offset, count};
  }

  [[nodiscard]] sn::util::span<const Block> slice(std::size_t offset, std::size_t count) const {
    auto* self = const_cast<linear_block_storage*>(this);
    auto mutable_span = self->slice(offset, count);
    return sn::util::span<const Block>(mutable_span.data(), mutable_span.size());
  }

  [[nodiscard]] Block& at(std::size_t index) {
    ensure_index(index);
    return slots_[index];
  }

  [[nodiscard]] const Block& at(std::size_t index) const { return const_cast<linear_block_storage*>(this)->at(index); }

  void set(std::size_t index, const Block& value) { at(index) = value; }

  void set(std::size_t index, Block&& value) { at(index) = std::move(value); }

  void swap(std::size_t lhs, std::size_t rhs) {
    if (lhs == rhs) {
      return;
    }
    ensure_index(lhs);
    ensure_index(rhs);
    std::swap(slots_[lhs], slots_[rhs]);
  }

  void fill_dummy() {
    sn_prof_zone("linear_block_storage.fill_dummy");
    for (auto& slot : slots_) {
      slot.set_dummy(uid_gen_);
    }
  }

  void clear_section(const section& target) {
    sn_prof_zone("linear_block_storage.clear_section");
    auto region = target.span(*this);
    for (auto& slot : region) {
      slot.set_dummy(uid_gen_);
    }
  }

  // add a block to the first available slot
  obliv::choice add(Block& hand, const section& target) {
    sn_prof_zone("linear_block_storage.add");
#if defined(ORAM_DEBUG)
    log_.pedf("storage.add offset=%d length=%d", target.offset, target.length);
#endif
    auto region = target.span(*this);
    const bool initial_hand_real = hand.is_real().unwrap();
    obliv::choice insert_done(initial_hand_real ? obliv::choice(false) : obliv::choice(true));
    for (auto& slot : region) {
      const obliv::choice hand_is_real(hand.is_real());
      const obliv::choice slot_open(slot.is_dummy());

      // determine if we should move the hand into this slot
      const obliv::choice should_insert(hand_is_real && slot_open && !insert_done);

#if defined(ORAM_DEBUG)
      if (hand_is_real.unwrap()) {
        const bool slot_is_occupied = !slot_open.unwrap();
        if (slot_is_occupied) {
          log::ensure(
              slot.address != hand.address, "linear_block_storage.add: duplicate block detected in target section"
          );
        }
      }
#endif
      // conditionally move hand into slot
      obliv::ct_set_data(&slot, hand, should_insert.unwrap());

      insert_done = insert_done || should_insert;
    }

#if defined(ORAM_DEBUG)
    if (initial_hand_real) {
      log::ensure(insert_done.unwrap(), "linear_block_storage.add: no space in target section");
    }
#endif

    return insert_done;
  }

  // read a block by address, optionally removing it
  Block read(std::int64_t address, obliv::choice remove, const section& target) {
    sn_prof_zone("linear_block_storage.read");
#if defined(ORAM_DEBUG)
    log_.pedf(
        "storage.read addr=%d offset=%d length=%d remove=%d", address, target.offset, target.length, remove.unwrap()
    );
#endif
    auto region = target.span(*this);
    Block result = Block::make_dummy(uid_gen_);
    obliv::choice found(false);
#if defined(ORAM_DEBUG)
    std::uint32_t match_count = 0;
#endif

    for (auto& slot : region) {
      const obliv::choice slot_is_real(slot.is_real());
      const obliv::choice address_matches(obliv::ct_eq(slot.address, address));
      const obliv::choice slot_matches(slot_is_real && address_matches);

      // conditionally copy the matched slot into the result
      obliv::ct_set_data(&result, slot, slot_matches.unwrap());

      // conditionally clear the matched slot if requested
      const obliv::choice should_clear(slot_matches && remove);
      slot.set_dummy_cond(should_clear, uid_gen_);

      found = found || slot_matches;

#if defined(ORAM_DEBUG)
      if (slot_matches.unwrap()) {
        ++match_count;
      }
#endif
    }

#if defined(ORAM_DEBUG)
    if (!found.unwrap()) {
      log_.pedf("storage.read addr=%d not found", address);
    }
    log::ensure(match_count <= 1, "linear_block_storage.read: duplicate matches detected");
#endif
    return result;
  }

  obliv::choice update(const Block& replacement, const section& target) {
    sn_prof_zone("linear_block_storage.update");
#if defined(ORAM_DEBUG)
    log_.pedf("storage.update addr=%d offset=%d length=%d ", replacement.address, target.offset, target.length);
#endif
    if (!replacement.is_real().unwrap()) {
#if defined(ORAM_DEBUG)
      log_.dbg("storage.update: replacement is dummy; no-op");
#endif
      return obliv::choice(false);
    }
    auto region = target.span(*this);
    obliv::choice updated(false);
#if defined(ORAM_DEBUG)
    std::uint32_t match_count = 0;
#endif

    for (auto& slot : region) {
      const obliv::choice slot_is_real(slot.is_real());
      const obliv::choice address_matches(obliv::ct_eq(slot.address, replacement.address));
      const obliv::choice slot_matches(slot_is_real && address_matches);

      // replace matched slot with provided block
      obliv::ct_set_data(&slot, replacement, slot_matches.unwrap());

      updated = updated || slot_matches;

#if defined(ORAM_DEBUG)
      if (slot_matches.unwrap()) {
        ++match_count;
      }
#endif
    }

#if defined(ORAM_DEBUG)
    if (!updated.unwrap()) {
      log_.pedf("storage.update addr=%d not found", replacement.address);
    }
    log::ensure(match_count <= 1, "linear_block_storage.update: duplicate matches detected");
#endif

    return updated;
  }

#if defined(ORAM_DEBUG)
  void dump_layout(std::string_view tag) const { log_.pedf("storage[%s] size=%d", tag, slots_.size()); }
#endif

private:
  sn::oram::uid_generator& uid_gen_;
  sn::util::log::logger log_;
  std::vector<Block> slots_;

  void ensure_index(std::size_t index) const {
    log::ensure(index < slots_.size(), "linear_block_storage: index out of bounds");
  }

  void ensure_slice_bounds(std::size_t offset, std::size_t count) const {
    const std::size_t size = slots_.size();
    log::ensure(offset <= size, "linear_block_storage: slice offset out of range");
    const std::size_t remaining = size - offset;
    log::ensure(count <= remaining, "linear_block_storage: slice length out of range");
  }
};

} // namespace sn::oram::stash::core
