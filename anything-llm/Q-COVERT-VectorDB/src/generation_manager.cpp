#include "qcovert/generation_manager.h"
#include "qcovert/ivf_generation_builder.h"

#include "simple_json.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifndef QCOVERT_SOURCE_ROOT
#define QCOVERT_SOURCE_ROOT "."
#endif

namespace fs = std::filesystem;

namespace qcovert {

namespace {

uint64_t now_unix_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20) {
                    throw std::invalid_argument("JSON string contains a control character");
                }
                escaped.push_back(static_cast<char>(ch));
        }
    }
    return escaped;
}

void write_text_atomically(const fs::path& path, const std::string& text) {
    const fs::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create file: " + temporary.string());
    }
    output << text;
    output.close();
    if (!output) {
        throw std::runtime_error("cannot write file: " + temporary.string());
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) {
        throw std::runtime_error("cannot atomically replace file: " + path.string() + ": " + error.message());
    }
}

GenerationJobState parse_job_state(const std::string& value) {
    if (value == "queued") return GenerationJobState::Queued;
    if (value == "building") return GenerationJobState::Building;
    if (value == "succeeded") return GenerationJobState::Succeeded;
    if (value == "failed") return GenerationJobState::Failed;
    throw std::runtime_error("invalid generation job state");
}

uint64_t required_uint64(const simple_json::Value& object, const char* key) {
    const simple_json::Value* value = object.get(key);
    if (value == nullptr || !value->is_number() || value->number_value < 0.0 ||
        std::floor(value->number_value) != value->number_value) {
        throw std::runtime_error(std::string("invalid generation job field: ") + key);
    }
    return static_cast<uint64_t>(value->number_value);
}

std::string required_string(const simple_json::Value& object, const char* key) {
    const simple_json::Value* value = object.get(key);
    if (value == nullptr || !value->is_string()) {
        throw std::runtime_error(std::string("invalid generation job field: ") + key);
    }
    return value->string_value;
}

} // namespace

GenerationManager::~GenerationManager() {
    shutdown();
}

void GenerationManager::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    while (true) {
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            workers.swap(workers_);
        }
        if (workers.empty()) break;
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    }
}

GenerationManager::GenerationManager(CollectionCatalog& catalog,
                                     CollectionDataStore& data_store,
                                     GenerationBuildConfig build_config)
    : catalog_(&catalog), data_store_(&data_store), build_config_(std::move(build_config)),
      key_store_(catalog.root()) {
    if (build_config_.python_executable.empty()) {
        build_config_.python_executable = std::string(QCOVERT_SOURCE_ROOT) + "/.venv/bin/python";
    }
    if (build_config_.faiss_worker.empty()) {
        build_config_.faiss_worker =
            std::string(QCOVERT_SOURCE_ROOT) + "/core/ivf-rabitq/qcovert_faiss_cluster.py";
    }
    if (build_config_.kmeans_iterations == 0 || build_config_.kmeans_restarts == 0) {
        throw std::invalid_argument("Faiss k-means iterations and restarts must be positive");
    }
    // A detached worker cannot survive a process restart. Preserve an already
    // active generation, but release collections from the stale Building state.
    for (const CollectionManifest& manifest : catalog_->list_collections()) {
        if (!manifest.pending_generation.empty()) {
            catalog_->fail_generation(manifest.name);
        }
    }
}

GenerationJob GenerationManager::start_generation(
        const std::string& collection, bool allow_followup_generation) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    std::string generation;
    catalog_->begin_generation(valid_collection, generation);

    GenerationJob job;
    job.collection = valid_collection;
    job.generation = generation;
    job.state = GenerationJobState::Queued;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job.job_id = "job_" + generation;
    }
    try {
        const fs::path jobs_directory = catalog_->collection_directory(valid_collection) / "jobs";
        std::error_code error;
        fs::create_directories(jobs_directory, error);
        if (error) {
            throw std::runtime_error("cannot create collection jobs directory: " + error.message());
        }
        write_job(job);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                throw std::logic_error("generation manager is shutting down");
            }
            workers_.emplace_back(
                &GenerationManager::run_generation, this, job, allow_followup_generation);
        }
    } catch (...) {
        catalog_->fail_generation(valid_collection);
        throw;
    }
    return job;
}

