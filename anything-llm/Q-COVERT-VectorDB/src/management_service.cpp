#include "qcovert/management_service.h"

#include "qcovert/collection_data_store.h"
#include "qcovert/search_service.h"
#include "qcovert/storage_connection.h"

#include "simple_json.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace qcovert {
namespace {

uint64_t required_uint64(const simple_json::Value& object, const char* field) {
    const simple_json::Value* value = object.get(field);
    if (value == nullptr || !value->is_number() || value->number_value < 0.0 ||
        std::floor(value->number_value) != value->number_value ||
        value->number_value > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
        throw std::runtime_error(std::string("invalid management statistics field: ") + field);
    }
    return static_cast<uint64_t>(value->number_value);
}

int required_int(const simple_json::Value& object, const char* field) {
    const uint64_t value = required_uint64(object, field);
    if (value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string("management statistics field is too large: ") + field);
    }
    return static_cast<int>(value);
}

uint64_t directory_bytes(const fs::path& directory) {
    uint64_t total = 0;
    std::error_code error;
    if (!fs::exists(directory)) return 0;
    for (fs::recursive_directory_iterator current(
             directory, fs::directory_options::skip_permission_denied, error), end;
         current != end; current.increment(error)) {
        if (error) {
            throw std::runtime_error("cannot inspect trusted storage usage: " + error.message());
        }
        if (!current->is_regular_file(error)) {
            if (error) throw std::runtime_error("cannot inspect trusted file: " + error.message());
            continue;
        }
        const uint64_t size = current->file_size(error);
        if (error || size > std::numeric_limits<uint64_t>::max() - total) {
            throw std::runtime_error("trusted storage usage overflow or file error");
        }
        total += size;
    }
    return total;
}

ManagedOramTreeStats parse_tree(const simple_json::Value& root, const char* field) {
    const simple_json::Value* tree = root.get(field);
    if (tree == nullptr || !tree->is_object()) {
        throw std::runtime_error(std::string("active client manifest is missing tree: ") + field);
    }
    ManagedOramTreeStats stats;
    stats.real_blocks = required_uint64(*tree, "real_blocks");
    stats.logical_block_limit = required_uint64(*tree, "logical_block_limit");
    stats.real_block_capacity = required_uint64(*tree, "real_block_capacity");
    stats.num_buckets = required_uint64(*tree, "num_buckets");
    stats.block_size = required_int(*tree, "block_size");
    stats.num_levels = required_int(*tree, "num_levels");
    stats.cached_levels = required_int(*tree, "cached_levels");
    return stats;
}

uint64_t fvecs_row_count(const fs::path& path, uint32_t expected_dimension) {
    const uint64_t row_bytes = sizeof(int32_t) +
        static_cast<uint64_t>(expected_dimension) * sizeof(float);
    std::error_code error;
    const uint64_t bytes = fs::file_size(path, error);
    if (error || bytes == 0 || bytes % row_bytes != 0) {
        throw std::runtime_error("active centroid table has an invalid file size");
    }
    return bytes / row_bytes;
}

} // namespace

ManagementService::ManagementService(
        CollectionCatalog& catalog,
        CollectionDataStore& data_store,
        SearchService& search_service,
        std::string storage_host,
        int storage_port)
    : catalog_(&catalog),
      data_store_(&data_store),
      search_service_(&search_service),
      storage_host_(std::move(storage_host)),
      storage_port_(storage_port) {
    if (storage_host_.empty() || storage_port_ <= 0 || storage_port_ > 65535) {
        throw std::invalid_argument("ManagementService storage endpoint is invalid");
    }
}

GlobalManagementStats ManagementService::global_stats() const {
    GlobalManagementStats stats;
    for (const CollectionManifest& manifest : catalog_->list_collections()) {
        stats.collection_count++;
        if (manifest.vector_count > std::numeric_limits<uint64_t>::max() - stats.vector_count) {
            throw std::overflow_error("global vector count exceeds uint64");
        }
        stats.vector_count += manifest.vector_count;
        const uint64_t collection_bytes = directory_bytes(
            catalog_->collection_directory(manifest.name));
        if (collection_bytes >
            std::numeric_limits<uint64_t>::max() - stats.trusted_storage_bytes) {
            throw std::overflow_error("global trusted storage usage exceeds uint64");
        }
        stats.trusted_storage_bytes += collection_bytes;
        switch (manifest.state) {
            case CollectionState::Empty: stats.empty_collections++; break;
            case CollectionState::Building: stats.building_collections++; break;
            case CollectionState::Prepared: stats.prepared_collections++; break;
            case CollectionState::Active: stats.active_collections++; break;
            case CollectionState::Failed: stats.failed_collections++; break;
        }
    }
    return stats;
}

