#include "qcovert/collection_catalog.h"
#include "qcovert/collection_data_store.h"
#include "qcovert/generation_manager.h"
#include "qcovert/initialization_manager.h"
#include "qcovert/management_service.h"
#include "qcovert/search_service.h"
#include "qcovert/shutdown_signal.h"

#include "simple_json.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kHeaderLimitBytes = 32 * 1024;

struct QueryOptions {
    int port = 8080;
    std::filesystem::path root = "qcovert-data";
    std::string api_key;
    bool allow_unauthenticated = false;
    size_t max_request_bytes = 4 * 1024 * 1024;
    size_t worker_threads = std::max(4U, std::thread::hardware_concurrency());
    size_t max_pending_connections = 0;
    int socket_timeout_ms = 15000;
    qcovert::GenerationBuildConfig generation_build;

    QueryOptions() {
        generation_build.oram.enabled = true;
    }
};

struct HttpRequest {
    std::string method;
    std::string target;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string body;
};

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    throw std::invalid_argument("response string contains a control character");
                }
                out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

HttpResponse json_error(int status, const std::string& code, const std::string& message) {
    return {status, "{\"error\":{\"code\":\"" + json_escape(code) +
                    "\",\"message\":\"" + json_escape(message) + "\"}}"};
}

uint32_t require_dimension(const simple_json::Value& body) {
    const simple_json::Value* value = body.get("dimension");
    if (value == nullptr || !value->is_number() || value->number_value < 1.0 ||
        std::floor(value->number_value) != value->number_value) {
        throw std::invalid_argument("body.dimension must be a positive integer");
    }
    return static_cast<uint32_t>(value->number_value);
}

std::string optional_string(const simple_json::Value& body, const char* field) {
    const simple_json::Value* value = body.get(field);
    if (value == nullptr) {
        return "";
    }
    if (!value->is_string()) {
        throw std::invalid_argument(std::string("body.") + field + " must be a string");
    }
    return value->string_value;
}

std::string manifest_json(const qcovert::CollectionManifest& manifest) {
    std::ostringstream output;
    output << "{\"name\":\"" << json_escape(manifest.name)
           << "\",\"dimension\":" << manifest.dimension
           << ",\"metric\":\"" << json_escape(manifest.metric)
           << "\",\"embedding_model\":\"" << json_escape(manifest.embedding_model)
           << "\",\"vector_count\":" << manifest.vector_count
           << ",\"state\":\"" << qcovert::CollectionCatalog::collection_state_name(manifest.state)
           << "\",\"active_generation\":\"" << json_escape(manifest.active_generation)
           << "\",\"pending_generation\":\"" << json_escape(manifest.pending_generation)
           << "\",\"pending_generation_stage\":\""
           << qcovert::CollectionCatalog::generation_stage_name(manifest.pending_generation_stage)
           << "\",\"next_generation\":" << manifest.next_generation
           << "}";
    return output.str();
}

std::string oram_tree_stats_json(const qcovert::ManagedOramTreeStats& stats) {
    return "{\"real_blocks\":" + std::to_string(stats.real_blocks) +
        ",\"logical_block_limit\":" + std::to_string(stats.logical_block_limit) +
        ",\"real_block_capacity\":" + std::to_string(stats.real_block_capacity) +
        ",\"num_buckets\":" + std::to_string(stats.num_buckets) +
        ",\"block_size\":" + std::to_string(stats.block_size) +
        ",\"num_levels\":" + std::to_string(stats.num_levels) +
        ",\"cached_levels\":" + std::to_string(stats.cached_levels) + "}";
}

std::string global_stats_json(const qcovert::GlobalManagementStats& stats) {
    return "{\"collection_count\":" + std::to_string(stats.collection_count) +
        ",\"vector_count\":" + std::to_string(stats.vector_count) +
        ",\"active_collections\":" + std::to_string(stats.active_collections) +
        ",\"building_collections\":" + std::to_string(stats.building_collections) +
        ",\"prepared_collections\":" + std::to_string(stats.prepared_collections) +
        ",\"failed_collections\":" + std::to_string(stats.failed_collections) +
        ",\"empty_collections\":" + std::to_string(stats.empty_collections) +
        ",\"trusted_storage_bytes\":" + std::to_string(stats.trusted_storage_bytes) + "}";
}

std::string collection_stats_json(const qcovert::CollectionManagementStats& stats) {
    std::ostringstream output;
    output << "{\"collection\":" << manifest_json(stats.manifest)
           << ",\"generation_ready\":" << (stats.generation_ready ? "true" : "false")
           << ",\"runtime_cached\":" << (stats.runtime_cached ? "true" : "false")
           << ",\"trusted_storage_bytes\":" << stats.trusted_storage_bytes
           << ",\"backup_count\":" << stats.backup_count;
    if (stats.generation_ready) {
        output << ",\"active_generation_stats\":{\"cluster_count\":" << stats.cluster_count
               << ",\"generation_vector_capacity\":" << stats.generation_vector_capacity
               << ",\"generation_vector_count\":" << stats.generation_vector_count
               << ",\"last_log_sequence\":" << stats.last_log_sequence
               << ",\"replayed_records\":" << stats.replayed_records
               << ",\"cluster_stash_blocks\":" << stats.cluster_stash_blocks
               << ",\"ring_stash_blocks\":" << stats.ring_stash_blocks
               << ",\"cluster_oram\":" << oram_tree_stats_json(stats.cluster_oram)
               << ",\"ring_oram\":" << oram_tree_stats_json(stats.ring_oram) << "}";
    } else {
        output << ",\"active_generation_stats\":null";
    }
    output << '}';
    return output.str();
}

