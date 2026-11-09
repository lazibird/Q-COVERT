#ifndef QCOVERT_SEARCH_SERVICE_H
#define QCOVERT_SEARCH_SERVICE_H

#include "qcovert/collection_catalog.h"
#include "qcovert/collection_data_store.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace qcovert {

/** Parameters for one private IVF-RaBitQ query. */
struct SearchRequest {
    /** Plaintext 384-dimensional query visible only to qcovert_query. */
    std::vector<float> vector;
    /** Number of final exact neighbors requested. */
    uint32_t top_k = 10;
    /** k2 multiplier: k2=ceil(2*k+k*nprobe*candidate_factor). */
    double candidate_factor = 0.5;
};

/** One stable application-level neighbor returned by private search. */
struct SearchHit {
    /** External vector ID supplied by the caller during upsert. */
    std::string id;
    /** Exact squared L2 distance to the query. */
    float distance = 0.0f;
    /** Monotonic similarity score, 1/(1+distance). */
    float score = 0.0f;
    /** Canonical JSON metadata persisted with the latest ID version. */
    std::string metadata_json;
};

/** Search output plus the immutable generation that served it. */
struct SearchResponse {
    std::string generation;
    uint32_t nprobe = 0;
    uint32_t exact_candidate_limit = 0;
    /** Elapsed trusted-side search time; excludes HTTP parsing and response serialization. */
    uint64_t search_ms = 0;
    std::vector<SearchHit> hits;
};

/** Result of bringing one active ORAM generation up to the latest committed WAL record. */
struct ActiveGenerationSyncResult {
    /** Whether the collection had an active generation to update. */
    bool active_generation = false;
    /** Number of latest-version WAL records inserted into the active generation. */
    uint64_t applied_records = 0;
    /** True when no further physical RingORAM IDs remain in this generation. */
    bool capacity_exhausted = false;
    /** Internal signal that another worker checkpointed this cached runtime. */
    bool runtime_reload_required = false;
};

/** WAL and active-ORAM outcome for one serialized online upsert batch. */
struct OnlineUpsertResult {
    /** Durable append-log counts. */
    UpsertResult wal;
    /** Active generation synchronization details. */
    ActiveGenerationSyncResult active;
};

/** Loads and caches active DoubleORAM generations for trusted-side search. */
class SearchService {
public:
    /** Use catalog manifests and the configured storage daemon for runtime loading. */
    SearchService(
        CollectionCatalog& catalog,
        CollectionDataStore& data_store,
        std::string storage_host,
        int storage_port);
    ~SearchService();

    /** Execute one all-candidate ClusterORAM access plus one RingORAM access. */
    SearchResponse search(const std::string& collection, const SearchRequest& request);

    /** Apply committed WAL records to the active generation and checkpoint both ORAMs. */
    ActiveGenerationSyncResult synchronize_active(const std::string& collection);

    /** Capacity-check, commit, and synchronize one online upsert batch. */
    OnlineUpsertResult upsert(
        const std::string& collection,
        const std::vector<VectorRecord>& records);

    /** Durably checkpoint and evict a cached runtime before a management operation. */
    void checkpoint_and_release(const std::string& collection);

    /** Durably checkpoint every cached runtime during process shutdown. */
    void checkpoint_all();

    /** Return whether this process currently caches the collection's active runtime. */
    bool has_cached_runtime(const std::string& collection);

private:
    struct Runtime;

    CollectionCatalog* catalog_;
    CollectionDataStore* data_store_;
    std::string storage_host_;
    int storage_port_;
    std::mutex mutex_;
    std::mutex upsert_mutex_;
    std::unordered_map<std::string, std::shared_ptr<Runtime>> runtimes_;

    std::shared_ptr<Runtime> runtime_for(
        const std::string& collection,
        const CollectionManifest& manifest);
    void discard_runtime(const std::string& collection, const std::shared_ptr<Runtime>& runtime);
};

} // namespace qcovert

#endif // QCOVERT_SEARCH_SERVICE_H
