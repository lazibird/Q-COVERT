#include "qcovert/collection_catalog.h"

#include "simple_json.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <algorithm>
#include <thread>

#include <sys/stat.h>

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
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20) {
                    throw std::invalid_argument("manifest string contains a control character");
                }
                escaped.push_back(static_cast<char>(ch));
        }
    }
    return escaped;
}

uint64_t required_uint64(const simple_json::Value& root, const char* key) {
    const simple_json::Value* value = root.get(key);
    if (value == nullptr || !value->is_number() || value->number_value < 0.0 ||
        std::floor(value->number_value) != value->number_value) {
        throw std::runtime_error(std::string("invalid collection manifest field: ") + key);
    }
    return static_cast<uint64_t>(value->number_value);
}

std::string required_string(const simple_json::Value& root, const char* key) {
    const simple_json::Value* value = root.get(key);
    if (value == nullptr || !value->is_string()) {
        throw std::runtime_error(std::string("invalid collection manifest field: ") + key);
    }
    return value->string_value;
}

void ensure_owner_only(const fs::path& path) {
    std::error_code error;
    fs::permissions(path,
                    fs::perms::owner_all,
                    fs::perm_options::replace,
                    error);
    if (error) {
        throw std::runtime_error("cannot restrict permissions on " + path.string() + ": " + error.message());
    }
}

fs::path temporary_sibling(const fs::path& path, const char* suffix) {
    return path.string() + suffix + "." +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

void copy_directory(const fs::path& source, const fs::path& destination) {
    std::error_code error;
    fs::remove_all(destination, error);
    error.clear();
    fs::copy(source, destination, fs::copy_options::recursive, error);
    if (error) {
        const std::string message = error.message();
        fs::remove_all(destination, error);
        throw std::runtime_error("cannot copy trusted collection: " + message);
    }
    ensure_owner_only(destination);
}

} // namespace

CollectionCatalog::CollectionCatalog(fs::path root) : root_(std::move(root)) {
    if (root_.empty()) {
        throw std::invalid_argument("collection catalog root must not be empty");
    }
    std::error_code error;
    fs::create_directories(root_, error);
    if (error) {
        throw std::runtime_error("cannot create collection catalog root: " + error.message());
    }
    ensure_owner_only(root_);
}

CreateCollectionResult CollectionCatalog::create_collection(
        const std::string& name,
        uint32_t dimension,
        const std::string& metric,
        const std::string& embedding_model) {
    const std::string valid_name = validate_collection_name(name);
    if (dimension != kSupportedEmbeddingDimension) {
        throw std::invalid_argument("only 384-dimensional embeddings are supported");
    }
    if (metric != "l2") {
        throw std::invalid_argument("only l2 metric is supported");
    }
    if (embedding_model.size() > 512) {
        throw std::invalid_argument("embedding_model must contain at most 512 bytes");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const fs::path path = manifest_path(valid_name);
    if (fs::exists(path)) {
        CollectionManifest manifest = load_manifest_locked(valid_name);
        if (manifest.dimension != dimension || manifest.metric != metric ||
            (!embedding_model.empty() && !manifest.embedding_model.empty() &&
             manifest.embedding_model != embedding_model)) {
            throw std::invalid_argument("existing collection configuration does not match request");
        }
        return {false, std::move(manifest)};
    }

    const fs::path directory = collection_directory(valid_name);
    std::error_code error;
    fs::create_directories(directory / "generations", error);
    if (error) {
        throw std::runtime_error("cannot create collection directory: " + error.message());
    }
    ensure_owner_only(directory);
    ensure_owner_only(directory / "generations");

    CollectionManifest manifest;
    manifest.name = valid_name;
    manifest.dimension = dimension;
    manifest.metric = metric;
    manifest.embedding_model = embedding_model;
    manifest.created_at_unix_ms = now_unix_ms();
    manifest.updated_at_unix_ms = manifest.created_at_unix_ms;
    write_manifest_locked(manifest);
    return {true, std::move(manifest)};
}

CollectionManifest CollectionCatalog::get_collection(const std::string& name) const {
    const std::string valid_name = validate_collection_name(name);
    std::lock_guard<std::mutex> lock(mutex_);
    return load_manifest_locked(valid_name);
}

bool CollectionCatalog::exists(const std::string& name) const {
    const std::string valid_name = validate_collection_name(name);
    std::lock_guard<std::mutex> lock(mutex_);
    return fs::is_regular_file(manifest_path(valid_name));
}

std::vector<CollectionManifest> CollectionCatalog::list_collections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CollectionManifest> manifests;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(root_, error)) {
        if (error) {
            throw std::runtime_error("cannot enumerate collection catalog: " + error.message());
        }
        if (!entry.is_directory() || !fs::is_regular_file(entry.path() / "collection.json")) {
            continue;
        }
        try {
            manifests.push_back(load_manifest_locked(validate_collection_name(entry.path().filename().string())));
        } catch (const std::invalid_argument&) {
            // Ignore unrelated root entries; manifests are only accepted through the API.
        }
    }
    return manifests;
}

