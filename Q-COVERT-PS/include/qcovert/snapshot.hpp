#pragma once
// Initial, quiescent ORAM image. The outer stream is chunk-authenticated; the
// 16-byte stream key is stored raw for native public experiments and SGX-sealed
// for enclave experiments. This is not a mutable checkpoint format.
#include "qcovert/crypto.hpp"
#include "sonic/crypto/random.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>
namespace qcovert::snapshot {
using identity = std::array<std::uint8_t, 32>;
using read_fn = std::size_t (*)(void *, void *, std::size_t);
using write_fn = void (*)(void *, const void *, std::size_t);
using commit_fn = void (*)(void *);
using protect_fn = std::vector<std::uint8_t> (*)(const key128 &);
using unprotect_fn = key128 (*)(std::span<const std::uint8_t>);
inline constexpr std::uint64_t magic = 0x3250414e53435151ULL;
// Change when the serialized index/ORAM layout becomes incompatible. Query
// parameters, thread counts and a rebuild of the same format do not change it.
inline constexpr std::uint64_t index_format_version = 5;
inline constexpr std::size_t chunk_limit = 1U << 20;
struct transport {
  void *context = nullptr;
  read_fn read = nullptr;
  write_fn write = nullptr;
  commit_fn commit = nullptr;
  protect_fn protect = nullptr;
  unprotect_fn unprotect = nullptr;
};
inline nonce96 nonce(std::uint64_t sequence) {
  nonce96 value{};
  std::memcpy(value.data(), &sequence, sizeof(sequence));
  return value;
}
inline void read_exact(const transport &io, void *out, std::size_t size) {
  if (!io.read) throw std::invalid_argument("snapshot read transport missing");
  auto *dst = static_cast<std::uint8_t *>(out);
  while (size) {
    const auto n = io.read(io.context, dst, size);
    if (!n || n > size) throw std::invalid_argument("truncated ORAM snapshot");
    dst += n;
    size -= n;
  }
}
inline void write_exact(const transport &io, const void *data, std::size_t size) {
  if (!io.write) throw std::invalid_argument("snapshot write transport missing");
  io.write(io.context, data, size);
}
class writer {
  transport io_;
  key128 key_{};
  std::vector<std::uint8_t> plain_;
  std::uint64_t sequence_ = 1;
  bool finished_ = false;
  void flush_chunk() {
    if (plain_.empty()) return;
    const auto size = static_cast<std::uint32_t>(plain_.size());
    std::vector<std::uint8_t> cipher(size + 16);
    encrypt(key_, nonce(sequence_++), plain_.data(), size, cipher.data(), cipher.data() + size);
    write_exact(io_, &size, sizeof(size));
    write_exact(io_, cipher.data(), cipher.size());
    plain_.clear();
  }
public:
  writer(transport io, const identity &id) : io_(io) {
    if (!io_.protect) throw std::invalid_argument("snapshot key protector missing");
    plain_.reserve(chunk_limit);
    sn::crypto::random_device{}.fill(key_.data(), key_.size());
    auto wrapped = io_.protect(key_);
    if (wrapped.empty() || wrapped.size() > 4096) throw std::invalid_argument("invalid wrapped snapshot key");
    const auto key_size = static_cast<std::uint32_t>(wrapped.size());
    write_exact(io_, &magic, sizeof(magic));
    write_exact(io_, id.data(), id.size());
    write_exact(io_, &key_size, sizeof(key_size));
    write_exact(io_, wrapped.data(), wrapped.size());
    write(id.data(), id.size()); // Authenticated identity inside the encrypted stream.
  }
  writer(const writer &) = delete;
  void write(const void *data, std::size_t size) {
    if (finished_) throw std::logic_error("snapshot already finished");
    auto *src = static_cast<const std::uint8_t *>(data);
    while (size) {
      const auto part = std::min(size, chunk_limit - plain_.size());
      plain_.insert(plain_.end(), src, src + part);
      src += part;
      size -= part;
      if (plain_.size() == chunk_limit) flush_chunk();
    }
  }
  void finish() {
    flush_chunk();
    const std::uint32_t end = 0;
    write_exact(io_, &end, sizeof(end));
    finished_ = true;
  }
  ~writer() { volatile auto *p = key_.data(); for (std::size_t i = 0; i < key_.size(); ++i) p[i] = 0; }
};
class reader {
  transport io_;
  key128 key_{};
  std::vector<std::uint8_t> plain_;
  std::size_t offset_ = 0;
  std::uint64_t sequence_ = 1;
  void next_chunk() {
    std::uint32_t size = 0;
    read_exact(io_, &size, sizeof(size));
    if (!size || size > chunk_limit) throw std::invalid_argument("invalid ORAM snapshot chunk");
    std::vector<std::uint8_t> cipher(size + 16);
    read_exact(io_, cipher.data(), cipher.size());
    plain_.resize(size);
    decrypt(key_, nonce(sequence_++), cipher.data(), size, cipher.data() + size, plain_.data());
    offset_ = 0;
  }
public:
  reader(transport io, const identity &expected) : io_(io) {
    if (!io_.unprotect) throw std::invalid_argument("snapshot key unprotector missing");
    std::uint64_t found_magic = 0;
    identity found_id{};
    std::uint32_t key_size = 0;
    read_exact(io_, &found_magic, sizeof(found_magic));
    read_exact(io_, found_id.data(), found_id.size());
    read_exact(io_, &key_size, sizeof(key_size));
    if (found_magic != magic || found_id != expected || !key_size || key_size > 4096)
      throw std::invalid_argument("ORAM snapshot identity/version mismatch");
    std::vector<std::uint8_t> wrapped(key_size);
    read_exact(io_, wrapped.data(), wrapped.size());
    key_ = io_.unprotect(wrapped);
    identity authenticated{};
    read(authenticated.data(), authenticated.size());
    if (authenticated != expected) throw std::invalid_argument("ORAM snapshot authentication mismatch");
  }
  reader(const reader &) = delete;
  void read(void *data, std::size_t size) {
    auto *dst = static_cast<std::uint8_t *>(data);
    while (size) {
      if (offset_ == plain_.size()) next_chunk();
      const auto part = std::min(size, plain_.size() - offset_);
      std::memcpy(dst, plain_.data() + offset_, part);
      dst += part;
      size -= part;
      offset_ += part;
    }
  }
  void finish() {
    if (offset_ != plain_.size()) throw std::invalid_argument("ORAM snapshot has extra payload");
    std::uint32_t end = 1;
    read_exact(io_, &end, sizeof(end));
    std::uint8_t extra = 0;
    if (end != 0 || io_.read(io_.context, &extra, 1) != 0)
      throw std::invalid_argument("ORAM snapshot has trailing data");
  }
  ~reader() { volatile auto *p = key_.data(); for (std::size_t i = 0; i < key_.size(); ++i) p[i] = 0; }
};
} // namespace qcovert::snapshot
