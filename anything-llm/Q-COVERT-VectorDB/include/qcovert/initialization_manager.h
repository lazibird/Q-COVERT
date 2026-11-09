#ifndef QCOVERT_INITIALIZATION_MANAGER_H
#define QCOVERT_INITIALIZATION_MANAGER_H

#include "qcovert/collection_catalog.h"
#include "qcovert/collection_data_store.h"
#include "qcovert/generation_manager.h"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace qcovert {

/** A semantic conflict caused by an active or incompatible initialization session. */
class InitializationConflict : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

enum class InitializationState {
    Uploading,
    Building,
    Succeeded,
    Failed,
    Aborted,
};

struct InitializationBatchReceipt {
    uint64_t batch_index = 0;
    uint64_t record_count = 0;
    std::string checksum;
};

struct PendingInitializationBatch {
    bool present = false;
    uint64_t batch_index = 0;
    uint64_t record_count = 0;
    uint64_t first_sequence = 0;
    uint64_t last_sequence = 0;
    std::string checksum;
};

/** Durable trusted-side state for one empty-collection bulk initialization. */
struct InitializationSession {
    uint32_t schema_version = 1;
    std::string initialization_id;
    std::string collection;
    InitializationState state = InitializationState::Uploading;
    uint64_t expected_count = 0;
    uint64_t received_count = 0;
    uint64_t next_batch_index = 0;
    uint64_t last_log_sequence = 0;
    std::string fingerprint;
    std::string idempotency_key;
    std::string generation_job_id;
    std::string error;
    uint64_t created_at_unix_ms = 0;
    uint64_t updated_at_unix_ms = 0;
    std::vector<InitializationBatchReceipt> batches;
    PendingInitializationBatch pending_batch;
};

struct InitializationBatchResult {
    InitializationSession session;
    bool idempotent_replay = false;
};

/**
 * Coordinates resumable first-load sessions without performing online ORAM inserts.
 *
 * Vector bodies remain in vectors.log. The initialization manifest records batch
 * checksums and WAL boundaries, then finalize starts exactly one full generation.
 */
class InitializationManager {
public:
    InitializationManager(CollectionCatalog& catalog,
                          CollectionDataStore& data_store,
                          GenerationManager& generation_manager);

    InitializationSession create(
        const std::string& collection,
        uint64_t expected_count,
        const std::string& fingerprint,
        const std::string& idempotency_key);

    InitializationSession get(
        const std::string& collection, const std::string& initialization_id);

    InitializationBatchResult append_batch(
        const std::string& collection,
        const std::string& initialization_id,
        uint64_t batch_index,
        const std::string& checksum,
        const std::vector<VectorRecord>& records);

    InitializationSession finalize(
        const std::string& collection, const std::string& initialization_id);

    InitializationSession abort(
        const std::string& collection, const std::string& initialization_id);

    /** Reject a normal API operation while upload/build/failure awaits resolution. */
    void require_no_active_initialization(
        const std::string& collection, const char* operation);

    static const char* state_name(InitializationState state);
    static std::string validate_initialization_id(const std::string& initialization_id);
    static std::string validate_checksum(const std::string& checksum);

private:
    CollectionCatalog* catalog_;
    CollectionDataStore* data_store_;
    GenerationManager* generation_manager_;
    std::mutex mutex_;

    std::filesystem::path initialization_directory(const std::string& collection) const;
    std::filesystem::path session_path(
        const std::string& collection, const std::string& initialization_id) const;
    InitializationSession load_locked(
        const std::string& collection, const std::string& initialization_id) const;
    void write_locked(const InitializationSession& session) const;
    std::vector<InitializationSession> list_locked(const std::string& collection) const;
    void reconcile_pending_batch_locked(InitializationSession& session);
    void reconcile_generation_locked(InitializationSession& session);
    static bool blocks_normal_operations(InitializationState state);
};

} // namespace qcovert

#endif // QCOVERT_INITIALIZATION_MANAGER_H
