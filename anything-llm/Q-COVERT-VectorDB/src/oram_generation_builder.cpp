#include "qcovert/oram_generation_builder.h"

#include "qcovert/collection_catalog.h"
#include "qcovert/collection_data_store.h"
#include "qcovert/ivf_generation_builder.h"
#include "qcovert/storage_connection.h"
#include "qcovert/storage_protocol.h"

#include "online_vector_codec.h"

#include "Block.h"
#include "ClusterORAM.h"
#include "DoubleORAM.h"
#include "RandomForOram.h"
#include "RemoteRing.h"
#include "RingORAM.h"
#include "ivf_rabitq_index.h"
#include "oram_block_builders.h"
#include "simple_json.h"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace qcovert {

namespace {

constexpr uint32_t kClientStateVersion = 2;
constexpr uint32_t kClusterClientStateMagic = 0x31534351U; // "QCS1"
constexpr uint32_t kRingClientStateMagic = 0x31535251U; // "QRS1"
constexpr uint32_t kVectorsPerApproxBlock = 32;
constexpr uint64_t kGenerationVectorInterval = 10000;

using ServiceIndex = IvfRabitQIndex<384, 384, 4>;
using ApproxLayout = ApproxOramBlockLayout<384, 384>;
using RawExactLayout = RawFloat32ExactOramBlockLayout<384, 384>;

#pragma pack(push, 1)
struct ClientStateHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t storage_protocol_version;
    uint32_t tree_kind;
    int32_t block_size;
    int32_t real_bucket_size;
    int32_t dummy_size;
    int32_t evict_rate;
    int32_t num_levels;
    int32_t cached_levels;
    uint64_t num_buckets;
    uint64_t real_blocks;
    uint64_t logical_block_limit;
    uint64_t real_block_capacity;
    uint64_t eviction_counter;
    uint64_t delayed_leaves;
    uint32_t cluster_count;
    uint32_t reserved;
};
#pragma pack(pop)

class AtomicBinaryWriter {
public:
    explicit AtomicBinaryWriter(const fs::path& target)
        : target_(target), temporary_(target.string() + ".tmp." + std::to_string(getpid())) {
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        if (error) throw std::runtime_error("cannot create client-state directory: " + error.message());
        fd_ = open(temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd_ < 0) {
            throw std::runtime_error("cannot create client-state file: " +
                                     std::string(std::strerror(errno)));
        }
    }

    AtomicBinaryWriter(const AtomicBinaryWriter&) = delete;
    AtomicBinaryWriter& operator=(const AtomicBinaryWriter&) = delete;

    ~AtomicBinaryWriter() {
        if (fd_ >= 0) close(fd_);
        if (!committed_) unlink(temporary_.c_str());
    }

    void write_bytes(const void* data, size_t size) {
        const auto* cursor = static_cast<const unsigned char*>(data);
        while (size != 0) {
            const ssize_t count = write(fd_, cursor, size);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("cannot write client-state file");
            cursor += count;
            size -= static_cast<size_t>(count);
        }
    }

    template <typename T>
    void write_value(const T& value) {
        write_bytes(&value, sizeof(value));
    }

    template <typename T>
    void write_vector(const std::vector<T>& values) {
        if (!values.empty()) write_bytes(values.data(), values.size() * sizeof(T));
    }

    void commit() {
        if (fsync(fd_) != 0) throw std::runtime_error("cannot fsync client-state file");
        if (close(fd_) != 0) {
            fd_ = -1;
            throw std::runtime_error("cannot close client-state file");
        }
        fd_ = -1;
        if (rename(temporary_.c_str(), target_.c_str()) != 0) {
            throw std::runtime_error("cannot publish client-state file: " +
                                     std::string(std::strerror(errno)));
        }
        const int directory_fd = open(
            target_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd < 0 || fsync(directory_fd) != 0) {
            if (directory_fd >= 0) close(directory_fd);
            throw std::runtime_error("cannot fsync client-state directory");
        }
        close(directory_fd);
        committed_ = true;
    }

private:
    fs::path target_;
    fs::path temporary_;
    int fd_ = -1;
    bool committed_ = false;
};

class BinaryReader {
public:
    explicit BinaryReader(const fs::path& path) {
        fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd_ < 0) throw std::runtime_error("cannot open client-state file: " + path.string());
        struct stat info{};
        if (fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("client-state path is not a regular file: " + path.string());
        }
        remaining_ = static_cast<uint64_t>(info.st_size);
    }

    BinaryReader(const BinaryReader&) = delete;
    BinaryReader& operator=(const BinaryReader&) = delete;
    ~BinaryReader() { if (fd_ >= 0) close(fd_); }

    void read_bytes(void* output, size_t size) {
        if (size > remaining_) throw std::runtime_error("truncated client-state file");
        auto* cursor = static_cast<unsigned char*>(output);
        size_t pending = size;
        while (pending != 0) {
            const ssize_t count = read(fd_, cursor, pending);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("truncated client-state file");
            cursor += count;
            pending -= static_cast<size_t>(count);
        }
        remaining_ -= size;
    }

    template <typename T>
    T read_value() {
        T value{};
        read_bytes(&value, sizeof(value));
        return value;
    }

    template <typename T>
    void read_vector(std::vector<T>& values) {
        if (!values.empty()) read_bytes(values.data(), values.size() * sizeof(T));
    }

    void require_end() const {
        if (remaining_ != 0) throw std::runtime_error("client-state file contains trailing bytes");
    }

private:
    int fd_ = -1;
    uint64_t remaining_ = 0;
};

void validate_client_state_header(
        const ClientStateHeader& header,
        uint32_t magic,
        StorageTreeKind kind,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    if (header.magic != magic || header.version != kClientStateVersion ||
        header.storage_protocol_version != kStorageProtocolVersion ||
        header.tree_kind != static_cast<uint32_t>(kind) ||
        header.block_size != layout.block_size ||
        header.real_bucket_size != config.real_bucket_size ||
        header.dummy_size != config.dummy_size ||
        header.evict_rate != config.evict_rate ||
        header.num_levels != layout.num_levels ||
        header.cached_levels != layout.cached_levels ||
        header.num_buckets != layout.num_buckets ||
        header.real_blocks != layout.real_blocks ||
        header.logical_block_limit != layout.logical_block_limit ||
        header.real_block_capacity != layout.real_block_capacity ||
        header.eviction_counter > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        header.delayed_leaves > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        header.reserved != 0) {
        throw std::runtime_error("client-state header does not match the generation manifest");
    }
}

uint64_t checked_tree_buckets(int levels) {
    if (levels < 2 || levels >= 32) {
        throw std::invalid_argument("ORAM tree levels must be in [2, 31]");
    }
    return (1ULL << levels) - 1ULL;
}

uint64_t cached_tree_buckets(int cached_levels) {
    if (cached_levels < 0 || cached_levels >= 32) {
        throw std::invalid_argument("cached ORAM levels are outside the supported range");
    }
    return cached_levels == 0 ? 0ULL : (1ULL << cached_levels) - 1ULL;
}

OramTreeLayout choose_tree_layout(
        uint64_t real_blocks,
        uint64_t logical_block_limit,
        int block_size,
        const OramGenerationConfig& config) {
    if (real_blocks == 0 || logical_block_limit < real_blocks || block_size <= 0) {
        throw std::invalid_argument("invalid ORAM tree sizing input");
    }
    OramTreeLayout layout;
    layout.block_size = block_size;
    layout.real_blocks = real_blocks;
    layout.logical_block_limit = logical_block_limit;

    for (int levels = 2; levels < 32; ++levels) {
        const int cached = std::min(config.cached_levels, std::max(0, levels - 2));
        const uint64_t buckets = checked_tree_buckets(levels);
        const uint64_t cached_buckets = cached_tree_buckets(cached);
        const uint64_t capacity =
            (buckets - cached_buckets) * static_cast<uint64_t>(config.real_bucket_size);
        if (logical_block_limit <= capacity && logical_block_limit * 10ULL <= capacity * 6ULL) {
            layout.num_levels = levels;
            layout.cached_levels = cached;
            layout.num_buckets = buckets;
            layout.real_block_capacity = capacity;
            return layout;
        }
    }
    throw std::overflow_error("ORAM tree sizing exceeds the supported level range");
}

