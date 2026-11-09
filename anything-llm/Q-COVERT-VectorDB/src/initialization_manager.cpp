#include "qcovert/initialization_manager.h"

#include "simple_json.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <thread>
#include <unistd.h>

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
    for (const unsigned char ch : value) {
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
                    throw std::invalid_argument(
                        "initialization string contains a control character");
                }
                escaped.push_back(static_cast<char>(ch));
        }
    }
    return escaped;
}

uint64_t required_uint64(const simple_json::Value& object, const char* key) {
    const simple_json::Value* value = object.get(key);
    if (value == nullptr || !value->is_number() || !std::isfinite(value->number_value) ||
        value->number_value < 0.0 || std::floor(value->number_value) != value->number_value ||
        value->number_value > 9007199254740991.0) {
        throw std::runtime_error(std::string("invalid initialization field: ") + key);
    }
    return static_cast<uint64_t>(value->number_value);
}

std::string required_string(const simple_json::Value& object, const char* key) {
    const simple_json::Value* value = object.get(key);
    if (value == nullptr || !value->is_string()) {
        throw std::runtime_error(std::string("invalid initialization field: ") + key);
    }
    return value->string_value;
}

InitializationState parse_state(const std::string& value) {
    if (value == "uploading") return InitializationState::Uploading;
    if (value == "building") return InitializationState::Building;
    if (value == "succeeded") return InitializationState::Succeeded;
    if (value == "failed") return InitializationState::Failed;
    if (value == "aborted") return InitializationState::Aborted;
    throw std::runtime_error("invalid initialization state: " + value);
}

void validate_optional_text(const std::string& value, size_t maximum, const char* field) {
    if (value.size() > maximum) {
        throw std::invalid_argument(
            std::string(field) + " must contain at most " + std::to_string(maximum) + " bytes");
    }
    for (const unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f) {
            throw std::invalid_argument(std::string(field) + " must not contain control characters");
        }
    }
}

void fsync_path(const fs::path& path, bool directory) {
    const int flags = directory ? (O_RDONLY | O_DIRECTORY) : O_RDONLY;
    const int descriptor = open(path.c_str(), flags);
    if (descriptor < 0) {
        throw std::runtime_error("cannot open initialization path for fsync: " + path.string());
    }
    const int result = fsync(descriptor);
    close(descriptor);
    if (result != 0) {
        throw std::runtime_error("cannot fsync initialization path: " + path.string());
    }
}

} // namespace

InitializationManager::InitializationManager(
        CollectionCatalog& catalog,
        CollectionDataStore& data_store,
        GenerationManager& generation_manager)
    : catalog_(&catalog), data_store_(&data_store), generation_manager_(&generation_manager) {}

InitializationSession InitializationManager::create(
        const std::string& collection,
        uint64_t expected_count,
        const std::string& fingerprint,
        const std::string& idempotency_key) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    if (expected_count == 0 || expected_count > 9007199254740991ULL) {
        throw std::invalid_argument("expected_count must be a positive JSON-safe integer");
    }
    validate_optional_text(fingerprint, 1024, "fingerprint");
    validate_optional_text(idempotency_key, 512, "idempotency_key");

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InitializationSession> existing = list_locked(valid_collection);
    uint64_t next_id = 1;
    for (InitializationSession& session : existing) {
        reconcile_pending_batch_locked(session);
        reconcile_generation_locked(session);
        const std::string prefix = "initialization_";
        if (session.initialization_id.rfind(prefix, 0) == 0) {
            try {
                const uint64_t persisted_id = static_cast<uint64_t>(
                    std::stoull(session.initialization_id.substr(prefix.size())));
                if (persisted_id == std::numeric_limits<uint64_t>::max()) {
                    throw std::overflow_error("initialization identifier overflow");
                }
                next_id = std::max(next_id, persisted_id + 1);
            } catch (...) {
                throw std::runtime_error("invalid persisted initialization identifier");
            }
        }
        if (blocks_normal_operations(session.state)) {
            if (session.expected_count == expected_count && session.fingerprint == fingerprint &&
                session.idempotency_key == idempotency_key) {
                return session;
            }
            throw InitializationConflict(
                "another initialization session is already active for collection: " + valid_collection);
        }
    }

    const CollectionManifest manifest = catalog_->get_collection(valid_collection);
    if (manifest.vector_count != 0 || !manifest.active_generation.empty() ||
        !manifest.pending_generation.empty() || manifest.state != CollectionState::Empty) {
        throw InitializationConflict(
            "bulk initialization requires an empty collection with no generation");
    }

    InitializationSession session;
    session.initialization_id = "initialization_" + std::to_string(next_id);
    session.collection = valid_collection;
    session.expected_count = expected_count;
    session.fingerprint = fingerprint;
    session.idempotency_key = idempotency_key;
    session.created_at_unix_ms = now_unix_ms();
    session.updated_at_unix_ms = session.created_at_unix_ms;
    write_locked(session);
    return session;
}