std::string backups_json(const std::vector<std::string>& backups) {
    std::string output = "{\"backups\":[";
    for (size_t i = 0; i < backups.size(); ++i) {
        if (i != 0) output += ',';
        output += "\"" + json_escape(backups[i]) + "\"";
    }
    return output + "]}";
}

std::string json_value(const simple_json::Value& value) {
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
                output += json_value(value.array_value[i]);
            }
            return output + ']';
        }
        case simple_json::Value::Type::Object: {
            std::string output = "{";
            bool first = true;
            for (const auto& item : value.object_value) {
                if (!first) output += ',';
                first = false;
                output += "\"" + json_escape(item.first) + "\":" + json_value(item.second);
            }
            return output + '}';
        }
    }
    throw std::logic_error("unknown JSON value type");
}

std::vector<qcovert::VectorRecord> parse_vector_records(const simple_json::Value& body) {
    const simple_json::Value* input = body.get("records");
    if (input == nullptr || !input->is_array() || input->array_value.empty()) {
        throw std::invalid_argument("body.records must be a non-empty array");
    }
    std::vector<qcovert::VectorRecord> records;
    records.reserve(input->array_value.size());
    for (const simple_json::Value& item : input->array_value) {
        if (!item.is_object()) {
            throw std::invalid_argument("each records entry must be an object");
        }
        const simple_json::Value* id = item.get("id");
        const simple_json::Value* vector = item.get("vector");
        if (id == nullptr || !id->is_string() || vector == nullptr || !vector->is_array() ||
            vector->array_value.size() != qcovert::kSupportedEmbeddingDimension) {
            throw std::invalid_argument("each record requires id and a 384-value vector");
        }
        qcovert::VectorRecord record;
        record.id = qcovert::CollectionDataStore::validate_vector_id(id->string_value);
        record.values.reserve(vector->array_value.size());
        for (const simple_json::Value& value : vector->array_value) {
            if (!value.is_number() || !std::isfinite(value.number_value) ||
                value.number_value < -std::numeric_limits<float>::max() ||
                value.number_value > std::numeric_limits<float>::max()) {
                throw std::invalid_argument("vector values must be finite float32 numbers");
            }
            record.values.push_back(static_cast<float>(value.number_value));
        }
        const simple_json::Value* metadata = item.get("metadata");
        record.metadata_json = metadata == nullptr ? "null" : json_value(*metadata);
        records.push_back(std::move(record));
    }
    return records;
}

uint32_t optional_positive_integer(
        const simple_json::Value& body,
        const char* field,
        uint32_t default_value) {
    const simple_json::Value* value = body.get(field);
    if (value == nullptr) return default_value;
    if (!value->is_number() || !std::isfinite(value->number_value) ||
        value->number_value < 1.0 || std::floor(value->number_value) != value->number_value ||
        value->number_value > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        throw std::invalid_argument(std::string("body.") + field + " must be a positive integer");
    }
    return static_cast<uint32_t>(value->number_value);
}

qcovert::SearchRequest parse_search_request(const simple_json::Value& body) {
    if (!body.is_object()) {
        throw std::invalid_argument("request body must be a JSON object");
    }
    const simple_json::Value* vector = body.get("vector");
    if (vector == nullptr || !vector->is_array() ||
        vector->array_value.size() != qcovert::kSupportedEmbeddingDimension) {
        throw std::invalid_argument("body.vector must contain exactly 384 values");
    }

    qcovert::SearchRequest request;
    request.vector.reserve(vector->array_value.size());
    for (const simple_json::Value& value : vector->array_value) {
        if (!value.is_number() || !std::isfinite(value.number_value) ||
            value.number_value < -std::numeric_limits<float>::max() ||
            value.number_value > std::numeric_limits<float>::max()) {
            throw std::invalid_argument("body.vector values must be finite float32 numbers");
        }
        request.vector.push_back(static_cast<float>(value.number_value));
    }
    request.top_k = optional_positive_integer(body, "top_k", request.top_k);
    if (body.get("nprobe") != nullptr || body.get("path_batch_clusters") != nullptr) {
        throw std::invalid_argument(
            "nprobe and path_batch_clusters are computed by the service and cannot be supplied");
    }

    const simple_json::Value* factor = body.get("candidate_factor");
    if (factor != nullptr) {
        if (!factor->is_number() || !std::isfinite(factor->number_value) ||
            factor->number_value <= 0.0) {
            throw std::invalid_argument("body.candidate_factor must be a positive finite number");
        }
        request.candidate_factor = factor->number_value;
    }
    return request;
}