uint64_t compute_next_generation_vector_capacity(uint64_t vector_count) {
    const uint64_t generation = vector_count / kGenerationVectorInterval + 1ULL;
    if (generation > std::numeric_limits<uint64_t>::max() / kGenerationVectorInterval) {
        throw std::overflow_error("next generation vector capacity overflows uint64");
    }
    return generation * kGenerationVectorInterval;
}

uint64_t cluster_block_capacity_at_vector_target(
        const ServiceIndex& index,
        uint64_t target_vector_count) {
    if (target_vector_count < index.n) {
        throw std::invalid_argument("ClusterORAM target vector count is below the snapshot size");
    }
    uint64_t current_blocks = 0;
    std::priority_queue<int, std::vector<int>, std::greater<int>> next_block_costs;
    for (uint32_t cluster = 0; cluster < index.num_clusters; ++cluster) {
        const uint64_t blocks = index.cluster_num_blocks(cluster);
        const uint64_t slots = blocks * kVectorsPerApproxBlock;
        if (blocks == 0 || slots < index.len[cluster]) {
            throw std::runtime_error("IVF cluster block count cannot contain its current vectors");
        }
        current_blocks += blocks;
        const uint64_t free_slots = slots - index.len[cluster];
        if (free_slots > kVectorsPerApproxBlock) {
            throw std::runtime_error("IVF cluster has more than one partial block of free space");
        }
        // Creating the first new block in this cluster requires filling its
        // current tail block and inserting one additional vector. Every later
        // block in the same cluster costs another 32 inserted vectors.
        next_block_costs.push(static_cast<int>(free_slots + 1ULL));
    }

    uint64_t remaining_vectors = target_vector_count - index.n;
    uint64_t additional_blocks = 0;
    while (!next_block_costs.empty() &&
           static_cast<uint64_t>(next_block_costs.top()) <= remaining_vectors) {
        remaining_vectors -= static_cast<uint64_t>(next_block_costs.top());
        next_block_costs.pop();
        ++additional_blocks;
        next_block_costs.push(static_cast<int>(kVectorsPerApproxBlock));
    }
    return current_blocks + additional_blocks;
}

int packed_int_count(size_t bytes) {
    if (bytes > static_cast<size_t>(std::numeric_limits<int>::max()) * sizeof(int)) {
        throw std::overflow_error("ORAM payload is too large");
    }
    return static_cast<int>((bytes + sizeof(int) - 1) / sizeof(int));
}

std::vector<int> bytes_to_block_data(int block_size, const std::vector<uint8_t>& bytes) {
    std::vector<int> packed = double_oram::pack_bytes_to_ints(bytes);
    if (static_cast<int>(packed.size()) > block_size) {
        throw std::runtime_error("payload exceeds the configured ORAM block size");
    }
    packed.resize(static_cast<size_t>(block_size), 0);
    return packed;
}

double_oram::Block* make_block(int block_size, int index, std::vector<int> data) {
    auto* block = new double_oram::Block(block_size);
    block->index = index;
    block->data = std::move(data);
    return block;
}

std::vector<uint32_t> block_vector_ids(
        const ServiceIndex& index, uint32_t cluster_id, uint32_t block_id) {
    std::vector<uint32_t> ids(ServiceIndex::kBlockVectors, std::numeric_limits<uint32_t>::max());
    const uint32_t begin = block_id * ServiceIndex::kBlockVectors;
    const uint32_t active = std::min<uint32_t>(
        ServiceIndex::kBlockVectors, index.len[cluster_id] - begin);
    for (uint32_t slot = 0; slot < active; ++slot) {
        ids[slot] = index.ids[index.start[cluster_id] + begin + slot];
    }
    return ids;
}

std::vector<int> make_cluster_block_data(
        int block_size,
        const ServiceIndex& index,
        uint32_t cluster_id,
        uint32_t block_id,
        const std::vector<int>& ring_leaves) {
    std::vector<int> data(static_cast<size_t>(block_size), 0);
    data[0] = static_cast<int>(cluster_id);
    data[1] = static_cast<int>(block_id);
    const std::vector<uint32_t> ids = block_vector_ids(index, cluster_id, block_id);
    for (uint32_t slot = 0; slot < ServiceIndex::kBlockVectors; ++slot) {
        const uint32_t id = ids[slot];
        if (id == std::numeric_limits<uint32_t>::max()) {
            data[2 + slot] = -1;
            data[2 + ServiceIndex::kBlockVectors + slot] = -1;
        } else {
            if (id >= ring_leaves.size()) throw std::runtime_error("IVF internal ID is out of range");
            data[2 + slot] = static_cast<int>(id);
            data[2 + ServiceIndex::kBlockVectors + slot] = ring_leaves[id];
        }
    }
    const std::vector<uint8_t> payload = build_approx_oram_payload(index, cluster_id, block_id);
    const std::vector<int> packed = double_oram::pack_bytes_to_ints(payload);
    const size_t offset = 2 + 2 * ServiceIndex::kBlockVectors;
    if (offset + packed.size() > data.size()) {
        throw std::runtime_error("ClusterORAM approximate payload exceeds block layout");
    }
    std::copy(packed.begin(), packed.end(), data.begin() + static_cast<std::ptrdiff_t>(offset));
    return data;
}

StorageHandshake make_handshake(
        StorageAction action,
        StorageTreeKind kind,
        const std::string& collection,
        const std::string& generation,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    StorageHandshake handshake{};
    if (collection.empty() || collection.size() >= sizeof(handshake.collection) ||
        generation.empty() || generation.size() >= sizeof(handshake.generation)) {
        throw std::invalid_argument("collection or generation identifier is too long for storage protocol");
    }
    handshake.action = static_cast<uint32_t>(action);
    handshake.tree_kind = static_cast<uint32_t>(kind);
    handshake.block_size = layout.block_size;
    handshake.real_bucket_size = config.real_bucket_size;
    handshake.dummy_size = config.dummy_size;
    handshake.evict_rate = config.evict_rate;
    handshake.num_levels = layout.num_levels;
    handshake.cached_levels = layout.cached_levels;
    std::memcpy(handshake.collection, collection.c_str(), collection.size());
    std::memcpy(handshake.generation, generation.c_str(), generation.size());
    return handshake;
}

