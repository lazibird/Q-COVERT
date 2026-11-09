#pragma once
#ifndef PICOFORMAT_HPP
#define PICOFORMAT_HPP

#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <charconv>

namespace pfm {

struct format_spec {
  bool alt = false;
  bool zero = false;
  bool left = false;
  bool space = false;
  bool plus = false;
  bool uppercase = false;
  int width = -1;
  int precision = -1;
  char conv = 's';
};

template <class T> void format_value(std::string&, const format_spec&, const T&) = delete;

namespace detail {

inline int parse_int(const char*& p) {
  int v = 0;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    ++p;
  }
  return v;
}

inline void append_n(std::string& out, char ch, int n) {
  if (n > 0) {
    out.append(static_cast<size_t>(n), ch);
  }
}

inline void align(std::string& out, std::string_view body, int width, bool left, char fill) {
  int pad = width - static_cast<int>(body.size());
  if (pad <= 0) {
    out.append(body);
    return;
  }
  if (left) {
    out.append(body);
    append_n(out, fill, pad);
  } else {
    append_n(out, fill, pad);
    out.append(body);
  }
}

template <class U> inline std::pair<const char*, const char*> u_to_chars(U u, int base, bool upper, char* b, char* e) {
  auto r = std::to_chars(b, e, u, base);
  if (r.ec != std::errc{}) {
    static const char* dl = "0123456789abcdefghijklmnopqrstuvwxyz";
    static const char* du = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const char* d = upper ? du : dl;
    char* p = e;
    if (u == 0) {
      *--p = '0';
    }
    while (u) {
      *--p = d[static_cast<unsigned>(u % base)];
      u /= base;
    }
    return {p, e};
  }
  if (upper) {
    for (char* p = b; p < r.ptr; ++p) {
      if (*p >= 'a' && *p <= 'f') {
        *p = static_cast<char>(*p - 'a' + 'A');
      }
    }
  }
  return {b, r.ptr};
}

inline const char* append_literal(std::string& out, const char* f) {
  const char* p = f;
  for (;; ++p) {
    if (*p == '\0') {
      out.append(f, static_cast<size_t>(p - f));
      return p;
    }
    if (*p == '%') {
      out.append(f, static_cast<size_t>(p - f));
      if (*(p + 1) != '%') {
        return p;
      }
      f = ++p;
    }
  }
}

struct format_spec_rt {
  bool alt = false, zero = false, left = false, space = false, plus = false, uppercase = false;
  int width = -1, precision = -1;
  char conv = 's';
};

struct format_arg {
  const void* value = nullptr;
  void (*fmt)(std::string&, const format_spec_rt&, const void*) = nullptr;
  int (*as_int)(const void*) = nullptr;

  template <class T> explicit format_arg(const T& v) {
    value = &v;
    fmt = [](std::string& out, const format_spec_rt& s, const void* pv) {
      format_router(out, s, *static_cast<const T*>(pv));
    };
    as_int = [](const void* pv) -> int {
      using X = T;
      if constexpr (std::is_convertible_v<X, int>) {
        return static_cast<int>(*static_cast<const X*>(pv));
      } else {
        assert(false && "width/precision arg not int-convertible");
        std::abort();
      }
    };
  }

  void format_to(std::string& out, const format_spec_rt& s) const {
    assert(value && fmt);
    fmt(out, s, value);
  }

  int to_int() const {
    assert(value && as_int);
    return as_int(value);
  }

  static void put_number(
      std::string& out, std::string_view prefix, std::string_view digits, int width, bool left, bool zero
  ) {
    int total = static_cast<int>(prefix.size() + digits.size());
    int pad = width - total;
    if (pad <= 0) {
      out.append(prefix);
      out.append(digits);
      return;
    }
    if (left) {
      out.append(prefix);
      out.append(digits);
      append_n(out, ' ', pad);
    } else if (zero) {
      out.append(prefix);
      append_n(out, '0', pad);
      out.append(digits);
    } else {
      append_n(out, ' ', pad);
      out.append(prefix);
      out.append(digits);
    }
  }

