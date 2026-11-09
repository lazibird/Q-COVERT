/*
    Interface for an ANN algorithm to be ported with the ORAM
*/
#ifndef PORAM_API_H
#define PORAM_API_H

#include "ArgMapping.h"
#include "net_io_channel.h"

// stl
#include <sys/resource.h>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>
#include <fstream>
#include <sys/time.h>
#include <cstdint>

// oram
#include "RemoteServerStorage.h"
#include "RemoteServerRing.h"
#include "RandForOramInterface.h"
#include "RandomForOram.h"
#include "OramRing.h"
#include "OramReadPathEviction.h"
#include "OramInterface.h"
#include "RemoteRing.h"

#include "diskann/include/timer.h"

using namespace std;

using node_id_t = int32_t;
using block_id_t = int32_t;
using offset_id_t = int64_t;


inline double elapsed() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

inline void tprint(string s, double t0){
    cout << "[" << std::fixed << std::setprecision(3) << elapsed() - t0 << "s] " << s;
}

template<typename T, typename LabelT = node_id_t>
struct DiskANNNode {
    // layout:  node_id | neighbors | coords 
    // next_layer_neighbors is only for nodes in upper layers or layer_id != 0

    int len;        // == num_components_in_diskann_aligned_data
    vector<T> data;
    LabelT node_id;

    char* diskann_aligned_data;

    static int n_neighbors;
    static int dim; 

    DiskANNNode(){
        len = dim + 1 + n_neighbors; // coords | n_nbrs | nbr_ids
        data = vector<T>(len, -1);
        diskann_aligned_data = (char*) malloc(len * sizeof(T));
    }

    DiskANNNode(T* input, float large_number){

        //load INDEX
        node_id_t idx;
        memcpy(&idx, input, sizeof(node_id_t));
        this->node_id = (LabelT) idx;


        //load DATA
        len = dim + 1 + n_neighbors; // coords | n_nbrs | nbr_ids
        //copy input into data
        //entire input goes into data
        data.resize(len);
        memcpy(data.data(), input, len*sizeof(T));  
        //scaling coords in data
        for(int i = 0; i < dim; i++){
            data[1 + n_neighbors + i] /= large_number; 
        }

        //load DISKANN_ALIGNED_DATA
        diskann_aligned_data = (char*) malloc(len * sizeof(T));
        // copy node_coords
        // skip node_id and n_nbrs
        memcpy(diskann_aligned_data, data.data() + 1 + n_neighbors, dim*(sizeof(T)/sizeof(char)));  
        // copy the neighbour ids
        // find ACTUAL num_nbrs 
        T* nbr = input + 1;     // skipped the node_id
        T num_actual_nbrs = 0;
        int i = 0;
        LabelT minus_one = (LabelT) -1;
        LabelT nbrid;
        
        while(i < n_neighbors){
            //semantic value of nbr id is needed to see if its -1 or not
            memcpy(&nbrid, nbr, sizeof(T));

            if(nbrid < 0){
                // diskann_aligned_data[(dim + 1) + i] = (T) -1;
                memcpy(diskann_aligned_data + ((dim + 1) + i)*sizeof(T), &minus_one, 1*sizeof(T)/sizeof(char));
            } else {
                memcpy(diskann_aligned_data + ((dim + 1) + i)*sizeof(T), nbr, 1*sizeof(T)/sizeof(char));
                num_actual_nbrs++;
            }

            nbr++;
            i++;
        }

        // write number of ACTUAL neighbours
        memcpy(diskann_aligned_data + dim*sizeof(T), &num_actual_nbrs, 1*sizeof(T));

    }


    ~DiskANNNode(){
        if(diskann_aligned_data != NULL){
            free(diskann_aligned_data);
        }
    }

    void print_node_info(){
        cout << "Node ID: " << node_id << endl;
        cout << "Neighbors: ";
        for(int i = 0; i < n_neighbors; i++){
            if(data[1 + i] >= 0){
                cout << (LabelT)data[1 + i] << " ";
            }
        }
        cout << endl;
        cout << "Coordinates: ";
        for(int i = 0; i < dim; i++){
            cout << data[1 + n_neighbors + i] << " ";
        }
        cout << endl << endl;
    }

