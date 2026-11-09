#ifndef QCOVERT_ORAM_GENERATION_BUILDER_H
#define QCOVERT_ORAM_GENERATION_BUILDER_H

#include "evp.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace double_oram {
class ClusterORAM;
class RingORAM;
}

namespace qcovert {

class CollectionCatalog;
class CollectionDataStore;

/** Runtime-independent parameters used to construct both ORAM trees. */
struct OramGenerationConfig {
    /** Enable remote ORAM construction after IVF artifacts are ready. */
    bool enabled = false;
    /** Storage daemon IPv4 address. */
    std::string storage_host = "127.0.0.1";
    /** Storage daemon TCP port. */
    int storage_port = 19091;
    /** Real slots per bucket for both trees. */
    int real_bucket_size = 32;
    /** Dummy slots per bucket for both trees. */
    int dummy_size = 48;
    /** Reverse-lexicographic eviction rate for both trees. */
    int evict_rate = 48;
    /** Maximum client-cached top levels; short trees use fewer levels. */
    int cached_levels = 4;
    /** Deterministic initial RingORAM leaf seed. */
    uint32_t seed = 20260623U;
    /** Maximum plaintext bucket bytes assembled per upload batch. */
    size_t upload_batch_bytes = 16U * 1024U * 1024U;
};

/** Persisted physical layout for one initialized ORAM tree. */
struct OramTreeLayout {
    /** Plain block payload length in int slots. */
    int block_size = 0;
    /** Real blocks present when the generation snapshot was constructed. */
    uint64_t real_blocks = 0;
    /** Maximum logical block IDs accepted before this generation must rebuild. */
    uint64_t logical_block_limit = 0;
    /** Real-block capacity available outside the cached tree top. */
    uint64_t real_block_capacity = 0;
    /** Tree level count. */
    int num_levels = 0;
    /** Number of cached top levels. */
    int cached_levels = 0;
    /** Number of physical buckets. */
    uint64_t num_buckets = 0;
};

/** Summary returned after both encrypted trees and client states are durable. */
struct OramGenerationArtifacts {
    /** ClusterORAM approximate-data tree layout. */
    OramTreeLayout cluster;
    /** RingORAM exact-data tree layout. */
    OramTreeLayout ring;
    /** Number of IVF clusters represented by ClusterORAM metadata. */
    uint32_t cluster_count = 0;
    /** Raw float32 exact payload bytes, including the internal vector ID. */
    uint32_t exact_payload_bytes = 0;
};

/** Latest external-ID mapping persisted beside one active ORAM generation. */
struct OramLiveRecord {
    /** Monotonic physical RingORAM block ID for this latest version. */
    uint32_t internal_id = 0;
    /** Stable application-provided ID. */
    std::string external_id;
    /** Canonical JSON citation metadata. */
    std::string metadata_json;
};

/** Build and persist the D=384 ClusterORAM and RingORAM generation. */
class OramGenerationBuilder384 {
public:
    /** Return the exclusive next 10,000-vector generation capacity boundary. */
    static uint64_t next_vector_capacity(uint64_t current_vector_count);

    /**
     * Construct both trees from generation_directory/ivf_rabitq.index.
     *
     * The storage daemon must already be running. This function checkpoints
     * each tree, reopens it, verifies a sample block, and atomically writes the
     * trusted client state before returning.
     */
    static OramGenerationArtifacts build(
        const std::string& collection,
        const std::string& generation,
        const std::filesystem::path& generation_directory,
        const AeadKey& generation_key,
        const OramGenerationConfig& config);
};

/** Versioned persistence for trusted ClusterORAM/RingORAM client metadata. */
class OramClientStateStore {
public:
    /** Read per-cluster block counts needed before constructing ClusterORAM. */
    static std::vector<int> read_cluster_block_counts(
        const std::filesystem::path& path,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config);

    /** Restore all RingORAM metadata and counters into an empty matching object. */
    static void load_ring(
        const std::filesystem::path& path,
        double_oram::RingORAM& ring,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config);

    /** Restore all ClusterORAM metadata and counters into an empty matching object. */
    static void load_cluster(
        const std::filesystem::path& path,
        double_oram::ClusterORAM& cluster,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config);

    /** Atomically save RingORAM metadata, counters, and any remaining stash blocks. */
    static void save_ring(
        const std::filesystem::path& path,
        const double_oram::RingORAM& ring,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config);

    /** Atomically save ClusterORAM metadata, counters, and any remaining stash blocks. */
    static void save_cluster(
        const std::filesystem::path& path,
        const double_oram::ClusterORAM& cluster,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config);

    /** Checkpoint all trusted files describing a live online-updated generation. */
    static void save_active_generation(
        const std::filesystem::path& generation_directory,
        const double_oram::RingORAM& ring,
        const double_oram::ClusterORAM& cluster,
        const OramGenerationArtifacts& artifacts,
        const OramGenerationConfig& config,
        const std::vector<OramLiveRecord>& live_records,
        uint64_t vector_count,
        uint64_t last_log_sequence,
        uint64_t replayed_records);
};

/** Result of replaying post-snapshot vectors and activating one ORAM generation. */
struct OramGenerationFinalizeResult {
    /** Current number of distinct external vector IDs. */
    uint64_t vector_count = 0;
    /** Last vector-log sequence incorporated into the active generation. */
    uint64_t last_sequence = 0;
    /** Number of post-snapshot latest-version records replayed. */
    uint64_t replayed_records = 0;
    /** Final physical layouts, including obsolete blocks retained for update privacy. */
    OramGenerationArtifacts artifacts;
};

/** Replay concurrent inserts, persist residual stashes, validate, and activate D=384 ORAMs. */
class OramGenerationFinalizer384 {
public:
    /**
     * Restore an OramReady generation, replay vector-log deltas, and activate it.
     *
     * A final data-store write barrier covers the last replay, both storage
     * checkpoints, trusted client-state publication, and catalog activation.
     */
    static OramGenerationFinalizeResult finalize(
        CollectionCatalog& catalog,
        CollectionDataStore& data_store,
        const std::string& collection,
        const std::string& generation,
        const std::filesystem::path& generation_directory,
        const AeadKey& generation_key,
        uint64_t snapshot_last_sequence,
        OramGenerationArtifacts artifacts,
        const OramGenerationConfig& config);
};

} // namespace qcovert

#endif // QCOVERT_ORAM_GENERATION_BUILDER_H
