#pragma once

#include <algorithm>
#include <cstdint>

#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/util/log.hpp"
#include "sonic/oram/zingoram/detail/epoch_sync.hpp"
#include "sonic/oram/zingoram/detail/epoch_types.hpp"

namespace sn::oram::zingoram::detail {

struct serial_epoch {
  std::uint64_t base = 0;
  std::uint8_t phase = 0;
  std::uint32_t next_rank = 0;

  void reset(std::uint64_t new_base = 0, std::uint8_t new_phase = 0) noexcept {
    base = new_base;
    phase = new_phase;
    next_rank = 0;
  }

  epoch_ticket acquire(std::uint32_t span) noexcept {
    sn::util::log::ensure(span > 0U, "bucket_epoch_state: span must be positive");
    sn::util::log::ensure(next_rank < span, "bucket_epoch_state: rank exceeded span");
    epoch_ticket t{};
    t.span = span;
    t.base = base;
    t.phase = phase;
    t.ticket = base + static_cast<std::uint64_t>(next_rank);
    ++next_rank;
    return t;
  }

  void publish_next(std::uint32_t span) noexcept {
    if (span == 0U) {
      return;
    }
    base += static_cast<std::uint64_t>(span);
    next_rank = 0;
    phase = static_cast<std::uint8_t>(phase ^ 1U);
  }
};

// high-level epoch state and logic
class bucket_epoch_state {
public:
  bucket_epoch_state() = default;
  bucket_epoch_state(const bucket_epoch_state&) = delete;
  bucket_epoch_state& operator=(const bucket_epoch_state&) = delete;

  bucket_epoch_state(bucket_epoch_state&& other) noexcept { move_from(std::move(other)); }
  bucket_epoch_state& operator=(bucket_epoch_state&& other) noexcept {
    if (this != &other) {
      move_from(std::move(other));
    }
    return *this;
  }

  // bucket_heap compatibility; no-op
  void fill_dummy(sn::oram::uid_generator&) noexcept {}

  void configure(std::uint32_t span) {
    // span must match bucket dummy slot count
    sync_.configure(span);
    serial_.reset();
  }

  void reset() noexcept {
    sync_.reset();
    serial_.reset();
  }

  // concurrent path
  [[nodiscard]] std::uint64_t next_ticket(std::memory_order order = std::memory_order_relaxed) noexcept {
    return sync_.next_ticket(order);
  }

  [[nodiscard]] epoch_ticket acquire_epoch_ticket(std::memory_order order = std::memory_order_relaxed) noexcept {
    const std::uint64_t raw = next_ticket(order);
    return wait_for_ticket(raw);
  }

  [[nodiscard]] epoch_ticket wait_for_ticket(std::uint64_t raw_ticket) const noexcept {
    epoch_ticket t{};
    t.ticket = raw_ticket;
    t.span = span();
    const auto admission = sync_.admit(raw_ticket);
    t.base = admission.base;
    t.phase = admission.phase;
    return t;
  }

  void mark_ticket_completed(const epoch_ticket& ticket) noexcept { sync_.mark_done(ticket.rank(), ticket.phase); }

  // owner ops
  void wait_for_epoch_completion(std::uint32_t skip_rank, std::uint8_t phase) noexcept {
    sync_.wait_for_epoch(skip_rank, phase);
  }
  void wait_for_epoch_completion(const epoch_ticket& ticket) noexcept {
    wait_for_epoch_completion(ticket.rank(), ticket.phase);
  }

  void publish_next_epoch(const epoch_ticket& ticket) noexcept {
    const std::uint64_t next_base = ticket.base + static_cast<std::uint64_t>(ticket.span);
    const std::uint8_t next_phase = static_cast<std::uint8_t>(ticket.phase ^ 1U);
    sync_.publish(next_base, next_phase);
  }

  // serial path
  [[nodiscard]] epoch_ticket serial_acquire_ticket() noexcept { return serial_.acquire(span()); }

  void serial_publish_next_epoch() noexcept { serial_.publish_next(span()); }

  // diagnostics
  [[nodiscard]] std::uint32_t span() const noexcept { return sync_.span(); }

  [[nodiscard]] std::uint64_t issued(std::memory_order order = std::memory_order_relaxed) const noexcept {
    return sync_.issued(order);
  }
  [[nodiscard]] std::uint64_t publication(std::memory_order order = std::memory_order_relaxed) const noexcept {
    return sync_.publication(order);
  }

  [[nodiscard]] std::uint32_t admitted_in_epoch(std::memory_order order = std::memory_order_relaxed) const noexcept {
    const std::uint32_t local_span = span();
    if (local_span == 0U) {
      return 0;
    }
    const std::uint64_t publication_snapshot = sync_.publication(order);
    const std::uint64_t base = bucket_epoch_sync::decode_base(publication_snapshot);
    const std::uint64_t issued_count = sync_.issued(order);
    if (issued_count <= base) {
      return 0;
    }
    const std::uint64_t delta = issued_count - base;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(delta, local_span));
  }

  [[nodiscard]] bool needs_reshuffle(std::memory_order order = std::memory_order_relaxed) const noexcept {
    return admitted_in_epoch(order) >= span();
  }

  struct epoch_span_snapshot {
    std::uint64_t base = 0;
    std::uint32_t span = 0;
  };
  [[nodiscard]] epoch_span_snapshot current_epoch_span(
      std::memory_order order = std::memory_order_acquire
  ) const noexcept {
    return {bucket_epoch_sync::decode_base(sync_.publication(order)), span()};
  }

  [[nodiscard]] std::uint64_t load_epoch_base(std::memory_order order = std::memory_order_relaxed) const noexcept {
    return bucket_epoch_sync::decode_base(sync_.publication(order));
  }
  [[nodiscard]] std::uint64_t load_epoch_limit(std::memory_order order = std::memory_order_relaxed) const noexcept {
    return load_epoch_base(order) + static_cast<std::uint64_t>(span());
  }

  // publish epoch limit after eviction rebuild, which can happen at arbitrary touch count
  void publish_after_eviction_rebuild() noexcept {
    const std::uint32_t local_span = span();
    const std::uint64_t publication_snapshot = sync_.publication(std::memory_order_acquire);
    const std::uint64_t base = bucket_epoch_sync::decode_base(publication_snapshot);
    const std::uint8_t phase = bucket_epoch_sync::decode_phase(publication_snapshot);
    const std::uint64_t limit = base + static_cast<std::uint64_t>(local_span);
    const std::uint64_t issued_count = sync_.issued(std::memory_order_acquire);
    sn::util::log::ensure(issued_count >= base, "bucket_epoch_state: eviction publish saw issued < base");
    const std::uint64_t next_base = std::min(limit, issued_count);
    // force mismatch so next owner waits on fresh completions
    sync_.force_mismatch(phase);
    sync_.publish(next_base, phase);
    serial_.reset(next_base, phase);
  }

private:
  void move_from(bucket_epoch_state&& other) noexcept {
    sync_ = std::move(other.sync_);
    serial_ = other.serial_;
    other.serial_.reset();
  }

  bucket_epoch_sync sync_{};
  serial_epoch serial_{};
};

} // namespace sn::oram::zingoram::detail
