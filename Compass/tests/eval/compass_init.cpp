#include "ArgMapping.h"
#include "evp.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <sys/time.h>
#include <vector>

#include "RandForOramInterface.h"
#include "RandomForOram.h"

#include "../../src/faiss/impl/HNSW.h"
#include "../../src/faiss/index_io.h"
#include "../../src/node.h"

#include "../utils/config_parser.h"

using namespace std;
namespace fs = std::filesystem;

string dataset = "";

namespace {

double elapsed() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

inline void tprint(const string& s, double t0) {
    cout << "[" << fixed << setprecision(3) << (elapsed() - t0) * 1000.0 << " ms] " << s;
}

bool has_suffix(const string& value, const string& suffix) {
    return value.size() >= suffix.size()
            && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void checked_read(FILE* f, void* ptr, size_t bytes, const string& what) {
    size_t done = 0;
    unsigned char* out = static_cast<unsigned char*>(ptr);
    while (done < bytes) {
        const size_t nr = fread(out + done, 1, bytes - done, f);
        if (nr == 0) {
            if (ferror(f)) {
                perror(("read " + what).c_str());
            } else {
                cerr << "Unexpected EOF while reading " << what << endl;
            }
            abort();
        }
        done += nr;
    }
}

void checked_write(FILE* f, const void* ptr, size_t bytes, const string& what) {
    size_t done = 0;
    const unsigned char* in = static_cast<const unsigned char*>(ptr);
    while (done < bytes) {
        const size_t nw = fwrite(in + done, 1, bytes - done, f);
        if (nw == 0) {
            perror(("write " + what).c_str());
            abort();
        }
        done += nw;
    }
}

template <typename T>
void checked_write_scalar(FILE* f, const T& value, const string& what) {
    checked_write(f, &value, sizeof(T), what);
}

class SequentialBaseReader {
public:
    explicit SequentialBaseReader(const string& path)
            : path_(path), f_(nullptr), is_bigann_(has_suffix(path, ".bin")),
              d_(0), available_n_(0), elem_size_(0), row_(0) {
        f_ = fopen(path.c_str(), "rb");
        if (!f_) {
            fprintf(stderr, "could not open %s\n", path.c_str());
            perror("");
            abort();
        }

        if (is_bigann_) {
            uint32_t n32 = 0;
            uint32_t d32 = 0;
            checked_read(f_, &n32, sizeof(uint32_t), path + " header n");
            checked_read(f_, &d32, sizeof(uint32_t), path + " header d");
            if (n32 == 0 || d32 == 0 || d32 >= 1000000) {
                cerr << path << " has invalid BigANN header: n=" << n32
                     << " d=" << d32 << endl;
                abort();
            }

            const uint64_t header_count =
                    static_cast<uint64_t>(n32) * static_cast<uint64_t>(d32);
            const uint64_t payload_size = fs::file_size(path) - 2 * sizeof(uint32_t);
            if (payload_size == header_count * sizeof(uint8_t)) {
                available_n_ = n32;
                elem_size_ = sizeof(uint8_t);
            } else if (payload_size == header_count * sizeof(float)) {
                available_n_ = n32;
                elem_size_ = sizeof(float);
            } else if (payload_size % d32 == 0) {
                available_n_ = payload_size / d32;
                elem_size_ = sizeof(uint8_t);
            } else if (payload_size % (static_cast<uint64_t>(d32) * sizeof(float)) == 0) {
                available_n_ = payload_size / (static_cast<uint64_t>(d32) * sizeof(float));
                elem_size_ = sizeof(float);
            } else {
                cerr << path << " has unsupported BigANN payload size: "
                     << payload_size << endl;
                abort();
            }
            d_ = d32;
            u8_buffer_.resize(d_);
        } else {
            int d = 0;
            checked_read(f_, &d, sizeof(int), path + " first dim");
            if (d <= 0 || d >= 1000000) {
                cerr << path << " has unreasonable fvecs dim: " << d << endl;
                abort();
            }
            const uint64_t row_bytes = static_cast<uint64_t>(d + 1) * sizeof(float);
            const uint64_t file_bytes = fs::file_size(path);
            if (file_bytes % row_bytes != 0) {
                cerr << path << " has invalid fvecs size " << file_bytes
                     << " for dim " << d << endl;
                abort();
            }
            d_ = d;
            available_n_ = file_bytes / row_bytes;
            fseek(f_, 0, SEEK_SET);
        }
    }

    ~SequentialBaseReader() {
        if (f_) {
            fclose(f_);
        }
    }

    size_t dim() const {
        return d_;
    }

    size_t available_rows() const {
        return available_n_;
    }

    void read_next(vector<float>& out) {
        if (row_ >= available_n_) {
            cerr << "Base reader exhausted at row " << row_ << " for " << path_ << endl;
            abort();
        }
        out.resize(d_);
        if (is_bigann_) {
            if (elem_size_ == sizeof(uint8_t)) {
                checked_read(f_, u8_buffer_.data(), d_ * sizeof(uint8_t), path_ + " row");
                for (size_t i = 0; i < d_; i++) {
                    out[i] = static_cast<float>(u8_buffer_[i]);
                }
            } else {
                checked_read(f_, out.data(), d_ * sizeof(float), path_ + " row");
            }
        } else {
            int row_d = 0;
            checked_read(f_, &row_d, sizeof(int), path_ + " row dim");
            if (row_d != static_cast<int>(d_)) {
                cerr << path_ << " row " << row_ << " has dim " << row_d
                     << ", expected " << d_ << endl;
                abort();
            }
            checked_read(f_, out.data(), d_ * sizeof(float), path_ + " row");
        }
        row_++;
    }

private:
    string path_;
    FILE* f_;
    bool is_bigann_;
    size_t d_;
    size_t available_n_;
    size_t elem_size_;
    size_t row_;
    vector<uint8_t> u8_buffer_;
};

OptNode make_opt_node(
        faiss::IndexHNSW* index,
        node_id_t nid,
        int layer,
        vector<float>& coords) {
    OptNode on;
    on.set_id(nid);
    on.set_layer(layer);

    size_t begin = 0;
    size_t end = 0;
    index->hnsw.neighbor_range(nid, layer, &begin, &end);
    vector<node_id_t> nb_list(
            index->hnsw.neighbors.begin() + begin,
            index->hnsw.neighbors.begin() + end);
    on.set_neighbors(nb_list);

    if (layer >= 1) {
        index->hnsw.neighbor_range(nid, layer - 1, &begin, &end);
        vector<node_id_t> next_layer_nb_list(
                index->hnsw.neighbors.begin() + begin,
                index->hnsw.neighbors.begin() + end);
        if (layer == 1) {
            next_layer_nb_list.resize(OptNode::n_neighbors / 2);
        }
        on.set_next_layer_neighbors(next_layer_nb_list);
    }

    on.set_coord(coords);
    return on;
}

class StreamingRingInitializer {
public:
    StreamingRingInitializer(
            int ntotal,
            int nlevels,
            RingOramConfig config,
            RandForOramInterface* rand)
            : ntotal_(ntotal), nlevels_(nlevels), config_(config), rand_(rand), graph_cnt_(0) {
        const size_t map_entries = static_cast<size_t>(ntotal_) * static_cast<size_t>(nlevels_);
        node_to_block_.assign(map_entries, 0);
        graph_position_map_.reserve(ntotal_);
        real_slots_.assign(
                static_cast<size_t>(config_.num_buckets) *
                        static_cast<size_t>(config_.real_bucket_size),
                nullptr);
    }

    ~StreamingRingInitializer() {
        for (Block* block : real_slots_) {
            delete block;
        }
    }

    block_id_t add_block_with_list(OptNode* on, const vector<node_id_t>& nlist) {
        if (static_cast<size_t>(graph_cnt_) >= real_slots_.size()) {
            cerr << "ORAM real-slot capacity exhausted: graph_cnt=" << graph_cnt_
                 << " real_slots=" << real_slots_.size() << endl;
            abort();
        }
        const block_id_t bid = graph_cnt_++;
        const int leaf = oram_insert(bid, on->get_int_data(), on->get_int_size());
        graph_position_map_.push_back(leaf);

        const int layer = on->get_layer();
        for (node_id_t nid : nlist) {
            node_to_block_[static_cast<size_t>(nid) * static_cast<size_t>(nlevels_) + layer] = bid;
        }
        return bid;
    }

    void save_block_mapping(const char* fname) {
        FILE* f = fopen(fname, "wb");
        if (!f) {
            fprintf(stderr, "could not open %s\n", fname);
            perror("");
            abort();
        }
        checked_write(
                f,
                node_to_block_.data(),
                node_to_block_.size() * sizeof(block_id_t),
                fname);
        fclose(f);
    }

    void save_position_map(const char* fname) {
        FILE* f = fopen(fname, "wb");
        if (!f) {
            fprintf(stderr, "could not open %s\n", fname);
            perror("");
            abort();
        }
        const int s = static_cast<int>(graph_position_map_.size());
        checked_write_scalar(f, s, fname);
        checked_write(
                f,
                graph_position_map_.data(),
                graph_position_map_.size() * sizeof(int),
                fname);
        fclose(f);
    }

    void write_encrypted_buckets_and_metadata(
            const char* buckets_path,
            const char* metadata_path,
            bool integrity) {
        if (integrity) {
            cerr << "Streaming compass_init currently supports integrity=false only." << endl;
            abort();
        }

        FILE* buckets = fopen(buckets_path, "wb");
        if (!buckets) {
            fprintf(stderr, "could not open %s\n", buckets_path);
            perror("");
            abort();
        }
        FILE* metadata = fopen(metadata_path, "wb");
        if (!metadata) {
            fprintf(stderr, "could not open %s\n", metadata_path);
            perror("");
            abort();
        }

        checked_write_scalar(buckets, config_.num_buckets, buckets_path);
        const int sb_size = SBucket::getCipherSize();
        checked_write_scalar(buckets, sb_size, buckets_path);
        checked_write_scalar(buckets, integrity, buckets_path);

        const size_t total_slots =
                static_cast<size_t>(config_.num_buckets) * static_cast<size_t>(config_.bucket_size);
        if (total_slots > static_cast<size_t>(numeric_limits<int>::max())) {
            cerr << "Too many ORAM slots for metadata int header: " << total_slots << endl;
            abort();
        }
        const int metadata_entries = static_cast<int>(total_slots);
        checked_write_scalar(metadata, metadata_entries, metadata_path);

        const int per_block_size = (1 + config_.block_size) * sizeof(int);
        vector<unsigned char> payload(SBucket::getCipherSize());
        vector<unsigned char> ciphertext(SBucket::getCipherSize());
        Block dummy(config_.block_size);

        for (int bucket_id = 0; bucket_id < config_.num_buckets; bucket_id++) {
            for (int j = 0; j < config_.bucket_size; j++) {
                Block* block = nullptr;
                if (j < config_.real_bucket_size) {
                    const size_t real_slot =
                            static_cast<size_t>(bucket_id) * config_.real_bucket_size + j;
                    block = real_slots_[real_slot];
                }

                int block_id = -1;
                if (block) {
                    block_id = block->index;
                    write_encrypted_block(buckets, *block, per_block_size, payload, ciphertext);
                    delete block;
                    real_slots_[static_cast<size_t>(bucket_id) * config_.real_bucket_size + j] = nullptr;
                } else {
                    write_encrypted_block(buckets, dummy, per_block_size, payload, ciphertext);
                }
                checked_write_scalar(metadata, block_id, metadata_path);
            }

            if (bucket_id % 100000 == 0) {
                cout << "-> Encrypted buckets: " << bucket_id << " / "
                     << config_.num_buckets << endl;
            }
        }

        fclose(metadata);
        fclose(buckets);
    }

    int graph_block_count() const {
        return graph_cnt_;
    }

private:
    int P(int leaf, int level) const {
        return (1 << level) - 1 + (leaf >> (config_.num_levels - level - 1));
    }

    int oram_insert(int index, int* data, int len) {
        assert(len <= config_.block_size);
        Block b(config_.block_size);
        b.index = index;
        for (int i = 0; i < len; i++) {
            b.data[i] = data[i];
        }

        while (true) {
            const int leaf = rand_->getRandomLeafWithBound(config_.num_leaves);
            for (int l = config_.num_levels - 1; l >= 0; l--) {
                const int bucket_offset = P(leaf, l);
                for (int i = 0; i < config_.real_bucket_size; i++) {
                    const size_t real_slot =
                            static_cast<size_t>(bucket_offset) * config_.real_bucket_size + i;
                    if (real_slots_[real_slot] == nullptr) {
                        real_slots_[real_slot] = new Block(b);
                        return leaf;
                    }
                }
            }
        }
    }

    void write_encrypted_block(
            FILE* out,
            Block& block,
            int per_block_size,
            vector<unsigned char>& payload,
            vector<unsigned char>& ciphertext) {
        fill(payload.begin(), payload.end(), 0);
        block.to_ptr(payload.data());
        const int ctx_len = encrypt_wrapper(
                payload.data(),
                static_cast<size_t>(per_block_size) * static_cast<size_t>(Bucket::getMaxSize()),
                ciphertext.data());
        assert(ctx_len == SBucket::getCipherSize());
        checked_write(out, ciphertext.data(), ciphertext.size(), "encrypted bucket slot");
    }

    int ntotal_;
    int nlevels_;
    RingOramConfig config_;
    RandForOramInterface* rand_;
    int graph_cnt_;
    vector<block_id_t> node_to_block_;
    vector<int> graph_position_map_;
    vector<Block*> real_slots_;
};

} // namespace

int main(int argc, char** argv) {
    std::string config_path = compass_default_config();
    ArgMapping amap;
    amap.arg("c", config_path, "Configuration JSON (default: tests/config/config_ring.json)");
    amap.arg("d", dataset, "Dataset: [sift, deep1M, sift10M, sift100M, trip, msmarco, laion]");
    amap.parse(argc, argv);

    const double t0 = elapsed();

    Metadata md;

    cout << "-> Parsing json..." << endl;
    if (parseJson(config_path, md, dataset) != 0) {
        cerr << "Early exit..." << endl;
        return 0;
    }

    if (md.integrity) {
        cerr << "compass_init streaming mode currently supports integrity=false only; dataset "
             << dataset << " has integrity=true." << endl;
        return 1;
    }

    cout << "-> Loading files..." << endl;

    RingOramConfig config(
            md.block_size,
            md.real_bucket_size,
            md.dummy_size,
            md.evict_rate,
            md.base_size,
            md.num_levels,
            md.oram_cached_levels);

    OptNode::n_neighbors = 2 * md.M;
    OptNode::dim = md.dim;

    Bucket::setMaxSize(1);
    SBucket::setCipherSize(compute_ctx_len((1 + md.block_size) * sizeof(int)));

    cout << "-> Loading HNSW index: " << md.index_path << endl;
    faiss::IndexHNSW* index = faiss::read_index(md.index_path.c_str(), 0);
    cout << "-> Loaded HNSW index, ntotal=" << index->ntotal
         << ", max_level=" << index->hnsw.max_level << endl;

    SequentialBaseReader base_reader(md.base_path);
    cout << "-> Base reader: " << md.base_path
         << " available_rows=" << base_reader.available_rows()
         << ", dim=" << base_reader.dim()
         << ", configured_rows=" << md.base_size << endl;

    if (base_reader.dim() != static_cast<size_t>(md.dim)) {
        cerr << "Base vector dimension mismatch: config dim=" << md.dim
             << ", file dim=" << base_reader.dim() << endl;
        return 1;
    }
    if (base_reader.available_rows() < static_cast<size_t>(md.base_size)) {
        cerr << "Base file has only " << base_reader.available_rows()
             << " available rows, requested " << md.base_size << endl;
        return 1;
    }
    if (index->ntotal != static_cast<faiss::idx_t>(md.base_size)) {
        cerr << "HNSW index size mismatch: index ntotal=" << index->ntotal
             << ", config base_size=" << md.base_size << endl;
        return 1;
    }

    fs::create_directories(fs::path(md.buckets_path).parent_path());
    fs::create_directories(fs::path(md.pos_map_path).parent_path());

    RandForOramInterface* random = new RandomForOram();
    StreamingRingInitializer initializer(
            md.base_size,
            index->hnsw.max_level + 1,
            config,
            random);

    FILE* graph_cache = fopen(md.graph_cache_path.c_str(), "wb");
    if (!graph_cache) {
        fprintf(stderr, "could not open %s\n", md.graph_cache_path.c_str());
        perror("");
        return 1;
    }

    cout << "-> Streaming graph_cache and ORAM plaintext slots..." << endl;
    vector<float> coords;
    vector<node_id_t> singleton;
    singleton.reserve(1);
    for (node_id_t nid = 0; nid < index->ntotal; nid++) {
        base_reader.read_next(coords);
        for (int l = 0; l < index->hnsw.levels[nid]; l++) {
            OptNode on = make_opt_node(index, nid, l, coords);

            if (l >= md.ef_lowest_cached_layer) {
                checked_write(
                        graph_cache,
                        on.data.data(),
                        on.data.size() * sizeof(int),
                        md.graph_cache_path);
            }

            singleton.clear();
            singleton.push_back(nid);
            initializer.add_block_with_list(&on, singleton);
        }

        if (nid % 100000 == 0) {
            cout << "-> Done streaming nodes: " << nid << " / " << index->ntotal << endl;
        }
    }
    fclose(graph_cache);

    cout << "-> Graph blocks inserted: " << initializer.graph_block_count() << endl;
    cout << "-> Saving block_mapping and position_map..." << endl;
    initializer.save_block_mapping(md.block_map_path.c_str());
    initializer.save_position_map(md.pos_map_path.c_str());

    cout << "-> Encrypting ORAM Tree directly to disk..." << endl;
    initializer.write_encrypted_buckets_and_metadata(
            md.buckets_path.c_str(),
            md.metadata_path.c_str(),
            md.integrity);

    delete random;
    delete index;

    tprint("compass_init done\n", t0);
    return 0;
}
