#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/oram/stash/core/linear_block_storage.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::stash::core {

// a cursor to track staged writes within a storage section
template <typename Block> class section_cursor {
public:
  section_cursor() = default;

  section_cursor(
      linear_block_storage<Block>& storage, typename linear_block_storage<Block>::section section,
      std::string_view label
  ) {
    configure(storage, section, label);
  }

  // configure cursor for a section of storage
  void configure(
      linear_block_storage<Block>& storage, typename linear_block_storage<Block>::section section,
      std::string_view label
  ) {
    storage_ = &storage;
    section_ = section;
    label_ = label;
    next_ = 0;
  }

  // check whether cursor is bound to storage
  [[nodiscard]] bool configured() const noexcept { return storage_ != nullptr; }

  // total capacity of the section
  [[nodiscard]] std::size_t capacity() const {
    ensure_configured();
    return section_.length;
  }

  // number of staged slots used
  [[nodiscard]] std::size_t used() const {
    ensure_configured();
    return next_;
  }

  // remaining free slots
  [[nodiscard]] std::size_t remaining() const {
    ensure_configured();
    return capacity() - next_;
  }

  // reset usage cursor
  void reset() {
    ensure_configured();
    next_ = 0;
  }

  // append a single block into section
  void append(const Block& block) {
    ensure_configured();
    if (section_.length == 0) {
      overflow(1);
    }
    auto dest = reserve(1);
    dest[0] = block;
  }

  // append multiple contiguous blocks
  void append_span(sn::util::span<const Block> blocks) {
    ensure_configured();
    const std::size_t count = blocks.size();
    if (count == 0) {
      return;
    }
    if (section_.length == 0) {
      overflow(count);
    }
    auto dest = reserve(count);
    sn::obliv::copy(blocks.begin(), blocks.end(), dest.begin());
  }

  // mutable span over the section
  [[nodiscard]] sn::util::span<Block> span() {
    ensure_configured();
    return section_.span(*storage_);
  }

  // const span over the section
  [[nodiscard]] sn::util::span<const Block> span() const {
    ensure_configured();
    return section_.span(*storage_);
  }

private:
  linear_block_storage<Block>* storage_ = nullptr;
  typename linear_block_storage<Block>::section section_{};
  std::size_t next_ = 0;
  std::string label_{};

  // ensure cursor is initialized before use
  void ensure_configured() const {
    sn::util::log::ensure(storage_ != nullptr, "section_cursor: storage not configured");
  }

  // reserve contiguous slots for writing
  [[nodiscard]] sn::util::span<Block> reserve(std::size_t count) {
    if (count == 0) {
      return sn::util::span<Block>(nullptr, 0);
    }
    if (count > remaining()) {
      overflow(count);
    }
    auto dest = storage_->slice(section_.offset + next_, count);
    next_ += count;
    return dest;
  }

  // emit fatal log on overflow
  void overflow(std::size_t requested) const {
    sn::util::log::failf(
        "section_cursor[%s]: capacity exceeded (requested=%zu remaining=%zu)", label_, requested, remaining_safe()
    );
    std::abort();
  }

  // compute remaining capacity without assumptions
  [[nodiscard]] std::size_t remaining_safe() const noexcept {
    if (!storage_) {
      return 0;
    }
    if (section_.length < next_) {
      return 0;
    }
    return section_.length - next_;
  }
};

// a section view to stage writes via a bump cursor
template <typename Block> class section_writer {
public:
  section_writer() = default;

  section_writer(
      linear_block_storage<Block>& storage, typename linear_block_storage<Block>::section section,
      std::string_view label
  ) {
    configure(storage, section, label);
  }

  // configure slot storage and cursor
  void configure(
      linear_block_storage<Block>& storage, typename linear_block_storage<Block>::section section,
      std::string_view label
  ) {
    storage_ = &storage;
    section_ = section;
    cursor_.configure(storage, section, label);
  }

  // check whether slot is ready
  [[nodiscard]] bool configured() const noexcept { return storage_ != nullptr; }

  // expose cursor metrics
  [[nodiscard]] std::size_t capacity() const { return cursor_.capacity(); }
  [[nodiscard]] std::size_t used() const { return cursor_.used(); }
  [[nodiscard]] std::size_t remaining() const { return cursor_.remaining(); }

  // rewind staged usage without touching stored contents
  void reset_cursor() {
    ensure_configured();
    cursor_.reset();
  }

  // wipe backing storage and reset the cursor
  void clear() {
    ensure_configured();
    storage_->clear_section(section_);
    cursor_.reset();
  }

  // append a single block
  void append(const Block& block) { cursor_.append(block); }

  // append a span of blocks
  void append_span(sn::util::span<const Block> blocks) { cursor_.append_span(blocks); }

  // mutable view over section
  [[nodiscard]] sn::util::span<Block> span() {
    ensure_configured();
    return section_.span(*storage_);
  }

  // const view over section
  [[nodiscard]] sn::util::span<const Block> span() const {
    ensure_configured();
    return section_.span(*storage_);
  }

  // expose section descriptor
  [[nodiscard]] const typename linear_block_storage<Block>::section& section() const noexcept { return section_; }

private:
  linear_block_storage<Block>* storage_ = nullptr;
  typename linear_block_storage<Block>::section section_{};
  section_cursor<Block> cursor_;

  // ensure slot has storage before use
  void ensure_configured() const {
    sn::util::log::ensure(storage_ != nullptr, "section_writer: storage not configured");
  }
};

// a layout to sequentially split storage into sections
template <typename Block> class section_layout {
public:
  explicit section_layout(linear_block_storage<Block>& storage) : storage_(storage) {}

  // carve out the next contiguous section
  typename linear_block_storage<Block>::section take_section(std::uint64_t length) {
    const auto len = narrow_length(length);
    auto section = storage_.make_section(offset_, len);
    offset_ += len;
    return section;
  }

  // bind a writer to the next contiguous section
  void make_writer(section_writer<Block>& writer, std::uint64_t length, std::string_view label) {
    auto section = take_section(length);
    writer.configure(storage_, section, label);
  }

  // current offset when carving sections
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  // remaining capacity that has not been assigned yet
  [[nodiscard]] std::size_t remaining() const noexcept {
    sn::util::log::ensure(offset_ <= storage_.size(), "section_layout: offset exceeds storage size");
    return storage_.size() - offset_;
  }

  // ensure layout covers the entire storage
  void finish() const {
    sn::util::log::ensure(
        offset_ == storage_.size(), "section_layout: configured size does not match storage capacity"
    );
#if defined(ORAM_DEBUG)
    finalized_ = true;
#endif
  }

#if defined(ORAM_DEBUG)
  ~section_layout() {
    if (!finalized_) {
      sn::util::log::ensure(offset_ == storage_.size(), "section_layout: destroyed before covering storage");
    }
  }
#endif

private:
  linear_block_storage<Block>& storage_;
  std::size_t offset_ = 0;
#if defined(ORAM_DEBUG)
  mutable bool finalized_ = false;
#endif

  // clamp and validate length
  static std::size_t narrow_length(std::uint64_t length) {
    sn::util::log::ensure(
        length <= std::numeric_limits<std::size_t>::max(), "section_layout: section length exceeds size_t"
    );
    return static_cast<std::size_t>(length);
  }
};

} // namespace sn::oram::stash::core
