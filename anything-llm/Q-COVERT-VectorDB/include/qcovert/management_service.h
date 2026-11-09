#ifndef QCOVERT_MANAGEMENT_SERVICE_H
#define QCOVERT_MANAGEMENT_SERVICE_H

#include "qcovert/collection_catalog.h"

#include <cstdint>
#include <string>
#include <vector>

namespace qcovert {

class CollectionDataStore;
class SearchService;

/** Persisted capacity information for one active ORAM tree. */
struct ManagedOramTreeStats {
    uint64_t real_blocks = 0;
    uint64_t logical_block_limit = 0;
    uint64_t real_block_capacity = 0;
    uint64_t num_buckets = 0;
    int block_size = 0;
    int num_levels = 0;
    int cached_levels = 0;
};

/** Structured, disk-backed statistics for one collection. */
struct CollectionManagementStats {
    CollectionManifest manifest;
    bool generation_ready = false;
    bool runtime_cached = false;
    uint64_t trusted_storage_bytes = 0;
    uint64_t backup_count = 0;
    uint64_t cluster_count = 0;
    uint64_t generation_vector_capacity = 0;
    uint64_t generation_vector_count = 0;
    uint64_t last_log_sequence = 0;
    uint64_t replayed_records = 0;
    uint64_t cluster_stash_blocks = 0;
    uint64_t ring_stash_blocks = 0;
    ManagedOramTreeStats cluster_oram;
    ManagedOramTreeStats ring_oram;
};

/** Aggregate catalog statistics used by AnythingLLM's totalVectors operation. */
struct GlobalManagementStats {
    uint64_t collection_count = 0;
    uint64_t vector_count = 0;
    uint64_t active_collections = 0;
    uint64_t building_collections = 0;
    uint64_t prepared_collections = 0;
    uint64_t failed_collections = 0;
    uint64_t empty_collections = 0;
    uint64_t trusted_storage_bytes = 0;
};

/** Coordinates trusted state, active runtimes, and remote encrypted trees. */
class ManagementService {
public:
    ManagementService(
        CollectionCatalog& catalog,
        CollectionDataStore& data_store,
        SearchService& search_service,
        std::string storage_host,
        int storage_port);

    /** Return aggregate vector and lifecycle counts without loading an ORAM runtime. */
    GlobalManagementStats global_stats() const;

    /** Return persisted collection and active generation capacity information. */
    CollectionManagementStats collection_stats(const std::string& collection);

    /** Checkpoint active ORAM state and permanently delete the collection on both processes. */
    void delete_collection(const std::string& collection);

    /** Create a named trusted/server backup after a durable ORAM checkpoint. */
    void create_backup(const std::string& collection, const std::string& backup_name);

    /** Restore a named trusted/server backup and invalidate all process-local caches. */
    CollectionManifest restore_backup(
        const std::string& collection, const std::string& backup_name);

    /** List named backups known to the trusted query process. */
    std::vector<std::string> list_backups(const std::string& collection) const;

private:
    CollectionCatalog* catalog_;
    CollectionDataStore* data_store_;
    SearchService* search_service_;
    std::string storage_host_;
    int storage_port_;
};

} // namespace qcovert

#endif // QCOVERT_MANAGEMENT_SERVICE_H
