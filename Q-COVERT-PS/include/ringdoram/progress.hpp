#pragma once
// Host-only display for public benchmark progress; never included in the TEE.
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>

namespace ringdoram {
class progress_display {
  FILE *stream_;
  const char *prefix_;
  const bool terminal_;
  std::string stage_;
  bool active_ = false;
public:
  explicit progress_display(FILE *stream, const char *prefix = "COMPASS_PROGRESS")
      : stream_(stream), prefix_(prefix), terminal_(::isatty(::fileno(stream)) != 0) {}
  ~progress_display() { finish(); }
  void finish() {
    if (active_) {
      std::fputc('\n', stream_);
      std::fflush(stream_);
      active_ = false;
    }
  }
  void update(const char *stage, std::uint64_t done, std::uint64_t total) {
    // Pipes and log files cannot overwrite a previous line.
    if (!terminal_) {
      if (done >= total) {
        std::fprintf(stream_, "%s\t%s\t%llu\t%llu\n", prefix_, stage,
                     static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
        std::fflush(stream_);
      }
      return;
    }
    if (active_ && stage_ != stage) finish();
    std::fprintf(stream_, "%s%s\t%s\t%llu\t%llu", active_ ? "\r" : "", prefix_, stage,
                 static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
    stage_ = stage;
    active_ = true;
    if (done >= total) finish();
    else std::fflush(stream_);
  }
};
} // namespace ringdoram