  template <class T> static void format_router(std::string& out, const format_spec_rt& s, const T& v) {
    if constexpr (std::is_same_v<std::remove_cv_t<T>, char>) {
      format_char(out, s, v);
    } else if constexpr (std::is_same_v<std::remove_cv_t<T>, bool>) {
      format_bool(out, s, v);
    } else if constexpr (std::is_enum_v<T>) {
      using U = std::underlying_type_t<T>;
      format_integral(out, s, static_cast<U>(v));
    } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
      format_integral(out, s, v);
    } else if constexpr (std::is_floating_point_v<T>) {
      format_float(out, s, static_cast<double>(v));
    } else if constexpr (std::is_array_v<T> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<T>>, char>) {
      format_cstr(out, s, v);
    } else if constexpr (std::is_same_v<std::remove_cv_t<T>, const char*> ||
                         std::is_same_v<std::remove_cv_t<T>, char*>) {
      format_cstr(out, s, static_cast<const char*>(v));
    } else if constexpr (std::is_same_v<std::remove_cv_t<T>, std::string> ||
                         std::is_same_v<std::remove_cv_t<T>, std::string_view>) {
      format_str(out, s, std::string_view(v));
    } else if constexpr (std::is_pointer_v<T>) {
      format_ptr(out, s, (const void*) v);
    } else {
      using ::pfm::format_value;
      pfm::format_spec u{s.alt, s.zero, s.left, s.space, s.plus, s.uppercase, s.width, s.precision, s.conv};
      format_value(out, u, v);
    }
  }

  static void format_char(std::string& out, const format_spec_rt& s, char ch) {
    if (s.conv == 'c') {
      std::string t(1, ch);
      align(out, t, s.width, s.left, ' ');
    } else if (s.conv == 's') {
      std::string t(1, ch);
      if (s.precision >= 0 && s.precision < 1) {
        t.clear();
      }
      align(out, t, s.width, s.left, ' ');
    } else {
      format_integral(out, s, static_cast<int>(static_cast<unsigned char>(ch)));
    }
  }

  static void format_bool(std::string& out, const format_spec_rt& s, bool b) {
    if (s.conv == 's') {
      std::string_view sv = b ? "true" : "false";
      if (s.precision >= 0 && s.precision < (int) sv.size()) {
        sv = sv.substr(0, (size_t) s.precision);
      }
      align(out, sv, s.width, s.left, ' ');
    } else {
      format_integral(out, s, static_cast<int>(b));
    }
  }

  template <class I> static void format_integral(std::string& out, const format_spec_rt& s, I v) {
    bool signed_dec =
        (std::is_signed_v<I> && s.conv != 'u' && s.conv != 'o' && s.conv != 'x' && s.conv != 'X' && s.conv != 'p');
    int base = 10;
    bool upper = s.uppercase;
    if (s.conv == 'o') {
      base = 8;
    } else if (s.conv == 'x') {
      base = 16;
      upper = false;
    } else if (s.conv == 'X') {
      base = 16;
      upper = true;
    }

    using U = std::make_unsigned_t<I>;
    U u = 0;
    bool neg = false;

    if (signed_dec) {
      if (v < 0) {
        neg = true;
        if (v == std::numeric_limits<I>::min()) {
          u = static_cast<U>(-(v + 1));
          u += 1;
        } else {
          u = static_cast<U>(-v);
        }
      } else {
        u = static_cast<U>(v);
      }
    } else {
      u = static_cast<U>(v);
    }

    char buf[3 + 64];
    auto pr = u_to_chars(u, base, upper, buf, buf + sizeof(buf));
    std::string digits(pr.first, pr.second);
    if (s.precision == 0 && u == 0) {
      digits.clear();
    }
    if (s.precision > 0) {
      int need = s.precision - static_cast<int>(digits.size());
      if (need > 0) {
        digits.insert(0, (size_t) need, '0');
      }
    }

    std::string prefix;
    if (s.conv == 'x' && s.alt && u != 0) {
      prefix = "0x";
    } else if (s.conv == 'X' && s.alt && u != 0) {
      prefix = "0X";
    } else if (s.conv == 'o' && s.alt && (u != 0 || s.precision == 0)) {
      prefix = "0";
    }
    if (signed_dec) {
      if (neg) {
        prefix.insert(prefix.begin(), '-');
      } else if (s.plus) {
        prefix.insert(prefix.begin(), '+');
      } else if (s.space) {
        prefix.insert(prefix.begin(), ' ');
      }
    }
    if (s.conv == 'p' && prefix.empty()) {
      prefix = "0x";
    }

    bool zero_pad = s.zero && !s.left && s.precision < 0;
    put_number(out, prefix, digits, s.width, s.left, zero_pad);
  }

