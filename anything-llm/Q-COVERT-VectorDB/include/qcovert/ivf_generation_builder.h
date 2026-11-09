#ifndef QCOVERT_IVF_GENERATION_BUILDER_H
#define QCOVERT_IVF_GENERATION_BUILDER_H

#include <cstdint>
#include <filesystem>
#include <string>

namespace qcovert {

/** Fixed service-side dimensions for the first AnythingLLM integration. */
constexpr uint32_t kServiceVectorDimension = 384;
constexpr uint32_t kServiceRabitqBits = 384;
constexpr uint32_t kServiceQueryBits = 4;

/** Summary of one persisted IVF-RaBitQ generation build. */
struct IvfGenerationArtifacts {
    /** Number of source vectors indexed. */
    uint64_t vector_count = 0;
    /** Actual number of nearest-assignment IVF clusters. */
    uint32_t cluster_count = 0;
    /** Number of 32-vector approximate blocks across all clusters. */
    uint64_t approximate_block_count = 0;
};

/**
 * Build a fixed D=384, B=384 IVF-RaBitQ index from vectors.qvec.
 *
 * Collections below 10,000 vectors use one cluster. Larger collections use
 * ceil(sqrt(N)) clusters. Python Faiss trains the centroids and performs exact
 * IndexFlatL2 nearest assignment. All outputs remain in the trusted generation
 * directory.
 */
class IvfGenerationBuilder384 {
public:
    /** Build and persist all IVF-RaBitQ artifacts in generation_directory. */
    static IvfGenerationArtifacts build(
            const std::filesystem::path& snapshot_file,
            const std::filesystem::path& generation_directory,
            const std::filesystem::path& python_executable,
            const std::filesystem::path& faiss_worker,
            uint32_t kmeans_iterations = 25,
            uint32_t kmeans_restarts = 2,
            uint32_t seed = 0,
            uint32_t faiss_threads = 0);
};

} // namespace qcovert

#endif // QCOVERT_IVF_GENERATION_BUILDER_H
