#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <string>
#include <utility>

#include "sonic/util/log_config.hpp"

#if SN_LOG_HAS_CONSOLE
#include <iostream>
#include <ostream>
#endif

#if SN_LOG_HAS_COLOR
#include "sonic/util/picocolor.hpp"
#endif

#if SN_LOG_HAS_PROGRESS
#include "sonic/util/progress.hpp"
#endif

#include "sonic/util/picoformat.hpp"
#include "sonic/util/formatter.hpp"

#if SN_LOG_HAS_COLOR && !defined(SN_SGX_ENCLAVE)
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#endif

namespace sn::util::log {

namespace detail {

[[nodiscard]] inline bool branch_expect(bool condition, bool expected) noexcept {
  const bool cond = condition;
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_expect(cond ? 1 : 0, expected ? 1 : 0) != 0;
#else
  (void) expected;
  return cond;
#endif
}

[[nodiscard]] inline bool branch_likely(bool condition) noexcept { return branch_expect(condition, true); }
[[nodiscard]] inline bool branch_unlikely(bool condition) noexcept { return branch_expect(condition, false); }

#if SN_LOG_HAS_COLOR
[[nodiscard]] inline bool should_use_color() {
#if defined(SN_SGX_ENCLAVE)
  return true;
#else

  if (const char* env = std::getenv("SONIC_COLOR")) {
    return env[0] == '1' && env[1] == '\0';
  }

  return isatty(fileno(stdout)) != 0;
#endif
}
#endif

}

enum class level : int {
  annoying = 8,
  pedantic = 7,
  debug = 6,
  trace = 5,
  verbose = 4,
  info = 3,
  warn = 2,
  error = 1,
  critical = 0,
};

class logger {
public:
  explicit logger(level verbosity = level::info) : verbosity_(verbosity) {}

  logger(level verbosity, std::string_view source) : verbosity_(verbosity) { set_source(source); }

  [[nodiscard]] logger child(std::string_view source) const {
    logger child_logger(*this);
    child_logger.set_source(source);
    return child_logger;
  }

  void set_verbosity(level verbosity) noexcept { verbosity_ = verbosity; }
  [[nodiscard]] level verbosity() const noexcept { return verbosity_; }

  void ayg(std::string_view message) const { write(level::annoying, message); }
  void ped(std::string_view message) const { write(level::pedantic, message); }
  void dbg(std::string_view message) const { write(level::debug, message); }
  void trc(std::string_view message) const { write(level::trace, message); }
  void vrb(std::string_view message) const { write(level::verbose, message); }
  void inf(std::string_view message) const { write(level::info, message); }
  void wrn(std::string_view message) const { write(level::warn, message); }
  void err(std::string_view message) const { write(level::error, message); }
  void crt(std::string_view message) const { write(level::critical, message); }

