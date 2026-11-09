#pragma once
#include "ringdoram/progress.hpp"
namespace bison {
inline ringdoram::progress_display &host_progress() {
  static ringdoram::progress_display display(stderr, "BISON_PROGRESS");
  return display;
}
}
