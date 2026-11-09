#include "qcovert/collection_data_store.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <unistd.h>

namespace fs = std::filesystem;

namespace qcovert {

namespace {

constexpr uint32_t kVectorRecordMagic = 0x31564351U; // "QCV1"
constexpr uint32_t kVectorRecordVersion = 1;
constexpr uint32_t kSnapshotMagic = 0x31534351U; // "QCS1"
constexpr uint32_t kMaxVectorIdBytes = 512;
constexpr uint32_t kMaxMetadataBytes = 1024 * 1024;
constexpr uint32_t kLogBatchMagic = 0x32424351U; // "QCB2"
constexpr uint32_t kLogFooterMagic = 0x32464351U; // "QCF2"
constexpr uint32_t kLogBatchVersion = 2;

#pragma pack(push, 1)
struct DiskRecordHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t dimension;
    uint32_t id_bytes;
    uint32_t metadata_bytes;
    uint64_t sequence;
};

struct SnapshotHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t dimension;
    uint64_t vector_count;
    uint64_t last_sequence;
};

struct LogBatchHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t record_count;
    uint64_t payload_bytes;
    uint64_t first_sequence;
    uint64_t last_sequence;
};

struct LogBatchFooter {
    uint32_t magic;
    uint32_t version;
    uint64_t batch_offset;
    uint64_t record_count;
    uint64_t payload_bytes;
    uint64_t first_sequence;
    uint64_t last_sequence;
    uint64_t payload_checksum;
};
#pragma pack(pop)

static_assert(sizeof(DiskRecordHeader) == 28, "unexpected vector record header padding");
static_assert(sizeof(SnapshotHeader) == 28, "unexpected snapshot header padding");
static_assert(sizeof(LogBatchHeader) == 40, "unexpected log batch header padding");
static_assert(sizeof(LogBatchFooter) == 56, "unexpected log batch footer padding");

void require_stream(bool success, const std::string& message) {
    if (!success) {
        throw std::runtime_error(message);
    }
}

void validate_record_header(const DiskRecordHeader& header) {
    if (header.magic != kVectorRecordMagic || header.version != kVectorRecordVersion ||
        header.dimension != kSupportedEmbeddingDimension || header.id_bytes == 0 ||
        header.id_bytes > kMaxVectorIdBytes || header.metadata_bytes > kMaxMetadataBytes ||
        header.sequence == 0) {
        throw std::runtime_error("invalid vector log record header");
    }
}

uint64_t record_body_bytes(const DiskRecordHeader& header) {
    return static_cast<uint64_t>(header.id_bytes) +
           static_cast<uint64_t>(header.dimension) * sizeof(float) +
           static_cast<uint64_t>(header.metadata_bytes);
}

void write_record(std::ostream& output, const VectorRecord& record, uint64_t sequence) {
    DiskRecordHeader header{};
    header.magic = kVectorRecordMagic;
    header.version = kVectorRecordVersion;
    header.dimension = kSupportedEmbeddingDimension;
    header.id_bytes = static_cast<uint32_t>(record.id.size());
    header.metadata_bytes = static_cast<uint32_t>(record.metadata_json.size());
    header.sequence = sequence;
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(record.id.data(), static_cast<std::streamsize>(record.id.size()));
    output.write(reinterpret_cast<const char*>(record.values.data()),
                 static_cast<std::streamsize>(record.values.size() * sizeof(float)));
    output.write(record.metadata_json.data(), static_cast<std::streamsize>(record.metadata_json.size()));
    require_stream(static_cast<bool>(output), "cannot write vector record");
}

void fsync_file(const fs::path& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("cannot open file for fsync: " + path.string());
    }
    const int result = fsync(fd);
    close(fd);
    if (result != 0) {
        throw std::runtime_error("cannot fsync file: " + path.string());
    }
}