std::string search_response_json(const qcovert::SearchResponse& response) {
    std::ostringstream output;
    output.precision(std::numeric_limits<double>::max_digits10);
    output << "{\"generation\":\"" << json_escape(response.generation)
           << "\",\"nprobe\":" << response.nprobe
           << ",\"exact_candidate_limit\":" << response.exact_candidate_limit
           << ",\"search_ms\":" << response.search_ms
           << ",\"results\":[";
    for (size_t i = 0; i < response.hits.size(); ++i) {
        if (i != 0) output << ',';
        const qcovert::SearchHit& hit = response.hits[i];
        output << "{\"id\":\"" << json_escape(hit.id)
               << "\",\"distance\":" << hit.distance
               << ",\"score\":" << hit.score
               << ",\"metadata\":" << hit.metadata_json << '}';
    }
    output << "]}";
    return output.str();
}

std::string generation_job_json(const qcovert::GenerationJob& job) {
    return "{\"job_id\":\"" + json_escape(job.job_id) +
           "\",\"collection\":\"" + json_escape(job.collection) +
           "\",\"generation\":\"" + json_escape(job.generation) +
           "\",\"state\":\"" + qcovert::GenerationManager::job_state_name(job.state) +
           "\",\"vector_count\":" + std::to_string(job.vector_count) +
           ",\"error\":\"" + json_escape(job.error) + "\"}";
}

uint64_t required_json_safe_uint64(const simple_json::Value& body, const char* field) {
    const simple_json::Value* value = body.get(field);
    if (value == nullptr || !value->is_number() || !std::isfinite(value->number_value) ||
        value->number_value < 0.0 || std::floor(value->number_value) != value->number_value ||
        value->number_value > 9007199254740991.0) {
        throw std::invalid_argument(
            std::string("body.") + field + " must be a non-negative JSON-safe integer");
    }
    return static_cast<uint64_t>(value->number_value);
}

std::string initialization_json(const qcovert::InitializationSession& session) {
    std::ostringstream output;
    output << "{\"initialization_id\":\"" << json_escape(session.initialization_id)
           << "\",\"collection\":\"" << json_escape(session.collection)
           << "\",\"state\":\"" << qcovert::InitializationManager::state_name(session.state)
           << "\",\"expected_count\":" << session.expected_count
           << ",\"received_count\":" << session.received_count
           << ",\"next_batch_index\":" << session.next_batch_index
           << ",\"last_log_sequence\":" << session.last_log_sequence
           << ",\"fingerprint\":\"" << json_escape(session.fingerprint)
           << "\",\"generation_job_id\":\"" << json_escape(session.generation_job_id)
           << "\",\"error\":\"" << json_escape(session.error)
           << "\",\"created_at_unix_ms\":" << session.created_at_unix_ms
           << ",\"updated_at_unix_ms\":" << session.updated_at_unix_ms << '}';
    return output.str();
}