InitializationSession InitializationManager::get(
        const std::string& collection, const std::string& initialization_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    InitializationSession session = load_locked(collection, initialization_id);
    reconcile_pending_batch_locked(session);
    reconcile_generation_locked(session);
    return session;
}

InitializationBatchResult InitializationManager::append_batch(
        const std::string& collection,
        const std::string& initialization_id,
        uint64_t batch_index,
        const std::string& checksum,
        const std::vector<VectorRecord>& records) {
    const std::string valid_checksum = validate_checksum(checksum);
    if (records.empty()) throw std::invalid_argument("initialization batch must not be empty");
    std::lock_guard<std::mutex> lock(mutex_);
    InitializationSession session = load_locked(collection, initialization_id);
    reconcile_pending_batch_locked(session);
    reconcile_generation_locked(session);
    if (session.state != InitializationState::Uploading) {
        throw InitializationConflict("initialization is not accepting batches");
    }

    if (batch_index < session.next_batch_index) {
        const auto receipt = std::find_if(
            session.batches.begin(), session.batches.end(), [batch_index](const auto& item) {
                return item.batch_index == batch_index;
            });
        if (receipt == session.batches.end() || receipt->checksum != valid_checksum ||
            receipt->record_count != records.size()) {
            throw InitializationConflict("batch index was already committed with different content");
        }
        return {session, true};
    }
    if (batch_index != session.next_batch_index) {
        throw InitializationConflict(
            "initialization batches must be committed in ascending contiguous order");
    }
    if (session.received_count + records.size() > session.expected_count) {
        throw InitializationConflict("initialization batch exceeds expected_count");
    }

    if (session.pending_batch.present) {
        if (session.pending_batch.batch_index != batch_index ||
            session.pending_batch.checksum != valid_checksum ||
            session.pending_batch.record_count != records.size()) {
            throw InitializationConflict(
                "a prepared batch must be retried with the same index, count and checksum");
        }
    } else {
        const VectorLogStatus status = data_store_->log_status(session.collection);
        if (status.vector_count != session.received_count ||
            status.last_sequence != session.last_log_sequence) {
            throw std::runtime_error("initialization WAL boundary differs from its manifest");
        }
        session.pending_batch.present = true;
        session.pending_batch.batch_index = batch_index;
        session.pending_batch.record_count = records.size();
        session.pending_batch.first_sequence = status.last_sequence + 1;
        session.pending_batch.last_sequence = status.last_sequence + records.size();
        session.pending_batch.checksum = valid_checksum;
        session.updated_at_unix_ms = now_unix_ms();
        write_locked(session);
    }

    try {
        const UpsertResult result = data_store_->insert_unique(session.collection, records);
        if (result.inserted != records.size() || result.updated != 0) {
            throw std::runtime_error("unique initialization insert returned an invalid count");
        }
        reconcile_pending_batch_locked(session);
        if (session.pending_batch.present) {
            throw std::runtime_error("committed initialization batch was not recoverable");
        }
    } catch (...) {
        try {
            reconcile_pending_batch_locked(session);
            if (session.pending_batch.present) {
                const VectorLogStatus status = data_store_->log_status(session.collection);
                if (status.vector_count == session.received_count &&
                    status.last_sequence == session.last_log_sequence) {
                    session.pending_batch = PendingInitializationBatch{};
                    session.updated_at_unix_ms = now_unix_ms();
                    write_locked(session);
                }
            }
        } catch (...) {
        }
        throw;
    }
    return {session, false};
}

InitializationSession InitializationManager::finalize(
        const std::string& collection, const std::string& initialization_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    InitializationSession session = load_locked(collection, initialization_id);
    reconcile_pending_batch_locked(session);
    reconcile_generation_locked(session);
    if (session.state == InitializationState::Succeeded ||
        session.state == InitializationState::Building) {
        return session;
    }
    if (session.state != InitializationState::Uploading &&
        session.state != InitializationState::Failed) {
        throw InitializationConflict("initialization cannot be finalized in its current state");
    }
    if (session.pending_batch.present) {
        throw InitializationConflict("the prepared initialization batch must be retried first");
    }
    const VectorLogStatus status = data_store_->log_status(session.collection);
    if (session.received_count != session.expected_count ||
        status.vector_count != session.expected_count ||
        status.last_sequence != session.last_log_sequence) {
        throw InitializationConflict(
            "initialization cannot finalize until exactly expected_count vectors are committed");
    }

    const GenerationJob job = generation_manager_->start_generation(session.collection, false);
    session.state = InitializationState::Building;
    session.generation_job_id = job.job_id;
    session.error.clear();
    session.updated_at_unix_ms = now_unix_ms();
    write_locked(session);
    return session;
}

