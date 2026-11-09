# pragma once 

#include <vector>
#include <cstdint>
#include <map>
#include <cassert>
#include <iostream>

#include "macro.h"
// #include "OramDeterministic.h"
#include "OramRing.h"
#include "OramLeak.h"
#include "evp.h"

// #define DIM 128
// #define N_NEIGHBORS 64

using namespace std;

// layout: layer_id | node_id | neighbors | (next_layer_neighbors) | coords 
// next_layer_neighbors is only for nodes in upper layers or layer_id != 0

struct OptNode {
    int len;
    vector<int> data;

    static int n_neighbors;
    static int dim; 

    OptNode(){
        len = 2 + dim + n_neighbors;
        data = vector<int>(len, -1);
    }


    OptNode(int* input, int l){
        // assert(l == 2 + dim + n_neighbors);
        len = 2 + dim + n_neighbors;
        data.resize(len);
        memcpy(data.data(), input, len*sizeof(int));
    }

    ~OptNode(){
    }

    int get_int_size(){
        return len;
    }

    int* get_int_data(){
        return data.data();
    }

    int get_layer(){
        return data[0];
    }

    void set_layer(int layer_id){
        data[0] = layer_id;
    }

    int get_id(){
        return data[1];
    }

    void set_id(int nid){
        data[1] = nid;
    }

    int get_neighbor(int i){
        return data[2 + i];
    }

    vector<int> get_neighbors(){
        if(data[0] == 0){
            // last layer
            return vector<int>(data.begin() + 2, data.begin() + 2 + OptNode::n_neighbors);
        } else{
            return vector<int>(data.begin() + 2, data.begin() + 2 + OptNode::n_neighbors / 2);
        }
    }

    void set_neighbors(vector<int>& nbs){
        assert(
            (data[0] == 0 && nbs.size() == OptNode::n_neighbors) ||
            (data[0] >= 0 && nbs.size() == OptNode::n_neighbors / 2) 
        );

        for(int i = 0; i < nbs.size(); i++){
            data[2 + i] = nbs[i];
        }
    }

    void set_next_layer_neighbors(vector<int>& nbs){
        assert(
            data[0] >= 0 && nbs.size() == OptNode::n_neighbors / 2 
        );

        for(int i = 0; i < nbs.size(); i++){
            data[2 + OptNode::n_neighbors / 2 + i] = nbs[i];
        }
    }

    int get_next_layer_neighbor(int i){
        assert(data[0] >= 0);
        return data[2 + OptNode::n_neighbors / 2 + i];
    }

    vector<int> get_next_layer_neighbors(){
        // std::cout  << "layer_id: " << data[0] << std::endl;
        assert(data[0] >= 0);
        return vector<int>(data.begin() + 2 + OptNode::n_neighbors / 2,  data.begin() + 2 + OptNode::n_neighbors);
    }

    void set_coord(vector<float>& coords){
        // bad but assuming sizeof(int) == size(float)
        assert(coords.size() == dim);
        memcpy(data.data() + 2 + OptNode::n_neighbors, coords.data(), dim*sizeof(int));
    }

    float* coord(){
        return (float *)(data.data() + 2 + OptNode::n_neighbors);
    }

