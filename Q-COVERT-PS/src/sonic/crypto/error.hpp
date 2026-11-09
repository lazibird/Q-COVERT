#pragma once

#include <stdexcept>
#include <string>

namespace sn::crypto {

class error : public std::runtime_error {
public:
  explicit error(const std::string& what_arg) : std::runtime_error(what_arg) {}
  explicit error(const char* what_arg) : std::runtime_error(what_arg) {}
};

}