InitializationSession InitializationManager::abort(
        const std::string& collection, const std::string& initialization_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    InitializationSession session = load_locked(collection, initialization_id);
    reconcile_pending_batch_locked(session);
    reconcile_generation_locked(session);
    if (session.state == InitializationState::Aborted) return session;
    if (session.state != InitializationState::Uploading) {
        throw InitializationConflict(
            "only an uploading initialization can be aborted; delete the collection after build failure");
    }
    data_store_->reset_uninitialized_collection(session.collection);
    session.state = InitializationState::Aborted;
    session.received_count = 0;
    session.last_log_sequence = 0;
    session.pending_batch = PendingInitializationBatch{};
    session.error.clear();
    session.updated_at_unix_ms = now_unix_ms();
    write_locked(session);
    return session;
}

void InitializationManager::require_no_active_initialization(
        const std::string& collection, const char* operation) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    std::lock_guard<std::mutex> lock(mutex_);
    for (InitializationSession& session : list_locked(valid_collection)) {
        reconcile_pending_batch_locked(session);
        reconcile_generation_locked(session);
        if (blocks_normal_operations(session.state)) {
            throw InitializationConflict(
                std::string(operation) + " is unavailable while " + session.initialization_id +
                " is " + state_name(session.state));
        }
    }
}

const char* InitializationManager::state_name(InitializationState state) {
    switch (state) {
        case InitializationState::Uploading: return "uploading";
        case InitializationState::Building: return "building";
        case InitializationState::Succeeded: return "succeeded";
        case InitializationState::Failed: return "failed";
        case InitializationState::Aborted: return "aborted";
    }
    throw std::logic_error("unknown initialization state");
}

std::string InitializationManager::validate_initialization_id(
        const std::string& initialization_id) {
    constexpr const char* prefix = "initialization_";
    if (initialization_id.rfind(prefix, 0) != 0 ||
        initialization_id.size() <= std::char_traits<char>::length(prefix) ||
        initialization_id.size() > 64) {
        throw std::invalid_argument("invalid initialization id");
    }
    for (size_t i = std::char_traits<char>::length(prefix); i < initialization_id.size(); ++i) {
        if (initialization_id[i] < '0' || initialization_id[i] > '9') {
            throw std::invalid_argument("invalid initialization id");
        }
    }
    return initialization_id;
}

