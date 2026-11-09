#include "qcovert/search_service.h"

#include "qcovert/generation_key_store.h"
#include "qcovert/oram_generation_builder.h"
#include "qcovert/storage_connection.h"
#include "qcovert/storage_protocol.h"

#include "online_vector_codec.h"

#include "ClusterORAM.h"
#include "DoubleORAM.h"
#include "RandomForOram.h"
#include "RemoteRing.h"
#include "RingORAM.h"
#include "double_oram_adapter.h"
#include "ivf_rabitq_index.h"
#include "matrix.h"
#include "oram_block_builders.h"
#include "private_ivf_rabitq.h"
#include "simple_json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace qcovert {

namespace {

using ServiceIndex = IvfRabitQIndex<384, 384, 4>;
using ServiceSearcher = PrivateIvfRabitQSearcher<384, 384, 4>;
using ApproxLayout = ApproxOramBlockLayout<384, 384>;
using RawExactLayout = RawFloat32ExactOramBlockLayout<384, 384>;

uint64_t required_uint64(const simple_json::Value& object, const char* field) {
    const simple_json::Value* value = object.get(field);
    if (value == nullptr || !value->is_number() || value->number_value < 0.0 ||
        std::floor(value->number_value) != value->number_value ||
        value->number_value > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
        throw std::runtime_error(std::string("invalid runtime manifest field: ") + field);
    }
    return static_cast<uint64_t>(value->number_value);
}

int required_int(const simple_json::Value& object, const char* field) {
    const uint64_t value = required_uint64(object, field);
    if (value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string("runtime manifest integer is too large: ") + field);
    }
    return static_cast<int>(value);
}

std::string required_string(const simple_json::Value& object, const char* field) {
    const simple_json::Value* value = object.get(field);
    if (value == nullptr || !value->is_string()) {
        throw std::runtime_error(std::string("invalid runtime manifest field: ") + field);
    }
    return value->string_value;
}

std::string json_escape(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch < 0x20) throw std::runtime_error("metadata contains a control character");
                output.push_back(static_cast<char>(ch));
        }
    }
    return output;
}

std::string serialize_json(const simple_json::Value& value) {
    switch (value.type) {
        case simple_json::Value::Type::Null: return "null";
        case simple_json::Value::Type::Bool: return value.bool_value ? "true" : "false";
        case simple_json::Value::Type::Number: {
            std::ostringstream output;
            output.precision(std::numeric_limits<double>::max_digits10);
            output << value.number_value;
            return output.str();
        }
        case simple_json::Value::Type::String:
            return "\"" + json_escape(value.string_value) + "\"";
        case simple_json::Value::Type::Array: {
            std::string output = "[";
            for (size_t i = 0; i < value.array_value.size(); ++i) {
                if (i != 0) output += ',';
                output += serialize_json(value.array_value[i]);
            }
            return output + ']';
        }
        case simple_json::Value::Type::Object: {
            std::string output = "{";
            bool first = true;
            for (const auto& item : value.object_value) {
                if (!first) output += ',';
                first = false;
                output += "\"" + json_escape(item.first) + "\":" + serialize_json(item.second);
            }
            return output + '}';
        }
    }
    throw std::logic_error("unknown metadata JSON type");
}

OramTreeLayout parse_tree_layout(const simple_json::Value& root, const char* field) {
    const simple_json::Value* tree = root.get(field);
    if (tree == nullptr || !tree->is_object()) {
        throw std::runtime_error(std::string("client manifest is missing tree: ") + field);
    }
    OramTreeLayout layout;
    layout.block_size = required_int(*tree, "block_size");
    layout.real_blocks = required_uint64(*tree, "real_blocks");
    layout.logical_block_limit = required_uint64(*tree, "logical_block_limit");
    layout.real_block_capacity = required_uint64(*tree, "real_block_capacity");
    layout.num_levels = required_int(*tree, "num_levels");
    layout.cached_levels = required_int(*tree, "cached_levels");
    layout.num_buckets = required_uint64(*tree, "num_buckets");
    if (layout.block_size <= 0 || layout.real_blocks == 0 ||
        layout.real_blocks > layout.logical_block_limit ||
        layout.logical_block_limit > layout.real_block_capacity ||
        layout.num_levels < 2 || layout.num_levels >= 32 ||
        layout.cached_levels < 0 || layout.cached_levels >= layout.num_levels ||
        layout.num_buckets != (1ULL << layout.num_levels) - 1ULL) {
        throw std::runtime_error(std::string("client manifest has an invalid tree layout: ") + field);
    }
    return layout;
}