std::string sha256_hex(const std::string& value) {
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_size = 0;
    if (EVP_Digest(value.data(), value.size(), digest, &digest_size, EVP_sha256(), nullptr) != 1 ||
        digest_size != 32) {
        throw std::runtime_error("cannot calculate request SHA-256");
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_size; ++i) {
        output << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return output.str();
}

QueryOptions parse_args(int argc, char** argv) {
    QueryOptions options;
    if (const char* environment_key = std::getenv("QCOVERT_API_KEY")) {
        options.api_key = environment_key;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        auto require_value = [&](const char* name) {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string("missing value for ") + name);
            }
            return std::string(argv[++i]);
        };
        if (arg == "--port") {
            options.port = std::stoi(require_value("--port"));
        } else if (arg == "--root") {
            options.root = require_value("--root");
        } else if (arg == "--api-key") {
            options.api_key = require_value("--api-key");
        } else if (arg == "--allow-unauthenticated") {
            options.allow_unauthenticated = true;
        } else if (arg == "--max-request-bytes") {
            options.max_request_bytes = static_cast<size_t>(std::stoull(require_value("--max-request-bytes")));
        } else if (arg == "--worker-threads") {
            options.worker_threads = static_cast<size_t>(std::stoull(require_value("--worker-threads")));
        } else if (arg == "--max-pending-connections") {
            options.max_pending_connections = static_cast<size_t>(
                std::stoull(require_value("--max-pending-connections")));
        } else if (arg == "--socket-timeout-ms") {
            options.socket_timeout_ms = std::stoi(require_value("--socket-timeout-ms"));
        } else if (arg == "--python") {
            options.generation_build.python_executable = require_value("--python");
        } else if (arg == "--faiss-worker") {
            options.generation_build.faiss_worker = require_value("--faiss-worker");
        } else if (arg == "--faiss-threads") {
            options.generation_build.faiss_threads =
                static_cast<uint32_t>(std::stoul(require_value("--faiss-threads")));
        } else if (arg == "--kmeans-niter") {
            options.generation_build.kmeans_iterations =
                static_cast<uint32_t>(std::stoul(require_value("--kmeans-niter")));
        } else if (arg == "--kmeans-nredo") {
            options.generation_build.kmeans_restarts =
                static_cast<uint32_t>(std::stoul(require_value("--kmeans-nredo")));
        } else if (arg == "--storage-host") {
            options.generation_build.oram.storage_host = require_value("--storage-host");
        } else if (arg == "--storage-port") {
            options.generation_build.oram.storage_port = std::stoi(require_value("--storage-port"));
        } else if (arg == "--oram-z") {
            options.generation_build.oram.real_bucket_size = std::stoi(require_value("--oram-z"));
        } else if (arg == "--oram-s") {
            options.generation_build.oram.dummy_size = std::stoi(require_value("--oram-s"));
        } else if (arg == "--oram-a") {
            options.generation_build.oram.evict_rate = std::stoi(require_value("--oram-a"));
        } else if (arg == "--oram-upload-batch-mib") {
            options.generation_build.oram.upload_batch_bytes =
                static_cast<size_t>(std::stoull(require_value("--oram-upload-batch-mib"))) *
                1024U * 1024U;
        } else if (arg == "--help") {
            std::cout << "Usage: qcovert_query [--port PORT] [--root TRUSTED_ROOT]"
                      << " [--api-key KEY|QCOVERT_API_KEY] [--allow-unauthenticated]"
                      << " [--max-request-bytes BYTES] [--worker-threads N]"
                      << " [--max-pending-connections N] [--socket-timeout-ms MS]"
                      << " [--python PATH] [--faiss-worker PATH] [--faiss-threads N]"
                      << " [--kmeans-niter N] [--kmeans-nredo N]"
                      << " [--storage-host HOST] [--storage-port PORT]"
                      << " [--oram-z N] [--oram-s N] [--oram-a N]"
                      << " [--oram-upload-batch-mib N]\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    if (options.port <= 0 || options.port > 65535 || options.max_request_bytes == 0 ||
        options.worker_threads == 0 || options.socket_timeout_ms <= 0 ||
        options.generation_build.kmeans_iterations == 0 ||
        options.generation_build.kmeans_restarts == 0 ||
        options.generation_build.oram.storage_host.empty() ||
        options.generation_build.oram.storage_port <= 0 ||
        options.generation_build.oram.storage_port > 65535 ||
        options.generation_build.oram.real_bucket_size <= 0 ||
        options.generation_build.oram.dummy_size < 0 ||
        options.generation_build.oram.evict_rate <= 0 ||
        options.generation_build.oram.upload_batch_bytes == 0) {
        throw std::invalid_argument("invalid query-service options");
    }
    if (options.api_key.empty() && !options.allow_unauthenticated) {
        throw std::invalid_argument(
            "an API key is required; set QCOVERT_API_KEY or explicitly use --allow-unauthenticated");
    }
    if (options.max_pending_connections == 0) {
        options.max_pending_connections = options.worker_threads * 16;
    }
    return options;
}

bool receive_all(int socket_fd, void* data, size_t size) {
    char* current = static_cast<char*>(data);
    while (size != 0) {
        const ssize_t read_count = recv(socket_fd, current, size, 0);
        if (read_count <= 0) {
            return false;
        }
        current += read_count;
        size -= static_cast<size_t>(read_count);
    }
    return true;
}

bool send_all(int socket_fd, const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t sent = send(socket_fd, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

HttpRequest receive_request(int socket_fd, size_t max_request_bytes) {
    std::string received;
    received.reserve(4096);
    char buffer[4096];
    size_t header_end = std::string::npos;
    while ((header_end = received.find("\r\n\r\n")) == std::string::npos) {
        const ssize_t count = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            throw std::runtime_error("client disconnected before HTTP headers");
        }
        received.append(buffer, static_cast<size_t>(count));
        if (received.size() > kHeaderLimitBytes) {
            throw std::length_error("HTTP headers exceed the supported limit");
        }
    }

    HttpRequest request;
    const std::string headers_text = received.substr(0, header_end);
    std::istringstream headers_stream(headers_text);
    std::string request_line;
    if (!std::getline(headers_stream, request_line) || request_line.size() < 6) {
        throw std::invalid_argument("invalid HTTP request line");
    }
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }
    std::istringstream line_stream(request_line);
    std::string protocol;
    if (!(line_stream >> request.method >> request.target >> protocol) || protocol != "HTTP/1.1") {
        throw std::invalid_argument("only HTTP/1.1 requests are supported");
    }
    if (request.target.empty() || request.target.front() != '/') {
        throw std::invalid_argument("invalid HTTP request target");
    }

    std::string line;
    while (std::getline(headers_stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t separator = line.find(':');
        if (separator == std::string::npos) {
            throw std::invalid_argument("invalid HTTP header");
        }
        std::string key = lower_ascii(line.substr(0, separator));
        size_t value_start = separator + 1;
        while (value_start < line.size() && std::isspace(static_cast<unsigned char>(line[value_start]))) {
            ++value_start;
        }
        if (!request.headers.emplace(std::move(key), line.substr(value_start)).second) {
            throw std::invalid_argument("duplicate HTTP headers are not supported");
        }
    }

    if (request.headers.find("transfer-encoding") != request.headers.end()) {
        throw std::invalid_argument("Transfer-Encoding is not supported");
    }

    size_t content_length = 0;
    const auto length = request.headers.find("content-length");
    if (length != request.headers.end()) {
        size_t parsed_chars = 0;
        content_length = static_cast<size_t>(std::stoull(length->second, &parsed_chars));
        if (parsed_chars != length->second.size()) {
            throw std::invalid_argument("invalid Content-Length header");
        }
    }
    if (content_length > max_request_bytes) {
        throw std::length_error("request body exceeds configured limit");
    }
    request.body = received.substr(header_end + 4);
    if (request.body.size() > content_length) {
        request.body.resize(content_length);
    }
    if (request.body.size() < content_length) {
        const size_t missing = content_length - request.body.size();
        const size_t old_size = request.body.size();
        request.body.resize(content_length);
        if (!receive_all(socket_fd, request.body.data() + old_size, missing)) {
            throw std::runtime_error("client disconnected while sending HTTP body");
        }
    }
    return request;
}

void send_response(int socket_fd, const HttpResponse& response) {
    std::ostringstream wire;
    wire << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status) << "\r\n"
         << "Content-Type: application/json\r\n"
         << "Content-Length: " << response.body.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << response.body;
    send_all(socket_fd, wire.str());
}

