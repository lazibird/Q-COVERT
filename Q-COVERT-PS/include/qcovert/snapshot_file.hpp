#pragma once
// Host-only file transport. The writer is committed atomically after a
// successful Setup; interrupted writes leave the previous image intact.
#include <sys/stat.h>
#include "qcovert/snapshot.hpp"
#include <cerrno>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <unistd.h>
namespace qcovert::snapshot {
class file_transport {
  std::filesystem::path path_, pending_;
  int source_ = -1, target_ = -1;
public:
  explicit file_transport(std::filesystem::path path)
      : path_(std::move(path)), pending_(path_.string() + ".pending") {
    source_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
  }
  file_transport(const file_transport &) = delete;
  ~file_transport() {
    if (source_ >= 0) ::close(source_);
    if (target_ >= 0) {
      ::close(target_);
      ::unlink(pending_.c_str());
    }
  }
  bool exists() const noexcept { return source_ >= 0; }
  std::size_t read(void *out, std::size_t size) {
    if (source_ < 0) return 0;
    ssize_t count;
    do { count = ::read(source_, out, size); } while (count < 0 && errno == EINTR);
    if (count < 0) throw std::runtime_error("cannot read ORAM snapshot");
    return static_cast<std::size_t>(count);
  }
  void write(const void *data, std::size_t size) {
    if (target_ < 0) {
      std::filesystem::create_directories(path_.parent_path());
      target_ = ::open(pending_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (target_ < 0) throw std::runtime_error("cannot create ORAM snapshot");
      if (::fchmod(target_, 0600) != 0) throw std::runtime_error("cannot restrict ORAM snapshot permissions");
    }
    auto *src = static_cast<const std::uint8_t *>(data);
    while (size) {
      ssize_t count;
      do { count = ::write(target_, src, size); } while (count < 0 && errno == EINTR);
      if (count <= 0) throw std::runtime_error("cannot write ORAM snapshot");
      src += count;
      size -= static_cast<std::size_t>(count);
    }
  }
  void commit() {
    if (target_ < 0) throw std::logic_error("ORAM snapshot was not written");
    if (::fsync(target_) != 0 || ::close(target_) != 0)
      throw std::runtime_error("cannot sync ORAM snapshot");
    target_ = -1;
    std::filesystem::rename(pending_, path_);
  }
  static std::size_t read_callback(void *context, void *out, std::size_t size) {
    return static_cast<file_transport *>(context)->read(out, size);
  }
  static void write_callback(void *context, const void *data, std::size_t size) {
    static_cast<file_transport *>(context)->write(data, size);
  }
  static void commit_callback(void *context) {
    static_cast<file_transport *>(context)->commit();
  }
};
inline std::vector<std::uint8_t> raw_protect(const key128 &key) {
  return {key.begin(), key.end()};
}
inline key128 raw_unprotect(std::span<const std::uint8_t> bytes) {
  if (bytes.size() != 16) throw std::invalid_argument("native ORAM snapshot key length mismatch");
  key128 key{};
  std::copy(bytes.begin(), bytes.end(), key.begin());
  return key;
}
} // namespace qcovert::snapshot
