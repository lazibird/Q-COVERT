#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <cstdio>
#include <cctype>

namespace sn::util::humanize {

inline std::uint64_t parse_bytes(std::string_view str) {
  if (str.empty()) {
    return 0;
  }

  std::size_t num_end = 0;
  while (num_end < str.size() && (std::isdigit(static_cast<unsigned char>(str[num_end])) || str[num_end] == '.')) {
    ++num_end;
  }
  if (num_end == 0) {
    return 0;
  }

  double value = 0;
  {
    std::string num_str(str.substr(0, num_end));
    value = std::stod(num_str);
  }

  std::string_view suffix = str.substr(num_end);

  while (!suffix.empty() && std::isspace(static_cast<unsigned char>(suffix.front()))) {
    suffix.remove_prefix(1);
  }
  std::uint64_t multiplier = 1;
  if (suffix.empty() || suffix == "B" || suffix == "b") {
    multiplier = 1;
  } else if (suffix == "K" || suffix == "k" || suffix == "KiB" || suffix == "KB" || suffix == "kB") {
    multiplier = 1024ULL;
  } else if (suffix == "M" || suffix == "m" || suffix == "MiB" || suffix == "MB" || suffix == "mB") {
    multiplier = 1024ULL * 1024;
  } else if (suffix == "G" || suffix == "g" || suffix == "GiB" || suffix == "GB" || suffix == "gB") {
    multiplier = 1024ULL * 1024 * 1024;
  } else if (suffix == "T" || suffix == "t" || suffix == "TiB" || suffix == "TB" || suffix == "tB") {
    multiplier = 1024ULL * 1024 * 1024 * 1024;
  } else if (suffix == "P" || suffix == "p" || suffix == "PiB" || suffix == "PB" || suffix == "pB") {
    multiplier = 1024ULL * 1024 * 1024 * 1024 * 1024;
  } else if (suffix == "E" || suffix == "e" || suffix == "EiB" || suffix == "EB" || suffix == "eB") {
    multiplier = 1024ULL * 1024 * 1024 * 1024 * 1024 * 1024;
  }
  return static_cast<std::uint64_t>(value * static_cast<double>(multiplier));
}

inline std::string bytes(std::uint64_t value) {
  static constexpr std::array<std::string_view, 7> suffixes = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
  double v = static_cast<double>(value);
  std::size_t ix = 0;
  while (v >= 1024.0 && ix + 1 < suffixes.size()) {
    v /= 1024.0;
    ++ix;
  }
  char buf[32];
  if (v >= 100.0 || ix == 0) {
    std::snprintf(buf, sizeof(buf), "%.0f %s", v, suffixes[ix].data());
  } else if (v >= 10.0) {
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, suffixes[ix].data());
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f %s", v, suffixes[ix].data());
  }
  return std::string(buf);
}

}