std::string collection_name_from_path(const std::string& target, const std::string& suffix = "") {
    constexpr const char* prefix = "/v1/collections/";
    if (target.rfind(prefix, 0) != 0) {
        return "";
    }
    std::string name = target.substr(std::char_traits<char>::length(prefix));
    if (!suffix.empty()) {
        if (name.size() <= suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return "";
        }
        name.resize(name.size() - suffix.size());
    }
    return name.find('/') == std::string::npos ? name : "";
}

struct InitializationRoute {
    std::string collection;
    std::string initialization_id;
    std::string action;
    uint64_t batch_index = 0;
    bool has_batch_index = false;
    bool valid = false;
};

InitializationRoute parse_initialization_route(const std::string& target) {
    constexpr const char* prefix = "/v1/collections/";
    InitializationRoute route;
    if (target.rfind(prefix, 0) != 0) return route;
    const std::string rest = target.substr(std::char_traits<char>::length(prefix));
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= rest.size()) {
        const size_t slash = rest.find('/', start);
        parts.push_back(rest.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (parts.size() < 2 || parts[0].empty() || parts[1] != "initializations") return route;
    route.collection = parts[0];
    if (parts.size() == 2) {
        route.action = "collection";
        route.valid = true;
        return route;
    }
    if (parts[2].empty()) return InitializationRoute{};
    route.initialization_id = parts[2];
    if (parts.size() == 3) {
        route.action = "session";
        route.valid = true;
        return route;
    }
    if (parts.size() == 4 && parts[3] == "finalize") {
        route.action = "finalize";
        route.valid = true;
        return route;
    }
    if (parts.size() == 5 && parts[3] == "batches" && !parts[4].empty()) {
        size_t consumed = 0;
        try {
            route.batch_index = std::stoull(parts[4], &consumed);
        } catch (...) {
            return InitializationRoute{};
        }
        if (consumed != parts[4].size() || route.batch_index > 9007199254740991ULL) {
            return InitializationRoute{};
        }
        route.action = "batch";
        route.has_batch_index = true;
        route.valid = true;
    }
    return route;
}

bool authorized(const HttpRequest& request, const std::string& api_key) {
    if (api_key.empty()) return true;
    const auto header = request.headers.find("authorization");
    if (header == request.headers.end()) return false;
    const std::string expected = "Bearer " + api_key;
    const std::string& actual = header->second;
    size_t difference = expected.size() ^ actual.size();
    const size_t compared = std::max(expected.size(), actual.size());
    for (size_t i = 0; i < compared; ++i) {
        const unsigned char left = i < expected.size() ? expected[i] : 0;
        const unsigned char right = i < actual.size() ? actual[i] : 0;
        difference |= left ^ right;
    }
    return difference == 0;
}

HttpResponse route_request(
        const HttpRequest& request,
        const std::string& api_key,
        qcovert::CollectionCatalog& catalog,
        qcovert::CollectionDataStore& data_store,
        qcovert::GenerationManager& generation_manager,
        qcovert::InitializationManager& initialization_manager,
        qcovert::SearchService& search_service,
        qcovert::ManagementService& management_service) {
    if (!authorized(request, api_key)) {
        return json_error(401, "unauthorized", "missing or invalid bearer token");
    }
    if (request.method == "GET" && request.target == "/v1/health") {
        return {200, "{\"status\":\"ok\",\"service\":\"qcovert-query\",\"api_version\":1,\"dimension\":384,\"metric\":\"l2\"}"};
    }
    if (request.method == "GET" && request.target == "/v1/stats") {
        return {200, global_stats_json(management_service.global_stats())};
    }

    const std::string collection = collection_name_from_path(request.target);
    const std::string stats_collection = collection_name_from_path(request.target, "/stats");
    const std::string vectors_collection = collection_name_from_path(request.target, "/vectors");
    const std::string generations_collection = collection_name_from_path(request.target, "/generations");
    const std::string search_collection = collection_name_from_path(request.target, "/search");
    const std::string backups_collection = collection_name_from_path(request.target, "/backups");
    const std::string restore_collection = collection_name_from_path(request.target, "/restore");
    const InitializationRoute initialization_route = parse_initialization_route(request.target);
    constexpr const char* jobs_prefix = "/v1/collections/";
    std::string job_collection;
    std::string job_id;
    if (request.target.rfind(jobs_prefix, 0) == 0) {
        const std::string rest = request.target.substr(std::char_traits<char>::length(jobs_prefix));
        const size_t marker = rest.find("/jobs/");
        if (marker != std::string::npos && marker != 0 && marker + 6 < rest.size() &&
            rest.find('/', marker + 6) == std::string::npos) {
            job_collection = rest.substr(0, marker);
            job_id = rest.substr(marker + 6);
        }
    }
    try {
        if (initialization_route.valid && initialization_route.action == "collection" &&
            request.method == "POST") {
            const simple_json::Value body = simple_json::parse_text(request.body);
            if (!body.is_object()) throw std::invalid_argument("request body must be a JSON object");
            const qcovert::InitializationSession session = initialization_manager.create(
                initialization_route.collection,
                required_json_safe_uint64(body, "expected_count"),
                optional_string(body, "fingerprint"),
                optional_string(body, "idempotency_key"));
            return {201, initialization_json(session)};
        }
        if (initialization_route.valid && initialization_route.action == "session" &&
            request.method == "GET") {
            return {200, initialization_json(initialization_manager.get(
                initialization_route.collection, initialization_route.initialization_id))};
        }
        if (initialization_route.valid && initialization_route.action == "session" &&
            request.method == "DELETE") {
            return {200, initialization_json(initialization_manager.abort(
                initialization_route.collection, initialization_route.initialization_id))};
        }
        if (initialization_route.valid && initialization_route.action == "batch" &&
            request.method == "PUT") {
            const auto checksum_header = request.headers.find("x-qcovert-content-sha256");
            if (checksum_header == request.headers.end()) {
                throw std::invalid_argument("X-QCovert-Content-SHA256 header is required");
            }
            const std::string expected_checksum =
                qcovert::InitializationManager::validate_checksum(checksum_header->second);
            if (sha256_hex(request.body) != expected_checksum) {
                throw std::invalid_argument("request body does not match X-QCovert-Content-SHA256");
            }
            const simple_json::Value body = simple_json::parse_text(request.body);
            if (!body.is_object()) throw std::invalid_argument("request body must be a JSON object");
            const qcovert::InitializationBatchResult result = initialization_manager.append_batch(
                initialization_route.collection,
                initialization_route.initialization_id,
                initialization_route.batch_index,
                expected_checksum,
                parse_vector_records(body));
            return {200, "{\"idempotent_replay\":" +
                std::string(result.idempotent_replay ? "true" : "false") +
                ",\"initialization\":" + initialization_json(result.session) + "}"};
        }
        if (initialization_route.valid && initialization_route.action == "finalize" &&
            request.method == "POST") {
            const qcovert::InitializationSession session = initialization_manager.finalize(
                initialization_route.collection, initialization_route.initialization_id);
            return {session.state == qcovert::InitializationState::Succeeded ? 200 : 202,
                    initialization_json(session)};
        }
        if (request.method == "PUT" && !collection.empty()) {
            const simple_json::Value body = simple_json::parse_text(request.body);
            if (!body.is_object()) {
                throw std::invalid_argument("request body must be a JSON object");
            }
            const qcovert::CreateCollectionResult result = catalog.create_collection(
                collection,
                require_dimension(body),
                optional_string(body, "metric").empty() ? "l2" : optional_string(body, "metric"),
                optional_string(body, "embedding_model"));
            return {result.created ? 201 : 200, manifest_json(result.manifest)};
        }
        if (request.method == "GET" && !collection.empty()) {
            return {200, manifest_json(catalog.get_collection(collection))};
        }
        if (request.method == "GET" && !stats_collection.empty()) {
            return {200, collection_stats_json(
                management_service.collection_stats(stats_collection))};
        }
        if (request.method == "POST" && !vectors_collection.empty()) {
            initialization_manager.require_no_active_initialization(
                vectors_collection, "online upsert");
            const simple_json::Value body = simple_json::parse_text(request.body);
            if (!body.is_object()) {
                throw std::invalid_argument("request body must be a JSON object");
            }
            const qcovert::OnlineUpsertResult online = search_service.upsert(
                vectors_collection, parse_vector_records(body));
            const qcovert::UpsertResult& result = online.wal;
            const uint64_t previous_vector_count = result.vector_count - result.inserted;
            const std::optional<qcovert::GenerationJob> automatic_generation =
                generation_manager.maybe_start_generation(
                    vectors_collection,
                    previous_vector_count,
                    result.vector_count,
                    online.active.capacity_exhausted);
            return {200, "{\"inserted\":" + std::to_string(result.inserted) +
                         ",\"updated\":" + std::to_string(result.updated) +
                         ",\"vector_count\":" + std::to_string(result.vector_count) +
                         ",\"active_oram_applied\":" +
                         std::to_string(online.active.applied_records) +
                         ",\"automatic_generation_job\":" +
                         (automatic_generation.has_value()
                              ? "\"" + json_escape(automatic_generation->job_id) + "\""
                              : "null") + "}"};
        }
        if (request.method == "POST" && !search_collection.empty()) {
            initialization_manager.require_no_active_initialization(search_collection, "search");
            const simple_json::Value body = simple_json::parse_text(request.body);
            return {200, search_response_json(
                search_service.search(search_collection, parse_search_request(body)))};
        }
        if (request.method == "DELETE" && !vectors_collection.empty()) {
            initialization_manager.require_no_active_initialization(
                vectors_collection, "vector deletion");
            catalog.get_collection(vectors_collection);
            return {200, "{\"deleted\":0,\"noop\":true}"};
        }
        if (request.method == "DELETE" && !collection.empty()) {
            management_service.delete_collection(collection);
            return {200, "{\"deleted\":true,\"collection\":\"" +
                         json_escape(collection) + "\"}"};
        }
        if (request.method == "POST" && !backups_collection.empty()) {
            initialization_manager.require_no_active_initialization(
                backups_collection, "backup");
            const simple_json::Value body = simple_json::parse_text(request.body);
            if (!body.is_object()) throw std::invalid_argument("request body must be a JSON object");
            const std::string backup_name = optional_string(body, "name");
            if (backup_name.empty()) throw std::invalid_argument("body.name is required");
            management_service.create_backup(backups_collection, backup_name);
            return {201, "{\"collection\":\"" + json_escape(backups_collection) +
                         "\",\"backup\":\"" + json_escape(backup_name) +
                         "\",\"created\":true}"};
        }
        if (request.method == "GET" && !backups_collection.empty()) {
            initialization_manager.require_no_active_initialization(
                backups_collection, "backup listing");
            return {200, backups_json(management_service.list_backups(backups_collection))};
        }
        if (request.method == "POST" && !restore_collection.empty()) {
            initialization_manager.require_no_active_initialization(
                restore_collection, "restore");
            const simple_json::Value body = simple_json::parse_text(request.body);
            if (!body.is_object()) throw std::invalid_argument("request body must be a JSON object");
            const std::string backup_name = optional_string(body, "name");
            if (backup_name.empty()) throw std::invalid_argument("body.name is required");
            return {200, "{\"restored\":true,\"backup\":\"" +
                         json_escape(backup_name) + "\",\"collection\":" +
                         manifest_json(management_service.restore_backup(
                             restore_collection, backup_name)) + "}"};
        }
        if (request.method == "POST" && !generations_collection.empty()) {
            initialization_manager.require_no_active_initialization(
                generations_collection, "manual generation");
            const qcovert::GenerationJob job = generation_manager.start_generation(generations_collection);
            return {202, generation_job_json(job)};
        }
        if (request.method == "GET" && !job_collection.empty() && !job_id.empty()) {
            return {200, generation_job_json(generation_manager.get_job(job_collection, job_id))};
        }
    } catch (const qcovert::InitializationConflict& error) {
        return json_error(409, "initialization_conflict", error.what());
    } catch (const std::length_error& error) {
        return json_error(409, "active_generation_capacity", error.what());
    } catch (const std::out_of_range& error) {
        return json_error(404, "collection_not_found", error.what());
    } catch (const std::invalid_argument& error) {
        return json_error(400, "invalid_request", error.what());
    } catch (const std::logic_error& error) {
        return json_error(409, "generation_not_ready", error.what());
    } catch (const std::exception& error) {
        return json_error(500, "internal_error", error.what());
    }
    return json_error(404, "not_found", "endpoint does not exist");
}

void serve_connection(int socket_fd, const std::string& api_key, qcovert::CollectionCatalog& catalog,
                      qcovert::CollectionDataStore& data_store,
                      qcovert::GenerationManager& generation_manager,
                      qcovert::InitializationManager& initialization_manager,
                      qcovert::SearchService& search_service,
                      qcovert::ManagementService& management_service,
                      std::shared_mutex& lifecycle_mutex,
                      size_t max_request_bytes, int socket_timeout_ms) {
    try {
        const timeval timeout{
            socket_timeout_ms / 1000,
            (socket_timeout_ms % 1000) * 1000,
        };
        if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
            setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
            throw std::runtime_error("cannot configure HTTP socket timeout");
        }
        const HttpRequest request = receive_request(socket_fd, max_request_bytes);
        const InitializationRoute initialization_route = parse_initialization_route(request.target);
        const bool management_request =
            (request.method == "DELETE" &&
             (!collection_name_from_path(request.target).empty() ||
              (initialization_route.valid && initialization_route.action == "session"))) ||
            (request.method == "POST" &&
             (!collection_name_from_path(request.target, "/backups").empty() ||
              !collection_name_from_path(request.target, "/restore").empty()));
        if (management_request) {
            std::unique_lock<std::shared_mutex> lock(lifecycle_mutex);
            send_response(socket_fd, route_request(
                request, api_key, catalog, data_store, generation_manager, initialization_manager,
                search_service, management_service));
        } else {
            std::shared_lock<std::shared_mutex> lock(lifecycle_mutex);
            send_response(socket_fd, route_request(
                request, api_key, catalog, data_store, generation_manager, initialization_manager,
                search_service, management_service));
        }
    } catch (const std::length_error& error) {
        send_response(socket_fd, json_error(413, "request_too_large", error.what()));
    } catch (const std::exception& error) {
        send_response(socket_fd, json_error(400, "invalid_http_request", error.what()));
    }
    close(socket_fd);
}