double_oram::RingOramConfig make_ring_config(
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    if (layout.logical_block_limit > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        throw std::overflow_error("ORAM logical block limit exceeds int capacity");
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

size_t buckets_per_upload(
        int block_size, int bucket_size, size_t upload_batch_bytes) {
    const size_t plain_bucket_bytes =
        static_cast<size_t>(block_size + 1) * sizeof(int) * static_cast<size_t>(bucket_size);
    return std::max<size_t>(1, upload_batch_bytes / std::max<size_t>(1, plain_bucket_bytes));
}

void verify_remote_block(
        double_oram::RemoteRing& storage,
        int bucket,
        int slot,
        int expected_index) {
    std::vector<int> positions{bucket};
    std::vector<int> offsets{slot};
    std::vector<double_oram::Block*> blocks;
    std::vector<bool> valids{true};
    storage.ReadBlockBatchAsBlockRing(positions, offsets, blocks, valids, false);
    if (blocks.size() != 1 || blocks[0] == nullptr || blocks[0]->index != expected_index) {
        for (double_oram::Block* block : blocks) delete block;
        throw std::runtime_error("reopened ORAM sample block failed validation");
    }
    delete blocks[0];
}

void checkpoint_and_verify(
        const std::string& collection,
        const std::string& generation,
        StorageTreeKind kind,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config,
        const AeadKey& key,
        StorageConnection& create_connection,
        std::unique_ptr<double_oram::RemoteRing>& create_storage,
        int sample_bucket,
        int sample_slot,
        int sample_index) {
    create_storage.reset();
    create_connection.close();

    StorageConnection open_connection = StorageConnection::connect(
        config.storage_host,
        config.storage_port,
        make_handshake(StorageAction::Open, kind, collection, generation, layout, config));
    const double_oram::RingOramConfig ring_config = make_ring_config(layout, config);
    std::unique_ptr<double_oram::RemoteRing> open_storage =
        open_connection.make_remote_ring(ring_config, false, key);
    verify_remote_block(*open_storage, sample_bucket, sample_slot, sample_index);
    open_storage.reset();
    open_connection.close();
}

ClientStateHeader client_state_header(
        uint32_t magic,
        StorageTreeKind kind,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config,
        uint64_t eviction_counter,
        uint64_t delayed_leaves,
        uint32_t cluster_count) {
    return ClientStateHeader{
        magic,
        kClientStateVersion,
        kStorageProtocolVersion,
        static_cast<uint32_t>(kind),
        layout.block_size,
        config.real_bucket_size,
        config.dummy_size,
        config.evict_rate,
        layout.num_levels,
        layout.cached_levels,
        layout.num_buckets,
        layout.real_blocks,
        layout.logical_block_limit,
        layout.real_block_capacity,
        eviction_counter,
        delayed_leaves,
        cluster_count,
        0};
}

void save_ring_client_state(
        const fs::path& path,
        const double_oram::RingORAM& ring,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    if (ring.block_size != layout.block_size || ring.num_levels != layout.num_levels ||
        ring.num_cached_levels != layout.cached_levels ||
        ring.num_buckets != static_cast<int>(layout.num_buckets) ||
        ring.num_blocks != static_cast<int>(layout.logical_block_limit)) {
        throw std::runtime_error("RingORAM object does not match the state layout being saved");
    }
    std::set<int> live_ids;
    for (const double_oram::OramMetaData& metadata : ring.metadata) {
        for (int slot = 0; slot < ring.bucket_size; ++slot) {
            const int id = metadata.block_ids[slot];
            if (id >= 0 && metadata.valids[static_cast<size_t>(slot)] &&
                !live_ids.insert(id).second) {
                throw std::runtime_error("RingORAM contains duplicate live tree blocks");
            }
        }
    }
    for (const auto& item : ring.mmstash) {
        if (!live_ids.insert(item.first.second).second) {
            throw std::runtime_error("RingORAM block is live in both the tree and stash");
        }
    }
    if (live_ids.size() != layout.real_blocks) {
        throw std::runtime_error("RingORAM tree and stash do not contain every real block");
    }
    AtomicBinaryWriter output(path);
    output.write_value(client_state_header(
        kRingClientStateMagic, StorageTreeKind::Ring, layout, config,
        static_cast<uint64_t>(ring.G), static_cast<uint64_t>(ring.cnt_delayed_leaves), 0));
    for (int bucket = 0; bucket < ring.num_buckets; ++bucket) {
        const double_oram::OramMetaData& metadata = ring.metadata[bucket];
        if (metadata.block_ids.size() != static_cast<size_t>(ring.bucket_size) ||
            metadata.block_leaves.size() != static_cast<size_t>(ring.bucket_size)) {
            throw std::runtime_error("RingORAM metadata is not fully initialized");
        }
        output.write_value(metadata.count);
        for (int slot = 0; slot < ring.bucket_size; ++slot) {
            const uint8_t valid = metadata.valids[static_cast<size_t>(slot)] ? 1U : 0U;
            output.write_value(valid);
        }
        output.write_vector(metadata.block_ids);
        output.write_vector(metadata.block_leaves);
    }
    const uint64_t stash_blocks = ring.mmstash.size();
    output.write_value(stash_blocks);
    for (const auto& item : ring.mmstash) {
        if (item.second == nullptr || item.second->index != item.first.second ||
            item.second->data.size() != static_cast<size_t>(ring.block_size)) {
            throw std::runtime_error("RingORAM stash contains an invalid block");
        }
        output.write_value(item.first.first);
        output.write_value(item.first.second);
        output.write_value(item.second->index);
        output.write_vector(item.second->data);
    }
    output.commit();
}

void save_cluster_client_state(
        const fs::path& path,
        const double_oram::ClusterORAM& cluster,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    if (cluster.block_size != layout.block_size || cluster.num_levels != layout.num_levels ||
        cluster.num_cached_levels != layout.cached_levels ||
        cluster.num_buckets != static_cast<int>(layout.num_buckets) ||
        cluster.num_blocks != static_cast<int>(layout.real_blocks)) {
        throw std::runtime_error("ClusterORAM object does not match the state layout being saved");
    }
    std::set<std::pair<int, int>> live_blocks;
    for (const auto& bucket : cluster.metadata) {
        for (const double_oram::ClusterORAMSlotMeta& metadata : bucket) {
            if (metadata.cluster_id >= 0 && metadata.valid &&
                !live_blocks.emplace(metadata.cluster_id, metadata.cluster_offset).second) {
                throw std::runtime_error("ClusterORAM contains duplicate live tree blocks");
            }
        }
    }
    for (const auto& group : cluster.cluster_stash) {
        for (const double_oram::ClusterORAMStashBlock& stash : group.second) {
            if (!live_blocks.emplace(stash.cluster_id, stash.cluster_offset).second) {
                throw std::runtime_error("ClusterORAM block is live in both the tree and stash");
            }
        }
    }
    if (live_blocks.size() != layout.real_blocks) {
        throw std::runtime_error("ClusterORAM tree and stash do not contain every real block");
    }
    AtomicBinaryWriter output(path);
    output.write_value(client_state_header(
        kClusterClientStateMagic, StorageTreeKind::Cluster, layout, config,
        static_cast<uint64_t>(cluster.G), static_cast<uint64_t>(cluster.cnt_delayed_leaves),
        static_cast<uint32_t>(cluster.cluster_block_counts.size())));
    output.write_vector(cluster.cluster_block_counts);
    output.write_vector(cluster.cluster_counters);
    output.write_vector(cluster.bucket_read_counts);
    for (int bucket = 0; bucket < cluster.num_buckets; ++bucket) {
        for (int slot = 0; slot < cluster.bucket_size; ++slot) {
            const double_oram::ClusterORAMSlotMeta& metadata = cluster.metadata[bucket][slot];
            output.write_value(metadata.cluster_id);
            output.write_value(metadata.cluster_offset);
            output.write_value(metadata.leaf);
            const uint8_t valid = metadata.valid ? 1U : 0U;
            output.write_value(valid);
        }
    }
    uint64_t stash_blocks = 0;
    for (const auto& group : cluster.cluster_stash) stash_blocks += group.second.size();
    output.write_value(stash_blocks);
    for (const auto& group : cluster.cluster_stash) {
        for (const double_oram::ClusterORAMStashBlock& stash : group.second) {
            if (stash.cluster_id != group.first ||
                stash.block.data.size() != static_cast<size_t>(cluster.block_size)) {
                throw std::runtime_error("ClusterORAM stash contains an invalid block");
            }
            output.write_value(stash.cluster_id);
            output.write_value(stash.cluster_offset);
            output.write_value(stash.leaf);
            output.write_value(stash.block.index);
            output.write_vector(stash.block.data);
        }
    }
    output.commit();
}

void write_client_manifest(
        const fs::path& path,
        const OramGenerationArtifacts& artifacts,
        const OramGenerationConfig& config) {
    std::ostringstream json;
    json << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"storage_protocol_version\": " << kStorageProtocolVersion << ",\n"
         << "  \"metadata_storage\": \"client\",\n"
         << "  \"exact_payload\": \"raw_float32\",\n"
         << "  \"exact_payload_bytes\": " << artifacts.exact_payload_bytes << ",\n"
         << "  \"generation_vector_capacity\": "
         << artifacts.ring.logical_block_limit << ",\n"
         << "  \"max_real_slot_utilization\": 0.6,\n"
         << "  \"real_bucket_size\": " << config.real_bucket_size << ",\n"
         << "  \"dummy_size\": " << config.dummy_size << ",\n"
         << "  \"evict_rate\": " << config.evict_rate << ",\n"
         << "  \"cluster\": {\"block_size\": " << artifacts.cluster.block_size
         << ", \"real_blocks\": " << artifacts.cluster.real_blocks
         << ", \"logical_block_limit\": " << artifacts.cluster.logical_block_limit
         << ", \"real_block_capacity\": " << artifacts.cluster.real_block_capacity
         << ", \"num_levels\": " << artifacts.cluster.num_levels
         << ", \"cached_levels\": " << artifacts.cluster.cached_levels
         << ", \"num_buckets\": " << artifacts.cluster.num_buckets
         << ", \"state_file\": \"cluster-client-state.bin\"},\n"
         << "  \"ring\": {\"block_size\": " << artifacts.ring.block_size
         << ", \"real_blocks\": " << artifacts.ring.real_blocks
         << ", \"logical_block_limit\": " << artifacts.ring.logical_block_limit
         << ", \"real_block_capacity\": " << artifacts.ring.real_block_capacity
         << ", \"num_levels\": " << artifacts.ring.num_levels
         << ", \"cached_levels\": " << artifacts.ring.cached_levels
         << ", \"num_buckets\": " << artifacts.ring.num_buckets
         << ", \"state_file\": \"ring-client-state.bin\"}\n"
         << "}\n";
    AtomicBinaryWriter output(path);
    const std::string text = json.str();
    output.write_bytes(text.data(), text.size());
    output.commit();
}

struct RingPlacementResult {
    std::vector<int> leaves;
    int sample_bucket = -1;
    int sample_slot = -1;
    int sample_index = -1;
};

RingPlacementResult place_ring_metadata(
        double_oram::RingORAM& ring,
        const ServiceIndex& index,
        uint32_t seed) {
    if (ring.bucket_size > 128) {
        throw std::invalid_argument("RingORAM bucket size exceeds the 128-slot metadata bitmap");
    }
    for (int bucket = 0; bucket < ring.num_buckets; ++bucket) {
        ring.metadata[bucket].count = 0;
        ring.metadata[bucket].valids.set();
        ring.metadata[bucket].block_ids.assign(static_cast<size_t>(ring.bucket_size), -1);
        ring.metadata[bucket].block_leaves.assign(static_cast<size_t>(ring.bucket_size), -1);
    }

    RingPlacementResult result;
    result.leaves.assign(index.n, -1);
    std::vector<int> used(static_cast<size_t>(ring.num_buckets), 0);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> leaf_distribution(0, ring.num_leaves - 1);

    for (uint32_t vector_id = 0; vector_id < index.n; ++vector_id) {
        bool placed = false;
        for (int attempt = 0; attempt < 4096 && !placed; ++attempt) {
            const int leaf = leaf_distribution(rng);
            for (int level = ring.num_levels - 1; level >= ring.num_cached_levels; --level) {
                const int bucket = ring.P(leaf, level);
                if (used[bucket] >= ring.real_bucket_size) continue;
                const int slot = used[bucket]++;
                ring.metadata[bucket].block_ids[slot] = static_cast<int>(vector_id);
                ring.metadata[bucket].block_leaves[slot] = leaf;
                result.leaves[vector_id] = leaf;
                if (result.sample_bucket < 0) {
                    result.sample_bucket = bucket;
                    result.sample_slot = slot;
                    result.sample_index = static_cast<int>(vector_id);
                }
                placed = true;
                break;
            }
        }
        if (!placed) {
            throw std::runtime_error("failed to place a RingORAM block below the cached tree top");
        }
    }
    return result;
}

void upload_ring_buckets(
        double_oram::RemoteRing& storage,
        const double_oram::RingORAM& ring,
        const ServiceIndex& index,
        const OramGenerationConfig& config) {
    std::vector<uint32_t> cluster_for_id(index.n, 0);
    std::vector<uint32_t> local_for_id(index.n, 0);
    for (uint32_t cluster = 0; cluster < index.num_clusters; ++cluster) {
        for (uint32_t local = 0; local < index.len[cluster]; ++local) {
            const uint32_t id = index.ids[index.start[cluster] + local];
            if (id >= index.n) throw std::runtime_error("IVF internal ID exceeds vector count");
            cluster_for_id[id] = cluster;
            local_for_id[id] = local;
        }
    }

    const size_t batch_buckets = buckets_per_upload(
        ring.block_size, ring.bucket_size, config.upload_batch_bytes);
    for (int begin = 0; begin < ring.num_buckets; begin += static_cast<int>(batch_buckets)) {
        const int end = std::min<int>(ring.num_buckets, begin + static_cast<int>(batch_buckets));
        std::map<int, std::vector<double_oram::Block*>> buckets;
        for (int bucket = begin; bucket < end; ++bucket) {
            std::vector<double_oram::Block*> blocks;
            blocks.reserve(static_cast<size_t>(ring.bucket_size));
            for (int slot = 0; slot < ring.bucket_size; ++slot) {
                const int vector_id = ring.metadata[bucket].block_ids[slot];
                if (vector_id < 0) {
                    blocks.push_back(new double_oram::Block(ring.block_size));
                } else {
                    const std::vector<uint8_t> payload = build_raw_float32_exact_oram_payload(
                        index, cluster_for_id[vector_id], local_for_id[vector_id]);
                    blocks.push_back(make_block(
                        ring.block_size, vector_id,
                        bytes_to_block_data(ring.block_size, payload)));
                }
            }
            buckets.emplace(bucket, std::move(blocks));
        }
        storage.WriteBucketBatchMapAsBlockRing(buckets, ring.bucket_size, false);
    }
}

struct ClusterPlacementResult {
    int sample_bucket = -1;
    int sample_slot = -1;
    int sample_index = -1;
};

ClusterPlacementResult place_cluster_metadata(
        double_oram::ClusterORAM& cluster,
        const ServiceIndex& index) {
    std::vector<int> used(static_cast<size_t>(cluster.num_buckets), 0);
    ClusterPlacementResult sample;
    int logical_address = 0;
    for (uint32_t cluster_id = 0; cluster_id < index.num_clusters; ++cluster_id) {
        for (uint32_t block_id = 0; block_id < index.cluster_num_blocks(cluster_id); ++block_id) {
            const int leaf = cluster.cluster_leaf(
                static_cast<int>(cluster_id), 0, static_cast<int>(block_id));
            bool placed = false;
            for (int level = cluster.num_levels - 1; level >= cluster.num_cached_levels; --level) {
                const int bucket = cluster.P(leaf, level);
                if (used[bucket] >= cluster.real_bucket_size) continue;
                const int slot = used[bucket]++;
                double_oram::ClusterORAMSlotMeta& metadata = cluster.metadata[bucket][slot];
                metadata.cluster_id = static_cast<int>(cluster_id);
                metadata.cluster_offset = static_cast<int>(block_id);
                metadata.leaf = leaf;
                metadata.valid = true;
                if (sample.sample_bucket < 0) {
                    sample.sample_bucket = bucket;
                    sample.sample_slot = slot;
                    sample.sample_index = logical_address;
                }
                placed = true;
                break;
            }
            if (!placed) {
                throw std::runtime_error("failed to place a ClusterORAM approximate block");
            }
            ++logical_address;
        }
    }
    return sample;
}

void upload_cluster_buckets(
        double_oram::RemoteRing& storage,
        const double_oram::ClusterORAM& cluster,
        const ServiceIndex& index,
        const std::vector<int>& ring_leaves,
        const OramGenerationConfig& config) {
    std::vector<int> cluster_starts(index.num_clusters, 0);
    int logical_address = 0;
    for (uint32_t cluster_id = 0; cluster_id < index.num_clusters; ++cluster_id) {
        cluster_starts[cluster_id] = logical_address;
        logical_address += static_cast<int>(index.cluster_num_blocks(cluster_id));
    }

    const size_t batch_buckets = buckets_per_upload(
        cluster.block_size, cluster.bucket_size, config.upload_batch_bytes);
    for (int begin = 0; begin < cluster.num_buckets; begin += static_cast<int>(batch_buckets)) {
        const int end = std::min<int>(cluster.num_buckets, begin + static_cast<int>(batch_buckets));
        std::map<int, std::vector<double_oram::Block*>> buckets;
        for (int bucket = begin; bucket < end; ++bucket) {
            std::vector<double_oram::Block*> blocks;
            blocks.reserve(static_cast<size_t>(cluster.bucket_size));
            for (int slot = 0; slot < cluster.bucket_size; ++slot) {
                const double_oram::ClusterORAMSlotMeta& metadata = cluster.metadata[bucket][slot];
                if (metadata.cluster_id < 0) {
                    blocks.push_back(new double_oram::Block(cluster.block_size));
                } else {
                    const int address = cluster_starts[metadata.cluster_id] + metadata.cluster_offset;
                    blocks.push_back(make_block(
                        cluster.block_size,
                        address,
                        make_cluster_block_data(
                            cluster.block_size,
                            index,
                            static_cast<uint32_t>(metadata.cluster_id),
                            static_cast<uint32_t>(metadata.cluster_offset),
                            ring_leaves)));
                }
            }
            buckets.emplace(bucket, std::move(blocks));
        }
        storage.WriteBucketBatchMapAsBlockRing(buckets, cluster.bucket_size, false);
    }
}

void validate_config(const OramGenerationConfig& config) {
    if (!config.enabled) throw std::invalid_argument("ORAM generation is disabled");
    if (config.storage_host.empty() || config.storage_port <= 0 || config.storage_port > 65535 ||
        config.real_bucket_size <= 0 || config.dummy_size < 0 || config.evict_rate <= 0 ||
        config.cached_levels < 0 || config.upload_batch_bytes == 0 ||
        config.real_bucket_size + config.dummy_size > 128) {
        throw std::invalid_argument("invalid ORAM generation configuration");
    }
}

struct LiveRecordEntry {
    int internal_id = -1;
    std::string serialized_line;
};

std::string json_escape_record_field(const std::string& value) {
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
                    throw std::invalid_argument("record ID contains a control character");
                }
                escaped.push_back(static_cast<char>(ch));
        }
    }
    return escaped;
}

