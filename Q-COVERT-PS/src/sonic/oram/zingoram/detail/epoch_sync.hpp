#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "sonic/threads/sync.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::zingoram::detail {

// lock-free epoch protocol:
// the goal: lock-free, linearizable access to block offsets, with periodic reshuffle
// - tickets are issued monotonically; admit spins until ticket is in [base, base+span)
// - rank = ticket - base; owner is rank == span-1
// - each touch marks done at its rank; owner waits for all ranks then publishes next window
// - publish advances base by span and flips phase; admit uses acquire, publish uses release
// - mark_done uses release, wait_for_epoch uses acquire so owner drain/rebuild sees prior bucket mutations

class bucket_epoch_sync {
public:
  bucket_epoch_sync() = default;

  bucket_epoch_sync(const bucket_epoch_sync&) = delete;
  bucket_epoch_sync& operator=(const bucket_epoch_sync&) = delete;

  bucket_epoch_sync(bucket_epoch_sync&& other) noexcept { move_from(std::move(other)); }
  bucket_epoch_sync& operator=(bucket_epoch_sync&& other) noexcept {
    if (this != &other) {
      cleanup();
      move_from(std::move(other));
    }
    return *this;
  }

  ~bucket_epoch_sync() { cleanup(); }

  struct admission {
    std::uint64_t base = 0;
    std::uint8_t phase = 0;
  };

  void configure(std::uint32_t span);

  void reset() noexcept;

  [[nodiscard]] std::uint32_t span() const noexcept { return span_; }

  [[nodiscard]] std::uint64_t next_ticket(std::memory_order order) noexcept {
    return tickets_issued_.fetch_add(1ULL, order);
  }

  [[nodiscard]] std::uint64_t issued(std::memory_order order) const noexcept { return tickets_issued_.load(order); }

  // packed publication value: (base << 1) | phase
  [[nodiscard]] std::uint64_t publication(std::memory_order order) const noexcept {
    return epoch_publication_.load(order);
  }

  [[nodiscard]] admission admit(std::uint64_t ticket) const noexcept;

  void mark_done(std::uint32_t rank, std::uint8_t phase) noexcept;
  void wait_for_epoch(std::uint32_t skip_rank, std::uint8_t phase) noexcept;

  void publish(std::uint64_t base, std::uint8_t phase) noexcept;
  void force_mismatch(std::uint8_t phase) noexcept;

  static constexpr std::uint64_t encode(std::uint64_t base, std::uint8_t phase) noexcept {
    return (base << 1U) | static_cast<std::uint64_t>(phase & 1U);
  }

  // initial state: base=0, phase=1 so first owner sees a mismatch
  static constexpr std::uint64_t initial_publication() noexcept { return encode(0ULL, 1U); }

  static constexpr std::uint64_t decode_base(std::uint64_t publication) noexcept { return publication >> 1U; }

  static constexpr std::uint8_t decode_phase(std::uint64_t publication) noexcept {
    return static_cast<std::uint8_t>(publication & 1U);
  }

private:
  class phase_flag {
  public:
    phase_flag() noexcept = default;
    phase_flag(const phase_flag&) = delete;
    phase_flag& operator=(const phase_flag&) = delete;

    void store(std::uint8_t value, std::memory_order order) noexcept { storage_.store(value, order); }

    [[nodiscard]] std::uint8_t load(std::memory_order order) const noexcept { return storage_.load(order); }

  private:
    std::atomic<std::uint8_t> storage_{0};
  };

  void cleanup() noexcept;
  void move_from(bucket_epoch_sync&& other) noexcept;

  std::atomic<std::uint64_t> tickets_issued_{0};
  std::atomic<std::uint64_t> epoch_publication_{initial_publication()};
  std::unique_ptr<phase_flag[]> flags_;
  std::uint32_t span_ = 0;
};

