#pragma once
#include "ringdoram/progress.hpp"
namespace compass {
using ringdoram::progress_display;
inline progress_display &host_progress() {
  static progress_display display(stderr);
  return display;
}
} // namespace compass
