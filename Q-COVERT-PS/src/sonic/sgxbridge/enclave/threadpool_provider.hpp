#pragma once

#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/sgxbridge/enclave/threadpool_portal.hpp"

namespace sn::sgxbridge::enclave {

tp::provider make_threadpool_provider(threadpool_portal& portal);

}