inline void bucket_epoch_sync::configure(std::uint32_t span) {
#if defined(ORAM_DEBUG)
  const auto issued_before = tickets_issued_.load(std::memory_order_relaxed);
  sn::util::log::ensure(issued_before == 0ULL, "bucket_epoch_sync::configure: not quiescent (issued != 0)");
#endif
  cleanup();
  span_ = span;
  if (span_ > 0) {
    flags_.reset(new phase_flag[span_]);
  }
  tickets_issued_.store(0ULL, std::memory_order_relaxed);
  epoch_publication_.store(initial_publication(), std::memory_order_relaxed);
  for (std::uint32_t i = 0; i < span_; ++i) {
    flags_[i].store(0, std::memory_order_relaxed);
  }
}

inline void bucket_epoch_sync::reset() noexcept {
#if defined(ORAM_DEBUG)
  const auto issued_before = tickets_issued_.load(std::memory_order_relaxed);
  sn::util::log::ensure(issued_before == 0ULL, "bucket_epoch_sync::reset: not quiescent (issued != 0)");
#endif
  tickets_issued_.store(0ULL, std::memory_order_relaxed);
  epoch_publication_.store(initial_publication(), std::memory_order_relaxed);
  for (std::uint32_t i = 0; i < span_; ++i) {
    flags_[i].store(0, std::memory_order_relaxed);
  }
}

inline bucket_epoch_sync::admission bucket_epoch_sync::admit(std::uint64_t ticket) const noexcept {
  const std::uint32_t local_span = span_;
  sn::util::log::ensure(local_span > 0, "bucket_epoch_sync: span must be positive");
  while (true) {
    // spin until the ticket falls within the publication window
    const std::uint64_t pub = epoch_publication_.load(std::memory_order_acquire);
    const std::uint64_t base = decode_base(pub);
    const std::uint64_t rank = ticket - base; // unsigned wrap ok
    if (rank < local_span) {
      return admission{base, decode_phase(pub)};
    }
    sn::threads::cpu_relax();
  }
}

inline void bucket_epoch_sync::mark_done(std::uint32_t rank, std::uint8_t phase) noexcept {
  // release to pair with owner's acquire wait; so owner's drain/rebuild sees bucket mutations
  flags_[rank].store(phase, std::memory_order_release);
}

inline void bucket_epoch_sync::wait_for_epoch(std::uint32_t skip_rank, std::uint8_t phase) noexcept {
  for (std::uint32_t i = 0; i < span_; ++i) {
    if (i == skip_rank) {
      continue;
    }
    // poll relaxed until we see the target phase, then acquire to synchronize with peers' release writes
    while (flags_[i].load(std::memory_order_relaxed) != phase) {
      sn::threads::cpu_relax();
    }
    (void) flags_[i].load(std::memory_order_acquire);
  }
}

inline void bucket_epoch_sync::publish(std::uint64_t base, std::uint8_t phase) noexcept {
  // release publish to advance epoch base; phase toggle will force new window and pair with admit acquire load
  epoch_publication_.store(encode(base, phase), std::memory_order_release);
}

inline void bucket_epoch_sync::force_mismatch(std::uint8_t phase) noexcept {
  // force every rank to disagree with the upcoming phase so the next owner waits for fresh completions
  const std::uint8_t value = static_cast<std::uint8_t>(phase ^ 1U);
  for (std::uint32_t i = 0; i < span_; ++i) {
    flags_[i].store(value, std::memory_order_relaxed);
  }
}

inline void bucket_epoch_sync::cleanup() noexcept {
  flags_.reset();
  span_ = 0;
  tickets_issued_.store(0ULL, std::memory_order_relaxed);
  epoch_publication_.store(initial_publication(), std::memory_order_relaxed);
}

inline void bucket_epoch_sync::move_from(bucket_epoch_sync&& other) noexcept {
  span_ = other.span_;
  flags_ = std::move(other.flags_);
  tickets_issued_.store(other.tickets_issued_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  epoch_publication_.store(other.epoch_publication_.load(std::memory_order_relaxed), std::memory_order_relaxed);

  other.span_ = 0;
  other.flags_.reset();
  other.tickets_issued_.store(0ULL, std::memory_order_relaxed);
  other.epoch_publication_.store(initial_publication(), std::memory_order_relaxed);
}

} // namespace sn::oram::zingoram::detail
