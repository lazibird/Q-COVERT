#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "sonic/threads/sync.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::zingoram::detail {

// a gate to coordinate windows of concurrent accesses, with periodic exclusive eviction
// overview:
// - construct with a fixed window size (this many tickets handed out per window)
// - the first window_size - 1 tickets simply increment the active counter
// - the final ticket flips into drain mode, and becomes the eviction leader
// - while draining/evicting, new callers spin
// - the leader waits for active tickets to finish, then transitions to evict and runs eviction
// - after eviction, the leader reopens the gate for the next window (incrementing epoch)
// state is stored in a single atomic word to keep everything lock-free

class access_gate {
public:
  enum class phase : std::uint16_t {
    access = 0,
    drain = 1,
    evict = 2,
  };

  class ticket {
  public:
    ticket() = default;
    ticket(access_gate* gate, std::uint16_t epoch, bool leader) noexcept :
        gate_(gate), epoch_(epoch), leader_(leader), released_(false), eviction_finished_(!leader) {}

    ticket(const ticket&) = delete;
    ticket& operator=(const ticket&) = delete;

    ticket(ticket&& other) noexcept { move_from(other); }
    ticket& operator=(ticket&& other) noexcept {
      if (this != &other) {
        release_if_needed();
        move_from(other);
      }
      return *this;
    }

    ~ticket() { release_if_needed(); }

    // release the ticket, returning true if this was the leader
    [[nodiscard]]
    bool release() {
      if (!gate_ || released_) {
        return false;
      }
      gate_->decrement_active(epoch_);
      released_ = true;
      return leader_;
    }

    // leader only: wait until all active tickets from this epoch have finished
    void wait_until_drained() const {
      ensure_leader();
      gate_->wait_until_drained(epoch_);
    }

    // leader only: transition from drain -> evict
    void begin_eviction() const {
      ensure_leader();
      gate_->begin_eviction(epoch_);
    }

    // leader only: reopen gate, transition from evict -> access, incrementing epoch
    void finish_eviction() {
      ensure_leader();
      gate_->finish_eviction(epoch_);
      eviction_finished_ = true;
      reset();
    }

    [[nodiscard]] bool is_leader() const noexcept { return leader_; }
    [[nodiscard]] std::uint16_t epoch() const noexcept { return epoch_; }

  private:
    void ensure_leader() const {
      sn::util::log::ensure(gate_ && leader_, "access_gate::ticket: leader-only operation");
    }

    void release_if_needed() {
      if (!gate_) {
        return;
      }
      if (!released_) {
        gate_->decrement_active(epoch_);
        released_ = true;
      }
      if (leader_ && !eviction_finished_) {
        gate_->recover_epoch(epoch_);
        eviction_finished_ = true;
      }
      reset();
    }

    void move_from(ticket& other) noexcept {
      gate_ = other.gate_;
      epoch_ = other.epoch_;
      leader_ = other.leader_;
      released_ = other.released_;
      eviction_finished_ = other.eviction_finished_;
      other.reset();
    }

    void reset() noexcept {
      gate_ = nullptr;
      epoch_ = 0;
      leader_ = false;
      released_ = true;
      eviction_finished_ = true;
    }

    access_gate* gate_ = nullptr;
    std::uint16_t epoch_ = 0;
    bool leader_ = false;
    bool released_ = true;
    bool eviction_finished_ = true;
  };

  explicit access_gate(std::uint32_t window_size) : window_size_(window_size) {
    sn::util::log::ensure(window_size > 0, "access_gate: window_size must be positive");
    sn::util::log::ensure(window_size <= max_window, "access_gate: window_size exceeds capacity");
    state_.store(
        encode_state(window_size_, 0, static_cast<std::uint16_t>(phase::access), 0), std::memory_order_relaxed
    );
  }

  ticket enter() {
    while (true) {
      std::uint64_t current = state_.load(std::memory_order_acquire);
      const auto ph = decode_phase(current);
      if (ph != phase::access) {
        sn::threads::thread_yield();
        continue;
      }

      const std::uint16_t tickets_left = decode_tickets(current);
      if (tickets_left == 0) {
        sn::threads::thread_yield();
        continue;
      }

      const std::uint16_t epoch = decode_epoch(current);
      const std::uint16_t active = decode_active(current);
      const bool leader = (tickets_left == 1);

      std::uint64_t desired = current;
      desired = set_tickets(desired, static_cast<std::uint16_t>(tickets_left - 1));
      desired = set_active(desired, static_cast<std::uint16_t>(active + 1));
      if (leader) {
        desired = set_phase(desired, phase::drain);
      }

      if (state_.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return ticket(this, epoch, leader);
      }
      sn::threads::thread_yield();
    }
  }

  void wait_until_access_phase() const {
    while (decode_phase(state_.load(std::memory_order_acquire)) != phase::access) {
      sn::threads::thread_yield();
    }
  }

  std::uint32_t window_size() const noexcept { return window_size_; }

  std::uint16_t current_epoch() const noexcept { return decode_epoch(state_.load(std::memory_order_acquire)); }

  phase current_phase() const noexcept { return decode_phase(state_.load(std::memory_order_acquire)); }

  static constexpr std::uint64_t max_supported_window() noexcept { return max_window; }

private:
  static constexpr std::uint32_t phase_shift = 0;
  static constexpr std::uint32_t tickets_shift = 16;
  static constexpr std::uint32_t active_shift = 32;
  static constexpr std::uint32_t epoch_shift = 48;

  static constexpr std::uint64_t max_window = 0xFFFF;

