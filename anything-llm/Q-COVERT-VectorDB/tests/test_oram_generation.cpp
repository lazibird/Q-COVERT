#include "qcovert/oram_generation_builder.h"
#include "qcovert/collection_catalog.h"
#include "qcovert/collection_data_store.h"
#include "qcovert/generation_manager.h"
#include "qcovert/initialization_manager.h"
#include "qcovert/search_service.h"
#include "online_vector_codec.h"

#include "ClusterORAM.h"
#include "RandomForOram.h"
#include "RingORAM.h"
#include "ivf_rabitq_index.h"
#include "matrix.h"
#include "simple_json.h"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <openssl/evp.h>

#ifndef QCOVERT_STORAGE_EXECUTABLE
#define QCOVERT_STORAGE_EXECUTABLE "qcovert_storage"
#endif

#ifndef QCOVERT_QUERY_EXECUTABLE
#define QCOVERT_QUERY_EXECUTABLE "qcovert_query"
#endif

namespace fs = std::filesystem;

namespace {

class ChildProcess {
public:
    explicit ChildProcess(pid_t pid) : pid_(pid) {}
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ~ChildProcess() {
        if (pid_ <= 0) return;
        kill(pid_, SIGTERM);
        for (int attempt = 0; attempt < 500; ++attempt) {
            if (waitpid(pid_, nullptr, WNOHANG) == pid_) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
    }

    void require_running() const {
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) throw std::runtime_error("child process exited before the test");
        if (result < 0) throw std::runtime_error("cannot inspect child process");
    }

    void stop_gracefully() {
        if (pid_ <= 0) return;
        if (kill(pid_, SIGTERM) != 0) {
            throw std::runtime_error("cannot send SIGTERM to child process");
        }
        for (int attempt = 0; attempt < 1500; ++attempt) {
            int status = 0;
            const pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    throw std::runtime_error("child process did not exit cleanly after SIGTERM");
                }
                return;
            }
            if (result < 0) throw std::runtime_error("cannot wait for child process shutdown");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
        pid_ = -1;
        throw std::runtime_error("child process graceful shutdown timed out");
    }

private:
    pid_t pid_;
};

int reserve_test_port() {
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) throw std::runtime_error("cannot create test port socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(socket_fd);
        throw std::runtime_error("cannot reserve a test port");
    }
    socklen_t size = sizeof(address);
    if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        close(socket_fd);
        throw std::runtime_error("cannot read reserved test port");
    }
    const int port = ntohs(address.sin_port);
    close(socket_fd);
    return port;
}

pid_t start_storage(int port, const fs::path& root) {
    const pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("cannot fork qcovert_storage");
    if (pid == 0) {
        const int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const std::string port_text = std::to_string(port);
        execl(QCOVERT_STORAGE_EXECUTABLE,
              QCOVERT_STORAGE_EXECUTABLE,
              "--port", port_text.c_str(),
              "--root", root.c_str(),
              "--worker-threads", "4",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    return pid;
}

pid_t start_query(int port, int storage_port, const fs::path& root) {
    const pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("cannot fork qcovert_query");
    if (pid == 0) {
        const int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const std::string port_text = std::to_string(port);
        const std::string storage_port_text = std::to_string(storage_port);
        execl(QCOVERT_QUERY_EXECUTABLE,
              QCOVERT_QUERY_EXECUTABLE,
              "--port", port_text.c_str(),
              "--root", root.c_str(),
              "--storage-host", "127.0.0.1",
              "--storage-port", storage_port_text.c_str(),
              "--worker-threads", "2",
              "--allow-unauthenticated",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    return pid;
}

std::string http_request(
        int port,
        const std::string& method,
        const std::string& target,
        const std::string& body = "",
        const std::string& extra_headers = "") {
    int socket_fd = -1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) throw std::runtime_error("cannot create HTTP test socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            break;
        }
        close(socket_fd);
        socket_fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (socket_fd < 0) throw std::runtime_error("qcovert_query did not accept HTTP connections");

    const std::string request =
        method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n" +
        extra_headers + "Content-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = send(
            socket_fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) {
            close(socket_fd);
            throw std::runtime_error("cannot send HTTP search request");
        }
        sent += static_cast<size_t>(count);
    }
    std::string response;
    char buffer[4096];
    while (true) {
        const ssize_t count = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count == 0) break;
        if (count < 0) {
            close(socket_fd);
            throw std::runtime_error("cannot receive HTTP search response");
        }
        response.append(buffer, static_cast<size_t>(count));
    }
    close(socket_fd);
    return response;
}