uint64_t fnv1a64(const char* data, size_t size) {
    uint64_t value = 1469598103934665603ULL;
    for (size_t i = 0; i < size; ++i) {
        value ^= static_cast<unsigned char>(data[i]);
        value *= 1099511628211ULL;
    }
    return value;
}

template <typename T>
void append_binary(std::string& output, const T& value) {
    output.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void append_record(std::string& output, const VectorRecord& record, uint64_t sequence) {
    const DiskRecordHeader header{
        kVectorRecordMagic,
        kVectorRecordVersion,
        kSupportedEmbeddingDimension,
        static_cast<uint32_t>(record.id.size()),
        static_cast<uint32_t>(record.metadata_json.size()),
        sequence,
    };
    append_binary(output, header);
    output.append(record.id);
    output.append(reinterpret_cast<const char*>(record.values.data()),
                  record.values.size() * sizeof(float));
    output.append(record.metadata_json);
}

SnapshotHeader read_snapshot_header(std::istream& input) {
    SnapshotHeader snapshot{};
    input.read(reinterpret_cast<char*>(&snapshot), sizeof(snapshot));
    if (!input || snapshot.magic != kSnapshotMagic || snapshot.version != kVectorRecordVersion ||
        snapshot.dimension != kSupportedEmbeddingDimension) {
        throw std::runtime_error("invalid generation vector snapshot header");
    }
    return snapshot;
}

} // namespace

CollectionDataStore::CollectionDataStore(CollectionCatalog& catalog) : catalog_(&catalog) {}

UpsertResult CollectionDataStore::upsert(const std::string& collection,
                                         const std::vector<VectorRecord>& records) {
    return append_batch(collection, records, false);
}

UpsertResult CollectionDataStore::insert_unique(
        const std::string& collection,
        const std::vector<VectorRecord>& records) {
    return append_batch(collection, records, true);
}

UpsertResult CollectionDataStore::append_batch(
        const std::string& collection,
        const std::vector<VectorRecord>& records,
        bool require_unique_ids) {
    if (records.empty()) {
        throw std::invalid_argument("records must not be empty");
    }
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    // Validate collection configuration before creating a plaintext data file.
    const CollectionManifest manifest = catalog_->get_collection(valid_collection);
    if (manifest.dimension != kSupportedEmbeddingDimension) {
        throw std::runtime_error("collection has unsupported vector dimension");
    }
    for (const VectorRecord& record : records) {
        validate_vector_id(record.id);
        if (record.values.size() != kSupportedEmbeddingDimension) {
            throw std::invalid_argument("each vector must contain exactly 384 values");
        }
        if (record.metadata_json.size() > kMaxMetadataBytes) {
            throw std::invalid_argument("metadata JSON exceeds 1 MiB");
        }
        for (const float value : record.values) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument("vector values must be finite numbers");
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    CollectionIndex& index = indexes_[valid_collection];
    load_index_locked(valid_collection, index);
    if (require_unique_ids) {
        std::unordered_set<std::string> unique_ids;
        unique_ids.reserve(records.size());
        for (const VectorRecord& record : records) {
            if (!unique_ids.insert(record.id).second) {
                throw std::logic_error("initialization batch contains a duplicate vector id");
            }
            if (index.latest.find(record.id) != index.latest.end()) {
                throw std::logic_error(
                    "initialization vector id already exists in the collection: " + record.id);
            }
        }
    }
    const fs::path path = log_path(valid_collection);
    const uint64_t batch_offset = fs::exists(path) ? fs::file_size(path) : 0;
    std::string payload;
    payload.reserve(records.size() * (sizeof(DiskRecordHeader) +
                    kSupportedEmbeddingDimension * sizeof(float) + 64));
    struct PendingLocation {
        std::string id;
        RecordLocation location;
    };
    std::vector<PendingLocation> pending;
    pending.reserve(records.size());
    std::unordered_set<std::string> batch_ids;
    UpsertResult result;
    uint64_t sequence = index.next_sequence;
    for (const VectorRecord& record : records) {
        const uint64_t offset = batch_offset + sizeof(LogBatchHeader) + payload.size();
        const bool existed = index.latest.find(record.id) != index.latest.end() ||
                             !batch_ids.insert(record.id).second;
        append_record(payload, record, sequence);
        pending.push_back({record.id, {offset, sequence}});
        if (existed) {
            ++result.updated;
        } else {
            ++result.inserted;
        }
        if (++sequence == 0) throw std::overflow_error("vector log sequence overflow");
    }
    const LogBatchHeader batch_header{
        kLogBatchMagic, kLogBatchVersion, records.size(), payload.size(),
        index.next_sequence, sequence - 1};
    const LogBatchFooter batch_footer{
        kLogFooterMagic, kLogBatchVersion, batch_offset, records.size(), payload.size(),
        index.next_sequence, sequence - 1, fnv1a64(payload.data(), payload.size())};

    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("cannot open vector log for append: " + path.string());
    output.write(reinterpret_cast<const char*>(&batch_header), sizeof(batch_header));
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.write(reinterpret_cast<const char*>(&batch_footer), sizeof(batch_footer));
    output.close();
    require_stream(static_cast<bool>(output), "cannot flush vector log");
    fsync_file(path);
    for (PendingLocation& item : pending) {
        index.latest[item.id] = item.location;
    }
    index.next_sequence = sequence;
    result.vector_count = index.latest.size();
    catalog_->set_vector_count(valid_collection, result.vector_count);
    return result;
}

VectorLogStatus CollectionDataStore::log_status(const std::string& collection) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    catalog_->get_collection(valid_collection);
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionIndex& index = indexes_[valid_collection];
    load_index_locked(valid_collection, index);
    return {static_cast<uint64_t>(index.latest.size()), index.next_sequence - 1};
}