    void print_node_info_and_Bytes(){

        cout << "Node ID: " << node_id << endl;
        cout << "Neighborsin LabelT: ";
        for(int i = 0; i < n_neighbors; i++){
            if(data[1 + i] >= 0){
                int32_t temp_int;
                memcpy(&temp_int, data.data() + 1 + i , sizeof(LabelT));
                // cout << (LabelT)data[1 + i] << " ";
                cout << (LabelT)temp_int << " ";
            }
        }
        cout << endl;
        cout << "Neighbors in T" << endl;
        for(int i = 0; i < n_neighbors; i++){
            if(data[1 + i] >= 0){
                float temp_float;
                memcpy(&temp_float, data.data() + 1 + i , sizeof(float));
                // cout << (T)data[1 + i] << " ";
                cout << (float)temp_float << " ";
            }
        }
        cout << endl;
        cout << "Coordinates in T: ";
        for(int i = 0; i < dim; i++){
            T temp_coord;
            memcpy(&temp_coord, data.data() + 1 + n_neighbors + i, sizeof(float));
            cout << temp_coord << " ";
            // cout << data[1 + n_neighbors + i] << " ";
        }
        cout << endl;


    }

    int get_int_size(){
        return len;
    }

    int get_diskann_aligned_size() const { return len * sizeof(T); }

    T* get_int_data(){
        return data.data();
    }

    char* get_diskann_aligned_data(){
        return diskann_aligned_data;
    }

    LabelT get_id(){
        assert(data.size() > 0);
        return node_id;
    }

    void set_id(LabelT nid){
        assert(nid >= 0);
        node_id = nid;
        // data[0] = (T) nid;
        memcpy(data.data(), &nid, sizeof(T));
    }

    vector<LabelT> get_neighbors(){
        return vector<LabelT>(data.begin() + 1, data.begin() + 1 + DiskANNNode::n_neighbors); 
    }

    void set_neighbors(vector<LabelT>& nbs){
        assert(nbs.size() == DiskANNNode::n_neighbors);

        memcpy(data.data() + 1, nbs.data(), sizeof(LabelT)*nbs.size());

        // for(int i = 0; i < nbs.size(); i++){
        //     // data[1 + i] = (T) nbs[i];
        //     memcpy(data.data() + 1 + i, nbs.data() + i, sizeof(LabelT));
        // }
    }

    void set_coords(vector<T>& coords){
        // cout << coords.size() << " " << dim << "\n";
        // bad but assuming sizeof(int) == size(float)
        assert(coords.size() == dim);
        memcpy(data.data() + 1 + DiskANNNode::n_neighbors, coords.data(), dim*sizeof(T));
    }

    T* get_coords(){
        return (T *)(data.data() + 1 + DiskANNNode::n_neighbors);
    }

    void print_node(){
        for(auto x : data){
            cout << x << " ";
        }
        cout << endl;
    }

    bool operator==(const DiskANNNode& other) const {
        if(other.len != len){
            return false;
        }
        for(int i = 0; i < len; i++){
            if(data[i] != other.data[i]){
                return false;
            }
        }
        
        return true;
    }
};

// uint8 vectors occupy one byte per coordinate, while node ids and graph
// neighbors always occupy four bytes. Keep the ORAM payload in 32-bit words
// and expose a DiskANN-compatible [coordinates | count | neighbor ids] view.
template<typename LabelT>
struct DiskANNNode<uint8_t, LabelT> {
    static int n_neighbors;
    static int dim;
    int len;
    LabelT node_id;
    std::vector<uint32_t> words;
    std::vector<uint8_t> aligned_data;

    static int word_count() { return 1 + n_neighbors + (dim + 3) / 4; }
    static int aligned_size() { return dim + 4 * (1 + n_neighbors); }