  static constexpr std::uint64_t phase_mask = 0xFFFFULL << phase_shift;
  static constexpr std::uint64_t tickets_mask = 0xFFFFULL << tickets_shift;
  static constexpr std::uint64_t active_mask = 0xFFFFULL << active_shift;
  static constexpr std::uint64_t epoch_mask = 0xFFFFULL << epoch_shift;

  static std::uint64_t encode_state(
      std::uint16_t tickets, std::uint16_t active, std::uint16_t phase_bits, std::uint16_t epoch
  ) {
    return (static_cast<std::uint64_t>(epoch) << epoch_shift) | (static_cast<std::uint64_t>(active) << active_shift) |
           (static_cast<std::uint64_t>(tickets) << tickets_shift) | static_cast<std::uint64_t>(phase_bits);
  }

  static phase decode_phase(std::uint64_t state) { return static_cast<phase>((state & phase_mask) >> phase_shift); }

  static std::uint16_t decode_tickets(std::uint64_t state) {
    return static_cast<std::uint16_t>((state & tickets_mask) >> tickets_shift);
  }

  static std::uint16_t decode_active(std::uint64_t state) {
    return static_cast<std::uint16_t>((state & active_mask) >> active_shift);
  }

  static std::uint16_t decode_epoch(std::uint64_t state) {
    return static_cast<std::uint16_t>((state & epoch_mask) >> epoch_shift);
  }

  static std::uint64_t set_phase(std::uint64_t state, phase ph) {
    state &= ~phase_mask;
    state |= static_cast<std::uint64_t>(static_cast<std::uint16_t>(ph)) << phase_shift;
    return state;
  }

  static std::uint64_t set_tickets(std::uint64_t state, std::uint16_t value) {
    state &= ~tickets_mask;
    state |= static_cast<std::uint64_t>(value) << tickets_shift;
    return state;
  }

  static std::uint64_t set_active(std::uint64_t state, std::uint16_t value) {
    state &= ~active_mask;
    state |= static_cast<std::uint64_t>(value) << active_shift;
    return state;
  }

  void decrement_active(std::uint16_t epoch) {
    std::uint64_t current = state_.load(std::memory_order_acquire);
    while (true) {
      sn::util::log::ensure(decode_epoch(current) == epoch, "access_gate::ticket released from wrong epoch");
      const std::uint16_t active = decode_active(current);
      sn::util::log::ensure(active > 0, "access_gate::ticket: active counter underflow");
      std::uint64_t desired = set_active(current, static_cast<std::uint16_t>(active - 1));
      if (state_.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
      }
      sn::threads::thread_yield();
    }
  }

  void wait_until_drained(std::uint16_t epoch) const {
    while (true) {
      std::uint64_t state = state_.load(std::memory_order_acquire);
      if (decode_epoch(state) != epoch) {
        return;
      }
      if (decode_active(state) == 0) {
        return;
      }
      sn::threads::thread_yield();
    }
  }

  void begin_eviction(std::uint16_t epoch) {
    std::uint64_t current = state_.load(std::memory_order_acquire);
    while (true) {
      sn::util::log::ensure(decode_epoch(current) == epoch, "access_gate::ticket: begin_eviction epoch mismatch");
      sn::util::log::ensure(
          decode_phase(current) == phase::drain, "access_gate::ticket: begin_eviction phase mismatch"
      );
      sn::util::log::ensure(decode_active(current) == 0, "access_gate::ticket: begin_eviction requires drained gate");
      std::uint64_t desired = set_phase(current, phase::evict);
      if (state_.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
      }
      sn::threads::thread_yield();
    }
  }

  void finish_eviction(std::uint16_t epoch) {
    std::uint64_t current = state_.load(std::memory_order_acquire);
    while (true) {
      sn::util::log::ensure(decode_epoch(current) == epoch, "access_gate::ticket: finish_eviction epoch mismatch");
      sn::util::log::ensure(
          decode_phase(current) == phase::evict, "access_gate::ticket: finish_eviction phase mismatch"
      );
      std::uint64_t desired = encode_state(
          static_cast<std::uint16_t>(window_size_), 0, static_cast<std::uint16_t>(phase::access),
          static_cast<std::uint16_t>(epoch + 1)
      );
      if (state_.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
      }
      sn::threads::thread_yield();
    }
  }

  void recover_epoch(std::uint16_t epoch) noexcept {
    while (true) {
      std::uint64_t current = state_.load(std::memory_order_acquire);
      if (decode_epoch(current) != epoch) {
        return;
      }

      const auto ph = decode_phase(current);
      if (ph == phase::access) {
        return;
      }

      if (ph == phase::drain) {
        if (decode_active(current) != 0) {
          // still draining, keep yielding
          sn::threads::thread_yield();
          continue;
        }
        const std::uint16_t tickets = decode_tickets(current);
        const std::uint16_t replenished =
            static_cast<std::uint16_t>(std::min<std::uint32_t>(static_cast<std::uint32_t>(tickets) + 1, window_size_));
        std::uint64_t desired = set_phase(current, phase::access);
        desired = set_tickets(desired, replenished);
        if (state_.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
          return;
        }
        sn::threads::thread_yield();
        continue;
      }

      std::uint64_t desired = encode_state(
          static_cast<std::uint16_t>(window_size_), 0, static_cast<std::uint16_t>(phase::access),
          static_cast<std::uint16_t>(epoch + 1)
      );
      if (state_.compare_exchange_weak(current, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
      }
      sn::threads::thread_yield();
    }
  }

  std::uint32_t window_size_ = 0;
  std::atomic<std::uint64_t> state_{0};
};

} // namespace sn::oram::zingoram::detail
