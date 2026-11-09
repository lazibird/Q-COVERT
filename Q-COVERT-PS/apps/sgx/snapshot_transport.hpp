#pragma once
#include "qcovert/snapshot.hpp"
namespace qcovert::snapshot {
// Shared OCALL transport and SGX sealing for experiment initialization images.
transport sgx_transport(void *host);
}
