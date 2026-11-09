#pragma once

#include <cstdint>

namespace sn::util::log {
class logger;
}

namespace sn::sgxbridge::sync {
class dispatcher;
}

namespace sn::sgxbridge::tp {
struct threadpool_services;
class registry;
}

namespace sn::sgxbridge::hostbuf {
class host_buffer_manager;
}

namespace sn::sgxbridge::storage {
class manager;
}

namespace sn::sgxbridge::common {

struct host_execution_context {
  tp::threadpool_services* threadpools{nullptr};
  sn::util::log::logger* logger{nullptr};
  hostbuf::host_buffer_manager* host_buffers{nullptr};
  storage::manager* storage{nullptr};
  void* app_context{nullptr};
  bool dist_enabled{false};
};

struct enclave_execution_context {
  void* host_cookie{nullptr};
  tp::registry* registry{nullptr};
  sync::dispatcher* dispatcher{nullptr};
  void* app_context{nullptr};
  double ns_per_cycle{0.0};
  bool has_cycle_scale{false};
};

struct run_arguments {
  std::uint32_t verbosity{0};
  std::uint32_t flags{0};
};

enum : std::uint32_t {
  run_flag_debug_enclave = 0x1u,
  run_flag_enable_dist = 0x2u,
};

}