std::string sha256_hex(const std::string& input) {
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int size = 0;
    if (EVP_Digest(input.data(), input.size(), digest, &size, EVP_sha256(), nullptr) != 1 ||
        size != 32) {
        throw std::runtime_error("cannot calculate test SHA-256");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(64);
    for (unsigned int i = 0; i < size; ++i) {
        output.push_back(hex[digest[i] >> 4U]);
        output.push_back(hex[digest[i] & 0x0fU]);
    }
    return output;
}

std::string http_post(int port, const std::string& target, const std::string& body) {
    return http_request(port, "POST", target, body);
}

std::string constant_search_body(float value) {
    std::ostringstream body;
    body << "{\"vector\":[";
    for (size_t i = 0; i < qcovert::kSupportedEmbeddingDimension; ++i) {
        if (i != 0) body << ',';
        body << value;
    }
    body << "],\"top_k\":1,\"candidate_factor\":4}";
    return body.str();
}

std::string constant_upsert_body(
        const std::string& id,
        float value,
        const std::string& source) {
    std::ostringstream body;
    body << "{\"records\":[{\"id\":\"" << id << "\",\"vector\":[";
    for (size_t i = 0; i < qcovert::kSupportedEmbeddingDimension; ++i) {
        if (i != 0) body << ',';
        body << value;
    }
    body << "],\"metadata\":{\"source\":\"" << source << "\"}}]}";
    return body.str();
}

void write_test_index(const fs::path& path) {
    constexpr uint32_t count = 64;
    constexpr uint32_t clusters = 4;
    Matrix<float> vectors(count, 384);
    Matrix<float> randomized_centroids(clusters, 384);
    Matrix<float> distances(count, 1);
    Matrix<float> x0(count, 1);
    Matrix<uint32_t> assignments(count, 1);
    Matrix<uint64_t> binary(count, 384 / 64);
    for (uint32_t cluster = 0; cluster < clusters; ++cluster) {
        for (uint32_t d = 0; d < 384; ++d) {
            randomized_centroids.row(cluster)[d] =
                static_cast<float>(cluster) + static_cast<float>(d) * 0.001f;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        assignments.values[i] = i % clusters;
        distances.values[i] = static_cast<float>(i % 11) * 0.1f;
        x0.values[i] = 0.8f;
        for (uint32_t d = 0; d < 384; ++d) {
            vectors.row(i)[d] = static_cast<float>(i) + static_cast<float>(d) * 0.01f;
        }
        for (uint32_t word = 0; word < 384 / 64; ++word) {
            binary.row(i)[word] = (static_cast<uint64_t>(i) << 32U) | word;
        }
    }
    IvfRabitQIndex<384, 384, 4> index(
        vectors, randomized_centroids, distances, x0, assignments, binary);
    index.save(path.string());
}

void require_private_file(const fs::path& path) {
    struct stat info{};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        (info.st_mode & 0777) != 0600) {
        throw std::runtime_error("expected a private regular file: " + path.string());
    }
}

double_oram::RingOramConfig make_test_oram_config(
        const qcovert::OramTreeLayout& layout,
        const qcovert::OramGenerationConfig& config) {
    return double_oram::RingOramConfig(
        layout.block_size,
        config.real_bucket_size,
        config.dummy_size,
        config.evict_rate,
        static_cast<int>(layout.logical_block_limit),
        layout.num_levels,
        layout.cached_levels);
}

void test_client_state_stash_round_trip(
        const fs::path& generation_directory,
        const qcovert::OramGenerationArtifacts& initial_artifacts,
        const qcovert::OramGenerationConfig& config) {
    double_oram::RandomForOram ring_random;
    const double_oram::RingOramConfig ring_config =
        make_test_oram_config(initial_artifacts.ring, config);
    double_oram::RingORAM ring(
        nullptr, &ring_random, ring_config, initial_artifacts.ring.num_levels,
        initial_artifacts.ring.cached_levels, true, true);
    qcovert::OramClientStateStore::load_ring(
        generation_directory / "ring-client-state.bin",
        ring,
        initial_artifacts.ring,
        config);
    if (!ring.mmstash.empty()) {
        throw std::runtime_error("new RingORAM generation unexpectedly starts with stash blocks");
    }

    std::vector<int> exact_payload(static_cast<size_t>(ring.block_size), 0x31415926);
    const int appended_vector_id = static_cast<int>(initial_artifacts.ring.real_blocks);
    const int appended_ring_leaf = ring.insert_block_to_stash(
        appended_vector_id, exact_payload, 0);
    qcovert::OramGenerationArtifacts updated_artifacts = initial_artifacts;
    updated_artifacts.ring.real_blocks++;
    const fs::path ring_state = generation_directory / "ring-client-state-with-stash.bin";
    qcovert::OramClientStateStore::save_ring(
        ring_state, ring, updated_artifacts.ring, config);

    double_oram::RandomForOram restored_ring_random;
    double_oram::RingORAM restored_ring(
        nullptr, &restored_ring_random, ring_config, updated_artifacts.ring.num_levels,
        updated_artifacts.ring.cached_levels, true, true);
    qcovert::OramClientStateStore::load_ring(
        ring_state, restored_ring, updated_artifacts.ring, config);
    const auto restored_ring_block = restored_ring.mmstash.find(
        {appended_ring_leaf, appended_vector_id});
    if (restored_ring.mmstash.size() != 1 || restored_ring_block == restored_ring.mmstash.end() ||
        restored_ring_block->second == nullptr ||
        restored_ring_block->second->data != exact_payload) {
        throw std::runtime_error("RingORAM stash did not survive client-state round trip");
    }

    const std::vector<int> initial_cluster_counts =
        qcovert::OramClientStateStore::read_cluster_block_counts(
            generation_directory / "cluster-client-state.bin",
            initial_artifacts.cluster,
            config);
    double_oram::RandomForOram cluster_random;
    const double_oram::RingOramConfig cluster_config =
        make_test_oram_config(initial_artifacts.cluster, config);
    double_oram::ClusterORAM cluster(
        nullptr, &cluster_random, cluster_config, initial_artifacts.cluster.num_levels,
        initial_cluster_counts);
    qcovert::OramClientStateStore::load_cluster(
        generation_directory / "cluster-client-state.bin",
        cluster,
        initial_artifacts.cluster,
        config);
    if (!cluster.cluster_stash.empty()) {
        throw std::runtime_error("new ClusterORAM generation unexpectedly starts with stash blocks");
    }

    std::vector<int> approximate_payload(
        static_cast<size_t>(cluster.block_size), 0x27182818);
    const int appended_cluster_offset =
        cluster.append_cluster_block_to_stash(0, approximate_payload);
    updated_artifacts.cluster.real_blocks++;
    const fs::path cluster_state = generation_directory / "cluster-client-state-with-stash.bin";
    qcovert::OramClientStateStore::save_cluster(
        cluster_state, cluster, updated_artifacts.cluster, config);

    const std::vector<int> updated_cluster_counts =
        qcovert::OramClientStateStore::read_cluster_block_counts(
            cluster_state, updated_artifacts.cluster, config);
    double_oram::RandomForOram restored_cluster_random;
    double_oram::ClusterORAM restored_cluster(
        nullptr, &restored_cluster_random, cluster_config,
        updated_artifacts.cluster.num_levels, updated_cluster_counts);
    qcovert::OramClientStateStore::load_cluster(
        cluster_state, restored_cluster, updated_artifacts.cluster, config);
    const int restored_stash_index =
        restored_cluster.cluster_stash_index(0, appended_cluster_offset);
    if (restored_stash_index < 0 || restored_cluster.cluster_stash.size() != 1 ||
        restored_cluster.get_cluster_stash_block(0, appended_cluster_offset).block.data !=
            approximate_payload) {
        throw std::runtime_error("ClusterORAM stash did not survive client-state round trip");
    }

    for (auto& item : ring.mmstash) delete item.second;
    ring.mmstash.clear();
    for (auto& item : restored_ring.mmstash) delete item.second;
    restored_ring.mmstash.clear();
}

void test_generation_manager_stage(const fs::path& root, int port) {
    const fs::path trusted = root / "manager-trusted";
    qcovert::CollectionCatalog catalog(trusted);
    catalog.create_collection("manager_collection", 384, "l2", "test-model");
    qcovert::CollectionDataStore data_store(catalog);
    qcovert::VectorRecord record;
    record.id = "chunk-1";
    record.values.resize(384);
    for (size_t i = 0; i < record.values.size(); ++i) {
        record.values[i] = static_cast<float>(i) * 0.01f;
    }
    record.metadata_json = "{\"source\":\"manager-test\"}";
    data_store.upsert("manager_collection", {record});

    qcovert::GenerationBuildConfig build_config;
    build_config.kmeans_iterations = 1;
    build_config.kmeans_restarts = 1;
    build_config.oram.enabled = true;
    build_config.oram.storage_port = port;
    build_config.oram.real_bucket_size = 32;
    build_config.oram.dummy_size = 16;
    build_config.oram.evict_rate = 48;
    build_config.oram.upload_batch_bytes = 512U * 1024U;
    qcovert::GenerationManager manager(catalog, data_store, build_config);

    catalog.create_collection("bulk_collection", 384, "l2", "test-model");
    qcovert::InitializationManager initialization_manager(catalog, data_store, manager);
    const qcovert::InitializationSession bulk_session = initialization_manager.create(
        "bulk_collection", 2, "bulk-fingerprint", "bulk-load-once");
    qcovert::VectorRecord bulk_first = record;
    bulk_first.id = "bulk-1";
    qcovert::VectorRecord bulk_second = record;
    bulk_second.id = "bulk-2";
    bulk_second.values.assign(384, 2.0f);
    initialization_manager.append_batch(
        "bulk_collection", bulk_session.initialization_id, 0,
        std::string(64, 'd'), {bulk_first, bulk_second});
    qcovert::InitializationSession bulk_build = initialization_manager.finalize(
        "bulk_collection", bulk_session.initialization_id);
    for (int attempt = 0; attempt < 3000 &&
         bulk_build.state == qcovert::InitializationState::Building; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        bulk_build = initialization_manager.get(
            "bulk_collection", bulk_session.initialization_id);
    }
    if (bulk_build.state != qcovert::InitializationState::Succeeded) {
        throw std::runtime_error(
            "bulk initialization generation failed: " + bulk_build.error);
    }
    const qcovert::CollectionManifest bulk_manifest = catalog.get_collection("bulk_collection");
    if (bulk_manifest.state != qcovert::CollectionState::Active ||
        bulk_manifest.vector_count != 2 || bulk_manifest.active_generation.empty() ||
        !bulk_manifest.pending_generation.empty()) {
        throw std::runtime_error("bulk initialization did not atomically activate its generation");
    }
    const simple_json::Value bulk_replay = simple_json::parse_file(
        (trusted / "bulk_collection" / "generations" / bulk_manifest.active_generation /
         "replay-state.json").string());
    if (bulk_replay.get("replayed_records") == nullptr ||
        bulk_replay.get("replayed_records")->number_value != 0.0 ||
        bulk_replay.get("vector_count") == nullptr ||
        bulk_replay.get("vector_count")->number_value != 2.0) {
        throw std::runtime_error("bulk initialization unexpectedly replayed online WAL deltas");
    }

    const qcovert::GenerationJob submitted = manager.start_generation("manager_collection");
    const fs::path building_directory = trusted / "manager_collection" / "generations" /
                                        (submitted.generation + ".building");
    bool snapshot_ready = false;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (fs::is_regular_file(building_directory / "vectors.qvec")) {
            snapshot_ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!snapshot_ready) throw std::runtime_error("generation snapshot was not published in time");

    record.values.assign(384, 9.0f);
    record.metadata_json = "{\"source\":\"post-snapshot-update\"}";
    qcovert::VectorRecord delta_record;
    delta_record.id = "chunk-after-snapshot";
    delta_record.values.assign(384, 7.0f);
    delta_record.metadata_json = "{\"source\":\"post-snapshot\"}";
    data_store.upsert("manager_collection", {record, delta_record});

    qcovert::GenerationJob completed;
    for (int attempt = 0; attempt < 3000; ++attempt) {
        completed = manager.get_job("manager_collection", submitted.job_id);
        if (completed.state == qcovert::GenerationJobState::Succeeded ||
            completed.state == qcovert::GenerationJobState::Failed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (completed.state != qcovert::GenerationJobState::Succeeded) {
        throw std::runtime_error("ORAM-enabled generation manager failed: " + completed.error);
    }
    const qcovert::CollectionManifest manifest = catalog.get_collection("manager_collection");
    if (manifest.active_generation != submitted.generation ||
        !manifest.pending_generation.empty() ||
        manifest.pending_generation_stage != qcovert::GenerationStage::None ||
        manifest.state != qcovert::CollectionState::Active || manifest.vector_count != 2 ||
        completed.vector_count != 2) {
        throw std::runtime_error("generation manager did not replay and activate the ORAM generation");
    }
    const fs::path generation_directory = trusted / "manager_collection" / "generations" /
                                          submitted.generation;
    const simple_json::Value generation = simple_json::parse_file(
        (generation_directory / "generation.json").string());
    if (generation.get("ivf_state") == nullptr ||
        generation.get("ivf_state")->string_value != "ready" ||
        generation.get("oram_state") == nullptr ||
        generation.get("oram_state")->string_value != "ready" ||
        !fs::is_regular_file(generation_directory / "client-manifest.json") ||
        !fs::is_regular_file(generation_directory / "live-records.jsonl") ||
        !fs::is_regular_file(generation_directory / "replay-state.json")) {
        throw std::runtime_error("ORAM-enabled generation manifest is incomplete");
    }
    const simple_json::Value replay = simple_json::parse_file(
        (generation_directory / "replay-state.json").string());
    if (replay.get("state") == nullptr || replay.get("state")->string_value != "validated" ||
        replay.get("vector_count") == nullptr || replay.get("vector_count")->number_value != 2.0 ||
        replay.get("replayed_records") == nullptr ||
        replay.get("replayed_records")->number_value != 2.0 ||
        replay.get("ring_oram_real_blocks") == nullptr ||
        replay.get("ring_oram_real_blocks")->number_value != 3.0) {
        throw std::runtime_error("ORAM replay state does not include the concurrent insertion");
    }
    std::ifstream live_records(generation_directory / "live-records.jsonl");
    std::string live_line;
    std::set<int> live_internal_ids;
    while (std::getline(live_records, live_line)) {
        const simple_json::Value live = simple_json::parse_text(live_line);
        if (live.get("internal_id") == nullptr || !live.get("internal_id")->is_number()) {
            throw std::runtime_error("final live-record map contains an invalid internal ID");
        }
        live_internal_ids.insert(static_cast<int>(live.get("internal_id")->number_value));
    }
    if (live_internal_ids != std::set<int>({1, 2})) {
        throw std::runtime_error("final live-record map retained an obsolete internal vector ID");
    }

    qcovert::SearchRequest search_request;
    search_request.vector.assign(384, 7.0f);
    search_request.top_k = 1;
    search_request.candidate_factor = 4.0;
    {
        qcovert::SearchService search_service(catalog, data_store, "127.0.0.1", port);
        const qcovert::SearchResponse response =
            search_service.search("manager_collection", search_request);
        if (response.generation != submitted.generation || response.hits.size() != 1 ||
            response.hits[0].id != "chunk-after-snapshot" || response.hits[0].distance != 0.0f ||
            response.nprobe != 1 || response.exact_candidate_limit != 6) {
            throw std::runtime_error("active DoubleORAM search returned the wrong inserted vector");
        }
    }

    search_request.vector.assign(384, 9.0f);
    {
        qcovert::SearchService restarted_search_service(
            catalog, data_store, "127.0.0.1", port);
        const qcovert::SearchResponse response =
            restarted_search_service.search("manager_collection", search_request);
        if (response.hits.size() != 1 || response.hits[0].id != "chunk-1" ||
            response.hits[0].distance != 0.0f ||
            response.hits[0].metadata_json !=
                "{\"source\":\"post-snapshot-update\"}") {
            throw std::runtime_error("restarted DoubleORAM search returned an obsolete ID version");
        }
    }


    qcovert::VectorRecord online_record;
    online_record.id = "online-vector";
    online_record.values.assign(384, 5.0f);
    online_record.metadata_json = "{\"source\":\"online-insert\"}";
    data_store.upsert("manager_collection", {online_record});
    {
        qcovert::SearchService online_search_service(
            catalog, data_store, "127.0.0.1", port);
        const qcovert::ActiveGenerationSyncResult synchronized =
            online_search_service.synchronize_active("manager_collection");
        if (!synchronized.active_generation || synchronized.applied_records != 1) {
            throw std::runtime_error("online insert did not update the active ORAM generation");
        }
        search_request.vector.assign(384, 5.0f);
        const qcovert::SearchResponse inserted =
            online_search_service.search("manager_collection", search_request);
        if (inserted.hits.empty() || inserted.hits[0].id != "online-vector" ||
            inserted.hits[0].distance != 0.0f) {
            throw std::runtime_error("online inserted vector was not immediately searchable");
        }

        online_record.values.assign(384, 6.0f);
        online_record.metadata_json = "{\"source\":\"online-update\"}";
        data_store.upsert("manager_collection", {online_record});
        search_request.vector.assign(384, 6.0f);
        const qcovert::SearchResponse updated =
            online_search_service.search("manager_collection", search_request);
        if (updated.hits.empty() || updated.hits[0].id != "online-vector" ||
            updated.hits[0].distance != 0.0f ||
            updated.hits[0].metadata_json != "{\"source\":\"online-update\"}") {
            throw std::runtime_error("Search did not replay the latest online WAL update");
        }
    }
    {
        qcovert::SearchService restarted_online_search(
            catalog, data_store, "127.0.0.1", port);
        const qcovert::SearchResponse response =
            restarted_online_search.search("manager_collection", search_request);
        if (response.hits.empty() || response.hits[0].id != "online-vector" ||
            response.hits[0].distance != 0.0f) {
            throw std::runtime_error("online ORAM update did not survive a runtime restart");
        }
    }

    const int query_port = reserve_test_port();
    ChildProcess query(start_query(query_port, port, trusted));
    const std::string upsert_response = http_post(
        query_port,
        "/v1/collections/manager_collection/vectors",
        constant_upsert_body("http-online-vector", 4.0f, "http-online"));
    if (upsert_response.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 ||
        upsert_response.find("\"active_oram_applied\":1") == std::string::npos) {
        throw std::runtime_error("HTTP online upsert did not synchronize active ORAM: " +
                                 upsert_response);
    }
    const std::string http_response = http_post(
        query_port,
        "/v1/collections/manager_collection/search",
        constant_search_body(4.0f));
    query.require_running();
    if (http_response.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 ||
        http_response.find("\"id\":\"http-online-vector\"") == std::string::npos ||
        http_response.find("\"distance\":0") == std::string::npos ||
        http_response.find("\"search_ms\":") == std::string::npos ||
        http_response.find("\"metadata\":{\"source\":\"http-online\"}") ==
            std::string::npos) {
        throw std::runtime_error("HTTP Search API returned an invalid response: " + http_response);
    }

    query.stop_gracefully();
    ChildProcess restarted_query(start_query(query_port, port, trusted));
    const std::string restarted_response = http_post(
        query_port,
        "/v1/collections/manager_collection/search",
        constant_search_body(4.0f));
    if (restarted_response.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 ||
        restarted_response.find("\"id\":\"http-online-vector\"") == std::string::npos ||
        restarted_response.find("\"distance\":0") == std::string::npos) {
        throw std::runtime_error(
            "gracefully restarted query service did not recover ORAM state: " +
            restarted_response);
    }

    const std::string bulk_create = http_request(
        query_port, "PUT", "/v1/collections/http_bulk",
        "{\"dimension\":384,\"metric\":\"l2\",\"embedding_model\":\"test-model\"}");
    if (bulk_create.rfind("HTTP/1.1 201 Created\r\n", 0) != 0) {
        throw std::runtime_error("HTTP bulk collection creation failed: " + bulk_create);
    }
    const std::string initialization_create = http_post(
        query_port, "/v1/collections/http_bulk/initializations",
        "{\"expected_count\":1,\"fingerprint\":\"http-test\","
        "\"idempotency_key\":\"http-test-once\"}");
    if (initialization_create.rfind("HTTP/1.1 201 Created\r\n", 0) != 0 ||
        initialization_create.find("\"initialization_id\":\"initialization_1\"") ==
            std::string::npos ||
        initialization_create.find("\"state\":\"uploading\"") == std::string::npos) {
        throw std::runtime_error("HTTP initialization creation failed: " + initialization_create);
    }
    const std::string initialization_body =
        constant_upsert_body("http-bulk-vector", 8.0f, "http-bulk");
    const std::string initialization_checksum = sha256_hex(initialization_body);
    const std::string initialization_batch = http_request(
        query_port, "PUT",
        "/v1/collections/http_bulk/initializations/initialization_1/batches/0",
        initialization_body,
        "X-QCovert-Content-SHA256: " + initialization_checksum + "\r\n");
    if (initialization_batch.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 ||
        initialization_batch.find("\"received_count\":1") == std::string::npos ||
        initialization_batch.find("\"idempotent_replay\":false") == std::string::npos) {
        throw std::runtime_error("HTTP initialization batch failed: " + initialization_batch);
    }
    const std::string initialization_replay = http_request(
        query_port, "PUT",
        "/v1/collections/http_bulk/initializations/initialization_1/batches/0",
        initialization_body,
        "X-QCovert-Content-SHA256: " + initialization_checksum + "\r\n");
    if (initialization_replay.find("\"idempotent_replay\":true") == std::string::npos) {
        throw std::runtime_error("HTTP initialization replay was not idempotent: " +
                                 initialization_replay);
    }
    const std::string gated_upsert = http_post(
        query_port, "/v1/collections/http_bulk/vectors", initialization_body);
    if (gated_upsert.rfind("HTTP/1.1 409 Conflict\r\n", 0) != 0 ||
        gated_upsert.find("\"code\":\"initialization_conflict\"") == std::string::npos) {
        throw std::runtime_error("HTTP initialization did not gate online upsert: " + gated_upsert);
    }
    const std::string initialization_abort = http_request(
        query_port, "DELETE",
        "/v1/collections/http_bulk/initializations/initialization_1");
    if (initialization_abort.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 ||
        initialization_abort.find("\"state\":\"aborted\"") == std::string::npos ||
        initialization_abort.find("\"received_count\":0") == std::string::npos) {
        throw std::runtime_error("HTTP initialization abort failed: " + initialization_abort);
    }
    const std::string bulk_delete = http_request(
        query_port, "DELETE", "/v1/collections/http_bulk");
    if (bulk_delete.rfind("HTTP/1.1 200 OK\r\n", 0) != 0) {
        throw std::runtime_error("HTTP bulk test cleanup failed: " + bulk_delete);
    }

    const std::string global_stats = http_request(query_port, "GET", "/v1/stats");
    if (global_stats.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 ||
        global_stats.find("\"collection_count\":2") == std::string::npos ||
        global_stats.find("\"vector_count\":6") == std::string::npos) {
        throw std::runtime_error("global management stats are invalid: " + global_stats);
    }
    const std::string collection_stats = http_request(
        query_port, "GET", "/v1/collections/manager_collection/stats");
    if (collection_stats.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 ||
        collection_stats.find("\"active_generation_stats\":{") == std::string::npos ||
        collection_stats.find("\"cluster_oram\":{") == std::string::npos ||
        collection_stats.find("\"ring_oram\":{") == std::string::npos) {
        throw std::runtime_error("collection management stats are invalid: " + collection_stats);
    }

    const std::string backup_response = http_post(
        query_port, "/v1/collections/manager_collection/backups", "{\"name\":\"p9_backup\"}");
    if (backup_response.rfind("HTTP/1.1 201 Created\r\n", 0) != 0 ||
        !fs::is_regular_file(trusted / ".backups" / "manager_collection" /
                             "p9_backup" / "collection.json") ||
        !fs::is_directory(root / "storage" / ".backups" / "manager_collection" /
                          "p9_backup")) {
        throw std::runtime_error("collection backup was not created on both processes: " +
                                 backup_response);
    }
    const std::string backup_list = http_request(
        query_port, "GET", "/v1/collections/manager_collection/backups");
    if (backup_list.find("\"p9_backup\"") == std::string::npos) {
        throw std::runtime_error("collection backup list omitted the created backup");
    }

    const std::string post_backup_upsert = http_post(
        query_port,
        "/v1/collections/manager_collection/vectors",
        constant_upsert_body("post-backup-vector", 7.0f, "post-backup"));
    if (post_backup_upsert.find("\"vector_count\":5") == std::string::npos) {
        throw std::runtime_error("post-backup insert failed: " + post_backup_upsert);
    }
    const std::string restore_response = http_post(
        query_port, "/v1/collections/manager_collection/restore", "{\"name\":\"p9_backup\"}");
    if (restore_response.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 ||
        restore_response.find("\"vector_count\":4") == std::string::npos) {
        throw std::runtime_error("collection restore did not reinstate backup state: " +
                                 restore_response);
    }

    const std::string delete_response = http_request(
        query_port, "DELETE", "/v1/collections/manager_collection");
    if (delete_response.rfind("HTTP/1.1 200 OK\r\n", 0) != 0 ||
        fs::exists(trusted / "manager_collection") ||
        fs::exists(root / "storage" / "manager_collection") ||
        fs::exists(trusted / ".backups" / "manager_collection") ||
        fs::exists(root / "storage" / ".backups" / "manager_collection")) {
        throw std::runtime_error("collection deletion did not remove both process states: " +
                                 delete_response);
    }
    const std::string missing_response = http_request(
        query_port, "GET", "/v1/collections/manager_collection");
    if (missing_response.rfind("HTTP/1.1 404 Not Found\r\n", 0) != 0) {
        throw std::runtime_error("deleted collection is still visible through the API");
    }
}

} // namespace

int main() {
    const fs::path root = "/tmp/qcovert-oram-generation-test";
    std::error_code error;
    fs::remove_all(root, error);
    try {
        if (qcovert::SearchRequest{}.top_k != 10) {
            throw std::runtime_error("Search API default top_k is not 10");
        }
        if (qcovert::OnlineVectorCodec384::candidate_cluster_count(10, 161) != 26 ||
            qcovert::OnlineVectorCodec384::candidate_cluster_count(100, 161) != 161 ||
            qcovert::OnlineVectorCodec384::candidate_cluster_count(1, 1) != 1) {
            throw std::runtime_error("candidate cluster count formula is incorrect");
        }
        if (qcovert::OnlineVectorCodec384::exact_candidate_count(10, 26, 1.0) != 280 ||
            qcovert::OnlineVectorCodec384::exact_candidate_count(10, 1, 0.5) != 25 ||
            qcovert::OnlineVectorCodec384::exact_candidate_count(1, 1, 4.0) != 6) {
            throw std::runtime_error("exact candidate count formula is incorrect");
        }
        if (qcovert::GenerationManager::requires_automatic_generation(0, 9999) ||
            !qcovert::GenerationManager::requires_automatic_generation(9999, 10000) ||
            qcovert::GenerationManager::requires_automatic_generation(10000, 10000) ||
            !qcovert::GenerationManager::requires_automatic_generation(5000, 5000, true)) {
            throw std::runtime_error("automatic generation threshold predicate is incorrect");
        }
        if (qcovert::OramGenerationBuilder384::next_vector_capacity(0) != 10000 ||
            qcovert::OramGenerationBuilder384::next_vector_capacity(9999) != 10000 ||
            qcovert::OramGenerationBuilder384::next_vector_capacity(10000) != 20000 ||
            qcovert::OramGenerationBuilder384::next_vector_capacity(19999) != 20000 ||
            qcovert::OramGenerationBuilder384::next_vector_capacity(20000) != 30000) {
            throw std::runtime_error("ORAM generation vector capacity boundary is incorrect");
        }
        const fs::path generation_directory = root / "trusted" / "test_collection" /
                                              "generations" / "generation_1.building";
        const fs::path storage_root = root / "storage";
        fs::create_directories(generation_directory);
        write_test_index(generation_directory / "ivf_rabitq.index");

        const int port = reserve_test_port();
        ChildProcess storage(start_storage(port, storage_root));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        storage.require_running();

        AeadKey key{};
        for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<unsigned char>(i + 1);
        qcovert::OramGenerationConfig config;
        config.enabled = true;
        config.storage_port = port;
        config.real_bucket_size = 32;
        config.dummy_size = 16;
        config.evict_rate = 48;
        config.cached_levels = 4;
        config.upload_batch_bytes = 512U * 1024U;

        const qcovert::OramGenerationArtifacts artifacts =
            qcovert::OramGenerationBuilder384::build(
                "test_collection", "generation_1", generation_directory, key, config);
        if (artifacts.cluster.real_blocks != 4 || artifacts.ring.real_blocks != 64 ||
            artifacts.cluster_count != 4 || artifacts.exact_payload_bytes != 384 * 4 + 4 ||
            artifacts.ring.logical_block_limit != 10000 ||
            artifacts.cluster.logical_block_limit != 316 ||
            artifacts.cluster.real_block_capacity < artifacts.cluster.logical_block_limit ||
            artifacts.ring.real_block_capacity < artifacts.ring.logical_block_limit ||
            artifacts.ring.logical_block_limit * 10ULL >
                artifacts.ring.real_block_capacity * 6ULL ||
            artifacts.cluster.logical_block_limit * 10ULL >
                artifacts.cluster.real_block_capacity * 6ULL) {
            throw std::runtime_error("ORAM generation returned an invalid layout summary");
        }

        require_private_file(generation_directory / "client-manifest.json");
        require_private_file(generation_directory / "cluster-client-state.bin");
        require_private_file(generation_directory / "ring-client-state.bin");
        require_private_file(storage_root / "test_collection" / "generation_1" / "cluster.ring");
        require_private_file(storage_root / "test_collection" / "generation_1" / "ring.ring");
        require_private_file(storage_root / "test_collection" / "generation_1" / "cluster.ring.json");
        require_private_file(storage_root / "test_collection" / "generation_1" / "ring.ring.json");

        const simple_json::Value manifest = simple_json::parse_file(
            (generation_directory / "client-manifest.json").string());
        if (!manifest.is_object() || manifest.get("exact_payload") == nullptr ||
            manifest.get("exact_payload")->string_value != "raw_float32" ||
            manifest.get("storage_protocol_version") == nullptr ||
            manifest.get("storage_protocol_version")->number_value != 3.0 ||
            manifest.get("generation_vector_capacity") == nullptr ||
            manifest.get("generation_vector_capacity")->number_value != 10000.0 ||
            manifest.get("max_real_slot_utilization") == nullptr ||
            manifest.get("max_real_slot_utilization")->number_value != 0.6) {
            throw std::runtime_error("client ORAM manifest is incomplete");
        }

        test_client_state_stash_round_trip(generation_directory, artifacts, config);

        test_generation_manager_stage(root, port);

        storage.stop_gracefully();
        fs::remove_all(root);
        std::cout << "test_oram_generation passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "test_oram_generation failed: " << exception.what() << "\n";
        return 1;
    }
}
