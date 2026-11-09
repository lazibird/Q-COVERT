#ifndef QCOVERT_STORAGE_PROTOCOL_H
#define QCOVERT_STORAGE_PROTOCOL_H

#include <cstdint>
#include <stdexcept>
#include <string>

namespace qcovert {

/** Wire-format version for the query-service to storage-service handshake. */
constexpr uint32_t kStorageProtocolVersion = 3;
/** Four-byte little-endian magic for a Q-COVERT storage handshake. */
constexpr uint32_t kStorageProtocolMagic = 0x56514351U; // "QCQV"

/** Requested lifecycle action for one ORAM tree connection. */
enum class StorageAction : uint32_t {
    /** Create a new empty encrypted ORAM tree. */
    Create = 1,
    /** Open an existing encrypted ORAM tree. */
    Open = 2,
    /** Remove every encrypted generation belonging to one collection. */
    DeleteCollection = 3,
    /** Copy every encrypted generation into a named server-side backup. */
    BackupCollection = 4,
    /** Replace every encrypted generation from a named server-side backup. */
    RestoreCollection = 5,
    /** Remove one named server-side collection backup. */
    DeleteBackup = 6,
};

/** Distinguishes Q-COVERT's low-precision and exact-data ORAM trees. */
enum class StorageTreeKind : uint32_t {
    /** ClusterORAM stores IVF-RaBitQ approximate blocks. */
    Cluster = 1,
    /** RingORAM stores exact vector payload blocks. */
    Ring = 2,
};

/**
 * Fixed-size handshake sent before the existing RemoteRing binary protocol.
 *
 * All fields are host-endian because this prototype only supports homogeneous
 * Linux deployments, just as the inherited RemoteRing protocol does. A future
 * wire-compatible version must explicitly serialize fields in network order.
 */
#pragma pack(push, 1)
struct StorageHandshake {
    /** Must equal kStorageProtocolMagic. */
    uint32_t magic = kStorageProtocolMagic;
    /** Must equal kStorageProtocolVersion. */
    uint32_t version = kStorageProtocolVersion;
    /** StorageAction value. */
    uint32_t action = 0;
    /** StorageTreeKind value. */
    uint32_t tree_kind = 0;
    /** Reserved; must be zero because AES-GCM authentication is always enabled. */
    uint32_t integrity = 0;
    /** ORAM block payload size in int slots. */
    int32_t block_size = 0;
    /** Number of real slots per bucket. */
    int32_t real_bucket_size = 0;
    /** Number of dummy slots per bucket. */
    int32_t dummy_size = 0;
    /** Reverse-lexicographic eviction rate. */
    int32_t evict_rate = 0;
    /** ORAM tree level count. */
    int32_t num_levels = 0;
    /** Number of client-cached top levels. */
    int32_t cached_levels = 0;
    /** Collection identifier, NUL terminated and limited to 95 bytes. */
    char collection[96] = {};
    /** Immutable generation identifier, NUL terminated and limited to 95 bytes. */
    char generation[96] = {};
};
#pragma pack(pop)

/** Server response for the handshake, before RemoteRing traffic starts. */
struct StorageHandshakeReply {
    /** Zero means success; nonzero is a stable StorageStatus value. */
    uint32_t status = 0;
};

/** Stable errors returned by the storage handshake. */
enum class StorageStatus : uint32_t {
    Ok = 0,
    InvalidHandshake = 1,
    InvalidIdentifier = 2,
    InvalidConfig = 3,
    TreeAlreadyExists = 4,
    TreeNotFound = 5,
    TreeLoadFailed = 6,
    InternalError = 7,
    /** Another client currently owns the mutable session for this tree. */
    TreeBusy = 8,
    /** The requested named backup already exists. */
    BackupAlreadyExists = 9,
    /** The requested named backup does not exist. */
    BackupNotFound = 10,
};

/** Validate and convert a fixed-size identifier stored in a handshake. */
std::string checked_identifier(const char* value, size_t capacity, const char* field_name);

/** Convert a StorageStatus into a short diagnostic suitable for a service log. */
const char* storage_status_message(StorageStatus status);

} // namespace qcovert

#endif // QCOVERT_STORAGE_PROTOCOL_H