void CollectionCatalog::delete_collection(const std::string& name) {
    const std::string valid_name = validate_collection_name(name);
    std::lock_guard<std::mutex> lock(mutex_);
    const CollectionManifest manifest = load_manifest_locked(valid_name);
    if (!manifest.pending_generation.empty()) {
        throw std::logic_error("cannot delete a collection while a generation is building");
    }
    std::error_code error;
    fs::remove_all(collection_directory(valid_name), error);
    if (error) {
        throw std::runtime_error("cannot delete trusted collection: " + error.message());
    }
    fs::remove_all(root_ / ".backups" / valid_name, error);
    if (error) {
        throw std::runtime_error("cannot delete trusted collection backups: " + error.message());
    }
}

void CollectionCatalog::backup_collection(
        const std::string& name,
        const std::string& backup_name) {
    const std::string valid_name = validate_collection_name(name);
    const std::string valid_backup = validate_backup_name(backup_name);
    std::lock_guard<std::mutex> lock(mutex_);
    const CollectionManifest manifest = load_manifest_locked(valid_name);
    if (!manifest.pending_generation.empty()) {
        throw std::logic_error("cannot back up a collection while a generation is building");
    }
    const fs::path backup = root_ / ".backups" / valid_name / valid_backup;
    if (fs::exists(backup)) {
        throw std::logic_error("collection backup already exists: " + valid_backup);
    }
    std::error_code error;
    fs::create_directories(backup.parent_path(), error);
    if (error) throw std::runtime_error("cannot create backup directory: " + error.message());
    const fs::path staging = temporary_sibling(backup, ".building");
    copy_directory(collection_directory(valid_name), staging);
    fs::rename(staging, backup, error);
    if (error) {
        const std::string message = error.message();
        fs::remove_all(staging, error);
        throw std::runtime_error("cannot publish trusted collection backup: " + message);
    }
}

CollectionManifest CollectionCatalog::restore_collection(
        const std::string& name,
        const std::string& backup_name) {
    const std::string valid_name = validate_collection_name(name);
    const std::string valid_backup = validate_backup_name(backup_name);
    std::lock_guard<std::mutex> lock(mutex_);
    const CollectionManifest current = load_manifest_locked(valid_name);
    if (!current.pending_generation.empty()) {
        throw std::logic_error("cannot restore a collection while a generation is building");
    }
    const fs::path backup = root_ / ".backups" / valid_name / valid_backup;
    if (!fs::is_regular_file(backup / "collection.json")) {
        throw std::out_of_range("collection backup does not exist: " + valid_backup);
    }
    const simple_json::Value backup_manifest = simple_json::parse_file(
        (backup / "collection.json").string());
    const simple_json::Value* pending = backup_manifest.get("pending_generation");
    if (!backup_manifest.is_object() ||
        required_string(backup_manifest, "name") != valid_name ||
        (pending != nullptr && (!pending->is_string() || !pending->string_value.empty()))) {
        throw std::runtime_error("collection backup manifest is invalid or incomplete");
    }

    const fs::path collection = collection_directory(valid_name);
    const fs::path staging = temporary_sibling(collection, ".restoring");
    const fs::path previous = temporary_sibling(collection, ".previous");
    copy_directory(backup, staging);
    std::error_code error;
    fs::rename(collection, previous, error);
    if (error) {
        fs::remove_all(staging, error);
        throw std::runtime_error("cannot stage trusted collection for restore: " + error.message());
    }
    fs::rename(staging, collection, error);
    if (error) {
        fs::rename(previous, collection, error);
        fs::remove_all(staging, error);
        throw std::runtime_error("cannot publish restored trusted collection");
    }
    try {
        CollectionManifest restored = load_manifest_locked(valid_name);
        fs::remove_all(previous, error);
        return restored;
    } catch (...) {
        fs::remove_all(collection, error);
        fs::rename(previous, collection, error);
        throw;
    }
}