    DiskANNNode() : len(word_count()), node_id(-1), words(len, 0), aligned_data(aligned_size(), 0) {}
    DiskANNNode(uint8_t* input, float) : DiskANNNode() {
        std::memcpy(words.data(), input, len * sizeof(uint32_t));
        std::memcpy(&node_id, input, sizeof(LabelT));
        uint8_t* coordinates = reinterpret_cast<uint8_t*>(words.data() + 1 + n_neighbors);
        std::memcpy(aligned_data.data(), coordinates, dim);
        uint32_t count = 0;
        for (int i = 0; i < n_neighbors; ++i) {
            uint32_t neighbor = words[1 + i];
            if (neighbor != UINT32_MAX) {
                std::memcpy(aligned_data.data() + dim + 4 + count * 4, &neighbor, 4);
                ++count;
            }
        }
        std::memcpy(aligned_data.data() + dim, &count, 4);
    }
    int get_int_size() const { return len; } // ORAM block size is in 32-bit words
    uint8_t* get_int_data() { return reinterpret_cast<uint8_t*>(words.data()); }
    char* get_diskann_aligned_data() { return reinterpret_cast<char*>(aligned_data.data()); }
    int get_diskann_aligned_size() const { return aligned_size(); }
    LabelT get_id() const { return node_id; }
    void set_id(LabelT id) { node_id = id; std::memcpy(words.data(), &id, sizeof(id)); }
    void set_neighbors(const std::vector<LabelT>& neighbors) {
        assert(neighbors.size() == static_cast<size_t>(n_neighbors));
        std::memcpy(words.data() + 1, neighbors.data(), n_neighbors * sizeof(LabelT));
    }
    void set_coords(const std::vector<uint8_t>& coordinates) {
        assert(coordinates.size() == static_cast<size_t>(dim));
        std::memcpy(words.data() + 1 + n_neighbors, coordinates.data(), dim);
    }
};


class OramAPI {
    public:
    OramInterface* oram;

    map<int, int> node_to_block_map;
    vector<int> node_to_block;
    bool debug;
    int oram_calls = 0;
    float large_number;

    // low-client-storage path: main graph ORAM keeps only O(log N) client
    // state. PQ codes and the main position map live in two auxiliary Ring
    // ORAMs, each served over its own connection.
    bool low_client_storage = false;
    OramRing* pq_oram = nullptr;
    OramRing* posmap_oram = nullptr;
    RecursivePositionMap* rec_posmap = nullptr;
    int pq_bytes = 0;
    int pq_codes_per_block = 0;
    // Query-level cache of already-read PQ sub-blocks. Cleared once per query
    // in evict_and_write_back(); avoids re-reading the same PQ-ORAM block for
    // every frontier node that shares a neighbor.
    std::unordered_map<int, std::vector<uint8_t>> pq_cache;

    OramAPI(
        RemoteRing* rss,
        RandForOramInterface* random,
        int total_blocks,
        int num_levels,
        int oram_cached_levels,
        int block_size,
        int real_bucket_size,
        int dummy_size,
        int evict_rate,
        string block_mapping_file,
        string metadata_path,
        string pos_map_path,
        bool debug,
        float large_number,
        RingOramConfig config,
        bool low_client_storage = false,
        bool recursive_posmap = true
    ){
        oram = new OramRing(
            rss, 
            random, 
            config,
            num_levels,
            low_client_storage ? 0 : oram_cached_levels,
            true,
            true,
            low_client_storage
        );

        this->debug = debug;
        this->large_number = large_number;
        this->low_client_storage = low_client_storage;

        printf("Loading ORAM metadata\n");
        ((OramRing*)oram)->loadMetaDataFromFile(metadata_path.c_str());

        int total_nodes = oram->getNumBlocks();    // == total_nodes_in_graph
        node_to_block.resize(total_nodes);

        if (low_client_storage) {
            // OramBuilder assigns graph node `i` to ORAM block `i`, so the
            // block mapping is the identity and needs no client-side file.
            for (int i = 0; i < total_nodes; i++) {
                node_to_block_map[i] = i;
                node_to_block[i] = i;
            }
            if (recursive_posmap) {
                printf("Low-client-storage path: position map is recursive, block map is identity\n");
            } else {
                printf("Low-client-storage path: local position map, block map is identity\n");
                ((OramRing*)oram)->loadPositionMapFromFile(pos_map_path.c_str());
            }
        } else {
            printf("Loading position map\n");
            ((OramRing*)oram)->loadPositionMapFromFile(pos_map_path.c_str());
            printf("Position map successfully loaded\n");

            printf("Loading block mapping file\n");
            read_node_to_block_mapping(block_mapping_file.c_str());
            printf("Block mapping file successfully loaded\n");
        }

        // ((OramRing*) oram)->init_cache_top();

    }

