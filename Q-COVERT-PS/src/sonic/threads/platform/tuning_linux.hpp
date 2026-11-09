#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sonic/util/log.hpp"

namespace sn::threads::detail::linux_tuning {

struct logical_cpu {
  std::size_t cpu = 0;
  std::size_t core_id = 0;
  std::size_t package_id = 0;
  std::size_t node_id = 0;
};

struct core_group {
  std::size_t core_id = 0;
  std::size_t package_id = 0;
  std::size_t node_id = 0;
  std::vector<std::size_t> cpus;
};

struct locality_hint {
  std::size_t node_id = 0;
  std::size_t package_id = 0;
};

struct cpu_topology {
  std::vector<logical_cpu> cpus;
  std::vector<core_group> cores;
  locality_hint locality{};
};

inline std::optional<std::size_t> read_size_from_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }

  long long value = 0;
  input >> value;
  if (!input || value < 0) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value);
}

inline std::vector<std::size_t> read_allowed_cpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (::pthread_getaffinity_np(::pthread_self(), sizeof(set), &set) != 0) {
    throw std::runtime_error("thread_context: pthread_getaffinity_np failed");
  }

  std::vector<std::size_t> cpus;
  for (std::size_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(static_cast<int>(cpu), &set)) {
      cpus.push_back(cpu);
    }
  }
  return cpus;
}

inline std::size_t read_cpu_node_id(const std::filesystem::path& cpu_dir) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(cpu_dir, ec)) {
    if (ec) {
      break;
    }

    const auto name = entry.path().filename().string();
    if (name.size() <= 4 || name.substr(0, 4) != "node") {
      continue;
    }

    std::size_t value = 0;
    bool valid = true;
    for (char ch : std::string_view(name).substr(4)) {
      if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
        valid = false;
        break;
      }
      value = value * 10 + static_cast<std::size_t>(ch - '0');
    }
    if (valid) {
      return value;
    }
  }
  return 0;
}

inline std::vector<logical_cpu> read_logical_cpus(const std::vector<std::size_t>& allowed_cpus) {
  std::vector<logical_cpu> records;
  records.reserve(allowed_cpus.size());
  for (std::size_t cpu : allowed_cpus) {
    const auto cpu_dir = std::filesystem::path("/sys/devices/system/cpu") / ("cpu" + std::to_string(cpu));
    records.push_back(
        logical_cpu{
            .cpu = cpu,
            .core_id = read_size_from_file(cpu_dir / "topology" / "core_id").value_or(cpu),
            .package_id = read_size_from_file(cpu_dir / "topology" / "physical_package_id").value_or(0),
            .node_id = read_cpu_node_id(cpu_dir),
        }
    );
  }
  return records;
}

inline std::vector<core_group> build_core_groups(const std::vector<logical_cpu>& cpus) {
  std::vector<core_group> groups;
  std::map<std::pair<std::size_t, std::size_t>, std::size_t> group_indices;
  groups.reserve(cpus.size());

  for (const auto& cpu : cpus) {
    const auto key = std::pair(cpu.package_id, cpu.core_id);
    const auto [it, inserted] = group_indices.try_emplace(key, groups.size());
    if (inserted) {
      groups.push_back(
          core_group{
              .core_id = cpu.core_id,
              .package_id = cpu.package_id,
              .node_id = cpu.node_id,
              .cpus = {},
          }
      );
    }
    groups[it->second].cpus.push_back(cpu.cpu);
  }

  return groups;
}

inline locality_hint detect_locality_hint(const std::vector<logical_cpu>& cpus) {
  locality_hint locality{
      .node_id = cpus.front().node_id,
      .package_id = cpus.front().package_id,
  };

  if (const int current_cpu_raw = ::sched_getcpu(); current_cpu_raw >= 0) {
    const auto current_cpu = static_cast<std::size_t>(current_cpu_raw);
    for (const auto& cpu : cpus) {
      if (cpu.cpu == current_cpu) {
        locality.node_id = cpu.node_id;
        locality.package_id = cpu.package_id;
        break;
      }
    }
  }

  return locality;
}

inline cpu_topology discover_cpu_topology() {
  const auto allowed_cpus = read_allowed_cpus();
  if (allowed_cpus.empty()) {
    throw std::runtime_error("thread_context: current affinity mask is empty");
  }

  cpu_topology topology;
  topology.cpus = read_logical_cpus(allowed_cpus);
  topology.cores = build_core_groups(topology.cpus);
  topology.locality = detect_locality_hint(topology.cpus);
  return topology;
}

inline void prefer_local_cores(std::vector<core_group>& cores, locality_hint locality) {
  std::stable_sort(cores.begin(), cores.end(), [&](const core_group& lhs, const core_group& rhs) {
    const auto lhs_key = std::pair(lhs.node_id != locality.node_id, lhs.package_id != locality.package_id);
    const auto rhs_key = std::pair(rhs.node_id != locality.node_id, rhs.package_id != locality.package_id);
    return lhs_key < rhs_key;
  });
}

inline std::vector<std::size_t> plan_cpu_order(const std::vector<core_group>& cores, thread_smt smt) {
  std::vector<std::size_t> order;
  order.reserve(cores.size());
  for (const auto& core : cores) {
    if (!core.cpus.empty()) {
      order.push_back(core.cpus.front());
    }
  }

  if (smt == thread_smt::avoid) {
    return order;
  }

  std::size_t max_threads_per_core = 1;
  for (const auto& core : cores) {
    max_threads_per_core = std::max(max_threads_per_core, core.cpus.size());
  }

  for (std::size_t sibling_index = 1; sibling_index < max_threads_per_core; ++sibling_index) {
    for (const auto& core : cores) {
      if (sibling_index < core.cpus.size()) {
        order.push_back(core.cpus[sibling_index]);
      }
    }
  }

  return order;
}