StorageHandshake make_open_handshake(
        StorageTreeKind kind,
        const std::string& collection,
        const std::string& generation,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    StorageHandshake handshake{};
    handshake.action = static_cast<uint32_t>(StorageAction::Open);
    handshake.tree_kind = static_cast<uint32_t>(kind);
    handshake.block_size = layout.block_size;
    handshake.real_bucket_size = config.real_bucket_size;
    handshake.dummy_size = config.dummy_size;
    handshake.evict_rate = config.evict_rate;
    handshake.num_levels = layout.num_levels;
    handshake.cached_levels = layout.cached_levels;
    if (collection.empty() || collection.size() >= sizeof(handshake.collection) ||
        generation.empty() || generation.size() >= sizeof(handshake.generation)) {
        throw std::invalid_argument("collection or generation identifier is too long");
    }
    std::memcpy(handshake.collection, collection.data(), collection.size());
    std::memcpy(handshake.generation, generation.data(), generation.size());
    return handshake;
}

double_oram::RingOramConfig make_ring_config(
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    if (layout.logical_block_limit > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("ORAM logical block limit exceeds runtime int capacity");
    }
    return double_oram::RingOramConfig(
        layout.block_size,
        config.real_bucket_size,
        config.dummy_size,
        config.evict_rate,
        static_cast<int>(layout.logical_block_limit),
        layout.num_levels,
        layout.cached_levels);
}

struct LiveRecord {
    std::string external_id;
    std::string metadata_json;
};

std::unordered_map<uint32_t, LiveRecord> load_live_records(const fs::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open active generation live-record map");
    std::unordered_map<uint32_t, LiveRecord> records;
    std::set<std::string> external_ids;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) throw std::runtime_error("live-record map contains an empty line");
        const simple_json::Value value = simple_json::parse_text(line);
        if (!value.is_object()) throw std::runtime_error("live-record entry must be an object");
        const uint64_t internal = required_uint64(value, "internal_id");
        const std::string id = required_string(value, "id");
        const simple_json::Value* metadata = value.get("metadata");
        if (internal > std::numeric_limits<uint32_t>::max() || id.empty() || metadata == nullptr ||
            !external_ids.insert(id).second ||
            !records.emplace(static_cast<uint32_t>(internal),
                             LiveRecord{id, serialize_json(*metadata)}).second) {
            throw std::runtime_error("live-record map contains duplicate or invalid IDs");
        }
    }
    if (!input.eof()) throw std::runtime_error("cannot read active generation live-record map");
    return records;
}

} // namespace

struct SearchService::Runtime {
    std::string collection;
    std::string generation;
    fs::path directory;
    OramGenerationConfig config;
    OramGenerationArtifacts artifacts;
    AeadKey key{};
    Matrix<float> centroids;
    Matrix<float> projection;
    ServiceIndex index;
    std::unordered_map<uint32_t, LiveRecord> records;
    std::unordered_map<std::string, uint32_t> internal_id_by_external_id;
    std::unordered_set<uint32_t> live_ids;
    double_oram::RandomForOram ring_random;
    double_oram::RandomForOram cluster_random;
    std::unique_ptr<StorageConnection> ring_connection;
    std::unique_ptr<double_oram::RemoteRing> ring_storage;
    std::unique_ptr<double_oram::RingORAM> ring;
    std::unique_ptr<StorageConnection> cluster_connection;
    std::unique_ptr<double_oram::RemoteRing> cluster_storage;
    std::unique_ptr<double_oram::ClusterORAM> cluster;
    std::unique_ptr<double_oram::DoubleORAM> double_oram;
    std::unique_ptr<ServiceSearcher> searcher;
    uint64_t last_log_sequence = 0;
    uint64_t replayed_records = 0;
    bool closed = false;
    std::mutex mutex;