std::string serialize_live_record(int internal_id, const VectorRecord& record) {
    return "{\"internal_id\":" + std::to_string(internal_id) +
           ",\"id\":\"" + json_escape_record_field(record.id) +
           "\",\"metadata\":" + record.metadata_json + "}";
}

std::unordered_map<std::string, LiveRecordEntry> load_live_records(const fs::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open initial generation record map: " + path.string());
    std::unordered_map<std::string, LiveRecordEntry> records;
    std::set<int> internal_ids;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) throw std::runtime_error("generation record map contains an empty line");
        const simple_json::Value value = simple_json::parse_text(line);
        const simple_json::Value* id = value.get("id");
        const simple_json::Value* internal = value.get("internal_id");
        if (!value.is_object() || id == nullptr || !id->is_string() || id->string_value.empty() ||
            internal == nullptr || !internal->is_number() || internal->number_value < 0.0 ||
            std::floor(internal->number_value) != internal->number_value ||
            internal->number_value > std::numeric_limits<int>::max()) {
            throw std::runtime_error("generation record map contains an invalid entry");
        }
        const int internal_id = static_cast<int>(internal->number_value);
        if (!internal_ids.insert(internal_id).second ||
            !records.emplace(id->string_value, LiveRecordEntry{internal_id, line}).second) {
            throw std::runtime_error("generation record map contains duplicate IDs");
        }
    }
    if (!input.eof()) throw std::runtime_error("cannot read initial generation record map");
    return records;
}

