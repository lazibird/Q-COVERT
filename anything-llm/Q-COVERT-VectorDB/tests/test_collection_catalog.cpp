#include "qcovert/collection_catalog.h"
#include "qcovert/collection_data_store.h"
#include "qcovert/generation_manager.h"
#include "qcovert/initialization_manager.h"

#include "ivf_rabitq_index.h"
#include "matrix.h"
#include "private_ivf_rabitq.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

int main() {
    const fs::path root = "/tmp/qcovert-catalog-test";
    std::error_code error;
    fs::remove_all(root, error);
    if (error) {
        std::cerr << "cannot clear test directory: " << error.message() << "\n";
        return 1;
    }

    try {
        qcovert::CollectionCatalog catalog(root);
        const qcovert::CreateCollectionResult created = catalog.create_collection(
            "anythingllm_workspace_1", 384, "l2", "Xenova/all-MiniLM-L6-v2");
        if (!created.created || created.manifest.state != qcovert::CollectionState::Empty ||
            created.manifest.vector_count != 0) {
            throw std::runtime_error("new collection manifest is invalid");
        }

        const qcovert::CreateCollectionResult repeated = catalog.create_collection(
            "anythingllm_workspace_1", 384, "l2", "Xenova/all-MiniLM-L6-v2");
        if (repeated.created || repeated.manifest.name != "anythingllm_workspace_1") {
            throw std::runtime_error("compatible collection creation is not idempotent");
        }

        bool rejected_mismatch = false;
        try {
            catalog.create_collection("anythingllm_workspace_1", 384, "l2", "another-model");
        } catch (const std::invalid_argument&) {
            rejected_mismatch = true;
        }
        if (!rejected_mismatch) {
            throw std::runtime_error("incompatible collection request was accepted");
        }

        qcovert::CollectionCatalog reloaded_catalog(root);
        const qcovert::CollectionManifest reloaded =
            reloaded_catalog.get_collection("anythingllm_workspace_1");
        if (reloaded.dimension != 384 || reloaded.metric != "l2" ||
            reloaded.embedding_model != "Xenova/all-MiniLM-L6-v2") {
            throw std::runtime_error("persisted collection manifest was not reloaded correctly");
        }

        qcovert::CollectionDataStore data_store(reloaded_catalog);
        qcovert::VectorRecord first;
        first.id = "chunk-a";
        first.values.assign(384, 1.0f);
        first.metadata_json = "{\"source\":\"first\"}";
        qcovert::VectorRecord second;
        second.id = "chunk-b";
        second.values.assign(384, 2.0f);
        second.metadata_json = "null";
        qcovert::UpsertResult upsert = data_store.upsert("anythingllm_workspace_1", {first, second});
        if (upsert.inserted != 2 || upsert.updated != 0 || upsert.vector_count != 2) {
            throw std::runtime_error("initial vector upsert result is invalid");
        }

        first.values[0] = 3.0f;
        first.metadata_json = "{\"source\":\"replacement\"}";
        upsert = data_store.upsert("anythingllm_workspace_1", {first});
        if (upsert.inserted != 0 || upsert.updated != 1 || upsert.vector_count != 2) {
            throw std::runtime_error("replacement vector upsert result is invalid");
        }

        qcovert::GenerationManager generation_manager(reloaded_catalog, data_store);

        reloaded_catalog.create_collection("bulk_initialization", 384, "l2", "bulk-model");
        qcovert::InitializationManager initialization_manager(
            reloaded_catalog, data_store, generation_manager);
        qcovert::InitializationSession initialization = initialization_manager.create(
            "bulk_initialization", 3, "dataset-fingerprint", "load-once");
        if (initialization.state != qcovert::InitializationState::Uploading ||
            initialization.received_count != 0 || initialization.next_batch_index != 0) {
            throw std::runtime_error("new initialization session is invalid");
        }
        qcovert::VectorRecord bulk_a = first;
        bulk_a.id = "bulk-a";
        qcovert::VectorRecord bulk_b = second;
        bulk_b.id = "bulk-b";
        qcovert::VectorRecord bulk_c = second;
        bulk_c.id = "bulk-c";
        const std::string checksum_a(64, 'a');
        const std::string checksum_b(64, 'b');
        qcovert::InitializationBatchResult first_batch = initialization_manager.append_batch(
            "bulk_initialization", initialization.initialization_id, 0,
            checksum_a, {bulk_a, bulk_b});
        if (first_batch.idempotent_replay || first_batch.session.received_count != 2 ||
            first_batch.session.next_batch_index != 1 ||
            data_store.log_status("bulk_initialization").last_sequence != 2) {
            throw std::runtime_error("initialization batch was not committed correctly");
        }
        const qcovert::InitializationBatchResult replay = initialization_manager.append_batch(
            "bulk_initialization", initialization.initialization_id, 0,
            checksum_a, {bulk_a, bulk_b});
        if (!replay.idempotent_replay || replay.session.received_count != 2) {
            throw std::runtime_error("initialization batch replay was not idempotent");
        }
        bool duplicate_id_rejected = false;
        try {
            initialization_manager.append_batch(
                "bulk_initialization", initialization.initialization_id, 1,
                checksum_b, {bulk_a});
        } catch (const std::logic_error&) {
            duplicate_id_rejected = true;
        }
        if (!duplicate_id_rejected || data_store.vector_count("bulk_initialization") != 2) {
            throw std::runtime_error("duplicate initialization ID changed the WAL");
        }
        const qcovert::InitializationBatchResult final_batch = initialization_manager.append_batch(
            "bulk_initialization", initialization.initialization_id, 1,
            checksum_b, {bulk_c});
        if (final_batch.session.received_count != 3 ||
            final_batch.session.last_log_sequence != 3) {
            throw std::runtime_error("final initialization batch counters are invalid");
        }
        qcovert::InitializationManager recovered_initialization_manager(
            reloaded_catalog, data_store, generation_manager);
        const qcovert::InitializationSession recovered_initialization =
            recovered_initialization_manager.get(
                "bulk_initialization", initialization.initialization_id);
        if (recovered_initialization.received_count != 3 ||
            recovered_initialization.next_batch_index != 2) {
            throw std::runtime_error("initialization session did not recover from disk");
        }
        bool online_operation_rejected = false;
        try {
            recovered_initialization_manager.require_no_active_initialization(
                "bulk_initialization", "test operation");
        } catch (const qcovert::InitializationConflict&) {
            online_operation_rejected = true;
        }
        if (!online_operation_rejected) {
            throw std::runtime_error("active initialization did not gate normal operations");
        }

        reloaded_catalog.create_collection("abort_initialization", 384, "l2", "bulk-model");
        const qcovert::InitializationSession abort_session = initialization_manager.create(
            "abort_initialization", 1, "abort-fingerprint", "abort-once");
        initialization_manager.append_batch(
            "abort_initialization", abort_session.initialization_id, 0,
            std::string(64, 'c'), {bulk_c});
        const qcovert::InitializationSession aborted = initialization_manager.abort(
            "abort_initialization", abort_session.initialization_id);
        if (aborted.state != qcovert::InitializationState::Aborted ||
            data_store.vector_count("abort_initialization") != 0) {
            throw std::runtime_error("initialization abort did not reset the empty collection");
        }
        const qcovert::InitializationSession restarted = initialization_manager.create(
            "abort_initialization", 1, "new-fingerprint", "new-load");
        if (restarted.initialization_id == abort_session.initialization_id) {
            throw std::runtime_error("aborted initialization identifier was reused");
        }

        const qcovert::GenerationJob submitted = generation_manager.start_generation("anythingllm_workspace_1");
        qcovert::GenerationJob completed;
        bool finished = false;
        for (int attempt = 0; attempt < 2000; ++attempt) {
            completed = generation_manager.get_job("anythingllm_workspace_1", submitted.job_id);
            if (completed.state == qcovert::GenerationJobState::Succeeded ||
                completed.state == qcovert::GenerationJobState::Failed) {
                finished = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!finished || completed.state != qcovert::GenerationJobState::Succeeded ||
            completed.vector_count != 2) {
            throw std::runtime_error("generation snapshot job did not complete successfully: " + completed.error);
        }
        const qcovert::CollectionManifest prepared =
            reloaded_catalog.get_collection("anythingllm_workspace_1");
        if (prepared.state != qcovert::CollectionState::Prepared ||
            !prepared.active_generation.empty() ||
            prepared.pending_generation != submitted.generation ||
            prepared.pending_generation_stage != qcovert::GenerationStage::IvfReady ||
            prepared.vector_count != 2 ||
            !fs::is_regular_file(root / "anythingllm_workspace_1" / "generations" /
                                 submitted.generation / "vectors.qvec")) {
            throw std::runtime_error("IVF-ready generation was activated before ORAM validation");
        }
        bool early_activation_rejected = false;
        try {
            reloaded_catalog.activate_generation(
                "anythingllm_workspace_1", submitted.generation, 2);
        } catch (const std::logic_error&) {
            early_activation_rejected = true;
        }
        if (!early_activation_rejected) {
            throw std::runtime_error("catalog activated a generation before ORAM validation");
        }

        using TestIndex = IvfRabitQIndex<384, 384, 4>;
        const fs::path generation_directory = root / "anythingllm_workspace_1" /
                                              "generations" / submitted.generation;
        TestIndex index;
        index.load((generation_directory / "ivf_rabitq.index").string());
        Matrix<float> projection((generation_directory / "projection.fvecs").string());
        if (index.n != 2 || index.num_clusters != 1 || projection.n != 384 || projection.d != 384) {
            throw std::runtime_error("persisted IVF-RaBitQ artifacts have unexpected dimensions");
        }
        std::vector<float> query = first.values;
        std::vector<float> randomized_query(384, 0.0f);
        for (size_t output_dim = 0; output_dim < 384; ++output_dim) {
            for (size_t input_dim = 0; input_dim < 384; ++input_dim) {
                randomized_query[output_dim] +=
                    query[input_dim] * projection.row(input_dim)[output_dim];
            }
        }
        LocalPlainBlockReader<384, 384, 4> reader(index);
        PrivateIvfRabitQSearcher<384, 384, 4> searcher(index);
        ResultHeap results = searcher.search_local_plain(
            query.data(), randomized_query.data(), 1, 1, reader);
        if (results.empty() || results.top().second != 0) {
            throw std::runtime_error("persisted IVF-RaBitQ index returned the wrong nearest vector");
        }
        reloaded_catalog.advance_generation(
            "anythingllm_workspace_1", submitted.generation, qcovert::GenerationStage::OramReady);
        reloaded_catalog.advance_generation(
            "anythingllm_workspace_1", submitted.generation, qcovert::GenerationStage::Validated);
        const qcovert::CollectionManifest active = reloaded_catalog.activate_generation(
            "anythingllm_workspace_1", submitted.generation, 2);
        if (active.state != qcovert::CollectionState::Active ||
            active.active_generation != submitted.generation ||
            !active.pending_generation.empty()) {
            throw std::runtime_error("validated generation did not activate atomically");
        }
    } catch (const std::exception& error) {
        std::cerr << "test_collection_catalog failed: " << error.what() << "\n";
        return 1;
    }

    fs::remove_all(root, error);
    if (error) {
        std::cerr << "cannot remove test directory: " << error.message() << "\n";
        return 1;
    }
    std::cout << "test_collection_catalog passed\n";
    return 0;
}
