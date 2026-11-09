#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sonic/util/log.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"

namespace sn::oram::tree {

// track which blocks have been materialized, mapping address -> uid
class assigned_block_map {
public:
  // configure for a given block count
  void configure(std::uint64_t block_count) {
    materialized_.assign(static_cast<std::size_t>(block_count), false);
    uids_.assign(static_cast<std::size_t>(block_count), 0);
  }

  // mark all blocks as unmaterialized
  void reset() {
    std::fill(materialized_.begin(), materialized_.end(), false);
    sn::obliv::fill(uids_.begin(), uids_.end(), 0);
  }

  // whether an address has been materialized to a real block
  bool is_materialized(std::uint64_t address) const {
    sn::util::log::ensure(address < materialized_.size(), "assigned_block_map: address out of range");
    return materialized_[static_cast<std::size_t>(address)];
  }

  // the uid that an address has been materialized to
  std::uint64_t recorded_uid(std::uint64_t address) const {
    sn::util::log::ensure(address < uids_.size(), "assigned_block_map: address out of range");
    return uids_[static_cast<std::size_t>(address)];
  }

  // record that an address has been materialized to a block with the given uid
  void record(std::uint64_t address, std::uint64_t uid) {
    sn::util::log::ensure(address < materialized_.size(), "assigned_block_map: address out of range");
    materialized_[static_cast<std::size_t>(address)] = true;
    uids_[static_cast<std::size_t>(address)] = uid;
  }

private:
  // a bitmap of whether each address has been materialized
  std::vector<bool> materialized_{};
  // the uid that each block address has been materialized to
  std::vector<std::uint64_t> uids_{};
};

} // namespace sn::oram::tree
