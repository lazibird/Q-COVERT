#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "sonic/util/cputimer.hpp"
#include "sonic/util/log_config.hpp"

namespace sn::util::progress {

#if SN_LOG_HAS_PROGRESS

class progress;

namespace detail {

namespace ansi {
constexpr const char* clear_line = "\033[2K";
constexpr const char* cursor_up = "\033[1A";
constexpr const char* carriage_return = "\r";

inline void clear_current_line(std::ostream& out) { out << clear_line << carriage_return; }

inline void clear_two_lines_from_second(std::ostream& out) {
  out << clear_line << cursor_up << clear_line << carriage_return;
}
}

[[nodiscard]] inline bool is_atty(FILE* stream) noexcept {
#if defined(_WIN32)
  return _isatty(_fileno(stream)) != 0;
#else
  return ::isatty(fileno(stream)) != 0;
#endif
}

[[nodiscard]] inline bool stream_supports_tty(std::ostream& stream) noexcept {
  if (&stream == &std::cout) {
    return is_atty(stdout);
  }
  if (&stream == &std::cerr || &stream == &std::clog) {
    return is_atty(stderr);
  }
  return false;
}

inline std::string format_duration(double ns, int precision) {
  struct unit_scale {
    double threshold;
    double divisor;
    const char* suffix;
  };

  constexpr unit_scale scales[] = {
      {1.0e9, 1.0e9, "s"},
      {1.0e6, 1.0e6, "ms"},
      {1.0e3, 1.0e3, "us"},
      {0.0, 1.0, "ns"},
  };

  const double abs_ns = std::abs(ns);
  const auto* scale = std::find_if(std::begin(scales), std::end(scales), [abs_ns](const unit_scale& s) {
    return abs_ns >= s.threshold;
  });

  const double value = ns / scale->divisor;
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f%s", precision, value, scale->suffix);
  return buffer;
}

inline std::string format_rate(double per_sec) {
  struct rate_format {
    double threshold;
    int precision;
    double multiplier;
    const char* suffix;
  };

  constexpr double kSecondsPerMinute = 60.0;
  constexpr rate_format formats[] = {
      {100.0, 0, 1.0, "/s"}, {10.0, 1, 1.0, "/s"}, {1.0, 2, 1.0, "/s"}, {0.01, 1, kSecondsPerMinute, "/min"},
      {0.0, 3, 1.0, "/s"},
  };

  const double rate = std::max(0.0, per_sec);
  const auto* fmt = std::find_if(std::begin(formats), std::end(formats), [rate](const rate_format& f) {
    return rate >= f.threshold;
  });

  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f%s", fmt->precision, rate * fmt->multiplier, fmt->suffix);
  return buffer;
}

void suspend_active_progress(std::ostream& stream);
void resume_active_progress(std::ostream& stream);

}

class progress {
public:
  progress(std::string_view label, std::size_t total = 0, std::ostream& output = std::cout) :
      out_(output), interactive_(detail::stream_supports_tty(out_)), label_(label), total_(total) {}

  progress(const progress&) = delete;
  progress& operator=(const progress&) = delete;

  ~progress() {
    if (emitted_ && !finished_) {
      finish();
    }
  }

  void set_total(std::size_t total) noexcept { total_ = total; }

  void set_extra_line(std::function<std::string()> fn) { extra_line_fn_ = std::move(fn); }

  void update(std::size_t current, std::string_view suffix = {}) {
    current_ = current;
    print_line(suffix, false);
  }

  void tick(std::string_view suffix = {}) {
    ++current_;
    print_line(suffix, false);
  }

  void finish(std::string_view suffix = {}) {
    if (finished_) {
      return;
    }
    if (total_ > 0) {
      current_ = total_;
    }
    print_line(suffix, true);
    finished_ = true;
  }

  [[nodiscard]] std::size_t current() const noexcept { return current_; }
  [[nodiscard]] std::size_t total() const noexcept { return total_; }
  [[nodiscard]] bool interactive() const noexcept { return interactive_; }
  [[nodiscard]] double elapsed_ns() const noexcept { return timer_.elapsed_ns(); }

private:
  static inline progress* active_instance_ = nullptr;

  void make_active() noexcept {
    if (interactive_) {
      active_instance_ = this;
    }
  }

  void clear_active() noexcept {
    if (active_instance_ == this) {
      active_instance_ = nullptr;
    }
  }

  friend void detail::suspend_active_progress(std::ostream& stream);
  friend void detail::resume_active_progress(std::ostream& stream);

  static std::string stats(std::size_t current, std::size_t total) {
    std::string text = std::to_string(current);
    if (total != 0) {
      text.push_back('/');
      text.append(std::to_string(total));
    }
    return text;
  }