CollectionManagementStats ManagementService::collection_stats(const std::string& collection) {
    CollectionManagementStats stats;
    stats.manifest = catalog_->get_collection(collection);
    stats.generation_ready = !stats.manifest.active_generation.empty();
    stats.runtime_cached = search_service_->has_cached_runtime(collection);
    stats.trusted_storage_bytes = directory_bytes(
        catalog_->collection_directory(collection));
    stats.backup_count = catalog_->list_backups(collection).size();
    if (!stats.generation_ready) return stats;

    const fs::path generation = catalog_->collection_directory(collection) /
        "generations" / stats.manifest.active_generation;
    const simple_json::Value client = simple_json::parse_file(
        (generation / "client-manifest.json").string());
    const simple_json::Value replay = simple_json::parse_file(
        (generation / "replay-state.json").string());
    if (!client.is_object() || !replay.is_object()) {
        throw std::runtime_error("active generation management manifests are invalid");
    }
    stats.cluster_count = fvecs_row_count(generation / "centroids.fvecs", 384);
    stats.generation_vector_capacity = required_uint64(client, "generation_vector_capacity");
    stats.generation_vector_count = required_uint64(replay, "vector_count");
    stats.last_log_sequence = required_uint64(replay, "last_log_sequence");
    stats.replayed_records = required_uint64(replay, "replayed_records");
    stats.cluster_stash_blocks = required_uint64(replay, "cluster_stash_blocks");
    stats.ring_stash_blocks = required_uint64(replay, "ring_stash_blocks");
    stats.cluster_oram = parse_tree(client, "cluster");
    stats.ring_oram = parse_tree(client, "ring");
    return stats;
}

void ManagementService::delete_collection(const std::string& collection) {
    const CollectionManifest manifest = catalog_->get_collection(collection);
    if (!manifest.pending_generation.empty()) {
        throw std::logic_error("cannot delete a collection while a generation is building");
    }
    search_service_->checkpoint_and_release(collection);
    StorageConnection::manage_collection(
        storage_host_, storage_port_, StorageAction::DeleteCollection, collection);
    data_store_->forget_collection(collection);
    catalog_->delete_collection(collection);
}

void ManagementService::create_backup(
        const std::string& collection,
        const std::string& backup_name) {
    const CollectionManifest manifest = catalog_->get_collection(collection);
    if (!manifest.pending_generation.empty()) {
        throw std::logic_error("cannot back up a collection while a generation is building");
    }
    search_service_->checkpoint_and_release(collection);
    catalog_->backup_collection(collection, backup_name);
    try {
        StorageConnection::manage_collection(
            storage_host_, storage_port_, StorageAction::BackupCollection,
            collection, backup_name);
    } catch (...) {
        catalog_->delete_backup(collection, backup_name);
        throw;
    }
}

CollectionManifest ManagementService::restore_backup(
        const std::string& collection,
        const std::string& backup_name) {
    const CollectionManifest manifest = catalog_->get_collection(collection);
    if (!manifest.pending_generation.empty()) {
        throw std::logic_error("cannot restore a collection while a generation is building");
    }
    const std::vector<std::string> backups = catalog_->list_backups(collection);
    if (std::find(backups.begin(), backups.end(), backup_name) == backups.end()) {
        throw std::out_of_range("collection backup does not exist: " + backup_name);
    }
    search_service_->checkpoint_and_release(collection);
    StorageConnection::manage_collection(
        storage_host_, storage_port_, StorageAction::RestoreCollection,
        collection, backup_name);
    CollectionManifest restored = catalog_->restore_collection(collection, backup_name);
    data_store_->forget_collection(collection);
    return restored;
}

std::vector<std::string> ManagementService::list_backups(
        const std::string& collection) const {
    return catalog_->list_backups(collection);
}

} // namespace qcovert