void save_live_records(
        const fs::path& path,
        const std::unordered_map<std::string, LiveRecordEntry>& records) {
    std::vector<const LiveRecordEntry*> ordered;
    ordered.reserve(records.size());
    for (const auto& item : records) ordered.push_back(&item.second);
    std::sort(ordered.begin(), ordered.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->internal_id < rhs->internal_id;
    });
    AtomicBinaryWriter output(path);
    for (const LiveRecordEntry* entry : ordered) {
        output.write_bytes(entry->serialized_line.data(), entry->serialized_line.size());
        const char newline = '\n';
        output.write_value(newline);
    }
    output.commit();
}

void write_replay_state(
        const fs::path& path,
        const OramGenerationFinalizeResult& result,
        size_t cluster_stash_blocks,
        size_t ring_stash_blocks) {
    std::ostringstream json;
    json << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"state\": \"validated\",\n"
         << "  \"vector_count\": " << result.vector_count << ",\n"
         << "  \"last_log_sequence\": " << result.last_sequence << ",\n"
         << "  \"replayed_records\": " << result.replayed_records << ",\n"
         << "  \"cluster_oram_real_blocks\": " << result.artifacts.cluster.real_blocks << ",\n"
         << "  \"ring_oram_real_blocks\": " << result.artifacts.ring.real_blocks << ",\n"
         << "  \"cluster_stash_blocks\": " << cluster_stash_blocks << ",\n"
         << "  \"ring_stash_blocks\": " << ring_stash_blocks << "\n"
         << "}\n";
    AtomicBinaryWriter output(path);
    const std::string text = json.str();
    output.write_bytes(text.data(), text.size());
    output.commit();
}

size_t cluster_stash_size(const double_oram::ClusterORAM& cluster) {
    size_t count = 0;
    for (const auto& group : cluster.cluster_stash) count += group.second.size();
    return count;
}

struct RingStashCleanup {
    double_oram::RingORAM& ring;
    ~RingStashCleanup() {
        for (auto& item : ring.mmstash) delete item.second;
        ring.mmstash.clear();
    }
};

} // namespace

uint64_t OramGenerationBuilder384::next_vector_capacity(uint64_t current_vector_count) {
    return compute_next_generation_vector_capacity(current_vector_count);
}

std::vector<int> OramClientStateStore::read_cluster_block_counts(
        const fs::path& path,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    BinaryReader input(path);
    const ClientStateHeader header = input.read_value<ClientStateHeader>();
    validate_client_state_header(
        header, kClusterClientStateMagic, StorageTreeKind::Cluster, layout, config);
    if (header.cluster_count == 0 ||
        header.cluster_count > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("ClusterORAM client state has an invalid cluster count");
    }

    std::vector<int> counts(header.cluster_count);
    input.read_vector(counts);
    uint64_t total = 0;
    for (int count : counts) {
        if (count < 0) {
            throw std::runtime_error("ClusterORAM client state has a negative block count");
        }
        total += static_cast<uint64_t>(count);
    }
    if (total != layout.real_blocks) {
        throw std::runtime_error("ClusterORAM block counts do not match the generation manifest");
    }
    return counts;
}