    Runtime(
            CollectionCatalog& catalog,
            const CollectionManifest& manifest,
            std::string storage_host,
            int storage_port)
        : collection(manifest.name),
          generation(manifest.active_generation),
          directory(catalog.collection_directory(collection) / "generations" / generation),
          centroids((directory / "centroids.fvecs").string()),
          projection((directory / "projection.fvecs").string()) {
        const simple_json::Value client = simple_json::parse_file(
            (directory / "client-manifest.json").string());
        if (!client.is_object() || required_uint64(client, "schema_version") != 1 ||
            required_string(client, "exact_payload") != "raw_float32" ||
            required_uint64(client, "exact_payload_bytes") != RawExactLayout::kPlainBytes) {
            throw std::runtime_error("active generation client manifest is incompatible");
        }
        config.enabled = true;
        config.storage_host = std::move(storage_host);
        config.storage_port = storage_port;
        config.real_bucket_size = required_int(client, "real_bucket_size");
        config.dummy_size = required_int(client, "dummy_size");
        config.evict_rate = required_int(client, "evict_rate");
        artifacts.cluster = parse_tree_layout(client, "cluster");
        artifacts.ring = parse_tree_layout(client, "ring");
        artifacts.exact_payload_bytes = static_cast<uint32_t>(RawExactLayout::kPlainBytes);

        const simple_json::Value replay = simple_json::parse_file(
            (directory / "replay-state.json").string());
        if (!replay.is_object() || required_string(replay, "state") != "validated") {
            throw std::runtime_error("active generation replay state is not validated");
        }
        const uint64_t durable_vector_count = required_uint64(replay, "vector_count");
        if (durable_vector_count > manifest.vector_count) {
            throw std::runtime_error("active generation is ahead of the collection WAL");
        }
        last_log_sequence = required_uint64(replay, "last_log_sequence");
        replayed_records = required_uint64(replay, "replayed_records");

        records = load_live_records(directory / "live-records.jsonl");
        if (records.size() != durable_vector_count) {
            throw std::runtime_error("active generation live-record count does not match replay state");
        }
        for (const auto& item : records) {
            if (item.first >= artifacts.ring.logical_block_limit) {
                throw std::runtime_error("live record references an out-of-range RingORAM block");
            }
            live_ids.insert(item.first);
            if (!internal_id_by_external_id.emplace(item.second.external_id, item.first).second) {
                throw std::runtime_error("active generation contains duplicate external IDs");
            }
        }

        index.load((directory / "ivf_rabitq.index").string());
        artifacts.cluster_count = index.num_clusters;
        if (centroids.n != index.num_clusters || centroids.d != 384 ||
            projection.n != 384 || projection.d != 384 || index.num_clusters == 0) {
            throw std::runtime_error("active generation ANN dimensions are incompatible");
        }

        const double_oram::RingOramConfig ring_config = make_ring_config(artifacts.ring, config);
        StorageConnection new_ring_connection = StorageConnection::connect(
            config.storage_host,
            config.storage_port,
            make_open_handshake(
                StorageTreeKind::Ring, collection, generation, artifacts.ring, config));
        ring_connection = std::make_unique<StorageConnection>(std::move(new_ring_connection));
        ring_storage = ring_connection->make_remote_ring(ring_config, false, load_key(catalog));
        ring = std::make_unique<double_oram::RingORAM>(
            ring_storage.get(), &ring_random, ring_config, artifacts.ring.num_levels,
            artifacts.ring.cached_levels, true, true);
        OramClientStateStore::load_ring(
            directory / "ring-client-state.bin", *ring, artifacts.ring, config);

        const std::vector<int> cluster_counts = OramClientStateStore::read_cluster_block_counts(
            directory / "cluster-client-state.bin", artifacts.cluster, config);
        if (cluster_counts.size() != index.num_clusters) {
            throw std::runtime_error("ClusterORAM state and IVF index have different cluster counts");
        }
        const double_oram::RingOramConfig cluster_config =
            make_ring_config(artifacts.cluster, config);
        StorageConnection new_cluster_connection = StorageConnection::connect(
            config.storage_host,
            config.storage_port,
            make_open_handshake(
                StorageTreeKind::Cluster, collection, generation, artifacts.cluster, config));
        cluster_connection = std::make_unique<StorageConnection>(std::move(new_cluster_connection));
        cluster_storage = cluster_connection->make_remote_ring(cluster_config, false, key);
        cluster = std::make_unique<double_oram::ClusterORAM>(
            cluster_storage.get(), &cluster_random, cluster_config,
            artifacts.cluster.num_levels, cluster_counts);
        OramClientStateStore::load_cluster(
            directory / "cluster-client-state.bin", *cluster, artifacts.cluster, config);

        index.approx_blocks.resize(index.num_clusters);
        index.approx_blocks_per_cluster = 0;
        for (uint32_t cluster_id = 0; cluster_id < index.num_clusters; ++cluster_id) {
            index.approx_blocks[cluster_id] = static_cast<uint32_t>(cluster_counts[cluster_id]);
            index.approx_blocks_per_cluster = std::max(
                index.approx_blocks_per_cluster, index.approx_blocks[cluster_id]);
            const uint64_t slots =
                static_cast<uint64_t>(cluster_counts[cluster_id]) * ServiceIndex::kBlockVectors;
            if (slots > std::numeric_limits<uint32_t>::max()) {
                throw std::runtime_error("ClusterORAM search slot count exceeds uint32");
            }
            index.len[cluster_id] = static_cast<uint32_t>(slots);
        }

        const double_oram::DoubleORAMConfig double_config(
            static_cast<int>(index.num_clusters),
            static_cast<int>(ApproxLayout::kPlainBytes),
            384,
            static_cast<int>(ServiceIndex::kBlockVectors),
            2,
            0,
            double_oram::DoubleORAMPathBlockLayout::AnnBatch);
        double_oram = std::make_unique<double_oram::DoubleORAM>(*cluster, *ring, double_config);
        searcher = std::make_unique<ServiceSearcher>(index, ExactPayloadFormat::RawFloat32);
        index.release_double_oram_search_unused_data();
    }