void CollectionDataStore::reset_uninitialized_collection(const std::string& collection) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    const CollectionManifest manifest = catalog_->get_collection(valid_collection);
    if (!manifest.active_generation.empty() || !manifest.pending_generation.empty()) {
        throw std::logic_error("cannot reset vectors after a generation has started");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionIndex& index = indexes_[valid_collection];
    load_index_locked(valid_collection, index);
    std::error_code error;
    fs::remove(log_path(valid_collection), error);
    if (error) {
        throw std::runtime_error("cannot remove initialization vector log: " + error.message());
    }
    index = CollectionIndex{};
    index.loaded = true;
    catalog_->set_vector_count(valid_collection, 0);
}

VectorSnapshot CollectionDataStore::write_latest_snapshot(
        const std::string& collection,
        const fs::path& snapshot_file) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    catalog_->get_collection(valid_collection);
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionIndex& index = indexes_[valid_collection];
    load_index_locked(valid_collection, index);

    std::vector<std::pair<std::string, RecordLocation>> records;
    records.reserve(index.latest.size());
    for (const auto& item : index.latest) {
        records.push_back(item);
    }
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    std::error_code error;
    fs::create_directories(snapshot_file.parent_path(), error);
    if (error) {
        throw std::runtime_error("cannot create generation snapshot directory: " + error.message());
    }
    std::ofstream output(snapshot_file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create generation vector snapshot: " + snapshot_file.string());
    }
    const uint64_t last_sequence = index.next_sequence - 1;
    const SnapshotHeader snapshot_header{
        kSnapshotMagic,
        kVectorRecordVersion,
        kSupportedEmbeddingDimension,
        static_cast<uint64_t>(records.size()),
        last_sequence,
    };
    output.write(reinterpret_cast<const char*>(&snapshot_header), sizeof(snapshot_header));
    for (const auto& item : records) {
        const VectorRecord record = read_record_at_locked(valid_collection, item.second.offset);
        write_record(output, record, item.second.sequence);
    }
    output.close();
    require_stream(static_cast<bool>(output), "cannot flush generation vector snapshot");
    fsync_file(snapshot_file);
    return {static_cast<uint64_t>(records.size()), last_sequence};
}