  [[nodiscard]] std::string build_timing_info() const {
    const double elapsed_ns = timer_.elapsed_ns();
    std::ostringstream oss;

    oss << detail::format_duration(elapsed_ns, 1);

    if (current_ > 0) {
      const double elapsed_sec = elapsed_ns / 1.0e9;
      const double rate = static_cast<double>(current_) / elapsed_sec;
      oss << " @ " << detail::format_rate(rate);

      if (total_ > 0 && current_ < total_) {
        const double remaining = static_cast<double>(total_ - current_);
        const double eta_ns = (remaining / static_cast<double>(current_)) * elapsed_ns;
        oss << " eta " << detail::format_duration(eta_ns, 1);
      }
    }

    return oss.str();
  }

  [[nodiscard]] std::string build_main_line(std::string_view suffix) const {
    std::ostringstream oss;
    oss << label_ << " -> " << stats(current_, total_) << " | " << build_timing_info();
    if (!suffix.empty()) {
      oss << " | " << suffix;
    }
    return oss.str();
  }

  [[nodiscard]] std::string build_extra_line() const {
    if (!extra_line_fn_) {
      return {};
    }
    constexpr std::string_view kExtraLineIndent = "  ";
    std::string line = extra_line_fn_();
    if (!line.empty() && line.front() != ' ') {
      line.insert(0, kExtraLineIndent);
    }
    return line;
  }

  void print_line(std::string_view suffix, bool final_line) {
    emitted_ = true;
    const std::string main_line = build_main_line(suffix);
    const std::string extra_line = build_extra_line();

    last_main_line_ = main_line;
    last_extra_line_ = extra_line;

    if (interactive_) {
      const bool had_extra = extra_line_length_ > 0;
      const bool has_extra = !extra_line.empty();

      if (had_extra) {
        detail::ansi::clear_two_lines_from_second(out_);
      } else if (main_line_length_ > 0) {
        detail::ansi::clear_current_line(out_);
      }

      out_ << main_line;
      main_line_length_ = main_line.size();

      if (has_extra) {
        out_ << '\n' << extra_line;
        extra_line_length_ = extra_line.size();
      } else {
        extra_line_length_ = 0;
      }

      if (final_line) {
        out_ << '\n';
        main_line_length_ = 0;
        extra_line_length_ = 0;
        clear_active();
        redraw_pending_ = false;
        last_main_line_.clear();
        last_extra_line_.clear();
      } else {
        make_active();
        redraw_pending_ = false;
      }
      out_ << std::flush;
    } else {
      out_ << main_line << '\n';
      if (!extra_line.empty()) {
        out_ << extra_line << '\n';
      }
      if (final_line) {
        last_main_line_.clear();
        last_extra_line_.clear();
      }
    }
  }

  void clear_line() {
    if (!interactive_ || (main_line_length_ == 0 && extra_line_length_ == 0)) {
      redraw_pending_ = false;
      return;
    }

    if (extra_line_length_ > 0) {
      detail::ansi::clear_two_lines_from_second(out_);
    } else {
      detail::ansi::clear_current_line(out_);
    }

    out_ << std::flush;
    redraw_pending_ = true;
  }

  void redraw_line() {
    if (!interactive_ || !redraw_pending_) {
      return;
    }
    if (last_main_line_.empty() && last_extra_line_.empty()) {
      return;
    }

    out_ << last_main_line_;
    main_line_length_ = last_main_line_.size();

    if (!last_extra_line_.empty()) {
      out_ << '\n' << last_extra_line_;
      extra_line_length_ = last_extra_line_.size();
    } else {
      extra_line_length_ = 0;
    }

    out_ << std::flush;
    redraw_pending_ = false;
  }

  std::ostream& out_;
  const bool interactive_;
  const std::string label_;
  std::size_t total_ = 0;
  std::size_t current_ = 0;
  std::size_t main_line_length_ = 0;
  std::size_t extra_line_length_ = 0;
  bool finished_ = false;
  bool emitted_ = false;
  bool redraw_pending_ = false;
  std::string last_main_line_;
  std::string last_extra_line_;
  sn::util::cpu_timer timer_;
  std::function<std::string()> extra_line_fn_;
};

namespace detail {

inline void suspend_active_progress(std::ostream&) {
  if (progress::active_instance_ == nullptr) {
    return;
  }
  progress::active_instance_->clear_line();
}

inline void resume_active_progress(std::ostream&) {
  if (progress::active_instance_ == nullptr) {
    return;
  }
  progress::active_instance_->redraw_line();
}

}

#else

namespace detail {

inline void suspend_active_progress(std::ostream&) {}
inline void resume_active_progress(std::ostream&) {}

}

#endif

}