    AeadKey load_key(CollectionCatalog& catalog) {
        GenerationKeyStore key_store(catalog.root());
        key = key_store.load(collection, generation);
        return key;
    }

    std::vector<OramLiveRecord> persistent_live_records() const {
        std::vector<OramLiveRecord> output;
        output.reserve(records.size());
        for (const auto& item : records) {
            output.push_back({item.first, item.second.external_id, item.second.metadata_json});
        }
        return output;
    }

    void checkpoint_and_persist_locked() {
        if (closed) return;
        artifacts.cluster.real_blocks = static_cast<uint64_t>(cluster->num_blocks);
        ring_storage.reset();
        if (ring_connection) ring_connection->close();
        cluster_storage.reset();
        if (cluster_connection) cluster_connection->close();
        OramClientStateStore::save_active_generation(
            directory,
            *ring,
            *cluster,
            artifacts,
            config,
            persistent_live_records(),
            records.size(),
            last_log_sequence,
            replayed_records);
        closed = true;
    }

    ActiveGenerationSyncResult synchronize_and_checkpoint(CollectionDataStore& data_store) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) {
            ActiveGenerationSyncResult result;
            result.active_generation = true;
            result.runtime_reload_required = true;
            return result;
        }
        const std::vector<VectorDelta> deltas =
            data_store.read_latest_after(collection, last_log_sequence);
        ActiveGenerationSyncResult result;
        result.active_generation = true;
        if (deltas.empty()) {
            result.capacity_exhausted =
                artifacts.ring.real_blocks >= artifacts.ring.logical_block_limit;
            return result;
        }
        const uint64_t remaining =
            artifacts.ring.logical_block_limit - artifacts.ring.real_blocks;
        if (deltas.size() > remaining ||
            artifacts.ring.real_blocks + deltas.size() >
                static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error(
                "active RingORAM insertion reserve is exhausted; rebuild the generation");
        }

        for (const VectorDelta& delta : deltas) {
            const int internal_id = static_cast<int>(artifacts.ring.real_blocks);
            const OnlineQuantizedVector384 quantized = OnlineVectorCodec384::quantize(
                delta.record, centroids, projection);
            double_oram->insert_ann_vector(
                quantized.target_cluster,
                quantized.candidate_clusters,
                internal_id,
                quantized.approximate,
                OnlineVectorCodec384::raw_float32_exact_payload(delta.record, internal_id),
                static_cast<int>(quantized.candidate_clusters.size()));
            artifacts.ring.real_blocks++;
            artifacts.cluster.real_blocks = static_cast<uint64_t>(cluster->num_blocks);

            const auto previous = internal_id_by_external_id.find(delta.record.id);
            if (previous != internal_id_by_external_id.end()) {
                live_ids.erase(previous->second);
                records.erase(previous->second);
            }
            const uint32_t new_internal_id = static_cast<uint32_t>(internal_id);
            internal_id_by_external_id[delta.record.id] = new_internal_id;
            live_ids.insert(new_internal_id);
            records.emplace(new_internal_id, LiveRecord{
                delta.record.id,
                delta.record.metadata_json,
            });
            last_log_sequence = delta.sequence;
            replayed_records++;
            result.applied_records++;

            double_oram->evict_cluster();
            double_oram->evict_ring_and_clear_cache();
        }
        checkpoint_and_persist_locked();
        result.runtime_reload_required = true;
        result.capacity_exhausted =
            artifacts.ring.real_blocks >= artifacts.ring.logical_block_limit;
        return result;
    }

    bool has_insertion_capacity(size_t record_count) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) return false;
        return record_count <= artifacts.ring.logical_block_limit - artifacts.ring.real_blocks &&
               artifacts.ring.real_blocks + record_count <=
                   static_cast<uint64_t>(std::numeric_limits<int>::max());
    }

    void checkpoint_and_close() {
        std::lock_guard<std::mutex> lock(mutex);
        checkpoint_and_persist_locked();
    }

    ~Runtime() {
        std::lock_guard<std::mutex> lock(mutex);
        try {
            checkpoint_and_persist_locked();
        } catch (...) {
            // Destructors cannot report checkpoint failures. The previous
            // durable client/server checkpoint remains the recovery point.
        }
        if (ring) {
            for (auto& item : ring->mmstash) delete item.second;
            ring->mmstash.clear();
        }
    }

    SearchResponse execute(const SearchRequest& request) {
        if (request.vector.size() != 384 || request.top_k == 0 || request.top_k > 1000 ||
            !std::isfinite(request.candidate_factor) || request.candidate_factor <= 0.0 ||
            request.candidate_factor > 100.0) {
            throw std::invalid_argument("invalid private search parameters");
        }
        for (float value : request.vector) {
            if (!std::isfinite(value)) throw std::invalid_argument("search vector must be finite");
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) throw std::logic_error("active generation runtime is closed");
        const uint32_t nprobe = OnlineVectorCodec384::candidate_cluster_count(
            request.top_k, index.num_clusters);
        const uint32_t candidate_limit = OnlineVectorCodec384::exact_candidate_count(
            request.top_k, nprobe, request.candidate_factor);

        std::vector<float> randomized_query(384, 0.0f);
        for (size_t input = 0; input < 384; ++input) {
            for (size_t output = 0; output < 384; ++output) {
                randomized_query[output] += request.vector[input] * projection.row(input)[output];
            }
        }

        DoubleOramAnnReader reader(
            *double_oram, static_cast<int>(artifacts.exact_payload_bytes), &live_ids);
        ResultHeap heap;
        try {
            heap = searcher->search_double_oram(
                request.vector.data(),
                randomized_query.data(),
                request.top_k,
                nprobe,
                reader,
                nprobe,
                candidate_limit);
        } catch (...) {
            try {
                reader.end_query();
            } catch (...) {
            }
            throw;
        }

        std::vector<Result> ordered;
        ordered.reserve(heap.size());
        while (!heap.empty()) {
            ordered.push_back(heap.top());
            heap.pop();
        }
        std::reverse(ordered.begin(), ordered.end());

        SearchResponse response;
        response.generation = generation;
        response.nprobe = nprobe;
        response.exact_candidate_limit = candidate_limit;
        for (const Result& result : ordered) {
            const auto record = records.find(result.second);
            if (record == records.end()) continue;
            response.hits.push_back({
                record->second.external_id,
                result.first,
                1.0f / (1.0f + result.first),
                record->second.metadata_json,
            });
        }
        return response;
    }
};