std::string InitializationManager::validate_checksum(const std::string& checksum) {
    if (checksum.size() != 64) {
        throw std::invalid_argument("batch checksum must be a 64-character SHA-256 hex digest");
    }
    std::string normalized;
    normalized.reserve(checksum.size());
    for (const unsigned char ch : checksum) {
        if (!std::isxdigit(ch)) {
            throw std::invalid_argument("batch checksum must contain hexadecimal characters");
        }
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

fs::path InitializationManager::initialization_directory(const std::string& collection) const {
    return catalog_->collection_directory(collection) / "initializations";
}

fs::path InitializationManager::session_path(
        const std::string& collection, const std::string& initialization_id) const {
    return initialization_directory(CollectionCatalog::validate_collection_name(collection)) /
        (validate_initialization_id(initialization_id) + ".json");
}

InitializationSession InitializationManager::load_locked(
        const std::string& collection, const std::string& initialization_id) const {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    const std::string valid_id = validate_initialization_id(initialization_id);
    const fs::path path = session_path(valid_collection, valid_id);
    if (!fs::is_regular_file(path)) {
        throw std::out_of_range("initialization session does not exist: " + valid_id);
    }
    const simple_json::Value root = simple_json::parse_file(path.string());
    if (!root.is_object()) throw std::runtime_error("initialization manifest must be an object");
    InitializationSession session;
    session.schema_version = static_cast<uint32_t>(required_uint64(root, "schema_version"));
    if (session.schema_version != 1) throw std::runtime_error("unsupported initialization schema");
    session.initialization_id = validate_initialization_id(required_string(root, "initialization_id"));
    session.collection = CollectionCatalog::validate_collection_name(
        required_string(root, "collection"));
    if (session.initialization_id != valid_id || session.collection != valid_collection) {
        throw std::runtime_error("initialization identity does not match its path");
    }
    session.state = parse_state(required_string(root, "state"));
    session.expected_count = required_uint64(root, "expected_count");
    session.received_count = required_uint64(root, "received_count");
    session.next_batch_index = required_uint64(root, "next_batch_index");
    session.last_log_sequence = required_uint64(root, "last_log_sequence");
    session.fingerprint = required_string(root, "fingerprint");
    session.idempotency_key = required_string(root, "idempotency_key");
    session.generation_job_id = required_string(root, "generation_job_id");
    session.error = required_string(root, "error");
    session.created_at_unix_ms = required_uint64(root, "created_at_unix_ms");
    session.updated_at_unix_ms = required_uint64(root, "updated_at_unix_ms");

    const simple_json::Value* batches = root.get("batches");
    if (batches == nullptr || !batches->is_array()) {
        throw std::runtime_error("invalid initialization field: batches");
    }
    for (const simple_json::Value& item : batches->array_value) {
        if (!item.is_object()) throw std::runtime_error("initialization batch receipt must be an object");
        session.batches.push_back({
            required_uint64(item, "batch_index"),
            required_uint64(item, "record_count"),
            validate_checksum(required_string(item, "checksum")),
        });
    }
    const simple_json::Value* pending = root.get("pending_batch");
    if (pending == nullptr) throw std::runtime_error("invalid initialization field: pending_batch");
    if (!pending->is_null()) {
        if (!pending->is_object()) throw std::runtime_error("pending_batch must be null or an object");
        session.pending_batch.present = true;
        session.pending_batch.batch_index = required_uint64(*pending, "batch_index");
        session.pending_batch.record_count = required_uint64(*pending, "record_count");
        session.pending_batch.first_sequence = required_uint64(*pending, "first_sequence");
        session.pending_batch.last_sequence = required_uint64(*pending, "last_sequence");
        session.pending_batch.checksum = validate_checksum(required_string(*pending, "checksum"));
    }
    if (session.expected_count == 0 || session.received_count > session.expected_count ||
        session.batches.size() != session.next_batch_index) {
        throw std::runtime_error("initialization manifest counters are inconsistent");
    }
    return session;
}

void InitializationManager::write_locked(const InitializationSession& session) const {
    const fs::path directory = initialization_directory(session.collection);
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) throw std::runtime_error("cannot create initialization directory: " + error.message());
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, error);
    if (error) throw std::runtime_error("cannot restrict initialization directory permissions");

    const fs::path path = session_path(session.collection, session.initialization_id);
    const fs::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create initialization manifest");
    output << "{\n"
           << "  \"schema_version\": " << session.schema_version << ",\n"
           << "  \"initialization_id\": \"" << json_escape(session.initialization_id) << "\",\n"
           << "  \"collection\": \"" << json_escape(session.collection) << "\",\n"
           << "  \"state\": \"" << state_name(session.state) << "\",\n"
           << "  \"expected_count\": " << session.expected_count << ",\n"
           << "  \"received_count\": " << session.received_count << ",\n"
           << "  \"next_batch_index\": " << session.next_batch_index << ",\n"
           << "  \"last_log_sequence\": " << session.last_log_sequence << ",\n"
           << "  \"fingerprint\": \"" << json_escape(session.fingerprint) << "\",\n"
           << "  \"idempotency_key\": \"" << json_escape(session.idempotency_key) << "\",\n"
           << "  \"generation_job_id\": \"" << json_escape(session.generation_job_id) << "\",\n"
           << "  \"error\": \"" << json_escape(session.error) << "\",\n"
           << "  \"created_at_unix_ms\": " << session.created_at_unix_ms << ",\n"
           << "  \"updated_at_unix_ms\": " << session.updated_at_unix_ms << ",\n"
           << "  \"batches\": [";
    for (size_t i = 0; i < session.batches.size(); ++i) {
        if (i != 0) output << ',';
        const InitializationBatchReceipt& batch = session.batches[i];
        output << "{\"batch_index\":" << batch.batch_index
               << ",\"record_count\":" << batch.record_count
               << ",\"checksum\":\"" << batch.checksum << "\"}";
    }
    output << "],\n  \"pending_batch\": ";
    if (!session.pending_batch.present) {
        output << "null\n";
    } else {
        output << "{\"batch_index\":" << session.pending_batch.batch_index
               << ",\"record_count\":" << session.pending_batch.record_count
               << ",\"first_sequence\":" << session.pending_batch.first_sequence
               << ",\"last_sequence\":" << session.pending_batch.last_sequence
               << ",\"checksum\":\"" << session.pending_batch.checksum << "\"}\n";
    }
    output << "}\n";
    output.close();
    if (!output) throw std::runtime_error("cannot write initialization manifest");
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, error);
    if (error) throw std::runtime_error("cannot restrict initialization manifest permissions");
    fsync_path(temporary, false);
    fs::rename(temporary, path, error);
    if (error) throw std::runtime_error("cannot publish initialization manifest: " + error.message());
    fsync_path(directory, true);
}