  template <typename... Args> void aygf(const char* fmt, Args&&... args) const {
    writef(level::annoying, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args> void pedf(const char* fmt, Args&&... args) const {
    writef(level::pedantic, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args> void dbgf(const char* fmt, Args&&... args) const {
    writef(level::debug, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args> void trcf(const char* fmt, Args&&... args) const {
    writef(level::trace, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args> void vrbf(const char* fmt, Args&&... args) const {
    writef(level::verbose, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args> void inff(const char* fmt, Args&&... args) const {
    writef(level::info, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args> void wrnf(const char* fmt, Args&&... args) const {
    writef(level::warn, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args> void errf(const char* fmt, Args&&... args) const {
    writef(level::error, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args> void crtf(const char* fmt, Args&&... args) const {
    writef(level::critical, fmt, std::forward<Args>(args)...);
  }

private:
  level verbosity_ = level::info;
  std::array<char, 48> source_{};
#if SN_LOG_HAS_COLOR
  bool use_color_ = detail::should_use_color();
#endif

  void set_source(std::string_view source) {
    const auto length = std::min(source.size(), source_.size() - 1);
    std::memcpy(source_.data(), source.data(), length);
    source_[length] = '\0';
  }

  static std::string_view short_name(level lvl) {
    switch (lvl) {
    case level::annoying:
      return "ayg";
    case level::pedantic:
      return "ped";
    case level::debug:
      return "dbg";
    case level::trace:
      return "trc";
    case level::verbose:
      return "vrb";
    case level::info:
      return "inf";
    case level::warn:
      return "wrn";
    case level::error:
      return "err";
    case level::critical:
      return "crt";
    }
    return "???";
  }

#if SN_LOG_HAS_COLOR
  static void colorize(std::ostream& os, level lvl) {
    switch (lvl) {
    case level::annoying:
    case level::pedantic:
    case level::debug:
      os << pcl::bright_grey();
      break;
    case level::trace:
      os << pcl::white();
      break;
    case level::verbose:
      os << pcl::blue();
      break;
    case level::info:
      os << pcl::green();
      break;
    case level::warn:
      os << pcl::yellow();
      break;
    case level::error:
      os << pcl::red();
      break;
    case level::critical:
      os << pcl::magenta();
      break;
    }
  }
#endif

  void write([[maybe_unused]] level lvl, [[maybe_unused]] std::string_view message) const {
#ifdef ENABLE_LOGGING
    if (!should_log(lvl)) {
      return;
    }
    write_impl(lvl, message);
#endif
  }

  [[nodiscard]] bool should_log(level lvl) const noexcept {
#ifdef ENABLE_LOGGING
    return static_cast<int>(lvl) <= static_cast<int>(verbosity_);
#else
    (void) lvl;
    return false;
#endif
  }

  template <typename... Args>
  void writef([[maybe_unused]] level lvl, [[maybe_unused]] const char* fmt, [[maybe_unused]] Args&&... args) const {
#ifdef ENABLE_LOGGING
    if (!should_log(lvl)) {
      return;
    }
    write_impl(lvl, pfm::format(fmt, std::forward<Args>(args)...));
#endif
  }

#if SN_LOG_HAS_CONSOLE
  void write_impl([[maybe_unused]] level lvl, [[maybe_unused]] std::string_view message) const {
#ifdef ENABLE_LOGGING
    std::lock_guard guard(output_lock());
    std::ostream& out = std::cout;
#if SN_LOG_HAS_PROGRESS
    sn::util::progress::detail::suspend_active_progress(out);
#endif
#if SN_LOG_HAS_COLOR
    if (use_color_) {
      if (source_[0] != '\0') {
        out << pcl::on_grey() << pcl::bright_grey() << '[' << source_.data() << ']' << pcl::reset() << ' ';
      }

      out << '[' << pcl::on_grey();
      colorize(out, lvl);
      out << short_name(lvl) << pcl::reset() << ']';
    } else {
      if (source_[0] != '\0') {
        out << '[' << source_.data() << ']' << ' ';
      }
      out << '[' << short_name(lvl) << ']';
    }
#else
    if (source_[0] != '\0') {
      out << '[' << source_.data() << ']' << ' ';
    }
    out << '[' << short_name(lvl) << ']';
#endif
    out << ' ' << message << std::endl;
#if SN_LOG_HAS_PROGRESS
    sn::util::progress::detail::resume_active_progress(out);
#endif
#endif
  }
#else
  void write_impl([[maybe_unused]] level, [[maybe_unused]] std::string_view) const {}
#endif

  class spinlock {
  public:
    void lock() noexcept {
      while (flag_.test_and_set(std::memory_order_acquire)) {

      }
    }

    void unlock() noexcept { flag_.clear(std::memory_order_release); }

  private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
  };

  static spinlock& output_lock() {
    static spinlock lock;
    return lock;
  }
};

inline logger& global_logger() {
  static logger instance{};
  return instance;
}

inline logger create(std::string_view source) { return global_logger().child(source); }

inline void fail(std::string_view message) {
  global_logger().err(message);
  throw std::runtime_error(std::string(message));
}

inline void fail(const logger& log, std::string_view message) {
  log.err(message);
  throw std::runtime_error(std::string(message));
}

template <typename... Args> inline void failf(const char* fmt, Args&&... args) {
  fail(pfm::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> inline void failf(const logger& log, const char* fmt, Args&&... args) {
  fail(log, pfm::format(fmt, std::forward<Args>(args)...));
}

inline void ensure(bool condition, std::string_view message) {
  if (detail::branch_unlikely(!condition)) {
    fail(message);
  }
}

inline void ensure(const logger& log, bool condition, std::string_view message) {
  if (detail::branch_unlikely(!condition)) {
    fail(log, message);
  }
}

template <typename... Args> inline void ensuref(bool condition, const char* fmt, Args&&... args) {
  if (detail::branch_unlikely(!condition)) {
    failf(fmt, std::forward<Args>(args)...);
  }
}

template <typename... Args> inline void ensuref(const logger& log, bool condition, const char* fmt, Args&&... args) {
  if (detail::branch_unlikely(!condition)) {
    failf(log, fmt, std::forward<Args>(args)...);
  }
}

class scoped_log_level {
public:
  explicit scoped_log_level(level new_level) : scoped_log_level(global_logger(), new_level) {}
  scoped_log_level(logger& log, level new_level) : log_(&log), previous_(log.verbosity()) {
    log_->set_verbosity(new_level);
  }
  scoped_log_level(const scoped_log_level&) = delete;
  scoped_log_level& operator=(const scoped_log_level&) = delete;
  ~scoped_log_level() {
    if (log_ != nullptr) {
      log_->set_verbosity(previous_);
    }
  }

private:
  logger* log_ = nullptr;
  level previous_;
};

}