std::optional<GenerationJob> GenerationManager::maybe_start_generation(
        const std::string& collection,
        uint64_t previous_vector_count,
        uint64_t current_vector_count,
        bool active_capacity_exhausted) {
    if (!requires_automatic_generation(
            previous_vector_count, current_vector_count, active_capacity_exhausted)) {
        return std::nullopt;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return std::nullopt;
    }

    const CollectionManifest manifest = catalog_->get_collection(collection);
    if (!manifest.pending_generation.empty()) return std::nullopt;
    try {
        return start_generation(collection);
    } catch (const std::logic_error&) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return std::nullopt;
        }
        // Another HTTP worker may have won the begin_generation race.
        if (!catalog_->get_collection(collection).pending_generation.empty()) {
            return std::nullopt;
        }
        throw;
    }
}

bool GenerationManager::requires_automatic_generation(
        uint64_t previous_vector_count,
        uint64_t current_vector_count,
        bool active_capacity_exhausted) {
    constexpr uint64_t interval = 10000;
    if (current_vector_count < previous_vector_count) {
        throw std::invalid_argument("current vector count cannot be less than previous count");
    }
    return active_capacity_exhausted ||
           current_vector_count / interval > previous_vector_count / interval;
}

GenerationJob GenerationManager::get_job(const std::string& collection, const std::string& job_id) const {
    const GenerationJob job = read_job(CollectionCatalog::validate_collection_name(collection), job_id);
    if (job.state == GenerationJobState::Queued || job.state == GenerationJobState::Building) {
        const CollectionManifest manifest = catalog_->get_collection(job.collection);
        if (manifest.pending_generation != job.generation &&
            manifest.active_generation != job.generation) {
            GenerationJob recovered = job;
            recovered.state = GenerationJobState::Failed;
            recovered.error = "query service restarted before this generation job completed";
            return recovered;
        }
    }
    return job;
}

const char* GenerationManager::job_state_name(GenerationJobState state) {
    switch (state) {
        case GenerationJobState::Queued: return "queued";
        case GenerationJobState::Building: return "building";
        case GenerationJobState::Succeeded: return "succeeded";
        case GenerationJobState::Failed: return "failed";
    }
    throw std::logic_error("unknown generation job state");
}

