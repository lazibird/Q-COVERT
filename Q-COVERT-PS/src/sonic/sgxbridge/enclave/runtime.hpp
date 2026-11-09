#pragma once

#include "sonic/sgxbridge/common/execution_context.hpp"

namespace sn::sgxbridge::enclave {

void set_execution_context(common::enclave_execution_context* ctx);
common::enclave_execution_context* current_execution_context();

class execution_context_guard {
public:
  explicit execution_context_guard(common::enclave_execution_context& ctx);
  execution_context_guard(const execution_context_guard&) = delete;
  execution_context_guard& operator=(const execution_context_guard&) = delete;
  ~execution_context_guard();

private:
  common::enclave_execution_context* previous_{nullptr};
};

std::uint64_t current_thread_uid() noexcept;
void reset_thread_uid() noexcept;

}