void OramClientStateStore::load_ring(
        const fs::path& path,
        double_oram::RingORAM& ring,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    BinaryReader input(path);
    const ClientStateHeader header = input.read_value<ClientStateHeader>();
    validate_client_state_header(
        header, kRingClientStateMagic, StorageTreeKind::Ring, layout, config);
    if (header.cluster_count != 0 || ring.block_size != layout.block_size ||
        ring.real_bucket_size != config.real_bucket_size ||
        ring.dummy_size != config.dummy_size || ring.evict_rate != config.evict_rate ||
        ring.num_levels != layout.num_levels || ring.num_cached_levels != layout.cached_levels ||
        ring.num_buckets != static_cast<int>(layout.num_buckets) ||
        ring.num_blocks != static_cast<int>(layout.logical_block_limit)) {
        throw std::runtime_error("RingORAM object does not match its persisted client state");
    }

    std::set<int> live_ids;
    for (int bucket = 0; bucket < ring.num_buckets; ++bucket) {
        double_oram::OramMetaData& metadata = ring.metadata[bucket];
        metadata.count = input.read_value<uint8_t>();
        metadata.valids.reset();
        for (int slot = 0; slot < ring.bucket_size; ++slot) {
            const uint8_t valid = input.read_value<uint8_t>();
            if (valid > 1) throw std::runtime_error("RingORAM metadata has an invalid valid bit");
            metadata.valids.set(static_cast<size_t>(slot), valid != 0);
        }
        metadata.block_ids.resize(static_cast<size_t>(ring.bucket_size));
        metadata.block_leaves.resize(static_cast<size_t>(ring.bucket_size));
        input.read_vector(metadata.block_ids);
        input.read_vector(metadata.block_leaves);
        for (int slot = 0; slot < ring.bucket_size; ++slot) {
            const int id = metadata.block_ids[slot];
            const int leaf = metadata.block_leaves[slot];
            if (id < -1 || id >= ring.num_blocks ||
                (id == -1 && leaf != -1) ||
                (id >= 0 && (leaf < 0 || leaf >= ring.num_leaves))) {
                throw std::runtime_error("RingORAM metadata contains an invalid block identity");
            }
            if (id >= 0 && metadata.valids[static_cast<size_t>(slot)] &&
                !live_ids.insert(id).second) {
                throw std::runtime_error("RingORAM client state contains duplicate live blocks");
            }
        }
    }

    for (auto& item : ring.mmstash) delete item.second;
    ring.mmstash.clear();
    ring.double_oram_leaf_cache.clear();
    const uint64_t stash_blocks = input.read_value<uint64_t>();
    if (stash_blocks > layout.real_blocks) {
        throw std::runtime_error("RingORAM stash count exceeds the number of real blocks");
    }
    for (uint64_t i = 0; i < stash_blocks; ++i) {
        const int leaf = input.read_value<int>();
        const int vector_id = input.read_value<int>();
        const int block_index = input.read_value<int>();
        if (leaf < 0 || leaf >= ring.num_leaves || vector_id < 0 ||
            vector_id >= ring.num_blocks || block_index != vector_id ||
            !live_ids.insert(vector_id).second) {
            throw std::runtime_error("RingORAM stash contains an invalid or duplicate block");
        }
        std::unique_ptr<double_oram::Block> block(new double_oram::Block(ring.block_size));
        block->index = block_index;
        input.read_vector(block->data);
        const std::pair<int, int> key{leaf, vector_id};
        if (!ring.mmstash.emplace(key, block.get()).second) {
            throw std::runtime_error("RingORAM stash contains a duplicate key");
        }
        block.release();
    }
    if (live_ids.size() != layout.real_blocks) {
        throw std::runtime_error("RingORAM tree and stash do not contain every real block");
    }
    ring.G = static_cast<int>(header.eviction_counter);
    ring.cnt_delayed_leaves = static_cast<int>(header.delayed_leaves);
    input.require_end();
}

void OramClientStateStore::load_cluster(
        const fs::path& path,
        double_oram::ClusterORAM& cluster,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    BinaryReader input(path);
    const ClientStateHeader header = input.read_value<ClientStateHeader>();
    validate_client_state_header(
        header, kClusterClientStateMagic, StorageTreeKind::Cluster, layout, config);
    if (header.cluster_count != cluster.cluster_block_counts.size() ||
        cluster.block_size != layout.block_size ||
        cluster.real_bucket_size != config.real_bucket_size ||
        cluster.dummy_size != config.dummy_size || cluster.evict_rate != config.evict_rate ||
        cluster.num_levels != layout.num_levels ||
        cluster.num_cached_levels != layout.cached_levels ||
        cluster.num_buckets != static_cast<int>(layout.num_buckets) ||
        cluster.num_blocks != static_cast<int>(layout.real_blocks)) {
        throw std::runtime_error("ClusterORAM object does not match its persisted client state");
    }

    std::vector<int> counts(header.cluster_count);
    input.read_vector(counts);
    if (counts != cluster.cluster_block_counts) {
        throw std::runtime_error("ClusterORAM object has different per-cluster block counts");
    }
    input.read_vector(cluster.cluster_counters);
    input.read_vector(cluster.bucket_read_counts);
    for (int count : cluster.bucket_read_counts) {
        if (count < 0) throw std::runtime_error("ClusterORAM has a negative bucket read count");
    }

    std::set<std::pair<int, int>> live_blocks;
    for (int bucket = 0; bucket < cluster.num_buckets; ++bucket) {
        for (int slot = 0; slot < cluster.bucket_size; ++slot) {
            double_oram::ClusterORAMSlotMeta& metadata = cluster.metadata[bucket][slot];
            metadata.cluster_id = input.read_value<int>();
            metadata.cluster_offset = input.read_value<int>();
            metadata.leaf = input.read_value<int>();
            const uint8_t valid = input.read_value<uint8_t>();
            if (valid > 1) throw std::runtime_error("ClusterORAM metadata has an invalid valid bit");
            metadata.valid = valid != 0;
            const bool dummy = metadata.cluster_id == -1;
            if ((dummy && (metadata.cluster_offset != -1 || metadata.leaf != -1)) ||
                (!dummy && (metadata.cluster_id < 0 ||
                 metadata.cluster_id >= static_cast<int>(counts.size()) ||
                 metadata.cluster_offset < 0 ||
                 metadata.cluster_offset >= counts[metadata.cluster_id] ||
                 metadata.leaf < 0 || metadata.leaf >= cluster.num_leaves))) {
                throw std::runtime_error("ClusterORAM metadata contains an invalid block identity");
            }
            if (!dummy && metadata.valid &&
                !live_blocks.emplace(metadata.cluster_id, metadata.cluster_offset).second) {
                throw std::runtime_error("ClusterORAM client state contains duplicate live blocks");
            }
        }
    }

    cluster.cluster_stash.clear();
    cluster.cluster_stash_indices.resize(counts.size());
    for (size_t cluster_id = 0; cluster_id < counts.size(); ++cluster_id) {
        cluster.cluster_stash_indices[cluster_id].assign(
            static_cast<size_t>(counts[cluster_id]), -1);
    }
    const uint64_t stash_blocks = input.read_value<uint64_t>();
    if (stash_blocks > layout.real_blocks) {
        throw std::runtime_error("ClusterORAM stash count exceeds the number of real blocks");
    }
    for (uint64_t i = 0; i < stash_blocks; ++i) {
        double_oram::ClusterORAMStashBlock stash;
        stash.cluster_id = input.read_value<int>();
        stash.cluster_offset = input.read_value<int>();
        stash.leaf = input.read_value<int>();
        stash.block.index = input.read_value<int>();
        if (stash.cluster_id < 0 || stash.cluster_id >= static_cast<int>(counts.size()) ||
            stash.cluster_offset < 0 || stash.cluster_offset >= counts[stash.cluster_id] ||
            stash.leaf < 0 || stash.leaf >= cluster.num_leaves || stash.block.index < 0 ||
            !live_blocks.emplace(stash.cluster_id, stash.cluster_offset).second) {
            throw std::runtime_error("ClusterORAM stash contains an invalid or duplicate block");
        }
        stash.block.block_size = cluster.block_size;
        stash.block.data.resize(static_cast<size_t>(cluster.block_size));
        input.read_vector(stash.block.data);
        std::vector<double_oram::ClusterORAMStashBlock>& group =
            cluster.cluster_stash[stash.cluster_id];
        group.push_back(std::move(stash));
        cluster.cluster_stash_indices[group.back().cluster_id][group.back().cluster_offset] =
            static_cast<int>(group.size()) - 1;
    }
    if (live_blocks.size() != layout.real_blocks) {
        throw std::runtime_error("ClusterORAM tree and stash do not contain every real block");
    }
    cluster.G = static_cast<int>(header.eviction_counter);
    cluster.cnt_delayed_leaves = static_cast<int>(header.delayed_leaves);
    input.require_end();
}