void GenerationManager::run_generation(
        GenerationJob job, bool allow_followup_generation) {
    try {
        bool followup_generation_required = false;
        job.state = GenerationJobState::Building;
        write_job(job);

        const fs::path generations = catalog_->collection_directory(job.collection) / "generations";
        const fs::path staging_directory = generations / (job.generation + ".building");
        const fs::path final_directory = generations / job.generation;
        if (fs::exists(staging_directory) || fs::exists(final_directory)) {
            throw std::runtime_error("generation directory already exists: " + job.generation);
        }
        std::error_code error;
        fs::create_directories(staging_directory, error);
        if (error) {
            throw std::runtime_error("cannot create generation staging directory: " + error.message());
        }

        const AeadKey generation_key = key_store_.create(job.collection, job.generation);
        const VectorSnapshot snapshot = data_store_->write_latest_snapshot(
            job.collection, staging_directory / "vectors.qvec");
        const IvfGenerationArtifacts ivf_artifacts = IvfGenerationBuilder384::build(
                staging_directory / "vectors.qvec",
                staging_directory,
                build_config_.python_executable,
                build_config_.faiss_worker,
                build_config_.kmeans_iterations,
                build_config_.kmeans_restarts,
                build_config_.seed,
                build_config_.faiss_threads);
        OramGenerationArtifacts oram_artifacts;
        if (build_config_.oram.enabled) {
            oram_artifacts = OramGenerationBuilder384::build(
                job.collection,
                job.generation,
                staging_directory,
                generation_key,
                build_config_.oram);
        }
        std::ostringstream generation_manifest;
        generation_manifest << "{\n"
                                << "  \"schema_version\": 1,\n"
                                << "  \"generation\": \"" << json_escape(job.generation) << "\",\n"
                                << "  \"vector_count\": " << snapshot.vector_count << ",\n"
                                << "  \"last_log_sequence\": " << snapshot.last_sequence << ",\n"
                                << "  \"cluster_count\": " << ivf_artifacts.cluster_count << ",\n"
                                << "  \"approximate_block_count\": "
                                << ivf_artifacts.approximate_block_count << ",\n"
                                << "  \"clustering\": \"faiss_nearest\",\n"
                                << "  \"kmeans_niter\": " << build_config_.kmeans_iterations << ",\n"
                                << "  \"kmeans_nredo\": " << build_config_.kmeans_restarts << ",\n"
                                << "  \"faiss_threads\": " << build_config_.faiss_threads << ",\n"
                                << "  \"ivf_state\": \"ready\",\n"
                                << "  \"oram_state\": \""
                                << (build_config_.oram.enabled ? "ready" : "pending") << "\",\n";
        if (build_config_.oram.enabled) {
            generation_manifest << "  \"exact_payload\": \"raw_float32\",\n"
                                << "  \"exact_payload_bytes\": "
                                << oram_artifacts.exact_payload_bytes << ",\n"
                                << "  \"cluster_oram_levels\": "
                                << oram_artifacts.cluster.num_levels << ",\n"
                                << "  \"cluster_oram_real_blocks\": "
                                << oram_artifacts.cluster.real_blocks << ",\n"
                                << "  \"cluster_oram_logical_block_limit\": "
                                << oram_artifacts.cluster.logical_block_limit << ",\n"
                                << "  \"ring_oram_levels\": "
                                << oram_artifacts.ring.num_levels << ",\n"
                                << "  \"ring_oram_real_blocks\": "
                                << oram_artifacts.ring.real_blocks << ",\n"
                                << "  \"ring_oram_logical_block_limit\": "
                                << oram_artifacts.ring.logical_block_limit << ",\n"
                                << "  \"generation_vector_capacity\": "
                                << oram_artifacts.ring.logical_block_limit << ",\n";
        }
        generation_manifest
                            << "  \"created_at_unix_ms\": " << now_unix_ms() << "\n"
                            << "}\n";
        write_text_atomically(staging_directory / "generation.json", generation_manifest.str());

        std::error_code rename_error;
        fs::rename(staging_directory, final_directory, rename_error);
        if (rename_error) {
            throw std::runtime_error("cannot publish prepared generation directory: " + rename_error.message());
        }
        catalog_->advance_generation(job.collection, job.generation, GenerationStage::IvfReady);
        if (build_config_.oram.enabled) {
            catalog_->advance_generation(job.collection, job.generation, GenerationStage::OramReady);
            const OramGenerationFinalizeResult finalized =
                OramGenerationFinalizer384::finalize(
                    *catalog_,
                    *data_store_,
                    job.collection,
                    job.generation,
                    final_directory,
                    generation_key,
                    snapshot.last_sequence,
                    oram_artifacts,
                    build_config_.oram);
            job.vector_count = finalized.vector_count;
            followup_generation_required =
                finalized.artifacts.ring.real_blocks >=
                finalized.artifacts.ring.logical_block_limit;
        } else {
            job.vector_count = snapshot.vector_count;
        }
        job.state = GenerationJobState::Succeeded;
        write_job(job);
        if (allow_followup_generation && followup_generation_required) {
            maybe_start_generation(
                job.collection, job.vector_count, job.vector_count, true);
        }
    } catch (const std::exception& error) {
        try {
            catalog_->fail_generation(job.collection);
        } catch (...) {
            // Preserve the original error and leave recovery to the next restart.
        }
        job.state = GenerationJobState::Failed;
        job.error = error.what();
        try {
            write_job(job);
        } catch (...) {
            // A client can still diagnose the collection state through the catalog.
        }
    }
}

void GenerationManager::write_job(const GenerationJob& job) const {
    CollectionDataStore::validate_vector_id(job.job_id);
    const fs::path path = catalog_->collection_directory(job.collection) / "jobs" / (job.job_id + ".json");
    std::ostringstream output;
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"job_id\": \"" << json_escape(job.job_id) << "\",\n"
           << "  \"collection\": \"" << json_escape(job.collection) << "\",\n"
           << "  \"generation\": \"" << json_escape(job.generation) << "\",\n"
           << "  \"state\": \"" << job_state_name(job.state) << "\",\n"
           << "  \"vector_count\": " << job.vector_count << ",\n"
           << "  \"error\": \"" << json_escape(job.error) << "\"\n"
           << "}\n";
    write_text_atomically(path, output.str());
}

GenerationJob GenerationManager::read_job(const std::string& collection, const std::string& job_id) const {
    CollectionDataStore::validate_vector_id(job_id);
    const fs::path path = catalog_->collection_directory(collection) / "jobs" / (job_id + ".json");
    if (!fs::is_regular_file(path)) {
        throw std::out_of_range("generation job does not exist: " + job_id);
    }
    const simple_json::Value root = simple_json::parse_file(path.string());
    if (!root.is_object()) {
        throw std::runtime_error("generation job must be a JSON object");
    }
    GenerationJob job;
    job.job_id = required_string(root, "job_id");
    job.collection = CollectionCatalog::validate_collection_name(required_string(root, "collection"));
    job.generation = required_string(root, "generation");
    job.state = parse_job_state(required_string(root, "state"));
    job.vector_count = required_uint64(root, "vector_count");
    job.error = required_string(root, "error");
    if (job.collection != collection || job.job_id != job_id) {
        throw std::runtime_error("generation job identity does not match its path");
    }
    return job;
}

} // namespace qcovert
