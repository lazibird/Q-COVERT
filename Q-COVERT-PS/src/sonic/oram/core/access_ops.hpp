#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#if defined(ORAM_DEBUG)
#include <string>
#include "sonic/oram/tree/assigned_block_map.hpp"
#endif

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/word_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/oram/core/access.hpp"
#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram {

namespace detail {

template <typename Block> constexpr void validate_block_layout() {
  // validate the block type meets layout requirements
  static_assert(Block::byte_size % sizeof(std::uint64_t) == 0, "access: block size must be divisible by 8 bytes");
  static_assert(alignof(Block) >= alignof(std::uint64_t), "access: block alignment must support 64-bit access");
}

} // namespace detail

// ensure the validity of an access request
template <typename Block>
inline void validate_access(std::int64_t block_count, std::int64_t leaf_count, const access_request& req) {
  detail::validate_block_layout<Block>();
  constexpr std::size_t block_bytes = Block::byte_size;
  constexpr std::int64_t dummy_address = Block::dummy_address;

  const bool address_in_range = (req.address >= 0) & (req.address < block_count);
  const bool address_is_dummy = req.address == dummy_address;
  sn::util::log::ensure(address_in_range | address_is_dummy, "access: address out of range");
  sn::util::log::ensure(req.cur_leaf >= 0 && req.cur_leaf < leaf_count, "access: current leaf out of range");
  sn::util::log::ensure(req.new_leaf >= 0 && req.new_leaf < leaf_count, "access: new leaf out of range");

  sn::util::log::ensure(!req.in.empty() && req.in.size() == block_bytes, "access: request input must match block size");
  sn::util::log::ensure(
      !req.out.empty() && req.out.size() == block_bytes, "access: request output must match block size"
  );
}

// default mutator: read/write semantics
struct read_write_mutator {
  template <typename Block> void operator()(Block& block, const access_request& req) const noexcept {
    sn::obliv::choice is_write(req.is_write);
    sn::obliv::choice is_read = !is_write;

    // if read, conditionally copy block data to output buffer
    sn::obliv::ct_set_words<Block::byte_size>(req.out.data(), block.data.data(), is_read.unwrap());
    // if write, conditionally copy input buffer to block data
    sn::obliv::ct_set_words<Block::byte_size>(block.data.data(), req.in.data(), is_write.unwrap());
  }
};

// apply mutator to a block in-place
template <typename Block, typename Mutator = read_write_mutator>
inline void apply_access(Block& block, const access_request& req, Mutator&& mutator = Mutator{}) {
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(block.address == req.address, "access: block address mismatch");
#endif
  // mutator mutates block data; leaf updated unconditionally
  std::forward<Mutator>(mutator)(block, req);
  block.leaf_ix = req.new_leaf;
}

// conditionally materialize a block as real and assigned to an address
template <typename Block>
inline void materialize_block_cond(
    Block& block, std::int64_t address, [[maybe_unused]] uid_generator& uid_gen,
    const sn::obliv::choice& should_materialize
) {
  // conditionally materialize into a real block with the target address
  sn::obliv::ct_set(&block.address, address, should_materialize.unwrap());
  sn::obliv::ct_set(&block.leaf_ix, static_cast<std::int64_t>(-1), should_materialize.unwrap());

#if defined(ORAM_DEBUG)
  if (should_materialize.unwrap()) {
    // set uid in debug mode
    block.uid = uid_gen.next();
  }
#endif
}

#if defined(ORAM_DEBUG)
namespace access_debug {

inline void verify_materialization_consistency(
    tree::assigned_block_map& assignments, std::uint64_t address, const sn::obliv::choice& target_is_dummy,
    const char* context_label
) {
  if (!target_is_dummy.unwrap()) {
    return;
  }

  if (!assignments.is_materialized(address)) {
    return;
  }

  const auto prev_uid = assignments.recorded_uid(address);
  sn::util::log::failf(
      "%s: previously materialized block#%llu for address=$%08llx was lost", context_label,
      static_cast<std::uint64_t>(prev_uid), static_cast<std::uint64_t>(address)
  );
}

template <typename Block>
inline void record_materialization(
    tree::assigned_block_map& assignments, std::uint64_t address, const Block& block, const char* context_label
) {
  const std::string ensure_message = std::string(context_label) + ": block should be real after materialization";
  sn::util::log::ensure(block.is_real().unwrap(), ensure_message);
  assignments.record(address, block.uid);
}

} // namespace access_debug
#endif

} // namespace sn::oram