void OramClientStateStore::save_ring(
        const fs::path& path,
        const double_oram::RingORAM& ring,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    save_ring_client_state(path, ring, layout, config);
}

void OramClientStateStore::save_cluster(
        const fs::path& path,
        const double_oram::ClusterORAM& cluster,
        const OramTreeLayout& layout,
        const OramGenerationConfig& config) {
    save_cluster_client_state(path, cluster, layout, config);
}

void OramClientStateStore::save_active_generation(
        const fs::path& generation_directory,
        const double_oram::RingORAM& ring,
        const double_oram::ClusterORAM& cluster,
        const OramGenerationArtifacts& artifacts,
        const OramGenerationConfig& config,
        const std::vector<OramLiveRecord>& records,
        uint64_t vector_count,
        uint64_t last_log_sequence,
        uint64_t replayed_records) {
    if (records.size() != vector_count ||
        artifacts.ring.real_blocks > artifacts.ring.logical_block_limit ||
        artifacts.cluster.real_blocks > artifacts.cluster.logical_block_limit ||
        artifacts.cluster.real_blocks != static_cast<uint64_t>(cluster.num_blocks)) {
        throw std::runtime_error("active generation state summary is inconsistent");
    }
    std::unordered_map<std::string, LiveRecordEntry> serialized_records;
    std::set<uint32_t> internal_ids;
    for (const OramLiveRecord& record : records) {
        if (record.external_id.empty() || record.internal_id >= artifacts.ring.real_blocks ||
            !internal_ids.insert(record.internal_id).second) {
            throw std::runtime_error("active generation contains an invalid live record");
        }
        VectorRecord vector_record;
        vector_record.id = record.external_id;
        vector_record.metadata_json = record.metadata_json;
        if (!serialized_records.emplace(
                record.external_id,
                LiveRecordEntry{
                    static_cast<int>(record.internal_id),
                    serialize_live_record(static_cast<int>(record.internal_id), vector_record),
                }).second) {
            throw std::runtime_error("active generation contains duplicate external IDs");
        }
    }

    save_ring_client_state(
        generation_directory / "ring-client-state.bin", ring, artifacts.ring, config);
    save_cluster_client_state(
        generation_directory / "cluster-client-state.bin", cluster, artifacts.cluster, config);
    write_client_manifest(
        generation_directory / "client-manifest.json", artifacts, config);
    save_live_records(generation_directory / "live-records.jsonl", serialized_records);

    OramGenerationFinalizeResult result;
    result.vector_count = vector_count;
    result.last_sequence = last_log_sequence;
    result.replayed_records = replayed_records;
    result.artifacts = artifacts;
    write_replay_state(
        generation_directory / "replay-state.json",
        result,
        cluster_stash_size(cluster),
        ring.mmstash.size());
}

OramGenerationFinalizeResult OramGenerationFinalizer384::finalize(
        CollectionCatalog& catalog,
        CollectionDataStore& data_store,
        const std::string& collection,
        const std::string& generation,
        const fs::path& generation_directory,
        const AeadKey& generation_key,
        uint64_t snapshot_last_sequence,
        OramGenerationArtifacts artifacts,
        const OramGenerationConfig& config) {
    validate_config(config);
    const CollectionManifest pending = catalog.get_collection(collection);
    if (pending.pending_generation != generation ||
        pending.pending_generation_stage != GenerationStage::OramReady) {
        throw std::logic_error("ORAM finalization requires the matching OramReady generation");
    }
    if (artifacts.cluster_count == 0 || artifacts.ring.real_blocks == 0 ||
        artifacts.ring.real_blocks > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("invalid ORAM generation artifacts for finalization");
    }

    Matrix<float> centroids((generation_directory / "centroids.fvecs").string());
    Matrix<float> projection((generation_directory / "projection.fvecs").string());
    if (centroids.n != artifacts.cluster_count || centroids.d != kServiceVectorDimension) {
        throw std::runtime_error("generation centroid table does not match ORAM artifacts");
    }
    std::unordered_map<std::string, LiveRecordEntry> live_records =
        load_live_records(generation_directory / "records.jsonl");
    if (live_records.size() != artifacts.ring.real_blocks) {
        throw std::runtime_error("initial record map does not match RingORAM real block count");
    }

    const double_oram::RingOramConfig ring_config = make_ring_config(artifacts.ring, config);
    StorageConnection ring_connection = StorageConnection::connect(
        config.storage_host,
        config.storage_port,
        make_handshake(StorageAction::Open, StorageTreeKind::Ring,
                       collection, generation, artifacts.ring, config));
    std::unique_ptr<double_oram::RemoteRing> ring_storage =
        ring_connection.make_remote_ring(ring_config, false, generation_key);
    double_oram::RandomForOram ring_random;
    double_oram::RingORAM ring(
        ring_storage.get(), &ring_random, ring_config, artifacts.ring.num_levels,
        artifacts.ring.cached_levels, true, true);
    RingStashCleanup ring_stash_cleanup{ring};
    OramClientStateStore::load_ring(
        generation_directory / "ring-client-state.bin", ring, artifacts.ring, config);

    const std::vector<int> cluster_counts = OramClientStateStore::read_cluster_block_counts(
        generation_directory / "cluster-client-state.bin", artifacts.cluster, config);
    const double_oram::RingOramConfig cluster_config = make_ring_config(artifacts.cluster, config);
    StorageConnection cluster_connection = StorageConnection::connect(
        config.storage_host,
        config.storage_port,
        make_handshake(StorageAction::Open, StorageTreeKind::Cluster,
                       collection, generation, artifacts.cluster, config));
    std::unique_ptr<double_oram::RemoteRing> cluster_storage =
        cluster_connection.make_remote_ring(cluster_config, false, generation_key);
    double_oram::RandomForOram cluster_random;
    double_oram::ClusterORAM cluster_oram(
        cluster_storage.get(), &cluster_random, cluster_config,
        artifacts.cluster.num_levels, cluster_counts);
    OramClientStateStore::load_cluster(
        generation_directory / "cluster-client-state.bin",
        cluster_oram,
        artifacts.cluster,
        config);

    const double_oram::DoubleORAMConfig double_config(
        static_cast<int>(artifacts.cluster_count),
        static_cast<int>(ApproxLayout::kPlainBytes),
        static_cast<int>(kServiceVectorDimension),
        static_cast<int>(kVectorsPerApproxBlock),
        2,
        0,
        double_oram::DoubleORAMPathBlockLayout::AnnBatch);
    double_oram::DoubleORAM double_oram_client(cluster_oram, ring, double_config);

    OramGenerationFinalizeResult result;
    result.artifacts = artifacts;
    uint64_t replay_cursor = snapshot_last_sequence;
    auto replay = [&](const std::vector<VectorDelta>& deltas) {
        for (const VectorDelta& delta : deltas) {
            if (delta.sequence <= replay_cursor) continue;
            if (result.artifacts.ring.real_blocks >= result.artifacts.ring.logical_block_limit ||
                result.artifacts.ring.real_blocks >=
                    static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                throw std::runtime_error(
                    "RingORAM insertion reserve was exhausted before generation activation");
            }
            const int internal_id = static_cast<int>(result.artifacts.ring.real_blocks);
            const OnlineQuantizedVector384 quantized = OnlineVectorCodec384::quantize(
                delta.record, centroids, projection);
            double_oram_client.insert_ann_vector(
                quantized.target_cluster,
                quantized.candidate_clusters,
                internal_id,
                quantized.approximate,
                OnlineVectorCodec384::raw_float32_exact_payload(delta.record, internal_id),
                static_cast<int>(quantized.candidate_clusters.size()));
            result.artifacts.ring.real_blocks++;
            result.artifacts.cluster.real_blocks = static_cast<uint64_t>(cluster_oram.num_blocks);
            live_records[delta.record.id] = {
                internal_id,
                serialize_live_record(internal_id, delta.record),
            };
            replay_cursor = delta.sequence;
            result.replayed_records++;

            // Follow the normal query-end schedule after every replayed insert.
            // Blocks that cannot be placed remain in their respective stashes.
            double_oram_client.evict_cluster();
            double_oram_client.evict_ring_and_clear_cache();
        }
    };

    replay(data_store.read_latest_after(collection, replay_cursor));
    data_store.with_latest_after_locked(
        collection,
        replay_cursor,
        [&](const VectorDeltaBarrier& barrier) {
            replay(barrier.deltas);
            if (live_records.size() != barrier.vector_count) {
                throw std::runtime_error(
                    "replayed live-record map does not match the vector log at the write barrier");
            }

            // One final normal eviction reduces each stash where the scheduled
            // paths permit it. A non-empty residual stash is a valid checkpoint.
            double_oram_client.evict_cluster();
            double_oram_client.evict_ring_and_clear_cache();
            result.artifacts.cluster.real_blocks = static_cast<uint64_t>(cluster_oram.num_blocks);

            ring_storage.reset();
            ring_connection.close();
            cluster_storage.reset();
            cluster_connection.close();

            OramClientStateStore::save_ring(
                generation_directory / "ring-client-state.bin",
                ring,
                result.artifacts.ring,
                config);
            OramClientStateStore::save_cluster(
                generation_directory / "cluster-client-state.bin",
                cluster_oram,
                result.artifacts.cluster,
                config);
            write_client_manifest(
                generation_directory / "client-manifest.json", result.artifacts, config);
            save_live_records(generation_directory / "live-records.jsonl", live_records);

            result.vector_count = barrier.vector_count;
            result.last_sequence = barrier.last_sequence;
            write_replay_state(
                generation_directory / "replay-state.json",
                result,
                cluster_stash_size(cluster_oram),
                ring.mmstash.size());
            catalog.validate_and_activate_generation(
                collection, generation, barrier.vector_count);
        });
    return result;
}

