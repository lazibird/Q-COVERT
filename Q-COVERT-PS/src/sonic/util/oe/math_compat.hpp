#pragma once

#if defined(__OPENENCLAVE__) || defined(__OPENENCLAVE_ENCLAVE)

#include <openenclave/3rdparty/libc/math.h>
#include <openenclave/3rdparty/libcxx/limits>

#undef signbit
inline int signbit(double value) noexcept { return __builtin_signbit(value); }
inline int signbit(float value) noexcept { return __builtin_signbit(static_cast<double>(value)); }
inline int signbit(long double value) noexcept { return __builtin_signbit(static_cast<double>(value)); }

#undef fpclassify
inline int fpclassify(double value) noexcept {
  return __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, value);
}
inline int fpclassify(float value) noexcept {
  return __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, static_cast<double>(value));
}
inline int fpclassify(long double value) noexcept {
  return __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, static_cast<double>(value));
}

#undef isfinite
inline int isfinite(double value) noexcept { return __builtin_isfinite(value); }
inline int isfinite(float value) noexcept { return __builtin_isfinite(static_cast<double>(value)); }
inline int isfinite(long double value) noexcept { return __builtin_isfinite(static_cast<double>(value)); }

#undef isinf
inline int isinf(double value) noexcept { return __builtin_isinf(value); }
inline int isinf(float value) noexcept { return __builtin_isinf(static_cast<double>(value)); }
inline int isinf(long double value) noexcept { return __builtin_isinf(static_cast<double>(value)); }

#undef isnan
inline int isnan(double value) noexcept { return __builtin_isnan(value); }
inline int isnan(float value) noexcept { return __builtin_isnan(static_cast<double>(value)); }
inline int isnan(long double value) noexcept { return __builtin_isnan(static_cast<double>(value)); }

#undef isnormal
inline int isnormal(double value) noexcept { return __builtin_isnormal(value); }
inline int isnormal(float value) noexcept { return __builtin_isnormal(static_cast<double>(value)); }
inline int isnormal(long double value) noexcept { return __builtin_isnormal(static_cast<double>(value)); }

#undef isgreater
inline int isgreater(double lhs, double rhs) noexcept { return __builtin_isgreater(lhs, rhs); }
inline int isgreater(float lhs, float rhs) noexcept {
  return __builtin_isgreater(static_cast<double>(lhs), static_cast<double>(rhs));
}
inline int isgreater(long double lhs, long double rhs) noexcept {
  return __builtin_isgreater(static_cast<double>(lhs), static_cast<double>(rhs));
}

#undef isgreaterequal
inline int isgreaterequal(double lhs, double rhs) noexcept { return __builtin_isgreaterequal(lhs, rhs); }
inline int isgreaterequal(float lhs, float rhs) noexcept {
  return __builtin_isgreaterequal(static_cast<double>(lhs), static_cast<double>(rhs));
}
inline int isgreaterequal(long double lhs, long double rhs) noexcept {
  return __builtin_isgreaterequal(static_cast<double>(lhs), static_cast<double>(rhs));
}

#undef isless
inline int isless(double lhs, double rhs) noexcept { return __builtin_isless(lhs, rhs); }
inline int isless(float lhs, float rhs) noexcept {
  return __builtin_isless(static_cast<double>(lhs), static_cast<double>(rhs));
}
inline int isless(long double lhs, long double rhs) noexcept {
  return __builtin_isless(static_cast<double>(lhs), static_cast<double>(rhs));
}

#undef islessequal
inline int islessequal(double lhs, double rhs) noexcept { return __builtin_islessequal(lhs, rhs); }
inline int islessequal(float lhs, float rhs) noexcept {
  return __builtin_islessequal(static_cast<double>(lhs), static_cast<double>(rhs));
}
inline int islessequal(long double lhs, long double rhs) noexcept {
  return __builtin_islessequal(static_cast<double>(lhs), static_cast<double>(rhs));
}

#undef islessgreater
inline int islessgreater(double lhs, double rhs) noexcept { return __builtin_islessgreater(lhs, rhs); }
inline int islessgreater(float lhs, float rhs) noexcept {
  return __builtin_islessgreater(static_cast<double>(lhs), static_cast<double>(rhs));
}
inline int islessgreater(long double lhs, long double rhs) noexcept {
  return __builtin_islessgreater(static_cast<double>(lhs), static_cast<double>(rhs));
}

#undef isunordered
inline int isunordered(double lhs, double rhs) noexcept { return __builtin_isunordered(lhs, rhs); }
inline int isunordered(float lhs, float rhs) noexcept {
  return __builtin_isunordered(static_cast<double>(lhs), static_cast<double>(rhs));
}
inline int isunordered(long double lhs, long double rhs) noexcept {
  return __builtin_isunordered(static_cast<double>(lhs), static_cast<double>(rhs));
}

#undef abs
inline double abs(double value) noexcept { return __builtin_fabs(value); }
inline float abs(float value) noexcept { return __builtin_fabsf(value); }
inline long double abs(long double value) noexcept { return __builtin_fabsl(value); }

#endif
