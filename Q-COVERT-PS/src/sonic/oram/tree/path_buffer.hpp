#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "sonic/util/span.hpp"

namespace sn::oram::tree {

using u64 = std::uint64_t;

namespace detail {
inline void validate_bucket_size(u64 bucket_size) {
  if (bucket_size == 0) {
    throw std::invalid_argument("path_buffer: bucket_size must be positive");
  }
}

inline void validate_depth(u64 depth, u64 height) {
  if (depth > height) {
    throw std::out_of_range("path_buffer: depth out of range");
  }
}

inline void validate_slot(u64 depth, u64 slot, u64 height, u64 bucket_size) {
  validate_depth(depth, height);
  if (slot >= bucket_size) {
    throw std::out_of_range("path_buffer: slot out of range");
  }
}

inline std::size_t bucket_offset(u64 depth, u64 bucket_size) {
  return static_cast<std::size_t>(depth) * static_cast<std::size_t>(bucket_size);
}

inline std::size_t block_count(u64 height, u64 bucket_size) {
  return static_cast<std::size_t>(height + 1) * static_cast<std::size_t>(bucket_size);
}

inline std::size_t node_count(u64 height) { return static_cast<std::size_t>(height + 1); }

} // namespace detail

// a view into a path buffer
template <typename Block> class path_buffer_view {
public:
  using node_value_type = std::conditional_t<std::is_const_v<Block>, const u64, u64>;

  path_buffer_view() = default;

  path_buffer_view(
      u64 height, u64 bucket_size, sn::util::span<Block> blocks, sn::util::span<node_value_type> node_ids
  ) :
      height_(height), bucket_size_(bucket_size), blocks_(blocks), node_ids_(node_ids) {
    detail::validate_bucket_size(bucket_size_);
    const auto expected_blocks = detail::block_count(height_, bucket_size_);
    if (blocks_.size() != expected_blocks) {
      throw std::invalid_argument("path_buffer_view: block span size mismatch");
    }
    if (node_ids_.size() != detail::node_count(height_)) {
      throw std::invalid_argument("path_buffer_view: node span size mismatch");
    }
  }

  [[nodiscard]] u64 height() const noexcept { return height_; }
  [[nodiscard]] u64 bucket_size() const noexcept { return bucket_size_; }
  [[nodiscard]] u64 path_block_count() const noexcept {
    return static_cast<u64>(detail::block_count(height_, bucket_size_));
  }

  [[nodiscard]] sn::util::span<Block> blocks() noexcept { return blocks_; }
  [[nodiscard]] sn::util::span<const Block> blocks() const noexcept {
    return sn::util::span<const Block>(blocks_.data(), blocks_.size());
  }

  [[nodiscard]] sn::util::span<Block> bucket_span(u64 depth) {
    detail::validate_depth(depth, height_);
    auto* start = blocks_.data() + detail::bucket_offset(depth, bucket_size_);
    return sn::util::span<Block>(start, bucket_size_);
  }

  [[nodiscard]] sn::util::span<const Block> bucket_span(u64 depth) const {
    detail::validate_depth(depth, height_);
    const auto* start = blocks_.data() + detail::bucket_offset(depth, bucket_size_);
    return sn::util::span<const Block>(start, bucket_size_);
  }

  [[nodiscard]] Block& bucket_block(u64 depth, u64 slot) {
    detail::validate_slot(depth, slot, height_, bucket_size_);
    return blocks_[detail::bucket_offset(depth, bucket_size_) + slot];
  }

  [[nodiscard]] const Block& bucket_block(u64 depth, u64 slot) const {
    detail::validate_slot(depth, slot, height_, bucket_size_);
    return blocks_[detail::bucket_offset(depth, bucket_size_) + slot];
  }

  [[nodiscard]] sn::util::span<node_value_type> node_ids() noexcept { return node_ids_; }
  [[nodiscard]] sn::util::span<const u64> node_ids() const noexcept {
    return sn::util::span<const u64>(node_ids_.data(), node_ids_.size());
  }

private:
  u64 height_ = 0;
  u64 bucket_size_ = 0;
  sn::util::span<Block> blocks_;
  sn::util::span<node_value_type> node_ids_;
};

// a buffer for a root-to-leaf path in an oram tree
template <typename Block> class path_buffer {
public:
  path_buffer() = default;

  path_buffer(u64 height, u64 bucket_size) { configure(height, bucket_size); }

  void configure(u64 height, u64 bucket_size) {
    detail::validate_bucket_size(bucket_size);
    height_ = height;
    bucket_size_ = bucket_size;
    blocks_.assign(detail::block_count(height_, bucket_size_), Block{});
    node_ids_.assign(detail::node_count(height_), 0);
  }

  void ensure(u64 height, u64 bucket_size) {
    const auto expected_blocks = detail::block_count(height, bucket_size);
    const auto expected_nodes = detail::node_count(height);
    if (height_ != height || bucket_size_ != bucket_size || blocks_.size() != expected_blocks ||
        node_ids_.size() != expected_nodes) {
      configure(height, bucket_size);
    }
  }

  [[nodiscard]] u64 height() const noexcept { return height_; }
  [[nodiscard]] u64 bucket_size() const noexcept { return bucket_size_; }
  [[nodiscard]] u64 path_block_count() const noexcept {
    return static_cast<u64>(detail::block_count(height_, bucket_size_));
  }

  [[nodiscard]] sn::util::span<Block> blocks() noexcept {
    return sn::util::span<Block>(blocks_.data(), blocks_.size());
  }

  [[nodiscard]] sn::util::span<const Block> blocks() const noexcept {
    return sn::util::span<const Block>(blocks_.data(), blocks_.size());
  }

  [[nodiscard]] sn::util::span<Block> bucket_span(u64 depth) {
    detail::validate_depth(depth, height_);
    auto* start = blocks_.data() + detail::bucket_offset(depth, bucket_size_);
    return sn::util::span<Block>(start, bucket_size_);
  }

  [[nodiscard]] sn::util::span<const Block> bucket_span(u64 depth) const {
    detail::validate_depth(depth, height_);
    const auto* start = blocks_.data() + detail::bucket_offset(depth, bucket_size_);
    return sn::util::span<const Block>(start, bucket_size_);
  }

  [[nodiscard]] Block& bucket_block(u64 depth, u64 slot) {
    detail::validate_slot(depth, slot, height_, bucket_size_);
    return blocks_[detail::bucket_offset(depth, bucket_size_) + slot];
  }

  [[nodiscard]] const Block& bucket_block(u64 depth, u64 slot) const {
    detail::validate_slot(depth, slot, height_, bucket_size_);
    return blocks_[detail::bucket_offset(depth, bucket_size_) + slot];
  }

  [[nodiscard]] sn::util::span<u64> node_ids() noexcept {
    return sn::util::span<u64>(node_ids_.data(), node_ids_.size());
  }

  [[nodiscard]] sn::util::span<const u64> node_ids() const noexcept {
    return sn::util::span<const u64>(node_ids_.data(), node_ids_.size());
  }

  [[nodiscard]] path_buffer_view<Block> view() noexcept {
    return path_buffer_view<Block>(
        height_, bucket_size_, sn::util::span<Block>(blocks_.data(), blocks_.size()),
        sn::util::span<u64>(node_ids_.data(), node_ids_.size())
    );
  }

  [[nodiscard]] path_buffer_view<const Block> view() const noexcept {
    return path_buffer_view<const Block>(
        height_, bucket_size_, sn::util::span<const Block>(blocks_.data(), blocks_.size()),
        sn::util::span<const u64>(node_ids_.data(), node_ids_.size())
    );
  }

private:
  u64 height_ = 0;
  u64 bucket_size_ = 0;
  std::vector<Block> blocks_;
  std::vector<u64> node_ids_;
};

// a buffer for multiple root-to-leaf paths in an oram tree
template <typename Block> class multipath_buffer {
public:
  multipath_buffer() = default;

  multipath_buffer(u64 height, u64 bucket_size, std::size_t path_count) { configure(height, bucket_size, path_count); }

  void configure(u64 height, u64 bucket_size, std::size_t path_count) {
    detail::validate_bucket_size(bucket_size);
    sn::util::log::ensure(path_count > 0, "multipath_buffer: path_count must be positive");
    height_ = height;
    bucket_size_ = bucket_size;
    path_count_ = path_count;
    blocks_.assign(path_count_ * detail::block_count(height_, bucket_size_), Block{});
    node_ids_.assign(path_count_ * detail::node_count(height_), 0);
  }

  [[nodiscard]] u64 height() const noexcept { return height_; }
  [[nodiscard]] u64 bucket_size() const noexcept { return bucket_size_; }
  [[nodiscard]] std::size_t path_count() const noexcept { return path_count_; }
  [[nodiscard]] std::size_t path_block_count() const noexcept { return detail::block_count(height_, bucket_size_); }
  [[nodiscard]] std::size_t path_node_count() const noexcept { return detail::node_count(height_); }

  [[nodiscard]] path_buffer_view<Block> path(std::size_t index) {
    validate_index(index);
    return make_view(index);
  }

  [[nodiscard]] path_buffer_view<const Block> path(std::size_t index) const {
    validate_index(index);
    return make_view(index);
  }

  [[nodiscard]] sn::util::span<Block> blocks() noexcept {
    return sn::util::span<Block>(blocks_.data(), blocks_.size());
  }

  [[nodiscard]] sn::util::span<const Block> blocks() const noexcept {
    return sn::util::span<const Block>(blocks_.data(), blocks_.size());
  }

  [[nodiscard]] sn::util::span<u64> node_ids() noexcept {
    return sn::util::span<u64>(node_ids_.data(), node_ids_.size());
  }

  [[nodiscard]] sn::util::span<const u64> node_ids() const noexcept {
    return sn::util::span<const u64>(node_ids_.data(), node_ids_.size());
  }

private:
  void validate_index(std::size_t index) const {
    if (index >= path_count_) {
      throw std::out_of_range("multipath_buffer: path index out of range");
    }
  }

  path_buffer_view<Block> make_view(std::size_t index) {
    const auto per_path_blocks = detail::block_count(height_, bucket_size_);
    const auto per_path_nodes = detail::node_count(height_);
    auto* block_ptr = blocks_.data() + index * per_path_blocks;
    auto* node_ptr = node_ids_.data() + index * per_path_nodes;
    return path_buffer_view<Block>(
        height_, bucket_size_, sn::util::span<Block>(block_ptr, per_path_blocks),
        sn::util::span<u64>(node_ptr, per_path_nodes)
    );
  }

  path_buffer_view<const Block> make_view(std::size_t index) const {
    const auto per_path_blocks = detail::block_count(height_, bucket_size_);
    const auto per_path_nodes = detail::node_count(height_);
    const auto* block_ptr = blocks_.data() + index * per_path_blocks;
    const auto* node_ptr = node_ids_.data() + index * per_path_nodes;
    return path_buffer_view<const Block>(
        height_, bucket_size_, sn::util::span<const Block>(block_ptr, per_path_blocks),
        sn::util::span<const u64>(node_ptr, per_path_nodes)
    );
  }

  u64 height_ = 0;
  u64 bucket_size_ = 0;
  std::size_t path_count_ = 0;
  std::vector<Block> blocks_;
  std::vector<u64> node_ids_;
};

#if defined(ORAM_DEBUG)
#include "sonic/util/log.hpp"
namespace debug {

template <typename PathView> void log_path_buffer(::sn::util::log::logger& log, PathView& path_view) {
  for (std::uint64_t depth = 0; depth <= path_view.height(); ++depth) {
    const auto node_id = path_view.node_ids()[depth];
    auto bucket = path_view.bucket_span(depth);
    log.dbgf("  bucket[%d] (depth=%d)", node_id, depth);
    for (std::size_t slot_ix = 0; slot_ix < bucket.size(); ++slot_ix) {
      const auto& block = bucket[slot_ix];
      if (block.is_real().unwrap()) {
        log.dbgf("    real block#%d (addr=$%08x leaf=%d)", block.uid, block.address, block.leaf_ix);
      } else {
        log.dbgf("    dummy block#%d", block.uid);
      }
    }
  }
}

} // namespace debug
#endif

} // namespace sn::oram::tree
