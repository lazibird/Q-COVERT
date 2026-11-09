#ifndef QCOVERT_GENERATION_MANAGER_H
#define QCOVERT_GENERATION_MANAGER_H

#include "qcovert/collection_catalog.h"
#include "qcovert/collection_data_store.h"
#include "qcovert/generation_key_store.h"
#include "qcovert/oram_generation_builder.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace qcovert {

/** Python Faiss worker configuration used by asynchronous generation builds. */
struct GenerationBuildConfig {
    /** Python interpreter containing numpy and faiss. Empty selects the project .venv. */
    std::string python_executable;
    /** Faiss worker script path. Empty selects the project bundled worker. */
    std::string faiss_worker;
    /** Number of Faiss OpenMP threads; zero keeps the Faiss default. */
    uint32_t faiss_threads = 0;
    /** Lloyd iteration count passed to faiss.Kmeans. */
    uint32_t kmeans_iterations = 25;
    /** Independent Faiss k-means restarts. */
    uint32_t kmeans_restarts = 2;
    /** Deterministic Faiss training seed. */
    uint32_t seed = 0;
    /** Remote encrypted-tree construction settings. */
    OramGenerationConfig oram;
};

/** Durable state exposed for an asynchronous immutable-generation job. */
enum class GenerationJobState {
    /** The request was accepted but the worker has not started snapshotting. */
    Queued,
    /** The worker is writing a private immutable source-data snapshot. */
    Building,
    /** All enabled build stages completed; catalog stage reports IVF or ORAM readiness. */
    Succeeded,
    /** The job failed and the prior active generation, if any, remains selected. */
    Failed,
};

/** Metadata for a generation job, persisted under collection/jobs/. */
struct GenerationJob {
    /** Opaque API job identifier. */
    std::string job_id;
    /** Collection being built. */
    std::string collection;
    /** Immutable generation identifier allocated by the catalog. */
    std::string generation;
    /** Current worker state. */
    GenerationJobState state = GenerationJobState::Queued;
    /** Number of records captured by the generation snapshot after success. */
    uint64_t vector_count = 0;
    /** Failure diagnostic, empty unless state is Failed. */
    std::string error;
};

/**
 * Creates immutable trusted-side vector snapshots without replacing the active
 * generation. New appends remain in vectors.log after the captured sequence;
 * ORAM initialization must replay them before validation and activation.
 */
class GenerationManager {
public:
    /** Coordinate generation jobs using one collection catalog and vector log. */
    GenerationManager(CollectionCatalog& catalog,
                      CollectionDataStore& data_store,
                      GenerationBuildConfig build_config = {});
    /** Wait for owned build workers before releasing catalog/data-store references. */
    ~GenerationManager();

    /** Reject new generation jobs and wait for all jobs already owned by this process. */
    void shutdown();

    /** Queue an asynchronous generation snapshot and return its durable job record. */
    GenerationJob start_generation(
        const std::string& collection, bool allow_followup_generation = true);

    /** Queue one rebuild after a 10,000-vector crossing or exhausted active reserve. */
    std::optional<GenerationJob> maybe_start_generation(
        const std::string& collection,
        uint64_t previous_vector_count,
        uint64_t current_vector_count,
        bool active_capacity_exhausted = false);

    /** Pure threshold predicate used by automatic generation scheduling. */
    static bool requires_automatic_generation(
        uint64_t previous_vector_count,
        uint64_t current_vector_count,
        bool active_capacity_exhausted = false);

    /** Load a durable job by collection and job ID. */
    GenerationJob get_job(const std::string& collection, const std::string& job_id) const;

    /** Convert a job state into its stable API representation. */
    static const char* job_state_name(GenerationJobState state);

private:
    CollectionCatalog* catalog_;
    CollectionDataStore* data_store_;
    mutable std::mutex mutex_;
    uint64_t next_job_sequence_ = 1;
    GenerationBuildConfig build_config_;
    GenerationKeyStore key_store_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;

    void run_generation(GenerationJob job, bool allow_followup_generation);
    void write_job(const GenerationJob& job) const;
    GenerationJob read_job(const std::string& collection, const std::string& job_id) const;
};

} // namespace qcovert

#endif // QCOVERT_GENERATION_MANAGER_H