std::vector<InitializationSession> InitializationManager::list_locked(
        const std::string& collection) const {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    catalog_->get_collection(valid_collection);
    std::vector<InitializationSession> sessions;
    const fs::path directory = initialization_directory(valid_collection);
    std::error_code error;
    if (!fs::exists(directory)) return sessions;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        if (error) throw std::runtime_error("cannot enumerate initialization sessions: " + error.message());
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        sessions.push_back(load_locked(valid_collection, entry.path().stem().string()));
    }
    std::sort(sessions.begin(), sessions.end(), [](const auto& left, const auto& right) {
        return left.created_at_unix_ms < right.created_at_unix_ms;
    });
    return sessions;
}

void InitializationManager::reconcile_pending_batch_locked(InitializationSession& session) {
    if (!session.pending_batch.present) return;
    const VectorLogStatus status = data_store_->log_status(session.collection);
    const uint64_t expected_count = session.received_count + session.pending_batch.record_count;
    if (status.last_sequence == session.pending_batch.last_sequence &&
        status.vector_count == expected_count) {
        session.batches.push_back({
            session.pending_batch.batch_index,
            session.pending_batch.record_count,
            session.pending_batch.checksum,
        });
        session.received_count = expected_count;
        session.next_batch_index = session.pending_batch.batch_index + 1;
        session.last_log_sequence = session.pending_batch.last_sequence;
        session.pending_batch = PendingInitializationBatch{};
        session.updated_at_unix_ms = now_unix_ms();
        write_locked(session);
        return;
    }
    if (status.last_sequence == session.last_log_sequence &&
        status.vector_count == session.received_count) {
        return;
    }
    throw std::runtime_error("cannot reconcile prepared initialization batch with vectors.log");
}

void InitializationManager::reconcile_generation_locked(InitializationSession& session) {
    if ((session.state == InitializationState::Uploading ||
         session.state == InitializationState::Failed) &&
        session.generation_job_id.empty() && session.received_count == session.expected_count) {
        const CollectionManifest manifest = catalog_->get_collection(session.collection);
        if (!manifest.pending_generation.empty()) {
            session.generation_job_id = "job_" + manifest.pending_generation;
            session.state = InitializationState::Building;
            session.error.clear();
            session.updated_at_unix_ms = now_unix_ms();
            write_locked(session);
        }
    }
    if (session.state != InitializationState::Building) return;
    if (session.generation_job_id.empty()) {
        session.state = InitializationState::Failed;
        session.error = "initialization build has no generation job";
        session.updated_at_unix_ms = now_unix_ms();
        write_locked(session);
        return;
    }
    GenerationJob job;
    try {
        job = generation_manager_->get_job(session.collection, session.generation_job_id);
    } catch (const std::out_of_range&) {
        const CollectionManifest manifest = catalog_->get_collection(session.collection);
        if (!manifest.pending_generation.empty()) return;
        session.state = InitializationState::Failed;
        session.error = "generation job disappeared before initialization completed";
        session.updated_at_unix_ms = now_unix_ms();
        write_locked(session);
        return;
    }
    if (job.state == GenerationJobState::Succeeded) {
        const CollectionManifest manifest = catalog_->get_collection(session.collection);
        if (manifest.active_generation != job.generation ||
            manifest.vector_count != session.expected_count ||
            job.vector_count != session.expected_count) {
            session.state = InitializationState::Failed;
            session.error = "generation succeeded without activating the complete initialization";
        } else {
            session.state = InitializationState::Succeeded;
            session.error.clear();
        }
        session.updated_at_unix_ms = now_unix_ms();
        write_locked(session);
    } else if (job.state == GenerationJobState::Failed) {
        session.state = InitializationState::Failed;
        session.error = job.error;
        session.updated_at_unix_ms = now_unix_ms();
        write_locked(session);
    }
}

bool InitializationManager::blocks_normal_operations(InitializationState state) {
    return state == InitializationState::Uploading ||
           state == InitializationState::Building ||
           state == InitializationState::Failed;
}

} // namespace qcovert