  static void format_ptr(std::string& out, const format_spec_rt& s, const void* p) {
    std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
    format_spec_rt t = s;
    t.conv = 'x';
    t.uppercase = false;
    t.alt = true;
    format_integral(out, t, a);
  }

  static void format_cstr(std::string& out, const format_spec_rt& s, const char* cs) {
    if (s.conv == 'p') {
      format_ptr(out, s, cs);
      return;
    }
    std::string_view sv = cs ? std::string_view(cs) : std::string_view("(null)");
    format_str(out, s, sv);
  }

  static void format_str(std::string& out, const format_spec_rt& s, std::string_view sv) {
    if (s.precision >= 0 && s.precision < (int) sv.size()) {
      sv = sv.substr(0, (size_t) s.precision);
    }
    align(out, sv, s.width, s.left, ' ');
  }

  static void format_float(std::string& out, const format_spec_rt& s, double x) {
    char fs[64];
    char* p = fs;
    *p++ = '%';
    if (s.alt) {
      *p++ = '#';
    }
    if (s.left) {
      *p++ = '-';
    }
    if (s.plus) {
      *p++ = '+';
    } else if (s.space) {
      *p++ = ' ';
    }
    if (s.zero && !s.left) {
      *p++ = '0';
    }
    if (s.width >= 0) {
      char tmp[32];
      auto r = std::to_chars(tmp, tmp + 32, s.width);
      for (char* q = tmp; q < r.ptr; ++q) {
        *p++ = *q;
      }
    }
    if (s.precision >= 0) {
      *p++ = '.';
      char tmp[32];
      auto r = std::to_chars(tmp, tmp + 32, s.precision);
      for (char* q = tmp; q < r.ptr; ++q) {
        *p++ = *q;
      }
    }
    char cv = s.conv;
    switch (cv) {
    case 'a':
    case 'A':
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
      break;
    default:
      cv = s.uppercase ? 'G' : 'g';
      break;
    }
    *p++ = cv;
    *p = '\0';

    char small[128];
    int need = std::snprintf(small, sizeof(small), fs, x);
    if (need < 0) {
      assert(false && "snprintf failure");
      std::abort();
    }
    if (need < (int) sizeof(small)) {
      out.append(small, (size_t) need);
    } else {
      std::string buf((size_t) need, '\0');
      std::snprintf(buf.data(), buf.size() + 1, fs, x);
      out.append(buf);
    }
  }
};

inline bool parse_width_or_precision(
    int& n, const char*& c, bool positional, const format_arg* args, int argc, int& scan_index
) {
  if (*c >= '0' && *c <= '9') {
    n = parse_int(c);
    return true;
  }
  if (*c == '*') {
    ++c;
    if (positional) {
      if (std::isdigit((unsigned char) *c)) {
        int pos = parse_int(c) - 1;
        if (*c != '$') {
          assert(false && "missing $ after positional *");
          std::abort();
        }
        ++c;
        if (pos < 0 || pos >= argc) {
          assert(false && "positional * out of range");
          std::abort();
        }
        n = args[pos].to_int();
      } else {
        assert(false && "positional format requires *n$");
        std::abort();
      }
    } else {
      if (scan_index >= argc) {
        assert(false && "not enough args for *");
        std::abort();
      }
      n = args[scan_index++].to_int();
    }
    return true;
  }
  return false;
}