std::vector<std::string> CollectionCatalog::list_backups(const std::string& name) const {
    const std::string valid_name = validate_collection_name(name);
    std::lock_guard<std::mutex> lock(mutex_);
    load_manifest_locked(valid_name);
    std::vector<std::string> backups;
    const fs::path directory = root_ / ".backups" / valid_name;
    std::error_code error;
    if (!fs::exists(directory)) return backups;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        if (error) throw std::runtime_error("cannot enumerate collection backups: " + error.message());
        if (entry.is_directory() && fs::is_regular_file(entry.path() / "collection.json")) {
            try {
                backups.push_back(validate_backup_name(entry.path().filename().string()));
            } catch (const std::invalid_argument&) {
            }
        }
    }
    std::sort(backups.begin(), backups.end());
    return backups;
}

void CollectionCatalog::delete_backup(
        const std::string& name,
        const std::string& backup_name) {
    const std::string valid_name = validate_collection_name(name);
    const std::string valid_backup = validate_backup_name(backup_name);
    std::lock_guard<std::mutex> lock(mutex_);
    const fs::path backup = root_ / ".backups" / valid_name / valid_backup;
    std::error_code error;
    fs::remove_all(backup, error);
    if (error) throw std::runtime_error("cannot delete trusted backup: " + error.message());
}

CollectionManifest CollectionCatalog::set_vector_count(const std::string& name, uint64_t vector_count) {
    const std::string valid_name = validate_collection_name(name);
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionManifest manifest = load_manifest_locked(valid_name);
    manifest.vector_count = vector_count;
    manifest.updated_at_unix_ms = now_unix_ms();
    write_manifest_locked(manifest);
    return manifest;
}

CollectionManifest CollectionCatalog::begin_generation(const std::string& name, std::string& generation_id) {
    const std::string valid_name = validate_collection_name(name);
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionManifest manifest = load_manifest_locked(valid_name);
    if (!manifest.pending_generation.empty()) {
        throw std::logic_error("a generation is already building for collection: " + valid_name);
    }
    generation_id = "generation_" + std::to_string(manifest.next_generation++);
    manifest.pending_generation = generation_id;
    manifest.pending_generation_stage = GenerationStage::Building;
    if (manifest.active_generation.empty()) manifest.state = CollectionState::Building;
    manifest.updated_at_unix_ms = now_unix_ms();
    write_manifest_locked(manifest);
    return manifest;
}

