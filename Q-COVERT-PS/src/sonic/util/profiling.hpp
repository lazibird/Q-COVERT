#pragma once

#include <cstdint>

namespace sn::prof {

constexpr bool enabled = false;

inline void set_thread_name(const char*) noexcept {}
inline void set_thread_name(const char*, std::int32_t) noexcept {}

template <typename T> inline void plot(const char*, T) noexcept {}

inline void message(const char*) noexcept {}
inline void message(const char*, std::uint32_t) noexcept {}

inline bool connected() noexcept { return false; }

template <typename... Args> inline bool await_connection(Args&&...) noexcept { return false; }

inline void frame_mark() noexcept {}
inline void frame_mark_named(const char*) noexcept {}
inline void frame_mark_start(const char*) noexcept {}
inline void frame_mark_end(const char*) noexcept {}

}

#define SN_PROF_ZONE_COLOR_ACTIVE_DEPTH(name_literal, color_expr, active_expr, depth_expr)                             \
  ((void) (color_expr), (void) (active_expr), (void) (depth_expr))
#define SN_PROF_ZONE(name_literal) ((void) 0)
#define SN_PROF_ZONE_IF(name_literal, active_expr) ((void) (active_expr))
#define SN_PROF_ZONE_DEPTH(name_literal, depth_expr) ((void) (depth_expr))
#define SN_PROF_ZONE_COLOR(name_literal, color_expr) ((void) (color_expr))
#define SN_PROF_ZONE_N_COLOR_ACTIVE_DEPTH(var, name_literal, color_expr, active_expr, depth_expr)                      \
  [[maybe_unused]] auto var = 0
#define SN_PROF_ZONE_N(var, name_literal) [[maybe_unused]] auto var = 0

#define sn_prof_zone(name_literal) ((void) 0)
#define sn_prof_zone_if(name_literal, active_expr) ((void) (active_expr))
#define sn_prof_zone_depth(name_literal, depth_expr) ((void) (depth_expr))
#define sn_prof_zone_color(name_literal, color_expr) ((void) (color_expr))
#define sn_prof_zone_named(var, name_literal) [[maybe_unused]] auto var = 0
#define sn_prof_zone_named_ex(var, name_literal, color_expr, active_expr, depth_expr) [[maybe_unused]] auto var = 0