inline const char* read_spec(
    format_spec_rt& s, bool& positional_mode, bool& had_index, int& explicit_index, const char* fstart,
    const format_arg* args, int argc, int& scan_index
) {
  const char* c = fstart + 1;
  had_index = false;
  explicit_index = -1;

  if (std::isdigit((unsigned char) *c)) {
    const char* d = c;
    int num = parse_int(d);
    if (*d == '$') {
      positional_mode = true;
      had_index = true;
      explicit_index = num - 1;
      c = d + 1;
    }
  }

  for (;; ++c) {
    if (*c == '#') {
      s.alt = true;
      continue;
    }
    if (*c == '0') {
      if (!s.left) {
        s.zero = true;
      }
      continue;
    }
    if (*c == '-') {
      s.left = true;
      s.zero = false;
      continue;
    }
    if (*c == ' ') {
      if (!s.plus) {
        s.space = true;
      }
      continue;
    }
    if (*c == '+') {
      s.plus = true;
      s.space = false;
      continue;
    }
    break;
  }

  int w = 0;
  if (parse_width_or_precision(w, c, positional_mode, args, argc, scan_index)) {
    if (w < 0) {
      s.left = true;
      w = -w;
    }
    s.width = w;
  }

  if (*c == '.') {
    ++c;
    int p = 0;
    if (parse_width_or_precision(p, c, positional_mode, args, argc, scan_index)) {
      if (p >= 0) {
        s.precision = p;
      }
    } else {
      s.precision = 0;
    }
  }

  while (*c == 'l' || *c == 'h' || *c == 'L' || *c == 'j' || *c == 'z' || *c == 't') {
    ++c;
  }

  s.conv = *c;
  if (s.conv == 'E' || s.conv == 'F' || s.conv == 'G' || s.conv == 'A' || s.conv == 'X') {
    s.uppercase = true;
  }
  if (*c == '\0') {
    assert(false && "incomplete conversion");
    std::abort();
  }

  return c + 1;
}

class format_list {
public:
  format_list() : args_(nullptr), n_(0) {}
  format_list(const format_arg* a, int n) : args_(a), n_(n) {}
  const format_arg* data() const { return args_; }
  int size() const { return n_; }

protected:
  void reset(const format_arg* a, int n) {
    args_ = a;
    n_ = n;
  }

private:
  const format_arg* args_;
  int n_;
};

template <size_t N> class format_list_n : public format_list {
public:
  template <class... Ts> explicit format_list_n(const Ts&... ts) : store_{format_arg(ts)...} {
    static_assert(sizeof...(Ts) == N, "size mismatch");
    this->reset(store_.data(), static_cast<int>(N));
  }

private:
  std::array<format_arg, N> store_;
};

template <> class format_list_n<0> : public format_list {
public:
  format_list_n() : format_list() {}
};

}

template <class... Ts> detail::format_list_n<sizeof...(Ts)> make_format_list(const Ts&... ts) {
  return detail::format_list_n<sizeof...(Ts)>(ts...);
}

inline void vformat_to(std::string& out, std::string_view fmt, const detail::format_list& list) {
  const char* f = fmt.data();
  const auto* args = list.data();
  int argc = list.size();

  bool positional_mode = false;
  int scan_index = 0;
  int pos_cursor = 0;

  while (true) {
    f = detail::append_literal(out, f);
    if (*f == '\0') {
      if (!positional_mode && scan_index < argc) {
        assert(false && "unused args");
        std::abort();
      }
      break;
    }

    detail::format_spec_rt s{};
    bool had_index = false;
    int explicit_index = -1;
    const char* end = detail::read_spec(s, positional_mode, had_index, explicit_index, f, args, argc, scan_index);

    int arg_i = -1;
    if (positional_mode) {
      arg_i = had_index ? explicit_index : pos_cursor++;
    } else {
      arg_i = scan_index++;
    }

    if (arg_i < 0 || arg_i >= argc) {
      assert(false && "argument index out of range");
      std::abort();
    }
    args[arg_i].format_to(out, s);
    f = end;
  }
}

inline std::string vformat(std::string_view fmt, const detail::format_list& list) {
  std::string out;
  out.reserve(fmt.size() + (size_t) list.size() * 8);
  vformat_to(out, fmt, list);
  return out;
}

template <class... Ts> std::string format(std::string_view fmt, const Ts&... ts) {
  auto fl = make_format_list(ts...);
  return vformat(fmt, fl);
}

template <class... Ts> void format_to(std::string& out, std::string_view fmt, const Ts&... ts) {
  auto fl = make_format_list(ts...);
  vformat_to(out, fmt, fl);
}

}

#endif
