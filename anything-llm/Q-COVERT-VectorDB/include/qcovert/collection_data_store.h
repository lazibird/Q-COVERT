#ifndef QCOVERT_COLLECTION_DATA_STORE_H
#define QCOVERT_COLLECTION_DATA_STORE_H

#include "qcovert/collection_catalog.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace qcovert {

/** One plaintext vector record accepted from the trusted query-service API. */
struct VectorRecord {
    /** Stable application-provided vector identifier. */
    std::string id;
    /** 384-dimensional float32 embedding. */
    std::vector<float> values;
    /** Canonical JSON encoding of opaque citation metadata. */
    std::string metadata_json;
};

/** Aggregate result for one idempotent upsert batch. */
struct UpsertResult {
    /** New IDs added to the collection. */
    uint64_t inserted = 0;
    /** Existing IDs replaced by a later log record. */
    uint64_t updated = 0;
    /** Current number of distinct vector IDs. */
    uint64_t vector_count = 0;
};

/** Current durable WAL boundary used by resumable initialization batches. */
struct VectorLogStatus {
    /** Number of distinct latest-version vector IDs. */
    uint64_t vector_count = 0;
    /** Highest committed record sequence, or zero for an empty log. */
    uint64_t last_sequence = 0;
};

/** Immutable snapshot details returned to the generation manager. */
struct VectorSnapshot {
    /** Number of latest-version records written to the snapshot. */
    uint64_t vector_count = 0;
    /** Last source-log sequence included in the snapshot. */
    uint64_t last_sequence = 0;
};

/** Header-only information for a snapshot, avoiding early vector materialization. */
struct VectorSnapshotInfo {
    uint64_t vector_count = 0;
    uint64_t last_sequence = 0;
};

/** Latest version of one ID appended after a generation snapshot boundary. */
struct VectorDelta {
    VectorRecord record;
    uint64_t sequence = 0;
};

/** Stable latest-record view supplied while inserts are blocked by the data lock. */
struct VectorDeltaBarrier {
    /** Latest version of every ID changed after the caller's sequence. */
    std::vector<VectorDelta> deltas;
    /** Current number of distinct external vector IDs. */
    uint64_t vector_count = 0;
    /** Highest durable source-log sequence at the barrier. */
    uint64_t last_sequence = 0;
};

/**
 * Trusted append-only vector log.
 *
 * The log is the source of truth until an ORAM-backed generation is built. The
 * process keeps only id-to-record offsets in memory, so loading a collection
 * does not require materializing every embedding.
 */
class CollectionDataStore {
public:
    /** Create a data store backed by collection directories in catalog.root(). */
    explicit CollectionDataStore(CollectionCatalog& catalog);

    /** Append or replace a batch of vectors and update the collection vector count. */
    UpsertResult upsert(const std::string& collection, const std::vector<VectorRecord>& records);

    /** Append a batch only when every ID is new to both the collection and this batch. */
    UpsertResult insert_unique(
        const std::string& collection, const std::vector<VectorRecord>& records);

    /** Return the distinct-count and committed sequence boundary under the WAL lock. */
    VectorLogStatus log_status(const std::string& collection);

    /** Remove the WAL of a collection that has never owned an active generation. */
    void reset_uninitialized_collection(const std::string& collection);

    /**
     * Write all latest records to snapshot_file while holding the data lock.
     *
     * Holding the lock prevents an insert from being omitted between snapshot
     * creation and generation activation. The later ORAM builder will replace
     * this short critical section with append-log replay around its long build.
     */
    VectorSnapshot write_latest_snapshot(const std::string& collection,
                                         const std::filesystem::path& snapshot_file);

    /** Return the current distinct vector count. */
    uint64_t vector_count(const std::string& collection);

    /** Drop an in-memory WAL offset index after collection delete or restore. */
    void forget_collection(const std::string& collection);

    /** Read latest ID versions appended after a snapshot for ORAM replay. */
    std::vector<VectorDelta> read_latest_after(const std::string& collection,
                                               uint64_t last_sequence);

    /**
     * Run a final generation commit while holding the insertion lock.
     *
     * The callback receives the latest post-boundary records and must finish
     * ORAM persistence plus catalog activation before returning.
     */
    void with_latest_after_locked(
        const std::string& collection,
        uint64_t last_sequence,
        const std::function<void(const VectorDeltaBarrier&)>& callback);

    /** Load and validate all records from one immutable generation snapshot. */
    static std::vector<VectorRecord> read_snapshot(const std::filesystem::path& snapshot_file);

    /** Validate and read only an immutable snapshot header. */
    static VectorSnapshotInfo inspect_snapshot(const std::filesystem::path& snapshot_file);

    /** Validate a stable vector ID before persisting it in the trusted log. */
    static std::string validate_vector_id(const std::string& id);

private:
    struct RecordLocation {
        uint64_t offset = 0;
        uint64_t sequence = 0;
    };

    struct CollectionIndex {
        bool loaded = false;
        uint64_t next_sequence = 1;
        std::unordered_map<std::string, RecordLocation> latest;
    };

    CollectionCatalog* catalog_;
    std::mutex mutex_;
    std::unordered_map<std::string, CollectionIndex> indexes_;

    std::filesystem::path log_path(const std::string& collection) const;
    UpsertResult append_batch(
        const std::string& collection,
        const std::vector<VectorRecord>& records,
        bool require_unique_ids);
    void load_index_locked(const std::string& collection, CollectionIndex& index);
    VectorRecord read_record_at_locked(const std::string& collection, uint64_t offset) const;
    VectorDeltaBarrier latest_after_locked(
        const std::string& collection,
        CollectionIndex& index,
        uint64_t last_sequence) const;
};

} // namespace qcovert

#endif // QCOVERT_COLLECTION_DATA_STORE_H