uint64_t CollectionDataStore::vector_count(const std::string& collection) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    catalog_->get_collection(valid_collection);
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionIndex& index = indexes_[valid_collection];
    load_index_locked(valid_collection, index);
    return index.latest.size();
}

void CollectionDataStore::forget_collection(const std::string& collection) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    std::lock_guard<std::mutex> lock(mutex_);
    indexes_.erase(valid_collection);
}

std::vector<VectorDelta> CollectionDataStore::read_latest_after(
        const std::string& collection,
        uint64_t last_sequence) {
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionIndex& index = indexes_[valid_collection];
    load_index_locked(valid_collection, index);
    return latest_after_locked(valid_collection, index, last_sequence).deltas;
}

void CollectionDataStore::with_latest_after_locked(
        const std::string& collection,
        uint64_t last_sequence,
        const std::function<void(const VectorDeltaBarrier&)>& callback) {
    if (!callback) throw std::invalid_argument("generation barrier callback must not be empty");
    const std::string valid_collection = CollectionCatalog::validate_collection_name(collection);
    std::lock_guard<std::mutex> lock(mutex_);
    CollectionIndex& index = indexes_[valid_collection];
    load_index_locked(valid_collection, index);
    callback(latest_after_locked(valid_collection, index, last_sequence));
}

VectorDeltaBarrier CollectionDataStore::latest_after_locked(
        const std::string& collection,
        CollectionIndex& index,
        uint64_t last_sequence) const {
    std::vector<std::pair<std::string, RecordLocation>> changed;
    for (const auto& item : index.latest) {
        if (item.second.sequence > last_sequence) changed.push_back(item);
    }
    std::sort(changed.begin(), changed.end(), [](const auto& left, const auto& right) {
        return left.second.sequence < right.second.sequence;
    });
    VectorDeltaBarrier output;
    output.vector_count = index.latest.size();
    output.last_sequence = index.next_sequence - 1;
    output.deltas.reserve(changed.size());
    for (const auto& item : changed) {
        output.deltas.push_back({read_record_at_locked(collection, item.second.offset),
                                 item.second.sequence});
    }
    return output;
}

std::vector<VectorRecord> CollectionDataStore::read_snapshot(const fs::path& snapshot_file) {
    std::ifstream input(snapshot_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open generation vector snapshot: " + snapshot_file.string());
    }
    const SnapshotHeader snapshot = read_snapshot_header(input);
    if (snapshot.vector_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error("generation vector snapshot is too large for this process");
    }
    std::vector<VectorRecord> records;
    records.reserve(static_cast<size_t>(snapshot.vector_count));
    uint64_t maximum_sequence = 0;
    for (uint64_t i = 0; i < snapshot.vector_count; ++i) {
        DiskRecordHeader header{};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        require_stream(static_cast<bool>(input), "truncated generation snapshot record header");
        validate_record_header(header);
        VectorRecord record;
        record.id.assign(header.id_bytes, '\0');
        input.read(record.id.data(), static_cast<std::streamsize>(record.id.size()));
        record.values.assign(header.dimension, 0.0f);
        input.read(reinterpret_cast<char*>(record.values.data()),
                   static_cast<std::streamsize>(record.values.size() * sizeof(float)));
        record.metadata_json.assign(header.metadata_bytes, '\0');
        input.read(record.metadata_json.data(), static_cast<std::streamsize>(record.metadata_json.size()));
        require_stream(static_cast<bool>(input), "truncated generation snapshot record payload");
        validate_vector_id(record.id);
        for (const float value : record.values) {
            if (!std::isfinite(value)) {
                throw std::runtime_error("generation snapshot contains a non-finite vector value");
            }
        }
        maximum_sequence = std::max(maximum_sequence, header.sequence);
        records.push_back(std::move(record));
    }
    char extra = 0;
    if (input.read(&extra, 1)) {
        throw std::runtime_error("generation vector snapshot has trailing bytes");
    }
    if (maximum_sequence > snapshot.last_sequence) {
        throw std::runtime_error("generation snapshot sequence exceeds its declared boundary");
    }
    return records;
}

