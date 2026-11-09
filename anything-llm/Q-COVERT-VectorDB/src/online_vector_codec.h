#ifndef QCOVERT_ONLINE_VECTOR_CODEC_H
#define QCOVERT_ONLINE_VECTOR_CODEC_H

#include "qcovert/collection_data_store.h"

#include "DoubleORAM.h"
#include "matrix.h"

#include <cstdint>
#include <vector>

namespace qcovert {

/** Quantized insertion data and the obliviously accessed IVF cluster set. */
struct OnlineQuantizedVector384 {
    /** Nearest cluster receiving the new approximate slot. */
    int target_cluster = -1;
    /** Clusters read to make insertion resemble a normal search. */
    std::vector<int> candidate_clusters;
    /** RaBitQ factor and packed one-bit code stored in ClusterORAM. */
    double_oram::AnnQuantizedVectorPayload approximate;
};

/** Shared D=384 online encoding used by finalization and active insertion. */
class OnlineVectorCodec384 {
public:
    /** Compute nprobe=min(C, 2*top_k+floor(C/25)). */
    static uint32_t candidate_cluster_count(uint32_t top_k, uint32_t total_clusters);

    /** Compute ceil(2*top_k+top_k*nprobe*candidate_factor). */
    static uint32_t exact_candidate_count(
        uint32_t top_k,
        uint32_t nprobe,
        double candidate_factor);

    /** Select candidate clusters and encode one vector relative to its nearest centroid. */
    static OnlineQuantizedVector384 quantize(
        const VectorRecord& record,
        const Matrix<float>& centroids,
        const Matrix<float>& projection,
        uint32_t top_k = 10);

    /** Encode 384 raw float32 values followed by their internal RingORAM ID. */
    static std::vector<uint8_t> raw_float32_exact_payload(
        const VectorRecord& record,
        int internal_id);
};

} // namespace qcovert

#endif // QCOVERT_ONLINE_VECTOR_CODEC_H