    // Low-client-storage path setup. Auxiliary ORAMs must have the same
    // per-block cipher size as the main ORAM (i.e. block_size == main
    // block_size), so all three share the process-wide SBucket cipher size.
    void setup_low_client(RemoteRing* pq_rss, RemoteRing* posmap_rss,
                          RandForOramInterface* random,
                          bool recursive_posmap,
                          const string& pq_metadata_path,
                          const string& pq_pos_map_path,
                          int pq_num_levels, int pq_num_blocks,
                          const string& posmap_metadata_path,
                          const string& posmap_pos_map_path,
                          int posmap_num_levels, int posmap_num_blocks,
                          int pq_bytes, int pq_block_size_ints, int posmap_block_size_ints,
                          int real_bucket_size,
                          int dummy_size, int evict_rate) {
        assert(low_client_storage);
        this->pq_bytes = pq_bytes;
        this->pq_codes_per_block = pq_block_size_ints * 4 / pq_bytes;

        RingOramConfig pq_cfg(pq_block_size_ints, real_bucket_size, dummy_size, evict_rate,
                              pq_num_blocks, pq_num_levels, 0);
        pq_oram = new OramRing(pq_rss, random, pq_cfg, pq_num_levels, 0, true, true, true);
        pq_oram->loadMetaDataFromFile(pq_metadata_path.c_str());
        pq_oram->loadPositionMapFromFile(pq_pos_map_path.c_str());

        if (recursive_posmap) {
            RingOramConfig posmap_cfg(posmap_block_size_ints, real_bucket_size, dummy_size, evict_rate,
                                      posmap_num_blocks, posmap_num_levels, 0);
            posmap_oram = new OramRing(posmap_rss, random, posmap_cfg, posmap_num_levels, 0, true, true, true);
            posmap_oram->loadMetaDataFromFile(posmap_metadata_path.c_str());
            posmap_oram->loadPositionMapFromFile(posmap_pos_map_path.c_str());

            rec_posmap = new RecursivePositionMap(posmap_oram, posmap_block_size_ints);
            ((OramRing*)oram)->set_position_map(rec_posmap);
        }
    }

    void init_cache_top() {
        if (low_client_storage)
            return;
        ((OramRing*)oram)->init_cache_top();
    }

    void evict_and_write_back() {
        ((OramRing*)oram)->evict_and_write_back();
        if (low_client_storage) {
            if (posmap_oram) posmap_oram->evict_and_write_back();
            if (pq_oram) pq_oram->evict_and_write_back();
        }
        pq_cache.clear();
    }

    void reset_pq_cache() { pq_cache.clear(); }

    // Reads PQ codes for `ids` into `out` (n_ids * pq_bytes bytes). Sub-blocks
    // already fetched earlier in the same query are served from `pq_cache`,
    // so the PQ-ORAM is only accessed once per distinct sub-block per query.
    void read_pq(const std::vector<uint32_t>& ids, uint8_t* out) {
        assert(low_client_storage && pq_oram && pq_bytes > 0);
        std::map<int, std::vector<int>> block_to_slots;
        std::vector<int> ordered_sub_blocks;
        for (size_t i = 0; i < ids.size(); i++) {
            int sub = (int)ids[i] / pq_codes_per_block;
            if (block_to_slots.find(sub) == block_to_slots.end())
                ordered_sub_blocks.push_back(sub);
            block_to_slots[sub].push_back((int)i);
        }

        std::vector<int> missing;
        for (int sub : ordered_sub_blocks)
            if (pq_cache.find(sub) == pq_cache.end())
                missing.push_back(sub);

        // Keep every PQ-ORAM batch below `dummy_size` so the early reshuffle
        // bookkeeping never sees a bucket touched 64+ times in one batch.
        // Match Compass: batch up to dummy_size-1 blocks per access.
        const int max_batch = std::max(1, pq_oram->dummy_size - 1);
        for (size_t base = 0; base < missing.size(); base += max_batch) {
            size_t end = std::min(missing.size(), base + max_batch);
            std::vector<int> batch_blocks(missing.begin() + base, missing.begin() + end);
            std::vector<uint8_t*> fetched = pq_oram->batch_multi_access_swap_ro_templated<uint8_t>(
                std::vector<OramInterface::Operation>(batch_blocks.size(), OramInterface::Operation::READ),
                batch_blocks,
                std::vector<uint8_t*>(batch_blocks.size(), nullptr),
                (int)batch_blocks.size());

            for (size_t bi = 0; bi < batch_blocks.size(); bi++) {
                int sub = batch_blocks[bi];
                uint8_t* data = fetched[bi];
                pq_cache.emplace(sub, std::vector<uint8_t>(data, data + pq_codes_per_block * pq_bytes));
            }
            for (uint8_t* p : fetched)
                delete[] p;
        }

        for (size_t i = 0; i < ids.size(); i++) {
            int sub = (int)ids[i] / pq_codes_per_block;
            int off = ((int)ids[i] % pq_codes_per_block) * pq_bytes;
            const std::vector<uint8_t>& block = pq_cache[sub];
            memcpy(out + i * pq_bytes, block.data() + off, pq_bytes);
        }
    }

