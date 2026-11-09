#pragma once

namespace pcl {

constexpr const char* reset() { return "\033[00m"; }

constexpr const char* grey() { return "\033[30m"; }
constexpr const char* red() { return "\033[31m"; }
constexpr const char* green() { return "\033[32m"; }
constexpr const char* yellow() { return "\033[33m"; }
constexpr const char* blue() { return "\033[34m"; }
constexpr const char* magenta() { return "\033[35m"; }
constexpr const char* cyan() { return "\033[36m"; }
constexpr const char* white() { return "\033[37m"; }

constexpr const char* bright_grey() { return "\033[90m"; }
constexpr const char* bright_red() { return "\033[91m"; }
constexpr const char* bright_green() { return "\033[92m"; }
constexpr const char* bright_yellow() { return "\033[93m"; }
constexpr const char* bright_blue() { return "\033[94m"; }
constexpr const char* bright_magenta() { return "\033[95m"; }
constexpr const char* bright_cyan() { return "\033[96m"; }
constexpr const char* bright_white() { return "\033[97m"; }

constexpr const char* on_grey() { return "\033[40m"; }
constexpr const char* on_red() { return "\033[41m"; }
constexpr const char* on_green() { return "\033[42m"; }
constexpr const char* on_yellow() { return "\033[43m"; }
constexpr const char* on_blue() { return "\033[44m"; }
constexpr const char* on_magenta() { return "\033[45m"; }
constexpr const char* on_cyan() { return "\033[46m"; }
constexpr const char* on_white() { return "\033[47m"; }

constexpr const char* on_bright_grey() { return "\033[100m"; }
constexpr const char* on_bright_red() { return "\033[101m"; }
constexpr const char* on_bright_green() { return "\033[102m"; }
constexpr const char* on_bright_yellow() { return "\033[103m"; }
constexpr const char* on_bright_blue() { return "\033[104m"; }
constexpr const char* on_bright_magenta() { return "\033[105m"; }
constexpr const char* on_bright_cyan() { return "\033[106m"; }
constexpr const char* on_bright_white() { return "\033[107m"; }

}
