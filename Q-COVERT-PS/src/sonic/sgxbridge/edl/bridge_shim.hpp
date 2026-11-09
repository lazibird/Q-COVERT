#pragma once

#include <cstddef>
#include <cstdint>

#include <sgx_eid.h>
#include <sgx_error.h>

extern "C" {

#if defined(SGX_TRUSTED)
#define SN_SGX_OCALL_PROTO(name, ...) sgx_status_t name(sgx_status_t* retval, __VA_ARGS__);
#else
#define SN_SGX_OCALL_PROTO(name, ...) sgx_status_t name(__VA_ARGS__);
#endif

SN_SGX_OCALL_PROTO(sonic_sgxbridge_log_sink, void* host_state, const char* message)
SN_SGX_OCALL_PROTO(sonic_sgxbridge_calculate_cycle_scale, void* host_state, double* ns_per_cycle)
SN_SGX_OCALL_PROTO(sonic_sgxbridge_sleep_ms, void* host_state, std::uint32_t millis)
SN_SGX_OCALL_PROTO(sonic_sgxbridge_query_time_ns, void* host_state, std::uint64_t* out_nanoseconds)

SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_threadpool_host_create, void* host_state, std::uint64_t pool_id, std::uint32_t worker_count,
    std::uint32_t queue_capacity, std::uint32_t queue_policy, void** out_handshake, std::uint32_t* out_status,
    std::uint32_t* out_detail
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_threadpool_host_destroy, void* host_state, std::uint64_t pool_id, std::uint32_t* out_status,
    std::uint32_t* out_detail
)

SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_hostbuf_create, void* host_state, std::size_t size, void** out_ptr, std::size_t* out_size,
    std::uint64_t* out_id, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(sonic_sgxbridge_hostbuf_release, void* host_state, std::uint64_t id, std::uint32_t* out_status)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_hostbuf_view, void* host_state, std::uint64_t id, void** out_ptr, std::size_t* out_size,
    std::uint32_t* out_status
)

SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_storage_open, void* host_state, const char* data_path, const char* meta_path, std::uint8_t create,
    std::uint8_t truncate, std::uint64_t* out_handle, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(sonic_sgxbridge_storage_close, void* host_state, std::uint64_t handle, std::uint32_t* out_status)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_storage_resize, void* host_state, std::uint64_t handle, std::uint64_t pages, std::size_t data_bytes,
    std::size_t meta_bytes, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_storage_read_data, void* host_state, std::uint64_t handle, std::uint64_t page_id, void* dst,
    std::size_t bytes, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_storage_write_data, void* host_state, std::uint64_t handle, std::uint64_t page_id, const void* src,
    std::size_t bytes, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_storage_read_meta, void* host_state, std::uint64_t handle, std::uint64_t page_id, void* dst,
    std::size_t bytes, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_storage_write_meta, void* host_state, std::uint64_t handle, std::uint64_t page_id, const void* src,
    std::size_t bytes, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_storage_write_pages, void* host_state, std::uint64_t handle, std::size_t page_count,
    const std::uint64_t* page_ids, const void* data, const void* meta, std::size_t data_bytes_per_page,
    std::size_t meta_bytes_per_page, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(sonic_sgxbridge_storage_flush, void* host_state, std::uint64_t handle, std::uint32_t* out_status)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_fs_remove, void* host_state, const char* path, std::uint8_t* out_removed, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_fs_remove_all, void* host_state, const char* path, std::uint64_t* out_removed,
    std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_fs_exists, void* host_state, const char* path, std::uint8_t* out_exists, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_fs_create_directories, void* host_state, const char* path, std::uint8_t* out_created,
    std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_fs_rename, void* host_state, const char* from_path, const char* to_path, std::uint8_t* out_renamed,
    std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_fs_copy_file, void* host_state, const char* from_path, const char* to_path,
    std::uint8_t overwrite_existing, std::uint8_t* out_copied, std::uint32_t* out_status
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_fs_file_size, void* host_state, const char* path, std::uint64_t* out_size, std::uint32_t* out_status
)

SN_SGX_OCALL_PROTO(sonic_sgxbridge_dist_rank, void* host_state, int* out_rank)
SN_SGX_OCALL_PROTO(sonic_sgxbridge_dist_world_size, void* host_state, int* out_world)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_dist_send_from_hostbuf, void* host_state, int dest_rank, std::uint64_t buffer_id,
    std::size_t offset, std::size_t len
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_dist_try_recv, void* host_state, int* out_has_msg, int* out_src_rank, std::uint64_t* out_id,
    std::size_t* out_size
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_dist_recv, void* host_state, int* out_src_rank, std::uint64_t* out_id, std::size_t* out_size
)
SN_SGX_OCALL_PROTO(
    sonic_sgxbridge_dist_recv_for, void* host_state, std::uint64_t timeout_ms, int* out_has_msg, int* out_src_rank,
    std::uint64_t* out_id, std::size_t* out_size
)
SN_SGX_OCALL_PROTO(sonic_sgxbridge_dist_pending_messages, void* host_state, std::size_t* out_count)
SN_SGX_OCALL_PROTO(sonic_sgxbridge_dist_barrier, void* host_state)
SN_SGX_OCALL_PROTO(sonic_sgxbridge_dist_allreduce_sum, void* host_state, double local_value, double* out_value)

#undef SN_SGX_OCALL_PROTO

#if !defined(SGX_TRUSTED)
sgx_status_t sonic_sgxbridge_threadpool_worker_entry(
    sgx_enclave_id_t enclave_id, std::uint32_t* retval, std::uint64_t pool_id, std::uint32_t worker_index
);
#endif

}