    bool operator==(const OptNode& other) const {
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


struct Cluster {

    int layer;
    vector<node_id_t> ids;
    vector<vector<node_id_t>> neighbors;
    vector<float> coords;
    vector<vector<uint8_t>> quantized_neighbor_coords;

    Cluster(){}

    Cluster(int l) : layer(l) {}


    void print(){
        int n = ids.size();
        cout << "Print cluster of " << n << " vertices: " << endl;
        for(int i = 0; i < n; i++){
            cout << "- ids: " << ids[i] << endl;
            cout << "- neighbors: ";
            for(node_id_t nb : neighbors[i]){
                cout << nb << " ";
            }
            cout << endl;

            // cout << "- coords: ";
            // for(int j = 0; j < 128; j++){
            //     cout << coords[j + i*128] << " ";
            // }
            // cout << endl;
        }

    }

    bool operator==(const Cluster& other) const {
        int n = ids.size();
        if(n != other.ids.size()){
            return false;
        }
        if(this->layer != other.layer){
            return false;
        }
        for(int i = 0; i < n; i++){
            if(this->ids[i] != other.ids[i]){
                return false;
            }
        }
        // for(int i = 0; i < n*128; i++){
        //     if(this->coords[i] != other.coords[i]){
        //         return false;
        //     }
        // }
        return true;
    }

};

class BlockFetcher {
    public:
        vector<vector<block_id_t>> node_to_block;
        
        virtual OptNode* get_block(node_id_t node_id, int l);
        virtual vector<OptNode> get_blocks(vector<node_id_t> node_ids, vector<int> ls, int pad_l);
        virtual block_id_t add_block(OptNode* on) = 0;
        virtual block_id_t add_block_with_list(OptNode* on, vector<node_id_t> nlist) = 0;
        virtual void evict(){};
        // virtual int get_stash_size() = 0;
    
        BlockFetcher(offset_id_t nb, int max_level);
    };

// class ObliviousBlockFetcher : public BlockFetcher{

//     OramInterface* oram;
    
//     int graph_cnt;

//     public:
//     vector<OptNode> get_blocks(vector<node_id_t> node_ids, vector<int> ls, int pad_l) override{
//         vector<block_id_t> bids;
//         int len = node_ids.size();
//         for(int i = 0; i < len; i++){
//             node_id_t node_id = node_ids[i];
//             int l = ls[i];
//             bids.push_back(node_to_block[node_id][l]);
//         }

//         vector<int*> accessed = ((OramReadPathEviction*)oram)->batch_multi_access_swap_ro(
//             vector<OramInterface::Operation>(len, OramInterface::Operation::READ),
//             bids,
//             vector<int*>(len, NULL),
//             pad_l
//         );

//         vector<OptNode> ret;
//         for(int i = 0; i < len; i++){
//             ret.push_back(OptNode(accessed[i], ((OramReadPathEviction*)oram)->block_size));
//             delete[] accessed[i];
//         }
//         return ret;
//     }

//     void evict(){
//         ((OramReadPathEviction*)oram)->evict_and_write_back();
//         // ((OramReadPathEviction*)oram)->clear_hash_map();
//     } 

    
//     OptNode* get_block(block_id_t node_id, int l) {
//         assert(0);
//         block_id_t bid = node_to_block[node_id][l];
//         // cout << "get_block: " << bid << endl;
//         // cout << "node_id: " << node_id << endl;
//         // cout << "l: " << l << endl;
//         // assert(0);
//         int* accessed = oram->batch_access(OramInterface::Operation::READ, bid, NULL);
//         // unsigned char ptx_text[1024];
//         // int ptx_len = decrypt_wrapper(tmp, 784, ptx_text);

//         // cout << "2" << endl;
//         OptNode* b = new OptNode(accessed,  ((OramReadPathEviction*)oram)->block_size);
//         delete accessed;
//         // cout << "3" << endl;
//         return b;
//     }

//     block_id_t add_block(OptNode* on) override {
//         block_id_t bid = graph_cnt;
//         graph_cnt++;

        
//         oram->batch_access(OramInterface::Operation::WRITE, bid, on->get_int_data());

//         node_id_t nid = on->get_id();
//         node_to_block[nid][on->get_layer()] = bid;
        
//         return bid;
//     }


//     block_id_t add_block_with_list(OptNode* on, vector<node_id_t> nlist) override {
//         block_id_t bid = graph_cnt;
//         graph_cnt++;

//         oram->batch_access(OramInterface::Operation::WRITE, bid, on->get_int_data());

//         for(node_id_t nid : nlist){
//             node_to_block[nid][on->get_layer()] = bid;
//         }
//         return bid;
//     }

//     void sync_block_mapping(NetIO*io){

//         int ntotal = node_to_block.size();
//         int nlevels = node_to_block[0].size();

//         int* data = new int[ntotal*nlevels];
//         io->recv_data(data, ntotal*nlevels*sizeof(int));

//         for(int i = 0; i < ntotal; i++){
//             memcpy(node_to_block[i].data(), data + i * nlevels, nlevels*sizeof(int));
//         }

//         // io->recv_data(coord_to_block.data(), coord_to_block.size()*sizeof(int));

//         // cout << "synced block mapping: ";
//         // cout << endl;
//     }

//      void read_block_mapping(const char* fname){

//         int ntotal = node_to_block.size();
//         int nlevels = node_to_block[0].size();

//         int* data = new int[ntotal*nlevels];

//         FILE* f = fopen(fname, "r");
//         if (!f) {
//             fprintf(stderr, "could not open %s\n", fname);
//             perror("");
//             abort();
//         }
//         fread(data, 1, ntotal*nlevels*sizeof(int), f);
//         fclose(f);

//         for(int i = 0; i < ntotal; i++){
//             memcpy(node_to_block[i].data(), data + i * nlevels, nlevels*sizeof(int));
//         }

//         // io->recv_data(coord_to_block.data(), coord_to_block.size()*sizeof(int));

//         // cout << "synced block mapping: ";
//         // cout << endl;
//     }

//     // int get_stash_size(){
//     //     return oram->getStashSize();
//     // }

//     ObliviousBlockFetcher(
//         int ntotal,
//         int nlevels,
//         OramInterface* oram) 
//         : BlockFetcher(ntotal, nlevels), 
//             oram(oram),
//             graph_cnt(0){

//     }

// };

class ObliviousBlockFetcherRing : public BlockFetcher{

    OramInterface* oram;
    
    int graph_cnt;

    public:
    /** For instrumentation; oram is always OramRing in this build. */
    OramRing* oram_for_stats() { return static_cast<OramRing*>(oram); }

    vector<OptNode> get_blocks(vector<node_id_t> node_ids, vector<int> ls, int pad_l){
        vector<block_id_t> bids;
        int len = node_ids.size();
        for(int i = 0; i < len; i++){
            node_id_t node_id = node_ids[i];
            int l = ls[i];
            bids.push_back(node_to_block[node_id][l]);
        }

        vector<int*> accessed = ((OramRing*)oram)->access(
            vector<OramInterface::Operation>(len, OramInterface::Operation::READ),
            bids,
            vector<int*>(len, NULL),
            pad_l
        );

        vector<OptNode> ret;
        for(int i = 0; i < len; i++){
            ret.push_back(OptNode(accessed[i], ((OramRing*)oram)->block_size));
            delete[] accessed[i];
        }
        return ret;
    }

    void evict(){
        ((OramRing*)oram)->evict_and_write_back();
        // ((OramRing*)oram)->clear_hash_map();
    } 

    
    OptNode* get_block(block_id_t node_id, int l) {
        assert(0);
        block_id_t bid = node_to_block[node_id][l];
        // cout << "get_block: " << bid << endl;
        // cout << "node_id: " << node_id << endl;
        // cout << "l: " << l << endl;
        // assert(0);
        int* accessed = oram->batch_access(OramInterface::Operation::READ, bid, NULL);
        // unsigned char ptx_text[1024];
        // int ptx_len = decrypt_wrapper(tmp, 784, ptx_text);

        // cout << "2" << endl;
        OptNode* b = new OptNode(accessed,  ((OramRing*)oram)->block_size);
        delete accessed;
        // cout << "3" << endl;
        return b;
    }

    block_id_t add_block(OptNode* on) override {
        block_id_t bid = graph_cnt;
        graph_cnt++;

        
        oram->batch_access(OramInterface::Operation::WRITE, bid, on->get_int_data());

        node_id_t nid = on->get_id();
        node_to_block[nid][on->get_layer()] = bid;
        
        return bid;
    }


    block_id_t add_block_with_list(OptNode* on, vector<node_id_t> nlist) override {
        block_id_t bid = graph_cnt;
        graph_cnt++;

        oram->batch_access(OramInterface::Operation::WRITE, bid, on->get_int_data());

        for(node_id_t nid : nlist){
            node_to_block[nid][on->get_layer()] = bid;
        }
        return bid;
    }

    void sync_block_mapping(NetIO*io){

        int ntotal = node_to_block.size();
        int nlevels = node_to_block[0].size();

        int* data = new int[ntotal*nlevels];
        io->recv_data(data, ntotal*nlevels*sizeof(int));

        for(int i = 0; i < ntotal; i++){
            memcpy(node_to_block[i].data(), data + i * nlevels, nlevels*sizeof(int));
        }

        // io->recv_data(coord_to_block.data(), coord_to_block.size()*sizeof(int));

        // cout << "synced block mapping: ";
        // cout << endl;
    }

     void read_block_mapping(const char* fname){

        int ntotal = node_to_block.size();
        int nlevels = node_to_block[0].size();

        int* data = new int[ntotal*nlevels];

        FILE* f = fopen(fname, "r");
        if (!f) {
            fprintf(stderr, "could not open %s\n", fname);
            perror("");
            abort();
        }
        fread(data, 1, ntotal*nlevels*sizeof(int), f);
        fclose(f);

        for(int i = 0; i < ntotal; i++){
            memcpy(node_to_block[i].data(), data + i * nlevels, nlevels*sizeof(int));
        }

        // io->recv_data(coord_to_block.data(), coord_to_block.size()*sizeof(int));

        // cout << "synced block mapping: ";
        // cout << endl;

        delete[] data;
    }

    // int get_stash_size(){
    //     return oram->getStashSize();
    // }

    ObliviousBlockFetcherRing(
        int ntotal,
        int nlevels,
        OramInterface* oram) 
        : BlockFetcher(ntotal, nlevels), 
            oram(oram),
            graph_cnt(0){

    }

};

class LeakBlockFetcherRing : public BlockFetcher{

    OramInterface* oram;
    
    int graph_cnt;

    public:
    vector<OptNode> get_blocks(vector<node_id_t> node_ids, vector<int> ls, int pad_l){
        vector<block_id_t> bids;
        int len = node_ids.size();
        for(int i = 0; i < len; i++){
            node_id_t node_id = node_ids[i];
            int l = ls[i];
            bids.push_back(node_to_block[node_id][l]);
        }

        vector<int*> accessed = ((OramLeak*)oram)->access(
            vector<OramInterface::Operation>(len, OramInterface::Operation::READ),
            bids,
            vector<int*>(len, NULL),
            pad_l
        );

        vector<OptNode> ret;
        for(int i = 0; i < accessed.size(); i++){
            ret.push_back(OptNode(accessed[i], ((OramLeak*)oram)->block_size));
            delete[] accessed[i];
        }
        return ret;
    }

    void evict(){
        ((OramLeak*)oram)->evict_and_write_back();
        // ((OramLeak*)oram)->clear_hash_map();
    } 

    
    OptNode* get_block(block_id_t node_id, int l) {
        assert(0);
        block_id_t bid = node_to_block[node_id][l];
        // cout << "get_block: " << bid << endl;
        // cout << "node_id: " << node_id << endl;
        // cout << "l: " << l << endl;
        // assert(0);
        int* accessed = oram->batch_access(OramInterface::Operation::READ, bid, NULL);
        // unsigned char ptx_text[1024];
        // int ptx_len = decrypt_wrapper(tmp, 784, ptx_text);

        // cout << "2" << endl;
        OptNode* b = new OptNode(accessed,  ((OramLeak*)oram)->block_size);
        delete accessed;
        // cout << "3" << endl;
        return b;
    }

    block_id_t add_block(OptNode* on) override {
        block_id_t bid = graph_cnt;
        graph_cnt++;

        
        oram->batch_access(OramInterface::Operation::WRITE, bid, on->get_int_data());

        node_id_t nid = on->get_id();
        node_to_block[nid][on->get_layer()] = bid;
        
        return bid;
    }


    block_id_t add_block_with_list(OptNode* on, vector<node_id_t> nlist) override {
        block_id_t bid = graph_cnt;
        graph_cnt++;

        oram->batch_access(OramInterface::Operation::WRITE, bid, on->get_int_data());

        for(node_id_t nid : nlist){
            node_to_block[nid][on->get_layer()] = bid;
        }
        return bid;
    }

    void sync_block_mapping(NetIO*io){

        int ntotal = node_to_block.size();
        int nlevels = node_to_block[0].size();

        int* data = new int[ntotal*nlevels];
        io->recv_data(data, ntotal*nlevels*sizeof(int));

        for(int i = 0; i < ntotal; i++){
            memcpy(node_to_block[i].data(), data + i * nlevels, nlevels*sizeof(int));
        }

        // io->recv_data(coord_to_block.data(), coord_to_block.size()*sizeof(int));

        // cout << "synced block mapping: ";
        // cout << endl;
    }

     void read_block_mapping(const char* fname){

        int ntotal = node_to_block.size();
        int nlevels = node_to_block[0].size();

        int* data = new int[ntotal*nlevels];

        FILE* f = fopen(fname, "r");
        if (!f) {
            fprintf(stderr, "could not open %s\n", fname);
            perror("");
            abort();
        }
        fread(data, 1, ntotal*nlevels*sizeof(int), f);
        fclose(f);

        for(int i = 0; i < ntotal; i++){
            memcpy(node_to_block[i].data(), data + i * nlevels, nlevels*sizeof(int));
        }

        // io->recv_data(coord_to_block.data(), coord_to_block.size()*sizeof(int));

        // cout << "synced block mapping: ";
        // cout << endl;

        delete[] data;
    }

    // int get_stash_size(){
    //     return oram->getStashSize();
    // }

    LeakBlockFetcherRing(
        int ntotal,
        int nlevels,
        OramInterface* oram) 
        : BlockFetcher(ntotal, nlevels), 
            oram(oram),
            graph_cnt(0){

    }

};



class FakeBlockFetcher : public BlockFetcher{
    
    OramConfig graph_config;
    RandForOramInterface* rand;
    int graph_cnt;
    vector<int> graph_position_map;

    public:
    vector<Bucket> bkts;

    int oram_access(
        OramConfig config, 
        RandForOramInterface* rand,
        int index,
        int* data,
        int len){

        Block b(config.block_size);
        b.index = index;
        for(int i = 0; i < len; i++){
            b.data[i] = data[i];
        }

        while(1){
            int leaf = rand->getRandomLeafWithBound(config.num_leaves);
            vector<Block> blocks;
            for (int l = config.num_levels - 1; l >= 0; l--) {
                int offset = P(leaf, l, config.num_levels);
                bool written = bkts[offset].replaceBlock(-1, b);
                if(written){
                    return leaf;
                }
            }
        }
    }


    int P(int leaf, int level, int num_levels) {
        /*
        * This function should be deterministic. 
        * INPUT: leaf in range 0 to num_leaves - 1, level in range 0 to num_levels - 1. 
        * OUTPUT: Returns the location in the storage of the bucket which is at the input level and leaf.
        */
        return (1<<level) - 1 + (leaf >> (num_levels - level - 1));
    }


    block_id_t add_block(OptNode* on) override {
        assert(0);
        return {};
    }

    block_id_t add_block_with_list(OptNode* on, vector<node_id_t> nlist) override {
        block_id_t bid = graph_cnt;
        graph_cnt++;

        // assert(bid < graph_position_map.size());

        graph_position_map.push_back(
            oram_access(
                graph_config,
                rand,
                bid,
                on->get_int_data(),
                on->get_int_size()
            )
        );

        for(node_id_t nid : nlist){
            node_to_block[nid][on->get_layer()] = bid;
        }
        return bid;
    }

    void sync_block_mapping(NetIO*io){

        int ntotal = node_to_block.size();
        int nlevels = node_to_block[0].size();

        int* data = new int[ntotal*nlevels];
        for(int i = 0; i < ntotal; i++){
            memcpy(data + i * nlevels, node_to_block[i].data(), nlevels*sizeof(int));
        }

        io->send_data(data, ntotal*nlevels*sizeof(int));
        // io->send_data(coord_to_block.data(), coord_to_block.size()*sizeof(int));
    }
    
    void sync_position_map(NetIO*io){
        int s = graph_position_map.size();
        io->send_data(&s, sizeof(int));
        io->send_data(graph_position_map.data(), graph_position_map.size()*sizeof(int));
    }

    void save_block_mapping(const char* fname){

        int ntotal = node_to_block.size();
        int nlevels = node_to_block[0].size();

        int* data = new int[ntotal*nlevels];
        for(int i = 0; i < ntotal; i++){
            memcpy(data + i * nlevels, node_to_block[i].data(), nlevels*sizeof(int));
        }

        FILE* f = fopen(fname, "w");
        if (!f) {
            fprintf(stderr, "could not open %s\n", fname);
            perror("");
            abort();
        }
        fwrite(data, 1, ntotal*nlevels*sizeof(int), f);
        fclose(f);

        // io->send_data(data, ntotal*nlevels*sizeof(int));
        // io->send_data(coord_to_block.data(), coord_to_block.size()*sizeof(int));
    }
    
    void save_position_map(const char* fname){
        int s = graph_position_map.size();
        FILE* f = fopen(fname, "w");
        if (!f) {
            fprintf(stderr, "could not open %s\n", fname);
            perror("");
            abort();
        }
        fwrite(&s, 1, sizeof(int), f);
        fwrite(graph_position_map.data(), 1, graph_position_map.size()*sizeof(int), f);
        fclose(f);
    }

    // void insecure_load(RemoteServerStorage* graph_rss){
    //     graph_rss->insecureLoad(bkts);
    // }

    FakeBlockFetcher(
        int ntotal,
        int nlevels,
        OramConfig graph_config,
        RandForOramInterface* rand)
        :   BlockFetcher(ntotal, nlevels),
            graph_cnt(0),
            graph_config(graph_config), 
            rand(rand){
        
        for(int i = 0; i < graph_config.num_buckets; i++){
            Bucket init_bkt = Bucket();
            for(int j = 0; j < Bucket::getMaxSize(); j++){
                init_bkt.addBlock(Block(graph_config.block_size));
            }
            bkts.push_back(init_bkt);
        }
    }

};


class FakeBlockFetcherRing : public BlockFetcher{
    
    RingOramConfig config;
    RandForOramInterface* rand;
    int graph_cnt;
    vector<int> graph_position_map;

    public:
    vector<Block*> bkts;

    int oram_access(
        int index,
        int* data,
        int len){

        Block b(config.block_size);
        b.index = index;
        for(int i = 0; i < len; i++){
            b.data[i] = data[i];
        }

        // cout << "bid: " << b.index << endl;

        while(1){
            int leaf = rand->getRandomLeafWithBound(config.num_leaves);
            vector<Block> blocks;
            for (int l = config.num_levels - 1; l >= 0; l--) {
                int bucket_offset = P(leaf, l, config.num_levels);
                for(int i = 0; i < config.real_bucket_size; i++){
                    int block_offset = bucket_offset * config.real_bucket_size + i;
                    if(bkts[block_offset] == nullptr){
                        bkts[block_offset] = new Block(b);
                        // cout << "leaf: " << leaf << endl;
                        return leaf;
                    }
                }
            }
        }
    }


    int P(int leaf, int level, int num_levels) {
        /*
        * This function should be deterministic. 
        * INPUT: leaf in range 0 to num_leaves - 1, level in range 0 to num_levels - 1. 
        * OUTPUT: Returns the location in the storage of the bucket which is at the input level and leaf.
        */
        return (1<<level) - 1 + (leaf >> (num_levels - level - 1));
    }


    block_id_t add_block(OptNode* on) override {
        assert(0);
        return {};
    }

    block_id_t add_block_with_list(OptNode* on, vector<node_id_t> nlist) override {
        block_id_t bid = graph_cnt;
        graph_cnt++;

        // assert(bid < graph_position_map.size());

        graph_position_map.push_back(
            oram_access(
                bid,
                on->get_int_data(),
                on->get_int_size()
            )
        );

        for(node_id_t nid : nlist){
            node_to_block[nid][on->get_layer()] = bid;
        }
        return bid;
    }

    void sync_block_mapping(NetIO*io){

        int ntotal = node_to_block.size();
        int nlevels = node_to_block[0].size();

        int* data = new int[ntotal*nlevels];
        for(int i = 0; i < ntotal; i++){
            memcpy(data + i * nlevels, node_to_block[i].data(), nlevels*sizeof(int));
        }

        io->send_data(data, ntotal*nlevels*sizeof(int));
        // io->send_data(coord_to_block.data(), coord_to_block.size()*sizeof(int));
    }
    
    void sync_position_map(NetIO*io){
        int s = graph_position_map.size();
        io->send_data(&s, sizeof(int));
        io->send_data(graph_position_map.data(), graph_position_map.size()*sizeof(int));
    }

    void save_block_mapping(const char* fname){

        int ntotal = node_to_block.size();
        int nlevels = node_to_block[0].size();

        FILE* f = fopen(fname, "w");
        if (!f) {
            fprintf(stderr, "could not open %s\n", fname);
            perror("");
            abort();
        }
        for(int i = 0; i < ntotal; i++){
            fwrite(node_to_block[i].data(), sizeof(int), nlevels, f);
        }
        fclose(f);

        // io->send_data(data, ntotal*nlevels*sizeof(int));
        // io->send_data(coord_to_block.data(), coord_to_block.size()*sizeof(int));
    }
    
    void save_position_map(const char* fname){
        int s = graph_position_map.size();
        FILE* f = fopen(fname, "w");
        if (!f) {
            fprintf(stderr, "could not open %s\n", fname);
            perror("");
            abort();
        }
        fwrite(&s, 1, sizeof(int), f);
        fwrite(graph_position_map.data(), 1, graph_position_map.size()*sizeof(int), f);
        fclose(f);
    }

    // void insecure_load(RemoteServerStorage* graph_rss){
    //     // graph_rss->insecureLoad(bkts);
    // }

    FakeBlockFetcherRing(
        int ntotal,
        int nlevels,
        RingOramConfig config,
        RandForOramInterface* rand)
        :   BlockFetcher(ntotal, nlevels),
            graph_cnt(0),
            config(config), 
            rand(rand){
        
        bkts.assign(
                static_cast<size_t>(config.num_buckets) *
                        static_cast<size_t>(config.real_bucket_size),
                nullptr);
    }

    ~FakeBlockFetcherRing(){
        for(Block* block : bkts){
            delete block;
        }
    }

};
