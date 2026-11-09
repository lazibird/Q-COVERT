#pragma once

namespace sn::sgxbridge::host {
class threadpool_manager;
}

namespace sn::sgxbridge::tp {

struct threadpool_services {
  host::threadpool_manager* manager{nullptr};
};

} // namespace sn::sgxbridge::tp
