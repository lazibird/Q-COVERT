#pragma once
#include "qcovert/crypto.hpp"
#include "qcovert/measurement.hpp"
#include "sonic/crypto/random.hpp"
#include "sonic/oram/storage/pin_mode.hpp"
#include "sonic/util/span.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
namespace qcovert {
// Per-bucket key, trusted per-slot version, nonce=(slot,version). Replay or
// relocation of a ciphertext is rejected while this enclave instance lives.
template <typename Block> class encrypted_store {
  struct ciphertext {
    std::array<std::uint8_t, sizeof(Block)> data;
    std::array<std::uint8_t, 16> tag;
  };
  struct release {
    void operator()(ciphertext *p) const noexcept { external_free(p); }
  };
  std::unique_ptr<ciphertext, release> outside_;
  key128 key_{};
  storage_counters *counters_ = nullptr;
  std::vector<std::uint64_t> versions_;
  nonce96 nonce(std::uint32_t slot) const {
    nonce96 iv{};
    std::memcpy(iv.data(), &slot, 4);
    std::memcpy(iv.data() + 4, &versions_[slot], 8);
    return iv;
  }
  void write(std::uint32_t slot, const Block &block) {
    if (++versions_[slot] == 0)
      throw std::overflow_error("ciphertext version exhausted");
    ciphertext c{};
    encrypt(key_, nonce(slot), &block, sizeof(block), c.data.data(), c.tag.data());
    external_write(outside_.get() + slot, &c, sizeof(c));
    if (counters_)
      counters_->write_bytes.fetch_add(sizeof(c), std::memory_order_relaxed);
  }

public:
  static constexpr bool owns_storage = true, is_trivial = false, contiguous = false;
  encrypted_store() = default;
  encrypted_store(encrypted_store &&) = default;
  encrypted_store &operator=(encrypted_store &&) = default;
  ~encrypted_store() {
    volatile std::uint8_t *key = key_.data();
    for (std::size_t i = 0; i < key_.size(); ++i)
      key[i] = 0;
  }
  void configure_node(std::uint64_t, std::uint32_t len) {
    sn::crypto::random_device{}.fill(key_.data(), key_.size());
    versions_.assign(len, 0);
    outside_.reset(static_cast<ciphertext *>(external_allocate(sizeof(ciphertext) * len)));
  }
  // Attach once after Setup, before worker access. No implicit TLS context:
  // each bucket retains the counters belonging to its own ORAM instance.
  void attach_counters(storage_counters &counters) {
    if (counters_)
      throw std::logic_error("storage counters already attached");
    counters_ = &counters;
    counters_->allocated_bytes.fetch_add(sizeof(ciphertext) * versions_.size(), std::memory_order_relaxed);
  }
  template <class Writer> void save_initial(Writer& writer) const {
    writer.write(key_.data(), key_.size());
    writer.write(versions_.data(), versions_.size() * sizeof(std::uint64_t));
    constexpr std::size_t chunk_bytes = 1U << 20;
    const std::size_t slots_per_chunk = std::max<std::size_t>(1, chunk_bytes / sizeof(ciphertext));
    std::vector<ciphertext> chunk(std::min(slots_per_chunk, versions_.size()));
    for (std::size_t offset = 0; offset < versions_.size(); offset += slots_per_chunk) {
      const auto count = std::min(slots_per_chunk, versions_.size() - offset);
      external_read(chunk.data(), outside_.get() + offset, count * sizeof(ciphertext));
      writer.write(chunk.data(), count * sizeof(ciphertext));
    }
  }
  template <class Reader> void load_initial(Reader& reader) {
    reader.read(key_.data(), key_.size());
    reader.read(versions_.data(), versions_.size() * sizeof(std::uint64_t));
    for (auto version : versions_)
      if (!version) throw std::invalid_argument("ORAM snapshot contains uninitialized slot");
    constexpr std::size_t chunk_bytes = 1U << 20;
    const std::size_t slots_per_chunk = std::max<std::size_t>(1, chunk_bytes / sizeof(ciphertext));
    std::vector<ciphertext> chunk(std::min(slots_per_chunk, versions_.size()));
    for (std::size_t offset = 0; offset < versions_.size(); offset += slots_per_chunk) {
      const auto count = std::min(slots_per_chunk, versions_.size() - offset);
      reader.read(chunk.data(), count * sizeof(ciphertext));
      external_write(outside_.get() + offset, chunk.data(), count * sizeof(ciphertext));
    }
  }
  Block read_block(std::uint32_t slot) const {
    Block result{};
    if (versions_[slot] == 0)
      return result; // only occurs during public initialization
    ciphertext c{};
    external_read(&c, outside_.get() + slot, sizeof(c));
    if (counters_)
      counters_->read_bytes.fetch_add(sizeof(c), std::memory_order_relaxed);
    decrypt(key_, nonce(slot), c.data.data(), sizeof(result), c.tag.data(), &result);
    return result;
  }
  class guard_t {
    encrypted_store *owner_;
    std::vector<Block> blocks_;
    bool dirty_ = false;
    bool writable_ = true;

  public:
    explicit guard_t(encrypted_store *owner, bool writable = true)
        : owner_(owner), blocks_(owner->versions_.size()), writable_(writable) {
      for (std::uint32_t i = 0; i < blocks_.size(); ++i)
        blocks_[i] = owner_->read_block(i);
    }
    guard_t(const guard_t &) = delete;
    guard_t(guard_t &&other) noexcept
        : owner_(other.owner_), blocks_(std::move(other.blocks_)), dirty_(other.dirty_),
          writable_(other.writable_) {
      other.owner_ = nullptr;
    }
    ~guard_t() noexcept {
      if (owner_ && dirty_)
        for (std::uint32_t i = 0; i < blocks_.size(); ++i)
          owner_->write(i, blocks_[i]);
    }
    sn::util::span<Block> span() { return {blocks_.data(), blocks_.size()}; }
    Block *data() { return blocks_.data(); }
    std::uint32_t size() const { return blocks_.size(); }
    void mark_dirty() {
      if (!writable_)
        throw std::logic_error("cannot dirty a const storage pin");
      if (!dirty_ && owner_->counters_)
        owner_->counters_->rebuilds.fetch_add(1, std::memory_order_relaxed);
      dirty_ = true;
    }
  };
  guard_t pin(sn::oram::zingoram::storage::pin_mode) { return guard_t(this); }
  guard_t pin(sn::oram::zingoram::storage::pin_mode mode) const {
    if (mode != sn::oram::zingoram::storage::pin_mode::shared)
      throw std::logic_error("exclusive pin on const storage");
    return guard_t(const_cast<encrypted_store *>(this), false);
  }
  void prefetch() const noexcept {}
};
} // namespace qcovert