class HttpWorkerPool {
public:
    HttpWorkerPool(size_t worker_count, size_t max_pending, const std::string& api_key,
                   qcovert::CollectionCatalog& catalog, qcovert::CollectionDataStore& data_store,
                   qcovert::GenerationManager& generation_manager,
                   qcovert::InitializationManager& initialization_manager,
                   qcovert::SearchService& search_service,
                   qcovert::ManagementService& management_service,
                   size_t max_request_bytes, int socket_timeout_ms)
        : max_pending_(max_pending), api_key_(api_key), catalog_(&catalog), data_store_(&data_store),
          generation_manager_(&generation_manager), initialization_manager_(&initialization_manager),
          search_service_(&search_service),
          management_service_(&management_service),
          max_request_bytes_(max_request_bytes),
          socket_timeout_ms_(socket_timeout_ms) {
        workers_.reserve(worker_count);
        for (size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~HttpWorkerPool() {
        shutdown();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (joined_) return;
            stopping_ = true;
        }
        condition_.notify_all();
        for (std::thread& worker : workers_) if (worker.joinable()) worker.join();
        workers_.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            joined_ = true;
        }
    }

    bool submit(int socket_fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || pending_.size() >= max_pending_) return false;
        pending_.push_back(socket_fd);
        condition_.notify_one();
        return true;
    }

