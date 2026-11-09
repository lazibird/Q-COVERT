#pragma once

#include <cstddef>
#include <cstdint>
#include "sonic/oram/stash/forestzing/stash.hpp"
#include "sonic/oram/storage/slab_store.hpp"
#include "sonic/oram/tree/block.hpp"
#include "sonic/oram/zingoram/bucket.hpp"
#include "sonic/oram/zingoram/options.hpp"

namespace sn::oram::zingoram {

enum class epoch_mode : std::uint8_t {
  default_epoch = 0,
  disjoint_epoch = 1,
};

template <
    std::size_t BlockBytes, epoch_mode Mode = epoch_mode::default_epoch,
    template <typename, typename...> class BlockStoreT = sn::oram::zingoram::storage::slab_store>
struct traits {
  static constexpr std::size_t block_bytes = BlockBytes;
  static constexpr epoch_mode mode = Mode;
  static constexpr bool disjoint_epoch_mode = Mode == epoch_mode::disjoint_epoch;

  using block_t = sn::oram::tree::block<BlockBytes>;
  using options_t = options;
  using block_store_t = BlockStoreT<block_t>;
  using bucket_t = sn::oram::zingoram::bucket<block_t, block_store_t>;
  using stash_t = sn::oram::stash::forestzing::stash<block_t>;
};

} // namespace sn::oram::zingoram