OramGenerationArtifacts OramGenerationBuilder384::build(
        const std::string& collection,
        const std::string& generation,
        const fs::path& generation_directory,
        const AeadKey& generation_key,
        const OramGenerationConfig& config) {
    validate_config(config);
    ServiceIndex index;
    index.load((generation_directory / "ivf_rabitq.index").string());
    if (index.n == 0 || index.n > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("ORAM generation requires 1..INT_MAX vectors");
    }

    uint64_t approximate_blocks = 0;
    std::vector<int> cluster_block_counts(index.num_clusters, 0);
    for (uint32_t cluster = 0; cluster < index.num_clusters; ++cluster) {
        cluster_block_counts[cluster] = static_cast<int>(index.cluster_num_blocks(cluster));
        approximate_blocks += index.cluster_num_blocks(cluster);
    }

    const uint64_t target_vector_count = next_vector_capacity(index.n);
    const uint64_t ring_logical_limit = target_vector_count;
    const uint64_t cluster_logical_limit =
        cluster_block_capacity_at_vector_target(index, target_vector_count);
    const int cluster_block_size =
        2 + 2 * static_cast<int>(ServiceIndex::kBlockVectors) +
        packed_int_count(ApproxLayout::kPlainBytes);
    const int ring_block_size = packed_int_count(RawExactLayout::kPlainBytes);

    OramGenerationArtifacts artifacts;
    artifacts.cluster = choose_tree_layout(
        approximate_blocks, cluster_logical_limit, cluster_block_size, config);
    artifacts.ring = choose_tree_layout(
        index.n, ring_logical_limit, ring_block_size, config);
    artifacts.cluster_count = index.num_clusters;
    artifacts.exact_payload_bytes = static_cast<uint32_t>(RawExactLayout::kPlainBytes);

    double_oram::RandomForOram random;

    const double_oram::RingOramConfig ring_config = make_ring_config(artifacts.ring, config);
    StorageConnection ring_connection = StorageConnection::connect(
        config.storage_host,
        config.storage_port,
        make_handshake(StorageAction::Create, StorageTreeKind::Ring,
                       collection, generation, artifacts.ring, config));
    std::unique_ptr<double_oram::RemoteRing> ring_storage =
        ring_connection.make_remote_ring(ring_config, false, generation_key);
    double_oram::RingORAM ring(
        ring_storage.get(), &random, ring_config, artifacts.ring.num_levels,
        artifacts.ring.cached_levels, true, true);
    const RingPlacementResult ring_placement = place_ring_metadata(ring, index, config.seed);
    upload_ring_buckets(*ring_storage, ring, index, config);
    save_ring_client_state(
        generation_directory / "ring-client-state.bin", ring, artifacts.ring, config);
    checkpoint_and_verify(
        collection, generation, StorageTreeKind::Ring, artifacts.ring, config,
        generation_key, ring_connection, ring_storage,
        ring_placement.sample_bucket, ring_placement.sample_slot, ring_placement.sample_index);

    const double_oram::RingOramConfig cluster_config = make_ring_config(artifacts.cluster, config);
    StorageConnection cluster_connection = StorageConnection::connect(
        config.storage_host,
        config.storage_port,
        make_handshake(StorageAction::Create, StorageTreeKind::Cluster,
                       collection, generation, artifacts.cluster, config));
    std::unique_ptr<double_oram::RemoteRing> cluster_storage =
        cluster_connection.make_remote_ring(cluster_config, false, generation_key);
    double_oram::ClusterORAM cluster(
        cluster_storage.get(), &random, cluster_config, artifacts.cluster.num_levels,
        cluster_block_counts);
    const ClusterPlacementResult cluster_placement = place_cluster_metadata(cluster, index);
    upload_cluster_buckets(*cluster_storage, cluster, index, ring_placement.leaves, config);
    save_cluster_client_state(
        generation_directory / "cluster-client-state.bin", cluster, artifacts.cluster, config);
    checkpoint_and_verify(
        collection, generation, StorageTreeKind::Cluster, artifacts.cluster, config,
        generation_key, cluster_connection, cluster_storage,
        cluster_placement.sample_bucket, cluster_placement.sample_slot,
        cluster_placement.sample_index);

    write_client_manifest(generation_directory / "client-manifest.json", artifacts, config);
    return artifacts;
}

} // namespace qcovert