private:
    size_t max_pending_;
    std::string api_key_;
    qcovert::CollectionCatalog* catalog_;
    qcovert::CollectionDataStore* data_store_;
    qcovert::GenerationManager* generation_manager_;
    qcovert::InitializationManager* initialization_manager_;
    qcovert::SearchService* search_service_;
    qcovert::ManagementService* management_service_;
    size_t max_request_bytes_;
    int socket_timeout_ms_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<int> pending_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    bool joined_ = false;
    std::shared_mutex lifecycle_mutex_;

    void worker_loop() {
        while (true) {
            int socket_fd = -1;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                if (stopping_ && pending_.empty()) return;
                socket_fd = pending_.front();
                pending_.pop_front();
            }
            serve_connection(socket_fd, api_key_, *catalog_, *data_store_, *generation_manager_,
                             *initialization_manager_,
                             *search_service_, *management_service_, lifecycle_mutex_,
                             max_request_bytes_, socket_timeout_ms_);
        }
    }
};

int create_listener(int port) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        throw std::runtime_error("cannot create query listener socket");
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listener, 128) < 0) {
        close(listener);
        throw std::runtime_error("cannot bind query listener");
    }
    return listener;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const QueryOptions options = parse_args(argc, argv);
        qcovert::CollectionCatalog catalog(options.root);
        qcovert::CollectionDataStore data_store(catalog);
        qcovert::GenerationManager generation_manager(catalog, data_store, options.generation_build);
        qcovert::InitializationManager initialization_manager(
            catalog, data_store, generation_manager);
        qcovert::SearchService search_service(
            catalog,
            data_store,
            options.generation_build.oram.storage_host,
            options.generation_build.oram.storage_port);
        qcovert::ManagementService management_service(
            catalog, data_store, search_service,
            options.generation_build.oram.storage_host,
            options.generation_build.oram.storage_port);
        const int listener = create_listener(options.port);
        qcovert::ShutdownSignal shutdown_signal;
        HttpWorkerPool workers(options.worker_threads, options.max_pending_connections,
                               options.api_key, catalog, data_store, generation_manager,
                               initialization_manager, search_service,
                               management_service,
                               options.max_request_bytes, options.socket_timeout_ms);
        std::cout << "[qcovert-query] listening on 0.0.0.0:" << options.port
                  << " root=" << std::filesystem::absolute(options.root)
                  << " dimension=384 metric=l2\n";
        std::cout << "[qcovert-query] press Ctrl+C or send SIGTERM for graceful shutdown\n";
        while (true) {
            pollfd descriptors[2] = {
                {listener, POLLIN, 0},
                {shutdown_signal.fd(), POLLIN, 0},
            };
            const int ready = poll(descriptors, 2, -1);
            if (ready < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("query listener poll failed");
            }
            if ((descriptors[1].revents & POLLIN) != 0 && shutdown_signal.consume()) break;
            if ((descriptors[0].revents & POLLIN) == 0) continue;
            const int accepted = accept(listener, nullptr, nullptr);
            if (accepted >= 0) {
                if (!workers.submit(accepted)) {
                    send_response(accepted, json_error(
                        503, "server_busy", "HTTP worker queue is full"));
                    close(accepted);
                }
            } else if (errno != EINTR) {
                throw std::runtime_error("query listener accept failed");
            }
        }
        close(listener);
        std::cout << "[qcovert-query] shutdown requested; draining HTTP requests\n";
        workers.shutdown();
        std::cout << "[qcovert-query] waiting for generation workers\n";
        generation_manager.shutdown();
        std::cout << "[qcovert-query] checkpointing active ORAM runtimes\n";
        search_service.checkpoint_all();
        std::cout << "[qcovert-query] graceful shutdown complete\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "qcovert_query failed: " << error.what() << "\n";
        return 1;
    }
}
