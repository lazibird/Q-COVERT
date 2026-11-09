#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "sonic/oram/core/access.hpp"
#include "sonic/oram/zingoram/client.hpp"
#include "sonic/oram/zingoram/options.hpp"
#include "sonic/oram/zingoram/threading.hpp"
#include "sonic/oram/zingoram/traits.hpp"
#include "sonic/threads/thread_team.hpp"

namespace ringdoram {

using access_request = sn::oram::access_request;
using batch_request = access_request;
using options = sn::oram::zingoram::options;
using eviction_backend = sn::oram::zingoram::eviction_backend;
using epoch_mode = sn::oram::zingoram::epoch_mode;
inline constexpr std::int64_t dummy_address = -1;

template <std::size_t BlockBytes> using bulk_record = sn::oram::zingoram::bulk_record<BlockBytes>;

template <
    std::size_t BlockBytes, epoch_mode Mode = epoch_mode::default_epoch,
    template <typename, typename...> class Store = sn::oram::zingoram::storage::slab_store>
using traits = sn::oram::zingoram::traits<BlockBytes, Mode, Store>;

template <typename Traits> using client = sn::oram::zingoram::client<Traits>;

enum class batch_close {
  keep_open,
  flush,
};

template <typename Client> class batch_workspace {
public:
  using access_scratch = typename Client::access_scratch;

  void configure(Client& oram, std::size_t lanes) {
    scratch_.resize(lanes);
    for (auto& scratch : scratch_) {
      oram.configure_access_scratch(scratch);
    }
  }

  [[nodiscard]] std::size_t lanes() const noexcept { return scratch_.size(); }

private:
  template <typename C>
  friend void AccessBatch(
      C&, std::span<const access_request>, batch_workspace<C>&, sn::threads::thread_team&, batch_close
  );

  std::vector<access_scratch> scratch_{};
};

template <typename Client>
inline void AccessBatch(
    Client& oram, std::span<const access_request> requests, batch_workspace<Client>& workspace,
    sn::threads::thread_team& workers, batch_close close = batch_close::keep_open
) {
  if constexpr (!Client::disjoint_epoch_mode) {
    if (close == batch_close::flush) {
      throw std::logic_error("AccessBatch: explicit flush requires disjoint_epoch mode");
    }
  }
  if (requests.size() > workspace.scratch_.size()) {
    throw std::invalid_argument("AccessBatch: insufficient scratch lanes");
  }
  if constexpr (Client::disjoint_epoch_mode) {
    const std::size_t pending = oram.pending_epoch_accesses();
    const std::size_t window = static_cast<std::size_t>(oram.options().disjoint_epoch_window);
    if (requests.size() > window - pending) {
      throw std::invalid_argument("AccessBatch: requests exceed the remaining disjoint epoch window");
    }
  }
  if (requests.empty()) {
    if (close == batch_close::flush) {
      if constexpr (Client::disjoint_epoch_mode) {
        oram.flush_epoch();
      }
    }
    return;
  }

  for (const auto& request : requests) {
    sn::oram::validate_access<typename Client::block_t>(
        static_cast<std::int64_t>(oram.options().block_count),
        static_cast<std::int64_t>(oram.shape().leaf_count), request
    );
  }

#if !defined(NDEBUG) || defined(RINGDORAM_ENABLE_CONTRACT_CHECKS)
  std::unordered_set<std::int64_t> addresses;
  addresses.reserve(requests.size());
  for (const auto& request : requests) {
    if (request.address >= 0 && !addresses.insert(request.address).second) {
      throw std::invalid_argument("AccessBatch: real addresses must be distinct within a batch");
    }
  }
#endif

  workers.parallel_for<std::size_t>(0, requests.size(), [&](std::size_t index) noexcept {
    oram.access(requests[index], workspace.scratch_[index]);
  });

  if (close == batch_close::flush) {
    if constexpr (Client::disjoint_epoch_mode) {
      oram.flush_epoch();
    }
  }
}

template <typename Client>
inline void access_batch(
    Client& oram, std::span<const batch_request> requests, batch_workspace<Client>& workspace,
    sn::threads::thread_team& workers, batch_close close = batch_close::keep_open
) {
  AccessBatch(oram, requests, workspace, workers, close);
}

} // namespace ringdoram