inline std::vector<std::size_t> compute_preferred_cpu_order(thread_policy policy) {
  auto topology = discover_cpu_topology();
  prefer_local_cores(topology.cores, topology.locality);
  return plan_cpu_order(topology.cores, policy.smt);
}

inline std::string format_cpu_list(const std::vector<std::size_t>& cpus) {
  std::string out;
  for (std::size_t i = 0; i < cpus.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += std::to_string(cpus[i]);
  }
  return out;
}

inline bool try_bind_current_thread_to_cpu(std::size_t cpu) noexcept {
  if (cpu >= CPU_SETSIZE) {
    return false;
  }

  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<int>(cpu), &set);
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

inline void bind_current_thread_to_cpu_or_throw(std::size_t cpu) {
  if (!try_bind_current_thread_to_cpu(cpu)) {
    throw std::runtime_error("thread_context: failed to set thread affinity to cpu " + std::to_string(cpu));
  }
}

}

namespace sn::threads::detail {

struct dedicated_cpu_domain {
  explicit dedicated_cpu_domain(std::vector<std::size_t> cpu_order) :
      cpu_order(std::move(cpu_order)), reserved_slots(this->cpu_order.size(), false) {}

  [[nodiscard]] std::size_t cpu_count() const noexcept { return cpu_order.size(); }
  [[nodiscard]] std::size_t cpu_at(std::size_t slot) const noexcept { return cpu_order[slot]; }

  std::vector<std::size_t> reserve(std::size_t count, std::string_view label) {
    std::vector<std::size_t> slots;
    slots.reserve(count);
    for (std::size_t slot = 0; slot < reserved_slots.size() && slots.size() < count; ++slot) {
      if (reserved_slots[slot]) {
        continue;
      }
      reserved_slots[slot] = true;
      slots.push_back(slot);
    }

    if (slots.size() == count) {
      return slots;
    }

    release(slots);
    throw std::runtime_error(
        std::string("thread_context: insufficient dedicated CPUs for ") +
        std::string(label.empty() ? "thread group" : label)
    );
  }

  [[nodiscard]] std::vector<std::size_t> cpus_for(const std::vector<std::size_t>& slots) const {
    std::vector<std::size_t> cpus;
    cpus.reserve(slots.size());
    for (std::size_t slot : slots) {
      cpus.push_back(cpu_at(slot));
    }
    return cpus;
  }

  void release(const std::vector<std::size_t>& slots) noexcept {
    for (std::size_t slot : slots) {
      if (slot < reserved_slots.size()) {
        reserved_slots[slot] = false;
      }
    }
  }

  std::vector<std::size_t> cpu_order;
  std::vector<bool> reserved_slots;
  bool caller_bound = false;
};

struct thread_context_impl {
  explicit thread_context_impl(thread_policy requested_policy) :
      policy(requested_policy), logger(sn::util::log::create("threads")) {
    if (policy.affinity != thread_affinity::dedicated) {
      return;
    }

    dedicated.emplace(linux_tuning::compute_preferred_cpu_order(policy));
    logger.ped("thread affinity enabled");
  }

  thread_policy policy{};
  std::mutex lock;
  std::optional<dedicated_cpu_domain> dedicated{};
  sn::util::log::logger logger;
};

inline std::shared_ptr<thread_context_impl> make_thread_context_impl(thread_policy policy) {
  return std::make_shared<thread_context_impl>(policy);
}

inline void bind_current_thread_impl(thread_context_impl& impl) {
  if (!impl.dedicated.has_value()) {
    return;
  }

  std::size_t cpu = 0;
  {
    std::lock_guard<std::mutex> guard(impl.lock);
    auto& domain = *impl.dedicated;
    if (domain.caller_bound) {
      return;
    }
    const auto slots = domain.reserve(1, "caller");
    cpu = domain.cpu_at(slots.front());
    domain.caller_bound = true;
  }

  linux_tuning::bind_current_thread_to_cpu_or_throw(cpu);
}

inline reservation_result reserve_group_impl(
    thread_context_impl& impl, std::size_t worker_count, std::string_view label
) {
  reservation_result result{};
  result.bindings.resize(worker_count);
  if (!impl.dedicated.has_value() || worker_count == 0) {
    return result;
  }

  std::vector<std::size_t> slots;
  {
    std::lock_guard<std::mutex> guard(impl.lock);
    slots = impl.dedicated->reserve(worker_count, label);
  }

  const auto cpus = impl.dedicated->cpus_for(slots);
  for (std::size_t i = 0; i < cpus.size(); ++i) {
    result.bindings[i].cpu = cpus[i];
  }
  result.slot_indices = std::move(slots);

  return result;
}

inline void release_slots_impl(thread_context_impl& impl, const std::vector<std::size_t>& slot_indices) noexcept {
  if (!impl.dedicated.has_value() || slot_indices.empty()) {
    return;
  }

  std::lock_guard<std::mutex> guard(impl.lock);
  impl.dedicated->release(slot_indices);
}

inline void apply_current_thread_binding_impl(const thread_binding& binding) {
  if (!binding.cpu.has_value()) {
    return;
  }

  linux_tuning::bind_current_thread_to_cpu_or_throw(*binding.cpu);
}

}
