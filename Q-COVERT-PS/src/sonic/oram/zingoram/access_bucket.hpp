// raii helpers for accessing a bucket
// we cooperate with the bucket's sync metadata to maintain invariants:
// - each access takes a unique ticket and waits for its epoch
// - the ticket's rank points to a unique dummy slot
// - the final ticket in the epoch (the bucket's last touch) becomes the owner
// - the owner waits for exclusivity then performs the reshuffle
// - the owner then reopens for the next epoch and publishes progress
// we keep it lock-free using atomics and proper memory ordering

#pragma once

#include <cstdint>

#include "sonic/obliv/types/choice.hpp"
#include "sonic/threads/sync.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/span.hpp"

#include "sonic/oram/zingoram/bucket.hpp"
#include "sonic/oram/zingoram/detail/epoch_state.hpp"
#include "sonic/oram/zingoram/state.hpp"

namespace sn::oram::zingoram {

enum class bucket_access_mode { serial, concurrent };

namespace detail {

#if defined(ORAM_DEBUG)
// sanity check that epoch span matches bucket layout
template <typename Bucket>
inline void validate_epoch_contract(const Bucket& bucket, const detail::bucket_epoch_state& epoch) {
  sn::util::log::ensure(
      epoch.span() == bucket.max_touch_count(), "zingoram::bucket_access: epoch span must match bucket dummy slots"
  );
}
#endif

// convert epoch ticket to sync-free access view
inline bucket_access_view to_access_view(const detail::epoch_ticket& t) noexcept {
  bucket_access_view v{};
  v.span = t.span;
  v.rank = t.rank();
  return v;
}

// perform access to bucket, return {block, is_real}
template <typename Bucket>
inline std::pair<typename Bucket::block_t, sn::obliv::choice> perform_access(
    Bucket& bucket, const detail::epoch_ticket& ticket, std::int64_t address
) {
  using block_t = typename Bucket::block_t;
  block_t out{};
  const auto view = to_access_view(ticket);
  const bool is_real = bucket.access_block(out, address, view);
  return {out, sn::obliv::choice(is_real)};
}

enum class wait_mode { no_wait, wait_for_peers };

template <bucket_access_mode Mode> constexpr wait_mode wait_policy() noexcept {
  if constexpr (Mode == bucket_access_mode::concurrent) {
    return wait_mode::wait_for_peers;
  }
  return wait_mode::no_wait;
}

template <typename Traits> struct owner_context {
  typename state<Traits>::access_scratch& scratch;
  sn::oram::uid_generator& uid_gen;
  sn::crypto::buffered_prng<>& prng;
};

// owner path helpers: wait -> drain -> rebuild -> publish
inline void owner_wait(detail::bucket_epoch_state& epoch, const detail::epoch_ticket& ticket, wait_mode mode) {
  if (mode == wait_mode::wait_for_peers) {
    sn_prof_zone("zingoram.bucket.owner_wait");
    epoch.wait_for_epoch_completion(ticket);
  }
}

// owner, drain any remaining valid real slots into scratch
template <typename Traits>
inline sn::util::span<const typename Traits::block_t> owner_drain(
    typename Traits::bucket_t& bucket, const owner_context<Traits>& ctx
) {
  sn_prof_zone("zingoram.bucket.owner_read");
  using block_t = typename Traits::block_t;
  const std::uint32_t real_slots = bucket.real_slots();
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(real_slots > 0, "zingoram::owner_drain: real_slots must be positive");
  sn::util::log::ensure(
      ctx.scratch.bucket_real_buf.size() >= real_slots, "zingoram::owner_drain: real buffer too small"
  );
  sn::util::log::ensure(
      ctx.scratch.bucket_offset_buf.size() >= real_slots, "zingoram::owner_drain: offset buffer too small"
  );
#endif

  // retrieve any remaining valid real slots
  bucket.read_bucket_max(ctx.scratch.bucket_real_buf.data(), ctx.scratch.bucket_offset_buf.data());
  return sn::util::span<const block_t>(ctx.scratch.bucket_real_buf.data(), static_cast<std::size_t>(real_slots));
}

// owner, rebuild the bucket from real slots
template <typename Traits>
inline void owner_rebuild(
    typename Traits::bucket_t& bucket, const owner_context<Traits>& ctx,
    sn::util::span<const typename Traits::block_t> real_span
) {
  sn_prof_zone("zingoram.bucket.owner_rebuild");
  bucket.rebuild(real_span, ctx.uid_gen, ctx.prng);
}

// owner, publish a new epoch after rebuild
inline void owner_publish(
    detail::bucket_epoch_state& epoch, const detail::epoch_ticket& ticket, bucket_access_mode mode
) {
  sn_prof_zone("zingoram.bucket.publish_next_epoch");
  if (mode == bucket_access_mode::serial) {
    epoch.serial_publish_next_epoch();
  } else {
    epoch.publish_next_epoch(ticket);
  }
}

// owner, finalize an access: wait, drain, rebuild, publish
template <typename Traits>
inline void owner_finalize(
    typename Traits::bucket_t& bucket, detail::bucket_epoch_state& epoch, const owner_context<Traits>& ctx,
    const detail::epoch_ticket& ticket, wait_mode wait, bucket_access_mode publish
) {
  sn_prof_zone("zingoram.bucket.owner_finalize");

  // wait for other accesses
  owner_wait(epoch, ticket, wait);

  // drain remaining real slots
  auto real_span = owner_drain<Traits>(bucket, ctx);

  // rebuild bucket
  owner_rebuild<Traits>(bucket, ctx, real_span);

  // publish new epoch
  owner_publish(epoch, ticket, publish);
}
} // namespace detail

// unified raii guard; policy via Mode
template <bucket_access_mode Mode, typename Traits> class bucket_access_guard {
public:
  using bucket_t = typename Traits::bucket_t;
  using block_t = typename Traits::block_t;
  using access_scratch = typename state<Traits>::access_scratch;

  bucket_access_guard(
      bucket_t& bucket, detail::bucket_epoch_state& epoch, access_scratch& scratch, sn::oram::uid_generator& uid_gen,
      sn::crypto::buffered_prng<>& prng
  ) :
      bucket_(bucket), epoch_(epoch), scratch_(scratch), uid_gen_(uid_gen), prng_(prng) {
    if constexpr (Mode == bucket_access_mode::concurrent) {
      sn_prof_zone("zingoram.bucket.acquire_ticket.concurrent");
      ticket_ = epoch_.acquire_epoch_ticket(std::memory_order_relaxed);
    } else {
      ticket_ = epoch_.serial_acquire_ticket();
    }
#if defined(ORAM_DEBUG)
    detail::validate_epoch_contract(bucket_, epoch_);
#endif
    owner_ = ticket_.is_owner();
  }

  ~bucket_access_guard() {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(accessed_, "zingoram::bucket_access_guard: finish called before access");
#endif

    if constexpr (Mode == bucket_access_mode::concurrent) {
      sn_prof_zone("zingoram.bucket.mark_ticket_complete");
      epoch_.mark_ticket_completed(ticket_);
    }

    if (owner_) {
      detail::owner_context<Traits> ctx{scratch_, uid_gen_, prng_};
      constexpr detail::wait_mode wait = detail::wait_policy<Mode>();
      detail::owner_finalize<Traits>(bucket_, epoch_, ctx, ticket_, wait, Mode);
    }
  }

  bucket_access_guard(const bucket_access_guard&) = delete;
  bucket_access_guard& operator=(const bucket_access_guard&) = delete;

  block_t access(std::int64_t address) {
    if constexpr (Mode == bucket_access_mode::concurrent) {
      sn_prof_zone("zingoram.bucket.access.concurrent");
    } else {
      sn_prof_zone("zingoram.bucket.access.serial");
    }
    auto [blk, was_real] = detail::perform_access(bucket_, ticket_, address);
    last_was_real_ = was_real;
    accessed_ = true;
    return blk;
  }

  sn::obliv::choice last_was_real() const noexcept { return last_was_real_; }

private:
  bucket_t& bucket_;
  detail::bucket_epoch_state& epoch_;
  access_scratch& scratch_;
  sn::oram::uid_generator& uid_gen_;
  sn::crypto::buffered_prng<>& prng_;
  bool owner_ = false;
  bool accessed_ = false;
  sn::obliv::choice last_was_real_ = sn::obliv::choice::false_value();
  detail::epoch_ticket ticket_{};
};

// helper to access bucket with guard; returns {block, is_real}
template <typename Traits>
inline std::pair<typename Traits::block_t, sn::obliv::choice> guarded_bucket_access(
    typename Traits::bucket_t& bucket, detail::bucket_epoch_state& epoch,
    typename state<Traits>::access_scratch& scratch, sn::oram::uid_generator& uid_gen,
    sn::crypto::buffered_prng<>& prng, std::int64_t address, bucket_access_mode mode
) {
  if (mode == bucket_access_mode::serial) {
    bucket_access_guard<bucket_access_mode::serial, Traits> guard(bucket, epoch, scratch, uid_gen, prng);
    return {guard.access(address), guard.last_was_real()};
  } else {
    bucket_access_guard<bucket_access_mode::concurrent, Traits> guard(bucket, epoch, scratch, uid_gen, prng);
    return {guard.access(address), guard.last_was_real()};
  }
}

} // namespace sn::oram::zingoram