VectorSnapshotInfo CollectionDataStore::inspect_snapshot(const fs::path& snapshot_file) {
    std::ifstream input(snapshot_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open generation vector snapshot: " + snapshot_file.string());
    }
    const SnapshotHeader snapshot = read_snapshot_header(input);
    return {snapshot.vector_count, snapshot.last_sequence};
}

std::string CollectionDataStore::validate_vector_id(const std::string& id) {
    if (id.empty() || id.size() > kMaxVectorIdBytes) {
        throw std::invalid_argument("vector id must contain 1 to 512 bytes");
    }
    for (const unsigned char ch : id) {
        if (ch < 0x20 || ch == 0x7f) {
            throw std::invalid_argument("vector id must not contain control characters");
        }
    }
    return id;
}

fs::path CollectionDataStore::log_path(const std::string& collection) const {
    return catalog_->collection_directory(collection) / "vectors.log";
}

void CollectionDataStore::load_index_locked(const std::string& collection, CollectionIndex& index) {
    if (index.loaded) {
        return;
    }
    const fs::path path = log_path(collection);
    if (!fs::exists(path)) {
        index.loaded = true;
        return;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read vector log: " + path.string());
    const uint64_t file_bytes = fs::file_size(path);
    uint64_t offset = 0;
    uint64_t committed_end = 0;
    uint64_t maximum_sequence = 0;
    bool incomplete_tail = false;

    auto install_record = [&](const char* bytes, size_t available, uint64_t absolute_offset,
                              uint64_t expected_sequence) -> size_t {
        if (available < sizeof(DiskRecordHeader)) return 0;
        DiskRecordHeader header{};
        std::memcpy(&header, bytes, sizeof(header));
        validate_record_header(header);
        const uint64_t total = sizeof(header) + record_body_bytes(header);
        if (total > available || total > std::numeric_limits<size_t>::max()) return 0;
        if (expected_sequence != 0 && header.sequence != expected_sequence) {
            throw std::runtime_error("vector log batch contains a non-contiguous sequence");
        }
        std::string id(bytes + sizeof(header), header.id_bytes);
        validate_vector_id(id);
        index.latest[id] = {absolute_offset, header.sequence};
        maximum_sequence = std::max(maximum_sequence, header.sequence);
        return static_cast<size_t>(total);
    };

    while (offset < file_bytes) {
        if (file_bytes - offset < sizeof(uint32_t)) {
            incomplete_tail = true;
            break;
        }
        input.seekg(static_cast<std::streamoff>(offset));
        uint32_t magic = 0;
        input.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        require_stream(static_cast<bool>(input), "cannot read vector log marker");

        if (magic == kVectorRecordMagic) {
            if (file_bytes - offset < sizeof(DiskRecordHeader)) {
                incomplete_tail = true;
                break;
            }
            DiskRecordHeader header{};
            input.seekg(static_cast<std::streamoff>(offset));
            input.read(reinterpret_cast<char*>(&header), sizeof(header));
            validate_record_header(header);
            const uint64_t total = sizeof(header) + record_body_bytes(header);
            if (total > file_bytes - offset) {
                incomplete_tail = true;
                break;
            }
            std::string encoded(static_cast<size_t>(total), '\0');
            input.seekg(static_cast<std::streamoff>(offset));
            input.read(encoded.data(), static_cast<std::streamsize>(encoded.size()));
            require_stream(static_cast<bool>(input), "cannot read legacy vector log record");
            if (install_record(encoded.data(), encoded.size(), offset, 0) != encoded.size()) {
                throw std::runtime_error("invalid legacy vector log record");
            }
            offset += total;
            committed_end = offset;
            continue;
        }

        if (magic != kLogBatchMagic) {
            throw std::runtime_error("vector log contains corruption after its last committed batch");
        }
        if (file_bytes - offset < sizeof(LogBatchHeader)) {
            incomplete_tail = true;
            break;
        }
        LogBatchHeader header{};
        input.seekg(static_cast<std::streamoff>(offset));
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        require_stream(static_cast<bool>(input), "cannot read vector log batch header");
        if (header.version != kLogBatchVersion || header.record_count == 0 ||
            header.first_sequence == 0 ||
            header.last_sequence != header.first_sequence + header.record_count - 1) {
            throw std::runtime_error("invalid vector log batch header");
        }
        const uint64_t total = sizeof(header) + header.payload_bytes + sizeof(LogBatchFooter);
        if (total < header.payload_bytes || total > file_bytes - offset) {
            incomplete_tail = true;
            break;
        }
        if (header.payload_bytes > std::numeric_limits<size_t>::max()) {
            throw std::runtime_error("vector log batch is too large");
        }
        std::string payload(static_cast<size_t>(header.payload_bytes), '\0');
        input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
        LogBatchFooter footer{};
        input.read(reinterpret_cast<char*>(&footer), sizeof(footer));
        require_stream(static_cast<bool>(input), "cannot read complete vector log batch");
        if (footer.magic != kLogFooterMagic || footer.version != kLogBatchVersion ||
            footer.batch_offset != offset || footer.record_count != header.record_count ||
            footer.payload_bytes != header.payload_bytes ||
            footer.first_sequence != header.first_sequence ||
            footer.last_sequence != header.last_sequence ||
            footer.payload_checksum != fnv1a64(payload.data(), payload.size())) {
            throw std::runtime_error("vector log committed batch checksum or footer is invalid");
        }
        size_t payload_offset = 0;
        for (uint64_t i = 0; i < header.record_count; ++i) {
            const size_t consumed = install_record(
                payload.data() + payload_offset, payload.size() - payload_offset,
                offset + sizeof(header) + payload_offset, header.first_sequence + i);
            if (consumed == 0) throw std::runtime_error("invalid record in committed vector log batch");
            payload_offset += consumed;
        }
        if (payload_offset != payload.size()) {
            throw std::runtime_error("vector log batch contains trailing payload bytes");
        }
        offset += total;
        committed_end = offset;
    }
    input.close();
    if (incomplete_tail) {
        fs::resize_file(path, committed_end);
        fsync_file(path);
    }
    index.next_sequence = maximum_sequence + 1;
    if (index.next_sequence == 0) {
        throw std::runtime_error("vector log sequence overflow");
    }
    index.loaded = true;
    const CollectionManifest manifest = catalog_->get_collection(collection);
    if (manifest.vector_count != index.latest.size()) {
        catalog_->set_vector_count(collection, index.latest.size());
    }
}

VectorRecord CollectionDataStore::read_record_at_locked(const std::string& collection, uint64_t offset) const {
    std::ifstream input(log_path(collection), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read vector log record");
    }
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("invalid vector log offset");
    }
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    DiskRecordHeader header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    require_stream(static_cast<bool>(input), "truncated vector log record header");
    validate_record_header(header);
    VectorRecord record;
    record.id.assign(header.id_bytes, '\0');
    input.read(record.id.data(), static_cast<std::streamsize>(record.id.size()));
    record.values.assign(header.dimension, 0.0f);
    input.read(reinterpret_cast<char*>(record.values.data()),
               static_cast<std::streamsize>(record.values.size() * sizeof(float)));
    record.metadata_json.assign(header.metadata_bytes, '\0');
    input.read(record.metadata_json.data(), static_cast<std::streamsize>(record.metadata_json.size()));
    require_stream(static_cast<bool>(input), "truncated vector log record payload");
    validate_vector_id(record.id);
    for (const float value : record.values) {
        if (!std::isfinite(value)) {
            throw std::runtime_error("vector log contains a non-finite value");
        }
    }
    return record;
}

} // namespace qcovert