CollectionManifest CollectionCatalog::activate_generation(
        const std::string& name,
        const std::string& generation_id,
        uint64_t vector_count) {
    const std::string valid_name = validate_collection_name(name);
    if (generation_id.empty()) {
        throw std::invalid_argument("generation id must not be empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionManifest manifest = load_manifest_locked(valid_name);
    if (manifest.pending_generation != generation_id ||
        manifest.pending_generation_stage != GenerationStage::Validated) {
        throw std::logic_error("cannot activate a generation before ORAM validation");
    }
    manifest.active_generation = generation_id;
    manifest.vector_count = vector_count;
    manifest.state = CollectionState::Active;
    manifest.pending_generation.clear();
    manifest.pending_generation_stage = GenerationStage::None;
    manifest.updated_at_unix_ms = now_unix_ms();
    write_manifest_locked(manifest);
    return manifest;
}

CollectionManifest CollectionCatalog::validate_and_activate_generation(
        const std::string& name,
        const std::string& generation_id,
        uint64_t vector_count) {
    const std::string valid_name = validate_collection_name(name);
    if (generation_id.empty()) {
        throw std::invalid_argument("generation id must not be empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionManifest manifest = load_manifest_locked(valid_name);
    if (manifest.pending_generation != generation_id ||
        manifest.pending_generation_stage != GenerationStage::OramReady) {
        throw std::logic_error("only an OramReady pending generation can be validated and activated");
    }
    manifest.active_generation = generation_id;
    manifest.vector_count = vector_count;
    manifest.state = CollectionState::Active;
    manifest.pending_generation.clear();
    manifest.pending_generation_stage = GenerationStage::None;
    manifest.updated_at_unix_ms = now_unix_ms();
    write_manifest_locked(manifest);
    return manifest;
}

CollectionManifest CollectionCatalog::advance_generation(
        const std::string& name,
        const std::string& generation_id,
        GenerationStage stage) {
    const std::string valid_name = validate_collection_name(name);
    if (stage == GenerationStage::None || stage == GenerationStage::Building) {
        throw std::invalid_argument("generation can only advance to a completed stage");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionManifest manifest = load_manifest_locked(valid_name);
    if (manifest.pending_generation != generation_id) {
        throw std::logic_error("generation is not the collection's pending generation");
    }
    if (static_cast<int>(stage) <= static_cast<int>(manifest.pending_generation_stage)) {
        throw std::logic_error("generation stage must advance monotonically");
    }
    manifest.pending_generation_stage = stage;
    if (manifest.active_generation.empty()) manifest.state = CollectionState::Prepared;
    manifest.updated_at_unix_ms = now_unix_ms();
    write_manifest_locked(manifest);
    return manifest;
}

CollectionManifest CollectionCatalog::fail_generation(const std::string& name) {
    const std::string valid_name = validate_collection_name(name);
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionManifest manifest = load_manifest_locked(valid_name);
    manifest.state = manifest.active_generation.empty() ? CollectionState::Failed : CollectionState::Active;
    manifest.pending_generation.clear();
    manifest.pending_generation_stage = GenerationStage::None;
    manifest.updated_at_unix_ms = now_unix_ms();
    write_manifest_locked(manifest);
    return manifest;
}

fs::path CollectionCatalog::collection_directory(const std::string& name) const {
    return root_ / validate_collection_name(name);
}

const fs::path& CollectionCatalog::root() const {
    return root_;
}

std::string CollectionCatalog::validate_collection_name(const std::string& name) {
    if (name.empty() || name.size() > 95) {
        throw std::invalid_argument("collection name must contain 1 to 95 characters");
    }
    for (const char ch : name) {
        const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!valid) {
            throw std::invalid_argument("collection name may only contain letters, digits, '-' and '_'");
        }
    }
    return name;
}

std::string CollectionCatalog::validate_backup_name(const std::string& name) {
    try {
        return validate_collection_name(name);
    } catch (const std::invalid_argument&) {
        throw std::invalid_argument(
            "backup name may only contain 1 to 95 letters, digits, '-' and '_'");
    }
}

const char* CollectionCatalog::collection_state_name(CollectionState state) {
    switch (state) {
        case CollectionState::Empty: return "empty";
        case CollectionState::Building: return "building";
        case CollectionState::Prepared: return "prepared";
        case CollectionState::Active: return "active";
        case CollectionState::Failed: return "failed";
    }
    throw std::logic_error("unknown collection state");
}

CollectionState CollectionCatalog::parse_collection_state(const std::string& value) {
    if (value == "empty") return CollectionState::Empty;
    if (value == "building") return CollectionState::Building;
    if (value == "prepared") return CollectionState::Prepared;
    if (value == "active") return CollectionState::Active;
    if (value == "failed") return CollectionState::Failed;
    throw std::runtime_error("invalid collection manifest state: " + value);
}

const char* CollectionCatalog::generation_stage_name(GenerationStage stage) {
    switch (stage) {
        case GenerationStage::None: return "none";
        case GenerationStage::Building: return "building";
        case GenerationStage::IvfReady: return "ivf_ready";
        case GenerationStage::OramReady: return "oram_ready";
        case GenerationStage::Validated: return "validated";
    }
    throw std::logic_error("unknown generation stage");
}

GenerationStage CollectionCatalog::parse_generation_stage(const std::string& value) {
    if (value == "none") return GenerationStage::None;
    if (value == "building") return GenerationStage::Building;
    if (value == "ivf_ready") return GenerationStage::IvfReady;
    if (value == "oram_ready") return GenerationStage::OramReady;
    if (value == "validated") return GenerationStage::Validated;
    throw std::runtime_error("invalid pending generation stage: " + value);
}

fs::path CollectionCatalog::manifest_path(const std::string& name) const {
    return collection_directory(name) / "collection.json";
}

CollectionManifest CollectionCatalog::load_manifest_locked(const std::string& name) const {
    const fs::path path = manifest_path(name);
    if (!fs::is_regular_file(path)) {
        throw std::out_of_range("collection does not exist: " + name);
    }
    const simple_json::Value root = simple_json::parse_file(path.string());
    if (!root.is_object()) {
        throw std::runtime_error("collection manifest must be a JSON object: " + path.string());
    }

    CollectionManifest manifest;
    manifest.schema_version = static_cast<uint32_t>(required_uint64(root, "schema_version"));
    if (manifest.schema_version != 1 && manifest.schema_version != kCollectionManifestSchemaVersion) {
        throw std::runtime_error("unsupported collection manifest schema");
    }
    manifest.name = validate_collection_name(required_string(root, "name"));
    if (manifest.name != name) {
        throw std::runtime_error("collection manifest name does not match directory");
    }
    manifest.dimension = static_cast<uint32_t>(required_uint64(root, "dimension"));
    manifest.metric = required_string(root, "metric");
    manifest.embedding_model = required_string(root, "embedding_model");
    manifest.vector_count = required_uint64(root, "vector_count");
    manifest.state = parse_collection_state(required_string(root, "state"));
    manifest.active_generation = required_string(root, "active_generation");
    if (manifest.schema_version >= 2) {
        manifest.pending_generation = required_string(root, "pending_generation");
        manifest.pending_generation_stage = parse_generation_stage(
            required_string(root, "pending_generation_stage"));
    }
    manifest.next_generation = required_uint64(root, "next_generation");
    manifest.created_at_unix_ms = required_uint64(root, "created_at_unix_ms");
    manifest.updated_at_unix_ms = required_uint64(root, "updated_at_unix_ms");
    if (manifest.dimension != kSupportedEmbeddingDimension || manifest.metric != "l2" ||
        manifest.next_generation == 0) {
        throw std::runtime_error("collection manifest has unsupported configuration");
    }
    manifest.schema_version = kCollectionManifestSchemaVersion;
    return manifest;
}

void CollectionCatalog::write_manifest_locked(const CollectionManifest& manifest) const {
    const fs::path path = manifest_path(manifest.name);
    const fs::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create collection manifest: " + temporary.string());
    }
    output << "{\n"
           << "  \"schema_version\": " << manifest.schema_version << ",\n"
           << "  \"name\": \"" << json_escape(manifest.name) << "\",\n"
           << "  \"dimension\": " << manifest.dimension << ",\n"
           << "  \"metric\": \"" << json_escape(manifest.metric) << "\",\n"
           << "  \"embedding_model\": \"" << json_escape(manifest.embedding_model) << "\",\n"
           << "  \"vector_count\": " << manifest.vector_count << ",\n"
           << "  \"state\": \"" << collection_state_name(manifest.state) << "\",\n"
           << "  \"active_generation\": \"" << json_escape(manifest.active_generation) << "\",\n"
           << "  \"pending_generation\": \"" << json_escape(manifest.pending_generation) << "\",\n"
           << "  \"pending_generation_stage\": \""
           << generation_stage_name(manifest.pending_generation_stage) << "\",\n"
           << "  \"next_generation\": " << manifest.next_generation << ",\n"
           << "  \"created_at_unix_ms\": " << manifest.created_at_unix_ms << ",\n"
           << "  \"updated_at_unix_ms\": " << manifest.updated_at_unix_ms << "\n"
           << "}\n";
    output.close();
    if (!output) {
        throw std::runtime_error("cannot write collection manifest: " + temporary.string());
    }
    ensure_owner_only(temporary);
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) {
        throw std::runtime_error("cannot atomically activate collection manifest: " + error.message());
    }
    ensure_owner_only(path);
}

} // namespace qcovert