    int block_size_for_pq() const { return pq_codes_per_block * pq_bytes; }

    void set_query_stats(diskann::QueryStats* stats) {
        if (!low_client_storage)
            return;
        if (pq_oram)
            ((RemoteRing*)pq_oram->storage)->current_query_stats = stats;
        if (posmap_oram)
            ((RemoteRing*)posmap_oram->storage)->current_query_stats = stats;
    }

    ~OramAPI() {
        if (low_client_storage) {
            if (rec_posmap) delete rec_posmap;
            if (pq_oram) delete pq_oram;
            if (posmap_oram) delete posmap_oram;
        }
        delete oram;
    }

    void read_node_to_block_mapping(const char* block_mapping_file){
        int total_nodes = oram->getNumBlocks();    // == total_nodes_in_graph
        if (debug) cout << "Total nodes: " << total_nodes << endl;

        int num_levels_in_compass = 1;

        int* data = new int[num_levels_in_compass*total_nodes];


        FILE* f = fopen(block_mapping_file, "r");
        if (!f) {
            fprintf(stderr, "could not open %s\n", block_mapping_file);
            perror("");
            abort();
        }
        fread(data, sizeof(int), num_levels_in_compass*total_nodes, f);
        fclose(f);

        if (debug) cout << "Starting to load node to block mapping" << endl;
        for(int i = 0; i < total_nodes; i++){
            this->node_to_block_map[i] = *(data + num_levels_in_compass*i);
            this->node_to_block[i] = *(data + num_levels_in_compass*i);
        }

        delete[] data;
    }

    vector<block_id_t> get_block_ids(vector<node_id_t> node_ids){
        vector<int> block_ids;
        for(auto node_id : node_ids){
            int block_id = node_to_block_map[node_id];
            block_ids.push_back(block_id);
            // cout << "Block ID: " << block_id << " ";
        }
        // cout << "\n";
        return block_ids;
    }

    template<typename T,  typename LabelT=node_id_t>
    vector<DiskANNNode<T, LabelT>*> oram_access(vector<node_id_t> node_ids, int padded_access_size){ 
        int real_accesses = node_ids.size();

        vector<block_id_t> block_ids = get_block_ids(node_ids);

        vector<T*> fetched_blocks = ((OramRing*) oram)->batch_multi_access_swap_ro_templated<T>(
            vector<OramInterface::Operation>(real_accesses, OramInterface::Operation::READ),
            block_ids,
            vector<T*>(real_accesses, NULL),
            padded_access_size
        );

        // cout << ((RemoteServerStorage*) ((OramRing*) oram)->storage)->io->num_rounds - num_rounds << " ";

        vector<DiskANNNode<T, LabelT>*> nodes;
        for(int i = 0; i < real_accesses; i++){
            // cout << "before converting from T* to node" << endl;
            // for(int i=0; i< )
            // cout << "converting block to node" << endl;
            DiskANNNode<T, LabelT>* node = new DiskANNNode<T, LabelT>(fetched_blocks[i], large_number);

            // cout << "node info after conversion:" << endl;
            // node->print_node_info_and_Bytes();

            nodes.push_back(node);

            // cout << " retN: " << node->node_id;

            // cout << "\noram_access: print info" << endl;
            // node->print_node_info();

            delete[] fetched_blocks[i];
        }

        // cout << endl << "retn size: " << nodes.size() << endl;

        return nodes;
    }

};

#endif