SearchService::SearchService(
        CollectionCatalog& catalog,
        CollectionDataStore& data_store,
        std::string storage_host,
        int storage_port)
    : catalog_(&catalog),
      data_store_(&data_store),
      storage_host_(std::move(storage_host)),
      storage_port_(storage_port) {
    if (storage_host_.empty() || storage_port_ <= 0 || storage_port_ > 65535) {
        throw std::invalid_argument("SearchService storage endpoint is invalid");
    }
}

SearchService::~SearchService() = default;

std::shared_ptr<SearchService::Runtime> SearchService::runtime_for(
        const std::string& collection,
        const CollectionManifest& manifest) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto existing = runtimes_.find(collection);
    if (existing != runtimes_.end() && existing->second->generation == manifest.active_generation) {
        return existing->second;
    }
    std::shared_ptr<Runtime> runtime = std::make_shared<Runtime>(
        *catalog_, manifest, storage_host_, storage_port_);
    runtimes_[collection] = runtime;
    return runtime;
}

void SearchService::discard_runtime(
        const std::string& collection,
        const std::shared_ptr<Runtime>& runtime) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = runtimes_.find(collection);
    if (existing != runtimes_.end() && existing->second == runtime) {
        runtimes_.erase(existing);
    }
}

ActiveGenerationSyncResult SearchService::synchronize_active(const std::string& collection) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const CollectionManifest manifest = catalog_->get_collection(valid_collection);
        if (manifest.state != CollectionState::Active || manifest.active_generation.empty()) {
            return {};
        }
        const std::shared_ptr<Runtime> runtime = runtime_for(valid_collection, manifest);
        ActiveGenerationSyncResult result =
            runtime->synchronize_and_checkpoint(*data_store_);
        if (result.runtime_reload_required) discard_runtime(valid_collection, runtime);
        if (!result.runtime_reload_required || result.applied_records != 0) return result;
    }
    throw std::runtime_error("active generation runtime could not be refreshed for insertion");
}

