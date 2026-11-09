//
// oram_initializer_low: builds the auxiliary ORAMs used by the
// low-client-storage search path:
//   1) PQ-ORAM        : stores the PQ compressed embeddings (Hints)
//   2) position-map ORAM : stores the main graph ORAM's position map
//
// The main graph ORAM itself is built by the existing `oram_initializer`.
//

#include "ArgMapping.h"
#include "defaults.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "config_parser.h"
#include "raw_oram_builder.h"
#include "OramInterface.h"

using namespace std;

string dataset;
string config_path;

static int num_levels_for_blocks(int num_blocks, int real_bucket_size, int headroom = 2) {
    long long need = (long long)ceil((double)headroom * num_blocks / real_bucket_size);
    int L = 1;
    while (((1LL << L) - 1) < need)
        L++;
    return L;
}

static void set_globals(int block_size_ints) {
    static bool bucket_initialized = false;
    if (!bucket_initialized) {
        Bucket::setMaxSize(1);
        bucket_initialized = true;
    }
    SBucket::resetState();
    SBucket::setCipherSize(compute_ctx_len((1 + block_size_ints) * (int)sizeof(int)));
}

int main(int argc, char **argv) {
    ArgMapping amap;
    amap.arg("d", dataset, "Dataset: [sift, deep1m, sift10m, msmarco1m]");
    amap.arg("c", config_path, "Path to config file");
    amap.parse(argc, argv);

    Metadata md;
    if (parseJson(config_path, md, dataset) != 0) {
        cerr << "Error: Failed to parse JSON configuration." << endl;
        return EXIT_FAILURE;
    }

    if (!md.low_client_storage) {
        cerr << "Error: low_client_storage is not enabled in config." << endl;
        return EXIT_FAILURE;
    }
    if (md.pq_oram_buckets_path.empty()) {
        cerr << "Error: pq_oram paths not set in config." << endl;
        return EXIT_FAILURE;
    }
    if (md.recursive_posmap && md.posmap_oram_buckets_path.empty()) {
        cerr << "Error: recursive_posmap is enabled but posmap_oram paths are not set." << endl;
        return EXIT_FAILURE;
    }

    const int Z = md.real_bucket_size;
    const int S = md.dummy_size;
    // ---------------- PQ ORAM ----------------
    {
        string pq_compressed = md.index_path_prefix + "_pq_compressed.bin";
        FILE *f = fopen(pq_compressed.c_str(), "rb");
        if (!f) { perror(pq_compressed.c_str()); return EXIT_FAILURE; }
        uint32_t n, nchunks;
        fread(&n, 1, sizeof(uint32_t), f);
        fread(&nchunks, 1, sizeof(uint32_t), f);
        size_t total_bytes = (size_t)n * nchunks;
        if (md.pq_bytes != 0 && (int)nchunks != md.pq_bytes) {
            cerr << "pq_bytes mismatch: config=" << md.pq_bytes << " file=" << nchunks << endl;
            fclose(f);
            return EXIT_FAILURE;
        }
        md.pq_bytes = (int)nchunks;

        int block_bytes = md.pq_oram_block_size_bytes;
        int block_ints = block_bytes / 4;
        int codes_per_block = block_bytes / (int)nchunks;
        if (codes_per_block <= 0) {
            cerr << "PQ ORAM block too small for " << nchunks << " PQ bytes" << endl;
            fclose(f);
            return EXIT_FAILURE;
        }
        int num_blocks = (int)ceil((double)n / codes_per_block);
        int num_levels = num_levels_for_blocks(num_blocks, Z);
        md.pq_oram_num_blocks = num_blocks;
        md.pq_oram_num_levels = num_levels;
        set_globals(block_ints);

        RingOramConfig cfg(block_ints, Z, S, md.evict_rate, num_blocks, num_levels, 0);

        vector<uint8_t> codes(total_bytes);
        fread(codes.data(), 1, total_bytes, f);
        fclose(f);

        vector<vector<int>> payloads(num_blocks, vector<int>(block_ints, 0));
        for (int i = 0; i < (int)n; i++) {
            int blk = i / codes_per_block;
            int off = (i % codes_per_block) * (int)nchunks;
            memcpy((uint8_t *)payloads[blk].data() + off, codes.data() + (size_t)i * nchunks, nchunks);
        }

        cout << "Building PQ ORAM: blocks=" << num_blocks << " levels=" << num_levels
             << " codes/block=" << codes_per_block << " block_ints=" << block_ints << endl;
        RawOramBuilder b(md.pq_oram_buckets_path.c_str(), md.pq_oram_pos_map_path.c_str(),
                         md.pq_oram_metadata_path.c_str(), md.pq_oram_hash_path.c_str(), &cfg);
        b.build(payloads);
    }

    // ---------------- position-map ORAM (only for recursive path) ----------------
    if (md.recursive_posmap)
    {
        FILE *f = fopen(md.pos_map_path.c_str(), "rb");
        if (!f) { perror(md.pos_map_path.c_str()); return EXIT_FAILURE; }
        int s;
        fread(&s, 1, sizeof(int), f);
        vector<int> pmap(s);
        fread(pmap.data(), 1, (size_t)s * sizeof(int), f);
        fclose(f);

        int block_bytes = md.posmap_oram_block_size_bytes;
        int block_ints = block_bytes / 4;
        int labels_per_block = block_ints;
        int num_blocks = (int)ceil((double)s / labels_per_block);
        int num_levels = num_levels_for_blocks(num_blocks, Z);
        md.posmap_oram_num_blocks = num_blocks;
        md.posmap_oram_num_levels = num_levels;
        set_globals(block_ints);

        RingOramConfig cfg(block_ints, Z, S, md.evict_rate, num_blocks, num_levels, 0);

        vector<vector<int>> payloads(num_blocks, vector<int>(block_ints, -1));
        for (int i = 0; i < s; i++) {
            payloads[i / labels_per_block][i % labels_per_block] = pmap[i];
        }

        cout << "Building position-map ORAM: blocks=" << num_blocks << " levels=" << num_levels
             << " labels/block=" << labels_per_block << " block_ints=" << block_ints << endl;
        RawOramBuilder b(md.posmap_oram_buckets_path.c_str(), md.posmap_oram_pos_map_path.c_str(),
                         md.posmap_oram_metadata_path.c_str(), md.posmap_oram_hash_path.c_str(), &cfg);
        b.build(payloads);
    }

    // Client and server must use the sizes of the states actually generated.
    for (const auto& field : {std::make_pair("pq_bytes", md.pq_bytes),
                             std::make_pair("pq_oram_num_blocks", md.pq_oram_num_blocks),
                             std::make_pair("pq_oram_num_levels", md.pq_oram_num_levels)}) {
        if (!overwrite_json_nested_field(config_path, dataset, field.first, field.second))
            return EXIT_FAILURE;
    }
    if (md.recursive_posmap) {
        if (!overwrite_json_nested_field(config_path, dataset, "posmap_oram_num_blocks", md.posmap_oram_num_blocks) ||
            !overwrite_json_nested_field(config_path, dataset, "posmap_oram_num_levels", md.posmap_oram_num_levels))
            return EXIT_FAILURE;
    }
    cout << "Low-client-storage ORAM initialization done." << endl;
    return EXIT_SUCCESS;
}