OnlineUpsertResult SearchService::upsert(
        const std::string& collection,
        const std::vector<VectorRecord>& records) {
    if (records.empty()) throw std::invalid_argument("online upsert records must not be empty");
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    std::lock_guard<std::mutex> upsert_lock(upsert_mutex_);

    for (int attempt = 0; attempt < 3; ++attempt) {
        const CollectionManifest manifest = catalog_->get_collection(valid_collection);
        if (manifest.state != CollectionState::Active || manifest.active_generation.empty()) break;
        const std::shared_ptr<Runtime> runtime = runtime_for(valid_collection, manifest);
        const ActiveGenerationSyncResult state =
            runtime->synchronize_and_checkpoint(*data_store_);
        if (state.runtime_reload_required) {
            discard_runtime(valid_collection, runtime);
            continue;
        }
        if (runtime->has_insertion_capacity(records.size())) break;
        throw std::length_error(
            "online upsert batch exceeds the active generation insertion reserve");
    }

    OnlineUpsertResult result;
    result.wal = data_store_->upsert(valid_collection, records);
    result.active = synchronize_active(valid_collection);
    return result;
}

SearchResponse SearchService::search(const std::string& collection, const SearchRequest& request) {
    const auto started_at = std::chrono::steady_clock::now();
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const CollectionManifest manifest = catalog_->get_collection(valid_collection);
        if (manifest.state != CollectionState::Active || manifest.active_generation.empty()) {
            throw std::logic_error("collection does not have an active searchable generation");
        }
        const std::shared_ptr<Runtime> runtime = runtime_for(valid_collection, manifest);
        const ActiveGenerationSyncResult synchronized =
            runtime->synchronize_and_checkpoint(*data_store_);
        if (!synchronized.runtime_reload_required) {
            SearchResponse response = runtime->execute(request);
            response.search_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started_at)
                    .count());
            return response;
        }
        discard_runtime(valid_collection, runtime);
    }
    throw std::runtime_error("active generation changed repeatedly while preparing search");
}

void SearchService::checkpoint_and_release(const std::string& collection) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    std::shared_ptr<Runtime> runtime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = runtimes_.find(valid_collection);
        if (found == runtimes_.end()) return;
        runtime = found->second;
        runtimes_.erase(found);
    }
    runtime->checkpoint_and_close();
}

void SearchService::checkpoint_all() {
    std::vector<std::shared_ptr<Runtime>> runtimes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimes.reserve(runtimes_.size());
        for (auto& item : runtimes_) runtimes.push_back(std::move(item.second));
        runtimes_.clear();
    }

    std::exception_ptr first_error;
    for (const std::shared_ptr<Runtime>& runtime : runtimes) {
        try {
            runtime->checkpoint_and_close();
        } catch (...) {
            if (!first_error) first_error = std::current_exception();
        }
    }
    if (first_error) std::rethrow_exception(first_error);
}

bool SearchService::has_cached_runtime(const std::string& collection) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    std::lock_guard<std::mutex> lock(mutex_);
    return runtimes_.find(valid_collection) != runtimes_.end();
}

} // namespace qcovert
