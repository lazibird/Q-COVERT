//
//
//

#include "RingORAM.h"
#include "evp.h"
#include <iostream>
#include <string>
#include <sstream>
#include <cmath>
#include <cassert>
#include <chrono>
#include <algorithm>
#include <random>
#include <stdexcept>
#include <numeric>
#include <iterator>

namespace double_oram {

namespace {

using hires_clock = std::chrono::high_resolution_clock;

inline double elapsed_sec_since(hires_clock::time_point t0) {
    return std::chrono::duration<double>(hires_clock::now() - t0).count();
}

inline uint64_t ring_cipher_block_bytes(int block_size) {
    return static_cast<uint64_t>(compute_ctx_len((1 + block_size) * sizeof(int)));
}

inline uint64_t ring_block_read_comm_bytes(size_t block_count, uint64_t cipher_block_bytes) {
    return static_cast<uint64_t>(sizeof(int) + sizeof(size_t) +
                                 block_count * 2 * sizeof(int) +
                                 block_count * cipher_block_bytes);
}

inline uint64_t ring_xor_read_comm_bytes(
        size_t block_count,
        size_t real_block_count,
        uint64_t cipher_block_bytes) {
    return static_cast<uint64_t>(sizeof(int) + 2 * sizeof(size_t) +
                                 block_count * 2 * sizeof(int) +
                                 real_block_count * (cipher_block_bytes - 16) +
                                 block_count * 16);
}

inline uint64_t ring_bucket_write_comm_bytes(
        size_t bucket_count,
        size_t bucket_size,
        uint64_t cipher_block_bytes) {
    return static_cast<uint64_t>(sizeof(int) + sizeof(size_t) +
                                 bucket_count * sizeof(int) +
                                 bucket_count * bucket_size * cipher_block_bytes);
}

/** Add wall time for full `access_without_position_map` (includes nested evict when !lazy). */
struct ScopedBmasrTime {
    RingORAM* ring;
    hires_clock::time_point t0;
    double net_time0;
    explicit ScopedBmasrTime(RingORAM* r)
        : ring(r),
          t0(hires_clock::now()),
          net_time0(r->stat_net_total_time_sec) {}
    ~ScopedBmasrTime() {
        double elapsed_sec = elapsed_sec_since(t0);
        if (!ring->network_simulation_sleep_enabled()) {
            elapsed_sec += ring->stat_net_total_time_sec - net_time0;
        }
        ring->stat_bmasr_time_sec += elapsed_sec;
    }
};

} // 匿名命名空间

inline double interval(std::chrono::_V2::system_clock::time_point start){
    auto end = std::chrono::high_resolution_clock::now();
    auto interval = (end - start)/1e+9;
    return interval.count();
}

int OramMetaData::GetBlockOffset(int block_id, bool& real){
    int ret = -1;

    bool dummy_found = false;
    int dummy_ret = -1;

    // This implementation is insecure
    // Should randomly sample from a dummy instead of linear traversal

    for(int i = 0; i < block_ids.size(); i++){
        if(block_id == block_ids[i] && valids[i] == 1){
            ret = i;
            real = true;
            valids[i] = 0;
            return ret;
        }
        if(!dummy_found){
            if(block_ids[i] == -1 && valids[i] == 1){
                dummy_ret = i;
                // TODO: insecure workaround, always use first dummy
                dummy_found = true;
            }
        }
    }

    // cout << "GetBlockOffset for "<< block_id << " : ";
    // for(auto id : block_ids){
    //     cout << id << " ";
    // }
    // cout << endl;
    valids[dummy_ret] = 0;
    return dummy_ret;
}

int OramMetaData::GetBlockOffsetLeak(int block_id, bool& real){
    int ret = -1;

    bool dummy_found = false;
    int dummy_ret = -1;

    // This implementation is insecure
    // Should randomly sample from a dummy instead of linear traversal

    for(int i = 0; i < block_ids.size(); i++){
        if(block_id == block_ids[i] && valids[i] == 1){
            ret = i;
            real = true;
            // valids[i] = 0;
            return ret;
        }
        if(!dummy_found){
            if(block_ids[i] == -1 && valids[i] == 1){
                dummy_ret = i;
                // TODO: insecure workaround, always use first dummy
                dummy_found = true;
            }
        }
    }

    // cout << "GetBlockOffset for "<< block_id << " : ";
    // for(auto id : block_ids){
    //     cout << id << " ";
    // }
    // cout << endl;
    // valids[dummy_ret] = 0;
    return dummy_ret;
}

void OramMetaData::print_stat(){
    cout << "- count: " << (int) count << endl;
    cout << "- valids: " << valids << endl;
    cout << "- block_ids: ";
    for(auto bid : block_ids){
        cout << bid << " ";
    }
    cout << endl;
    cout << "- block_leaves: ";
    for(auto leaf : block_leaves){
        cout << leaf << " ";
    }
    cout << endl;
}

// int OramMetaData::ShuffleAndReset(){
//     // Generate shuffle mapping
//     vector<int> mapping(block_ids.size());
//     // Fill the mapping vector with sequential indices
//     for (int i = 0; i < block_ids.size(); ++i) {
//         mapping[i] = i;
//     }

//     // Shuffle the mapping vector
//     std::random_device rd;  // Seed
//     std::mt19937 gen(rd()); // Random number generator
//     std::shuffle(mapping.begin(), mapping.end(), gen);
// }


RingORAM::RingORAM(RemoteRing* storage, RandForOramInterface* rand_gen, RingOramConfig config, int num_levels, int cached_levels, bool batch, bool lazy) {
    this->storage = storage;
    this->rand_gen = rand_gen;
    this->block_size = config.block_size;
    this->real_bucket_size = config.real_bucket_size;
    this->dummy_size = config.dummy_size;
    this->bucket_size = config.real_bucket_size + config.dummy_size;
    this->evict_rate = config.evict_rate;
    this->num_blocks = config.num_blocks;

    this->batching = batch;
    this->lazy_eveiction = lazy;

    // this->num_levels = ceil(log10(2 * num_blocks / evict_rate) / log10(2)) - 1;
    this->num_levels = num_levels;
    this->num_buckets = pow(2, num_levels)-1;

    this->G = 1;
    
    if (this->num_buckets*this->bucket_size < this->num_blocks) // deal with precision loss
    {
        throw runtime_error("RingORAM tree does not have enough physical block capacity");
    }
    this->num_leaves = pow(2, num_levels-1);
    this->rand_gen->setBound(num_leaves);
    this->metadata = vector<OramMetaData>(num_buckets);

    this->mmstash = map<std::pair<int, int>, Block*, PairComparator>();

    cache_top = cached_levels > 0;
    num_cached_levels = cached_levels;

    if(cache_top){
        num_cached_bucket = pow(2, num_cached_levels)-1;
    }

    cnt_early_reshuffle = 0;
    cnt_delayed_leaves = 0;
    metadata_remote_ = false;

    reset_query_access_stats();
}

void RingORAM::reset_query_access_stats() {
    stat_bmasr_invocations = 0;
    stat_real_block_indices = 0;
    stat_pad_slots = 0;
    stat_server_cipher_block_reads = 0;
    stat_comm_bytes = 0;
    stat_evict_invocations = 0;
    stat_evict_effective = 0;
    stat_evict_time_sec = 0.0;
    stat_evict_effective_time_sec = 0.0;
    stat_bmasr_time_sec = 0.0;
    stat_max_stash_blocks_after_evict = 0;
    stat_net_round_trips = 0;
    stat_access_net_round_trips = 0;
    stat_evict_net_round_trips = 0;
    stat_evict_write_net_round_trips = 0;
    stat_net_transfer_time_sec = 0.0;
    stat_net_latency_time_sec = 0.0;
    stat_net_total_time_sec = 0.0;
}

void RingORAM::set_network_profile(const NetworkProfile& profile) {
    network_profile_ = profile;
}

bool RingORAM::network_simulation_sleep_enabled() const {
    return network_profile_.enabled && network_profile_.sleep;
}

void RingORAM::set_metadata_remote(bool remote) {
    metadata_remote_ = remote;
}

bool RingORAM::metadata_stored_on_server() const {
    return metadata_remote_;
}

void RingORAM::account_network_transfer(
        uint64_t bytes,
        uint64_t round_trips,
        bool evict_phase,
        bool evict_write) {
    const NetworkSimulationResult result =
        simulate_network_transfer(network_profile_, bytes, round_trips);
    stat_net_round_trips += result.round_trips;
    if (evict_phase) {
        stat_evict_net_round_trips += result.round_trips;
        if (evict_write) {
            stat_evict_write_net_round_trips += result.round_trips;
        }
    } else {
        stat_access_net_round_trips += result.round_trips;
    }
    stat_net_transfer_time_sec += result.transfer_time_sec;
    stat_net_latency_time_sec += result.latency_time_sec;
    stat_net_total_time_sec += result.total_time_sec;
}

uint64_t RingORAM::metadata_bucket_bytes() const {
    const uint64_t valids_bytes = metadata.empty()
        ? 16
        : static_cast<uint64_t>(sizeof(metadata.front().valids));
    return static_cast<uint64_t>(sizeof(uint8_t)) +
           valids_bytes +
           static_cast<uint64_t>(bucket_size) * 2 * sizeof(int);
}

void RingORAM::account_remote_metadata_transfer(size_t bucket_count, bool evict_phase) {
    if (!metadata_remote_ || bucket_count == 0) {
        return;
    }
    const uint64_t bytes =
        static_cast<uint64_t>(bucket_count) * metadata_bucket_bytes();
    stat_comm_bytes += bytes;
    account_network_transfer(bytes, 1, evict_phase);
}

void RingORAM::init_cache_top(){

    // following code has an assumption that usually tree top is empty at the beginning
    vector<int> positions;
    vector<int> offsets;
    vector<bool> valids;
    vector<int> num_non_dummies;
    int total_non_dummies;
    for(int pos = 0; pos < num_cached_bucket; pos++){
        int non_dummies = 0;
        for(int i = 0; i < bucket_size; i++){
            if(metadata[pos].block_ids[i] != -1){
                // no need to check valids
                positions.push_back(pos);
                offsets.push_back(i);
                valids.push_back(true);
                non_dummies++;
            }
        }
        total_non_dummies += non_dummies;
        num_non_dummies.push_back(non_dummies);
        assert(non_dummies <= real_bucket_size);
    }

    if(total_non_dummies > 0) {
        assert(0);
    } else{
        return;
    }

    //retrieve
    std::vector<Block*> blocks;
    storage->ReadBlockBatchAsBlockRing(positions, offsets, blocks, valids, false);
    stat_server_cipher_block_reads += (uint64_t)positions.size();

    for(auto b : blocks){
        delete b;
    }

    // int cnt = 0;
    // for(int pos = 0; pos < num_cached_bucket; pos++){
    //     int non_dummies = num_non_dummies[pos];
    //     for(int i = 0; i < non_dummies; i++){
    //         cached_oram[pos*real_bucket_size + i] = blocks[cnt];
    //         cnt++;
    //     }
    //     for(int i = non_dummies; i < real_bucket_size; i++){
    //         cached_oram[pos*real_bucket_size + i] = new Block(block_size);
    //     }
    // }
}

int RingORAM::ReverseBits(int g, int bits_length) {
    /*
    INPUT: Integers g and bits_length.
    OUTPUT: Integer reverse of length bits_length consisting of reversed bits of g.
    To be used to traverse the leaves and run eviction in reverse lexicographical order.
    */
    int g_mod = g%num_leaves;
    int reverse = 0;
    while(g_mod) {
        reverse <<= 1;
        reverse |= g_mod & 1;
        g_mod >>= 1;
        bits_length--;
    }
    
    reverse <<= bits_length;
    return reverse;
}

uint64_t RingORAM::early_reshuffle(vector<int> buckets, bool account_read){
    std::set<int> unique_buckets(buckets.begin(), buckets.end());
    account_remote_metadata_transfer(unique_buckets.size());
    
    vector<int> positions;
    vector<int> offsets;
    vector<bool> valids;
    vector<int> selected_leaves;

    for(int i = 0; i < buckets.size(); i++){
        // Fetch exactly Z blocks from each buckets
        int pos = buckets[i];
        int z = 0;
        for(int j = 0; j < bucket_size; j++){
            if(metadata[pos].block_ids[j] != -1 && metadata[pos].valids[j] == 1){
                // valid non-empty block
                positions.push_back(pos);
                offsets.push_back(j);
                valids.push_back(true);
                selected_leaves.push_back(metadata[pos].block_leaves[j]);
                z++;
            }
        }

        for(int j = 0; j < bucket_size; j++){
            if(z >= real_bucket_size){
                break;
            }
            if(metadata[pos].block_ids[j] == -1 && metadata[pos].valids[j] == 1){
                // valid empty block
                positions.push_back(pos);
                offsets.push_back(j); 
                valids.push_back(true);
                selected_leaves.push_back(-1);
                z++;
            }
        }

        if(z < real_bucket_size){
            metadata[pos].print_stat();
        }

        // cout << "pos: " << pos << endl;
        // cout << "z: " << z << endl;

        assert(z == real_bucket_size);
    }

    const uint64_t cipher_block_bytes = ring_cipher_block_bytes(block_size);
    std::vector<Block*> blocks;
    const uint64_t read_comm_bytes =
        ring_block_read_comm_bytes(positions.size(), cipher_block_bytes);
    if (account_read) {
        stat_comm_bytes += read_comm_bytes;
        account_network_transfer(read_comm_bytes);
    }
    storage->ReadBlockBatchAsBlockRing(positions, offsets, blocks, valids, true);
    stat_server_cipher_block_reads += positions.size();

    std::random_device rd;  
    std::mt19937 g(rd());
    std::map<int, vector<Block*>> reshuffled_buckets;
    for(int i = 0; i < buckets.size(); i++){
        int pos = buckets[i];
        std::vector<Block*> tmp_bkt(
            blocks.begin() + i * real_bucket_size, 
            blocks.begin() + (i + 1) * real_bucket_size
        ); 
        std::vector<int> tmp_leaves(
            selected_leaves.begin() + i * real_bucket_size,
            selected_leaves.begin() + (i + 1) * real_bucket_size
        );

        for(int i = 0; i < dummy_size; i++){
            Block* dummy = new Block(block_size);
            tmp_bkt.push_back(dummy); //dummy block
            tmp_leaves.push_back(-1);
        }
        assert(tmp_bkt.size() == bucket_size);

        // shuffle
        std::vector<int> order(tmp_bkt.size());
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), g);
        std::vector<Block*> shuffled_bkt;
        std::vector<int> shuffled_leaves;
        shuffled_bkt.reserve(tmp_bkt.size());
        shuffled_leaves.reserve(tmp_leaves.size());
        for (int index : order) {
            shuffled_bkt.push_back(tmp_bkt[index]);
            shuffled_leaves.push_back(tmp_leaves[index]);
        }
        tmp_bkt.swap(shuffled_bkt);
        tmp_leaves.swap(shuffled_leaves);

        // update metadata
        for(int j = 0; j < bucket_size; j++){
            metadata[pos].block_ids[j] = tmp_bkt[j]->index;
            metadata[pos].block_leaves[j] = tmp_leaves[j];
        }
        metadata[pos].count = 0;
        metadata[pos].valids.set();

        reshuffled_buckets[pos] = tmp_bkt;
    }

    // write back to server
    const uint64_t write_comm_bytes = ring_bucket_write_comm_bytes(
        reshuffled_buckets.size(),
        static_cast<size_t>(bucket_size),
        cipher_block_bytes);
    stat_comm_bytes += write_comm_bytes;
    account_network_transfer(write_comm_bytes);
    storage->WriteBucketBatchMapAsBlockRing(reshuffled_buckets, bucket_size, true);
    return read_comm_bytes;

}

std::vector<RingORAMVectorRead> RingORAM::access_without_position_map(
        const std::vector<int>& vector_ids,
        const std::vector<int>& old_leaves,
        const std::vector<int>& new_leaves,
        int pad_l) {
    if (vector_ids.size() != old_leaves.size()) {
        throw std::invalid_argument("vector_ids and old_leaves must have the same size");
    }
    if (vector_ids.size() != new_leaves.size()) {
        throw std::invalid_argument("vector_ids and new_leaves must have the same size");
    }
    if (pad_l < static_cast<int>(vector_ids.size())) {
        throw std::invalid_argument("pad_l must be at least vector_ids.size()");
    }
    if (dummy_size <= 0) {
        throw std::invalid_argument("RingORAM dummy_size must be positive");
    }

    stat_bmasr_invocations++;
    stat_real_block_indices += static_cast<uint64_t>(vector_ids.size());
    stat_pad_slots += static_cast<uint64_t>(pad_l);
    ScopedBmasrTime _bmasr_timer(this);

    struct AccessPath {
        int vector_id;
        int old_leaf;
        int new_leaf;
        std::vector<int> path_positions;
    };
    struct ReadLabel {
        bool trigger_bucket;
        int bucket_pos;
        int vector_id;
        int leaf;
    };
    struct BucketBlock {
        Block* block;
        int vector_id;
        int leaf;
    };

    set<int> requested_blocks;
    vector<AccessPath> access_paths;
    access_paths.reserve(static_cast<size_t>(pad_l));
    std::map<int, int> requested_old_leaf;
    std::map<int, int> requested_new_leaf;
    std::map<int, int> access_counts_by_bucket;

    for (size_t i = 0; i < vector_ids.size(); i++) {
        int vector_id = vector_ids[i];
        int old_leaf = old_leaves[i];
        if (vector_id < 0 || vector_id >= num_blocks) {
            throw std::out_of_range("vector_id is outside the configured RingORAM block range");
        }
        if (old_leaf < 0 || old_leaf >= num_leaves) {
            throw std::out_of_range("old_leaf is outside the configured RingORAM leaf range");
        }
        if (new_leaves[i] < 0 || new_leaves[i] >= num_leaves) {
            throw std::out_of_range("new_leaf is outside the configured RingORAM leaf range");
        }
        if (!requested_blocks.insert(vector_id).second) {
            throw std::invalid_argument("duplicate vector_id in one DoubleORAM RingORAM access batch");
        }

        AccessPath path;
        path.vector_id = vector_id;
        path.old_leaf = old_leaf;
        path.new_leaf = new_leaves[i];
        path.path_positions.reserve(static_cast<size_t>(num_levels - num_cached_levels));
        for (int level = num_levels - 1; level >= num_cached_levels; level--) {
            int pos = P(old_leaf, level);
            path.path_positions.push_back(pos);
            access_counts_by_bucket[pos]++;
        }
        requested_old_leaf[vector_id] = old_leaf;
        requested_new_leaf[vector_id] = new_leaves[i];
        access_paths.push_back(std::move(path));
    }

    for (int i = static_cast<int>(vector_ids.size()); i < pad_l; i++) {
        int dummy_leaf = rand_gen->getRandomLeafWithBound(num_leaves);
        AccessPath path;
        path.vector_id = -1;
        path.old_leaf = dummy_leaf;
        path.new_leaf = -1;
        path.path_positions.reserve(static_cast<size_t>(num_levels - num_cached_levels));
        for (int level = num_levels - 1; level >= num_cached_levels; level--) {
            int pos = P(dummy_leaf, level);
            path.path_positions.push_back(pos);
            access_counts_by_bucket[pos]++;
        }
        access_paths.push_back(std::move(path));
    }

    std::set<int> reshuffle_buckets;
    for (const auto& count_pair : access_counts_by_bucket) {
        const int pos = count_pair.first;
        const int count = count_pair.second;
        if (static_cast<int>(metadata[pos].count) + count > dummy_size) {
            reshuffle_buckets.insert(pos);
        }
    }
    account_remote_metadata_transfer(access_counts_by_bucket.size());
    cnt_early_reshuffle += static_cast<int>(reshuffle_buckets.size());
    for (const auto& count_pair : access_counts_by_bucket) {
        const int pos = count_pair.first;
        if (reshuffle_buckets.find(pos) != reshuffle_buckets.end()) {
            continue;
        }
        metadata[pos].count = static_cast<uint8_t>(
            static_cast<int>(metadata[pos].count) + count_pair.second);
    }

    vector<int> positions;
    vector<int> offsets;
    vector<bool> valids;
    vector<ReadLabel> read_labels;
    std::map<int, std::vector<BucketBlock>> reshuffle_bucket_blocks;

    for (int pos : reshuffle_buckets) {
        std::vector<bool> selected_slots(static_cast<size_t>(bucket_size), false);
        int selected = 0;
        for (int slot = 0; slot < bucket_size && selected < real_bucket_size; slot++) {
            if (metadata[pos].block_ids[slot] == -1 || metadata[pos].valids[slot] == 0) {
                continue;
            }
            if (metadata[pos].block_leaves[slot] < 0 ||
                    metadata[pos].block_leaves[slot] >= num_leaves) {
                throw std::runtime_error("RingORAM metadata is missing a valid block leaf");
            }
            positions.push_back(pos);
            offsets.push_back(slot);
            valids.push_back(true);
            read_labels.push_back({true, pos, metadata[pos].block_ids[slot], metadata[pos].block_leaves[slot]});
            selected_slots[slot] = true;
            selected++;
        }
        if (selected > real_bucket_size) {
            throw std::runtime_error("RingORAM bucket has more valid real blocks than Z");
        }
        for (int slot = 0; slot < bucket_size && selected < real_bucket_size; slot++) {
            if (selected_slots[slot]) {
                continue;
            }
            positions.push_back(pos);
            offsets.push_back(slot);
            valids.push_back(true);
            read_labels.push_back({true, pos, -1, -1});
            selected_slots[slot] = true;
            selected++;
        }
        if (selected != real_bucket_size) {
            throw std::runtime_error("RingORAM early reshuffle cannot select fixed Z slots");
        }
        reshuffle_bucket_blocks[pos] = {};
    }

    auto find_real_slot = [&](int bucket_pos, int vector_id) {
        for (int slot = 0; slot < bucket_size; slot++) {
            if (metadata[bucket_pos].valids[slot] == 1 &&
                    metadata[bucket_pos].block_ids[slot] == vector_id) {
                return slot;
            }
        }
        return -1;
    };
    auto find_dummy_slot = [&](int bucket_pos) {
        for (int slot = 0; slot < bucket_size; slot++) {
            if (metadata[bucket_pos].valids[slot] == 1 &&
                    metadata[bucket_pos].block_ids[slot] == -1) {
                return slot;
            }
        }
        return -1;
    };

    uint64_t normal_positions = 0;
    uint64_t normal_groups = 0;
    for (const AccessPath& path : access_paths) {
        bool selected_any_normal_slot = false;
        for (int pos : path.path_positions) {
            if (reshuffle_buckets.find(pos) != reshuffle_buckets.end()) {
                continue;
            }
            int slot = -1;
            bool real_slot = false;
            if (path.vector_id != -1) {
                slot = find_real_slot(pos, path.vector_id);
                real_slot = slot >= 0;
            }
            if (slot < 0) {
                slot = find_dummy_slot(pos);
                real_slot = false;
            }
            if (slot < 0) {
                throw std::runtime_error("RingORAM bucket has no readable dummy slot");
            }

            positions.push_back(pos);
            offsets.push_back(slot);
            valids.push_back(true);
            read_labels.push_back({
                false,
                pos,
                real_slot ? path.vector_id : -1,
                real_slot ? path.old_leaf : -1});
            metadata[pos].valids[slot] = 0;
            selected_any_normal_slot = true;
            normal_positions++;
        }
        if (selected_any_normal_slot) {
            normal_groups++;
        }
    }

    std::set<int> found_blocks;
    auto add_target_to_stash = [&](int vector_id, int leaf, Block* block) {
        const std::pair<int, int> key = make_pair(leaf, vector_id);
        auto stash_it = mmstash.find(key);
        if (stash_it != mmstash.end()) {
            delete block;
        } else {
            mmstash[key] = block;
        }
        found_blocks.insert(vector_id);
    };

    const uint64_t cipher_block_bytes = ring_cipher_block_bytes(block_size);
    const uint64_t reshuffle_read_bytes =
        reshuffle_buckets.empty()
            ? 0
            : ring_block_read_comm_bytes(
                reshuffle_buckets.size() * static_cast<size_t>(real_bucket_size),
                cipher_block_bytes);
    const uint64_t normal_read_bytes =
        normal_groups == 0
            ? 0
            : ring_xor_read_comm_bytes(
                static_cast<size_t>(normal_positions),
                static_cast<size_t>(normal_groups),
                cipher_block_bytes);
    if (!positions.empty()) {
        vector<Block*> blocks;
        const uint64_t read_comm_bytes = reshuffle_read_bytes + normal_read_bytes;
        stat_comm_bytes += read_comm_bytes;
        account_network_transfer(read_comm_bytes);
        storage->ReadBlockBatchAsBlockRing(positions, offsets, blocks, valids, false);
        stat_server_cipher_block_reads += static_cast<uint64_t>(positions.size());

        for (size_t i = 0; i < blocks.size(); i++) {
            Block* block = blocks[i];
            const ReadLabel& label = read_labels[i];
            const int idx = block->index;
            if (idx == -1) {
                delete block;
                continue;
            }
            if (label.vector_id == -1) {
                delete block;
                continue;
            }
            if (requested_blocks.find(idx) != requested_blocks.end()) {
                add_target_to_stash(idx, requested_old_leaf[idx], block);
            } else if (label.trigger_bucket) {
                if (double_oram_leaf_cache.find(idx) != double_oram_leaf_cache.end()) {
                    delete block;
                } else {
                    reshuffle_bucket_blocks[label.bucket_pos].push_back({block, idx, label.leaf});
                }
            } else {
                delete block;
                throw std::runtime_error("RingORAM normal read returned an unrequested real block");
            }
        }
    }

    vector<RingORAMVectorRead> reads;
    reads.reserve(vector_ids.size());
    for (size_t i = 0; i < vector_ids.size(); i++) {
        const int vector_id = vector_ids[i];
        const int old_leaf = old_leaves[i];
        const int new_leaf = new_leaves[i];
        auto stash_it = mmstash.find(make_pair(old_leaf, vector_id));
        if (stash_it == mmstash.end()) {
            throw std::runtime_error("DoubleORAM RingORAM target block was not found in stash");
        }

        RingORAMVectorRead read;
        read.vector_id = vector_id;
        read.old_leaf = old_leaf;
        read.new_leaf = new_leaf;
        read.data = stash_it->second->data;
        reads.push_back(read);

        Block* block = stash_it->second;
        mmstash.erase(stash_it);
        mmstash[make_pair(new_leaf, vector_id)] = block;
        double_oram_leaf_cache[vector_id] = new_leaf;
    }

    if (!reshuffle_buckets.empty()) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::map<int, vector<Block*>> reshuffled_storage;
        std::map<int, vector<int>> reshuffled_leaves;

        for (int pos : reshuffle_buckets) {
            vector<Block*>& bucket = reshuffled_storage[pos];
            vector<int>& bucket_leaves = reshuffled_leaves[pos];
            for (BucketBlock& bucket_block : reshuffle_bucket_blocks[pos]) {
                bucket.push_back(bucket_block.block);
                bucket_leaves.push_back(bucket_block.leaf);
            }
            if (static_cast<int>(bucket.size()) > real_bucket_size) {
                throw std::runtime_error("RingORAM reshuffle bucket has more than Z real blocks");
            }
            while (static_cast<int>(bucket.size()) < bucket_size) {
                bucket.push_back(new Block(block_size));
                bucket_leaves.push_back(-1);
            }

            std::vector<int> order(bucket.size());
            std::iota(order.begin(), order.end(), 0);
            std::shuffle(order.begin(), order.end(), rng);
            std::vector<Block*> shuffled_bucket;
            std::vector<int> shuffled_leaves;
            shuffled_bucket.reserve(bucket.size());
            shuffled_leaves.reserve(bucket_leaves.size());
            for (int index : order) {
                shuffled_bucket.push_back(bucket[index]);
                shuffled_leaves.push_back(bucket_leaves[index]);
            }
            bucket.swap(shuffled_bucket);
            bucket_leaves.swap(shuffled_leaves);

            metadata[pos].count = 0;
            metadata[pos].valids.set();
            metadata[pos].block_ids.assign(bucket_size, -1);
            metadata[pos].block_leaves.assign(bucket_size, -1);
            for (int slot = 0; slot < bucket_size; slot++) {
                metadata[pos].block_ids[slot] = bucket[slot]->index;
                metadata[pos].block_leaves[slot] = bucket_leaves[slot];
            }
        }

        const uint64_t write_comm_bytes = ring_bucket_write_comm_bytes(
            reshuffled_storage.size(),
            static_cast<size_t>(bucket_size),
            cipher_block_bytes);
        stat_comm_bytes += write_comm_bytes;
        account_network_transfer(write_comm_bytes);
        storage->WriteBucketBatchMapAsBlockRing(reshuffled_storage, bucket_size, true);
    }

    cnt_delayed_leaves += pad_l;

    if (!lazy_eveiction) {
        evict_and_write_back();
    }

    return reads;
}

int RingORAM::insert_block_to_stash(
        int vector_id,
        const std::vector<int>& data,
        int dummy_access_count) {
    if (vector_id < 0 || vector_id >= num_blocks) {
        throw std::out_of_range("new RingORAM vector_id is outside the reserved block-id capacity");
    }
    if (static_cast<int>(data.size()) != block_size) {
        throw std::invalid_argument("new RingORAM block payload has the wrong block_size");
    }
    if (dummy_access_count < 0) {
        throw std::invalid_argument("RingORAM dummy_access_count must be non-negative");
    }
    if (rand_gen == nullptr) {
        throw std::logic_error("RingORAM insertion requires a random leaf generator");
    }
    // Use the normal access routine with no real target. This preserves the
    // query's dummy-path and early-reshuffle behavior without fetching a block
    // that does not yet exist on the server.
    if (dummy_access_count > 0) {
        access_without_position_map({}, {}, {}, dummy_access_count);
    }

    const int new_leaf = rand_gen->getRandomLeafWithBound(num_leaves);
    Block* block = new Block(block_size);
    block->index = vector_id;
    block->data = data;
    const std::pair<int, int> key = make_pair(new_leaf, vector_id);
    mmstash[key] = block;
    double_oram_leaf_cache[vector_id] = new_leaf;
    return new_leaf;
}

const std::unordered_map<int, int>& RingORAM::double_oram_cached_leaves() const {
    return double_oram_leaf_cache;
}

void RingORAM::clear_double_oram_leaf_cache() {
    double_oram_leaf_cache.clear();
}

int RingORAM::globalG() {
    return this->G;
}

void RingORAM::evict_and_write_back(){

    const hires_clock::time_point t_ev0 = hires_clock::now();
    const double net_time0 = stat_net_total_time_sec;
    stat_evict_invocations++;

    // find the # evicted paths
    int num_paths = (cnt_delayed_leaves / evict_rate);
    // cout << "evict - num_paths: " << num_paths << endl;

    if(num_paths == 0){
        stat_max_stash_blocks_after_evict = std::max(
            stat_max_stash_blocks_after_evict,
            static_cast<uint64_t>(mmstash.size()));
        double elapsed_sec = elapsed_sec_since(t_ev0);
        if (!network_simulation_sleep_enabled()) {
            elapsed_sec += stat_net_total_time_sec - net_time0;
        }
        stat_evict_time_sec += elapsed_sec;
        return;
    }

    stat_evict_effective++;
    const hires_clock::time_point t_effective0 = hires_clock::now();
    const double net_effective_time0 = stat_net_total_time_sec;

    // generate paths in reverse-lexicographical order
    vector<int> leaves;
    for(int i = 0; i < num_paths; i++){
        int g = ReverseBits(this->G, num_levels - 1);
        this->G = this->G + 1;
        leaves.push_back(g);
    }

    // Generate positions.
    vector<int> positions;
    set<int> position_cache; // avoid double evict in case of path overlap
    for (int l = num_levels - 1; l >= num_cached_levels; l--) {
        for(int leaf : leaves){
            int pos = P(leaf, l);
            if(position_cache.find(pos) == position_cache.end()){
                positions.push_back(pos);
                position_cache.insert(pos);
            }
        }
    }
    account_remote_metadata_transfer(positions.size(), true);

    vector<int> read_positions;
    vector<int> offsets;
    vector<bool> valids;
    vector<int> read_leaves;
    read_positions.reserve(positions.size() * real_bucket_size);
    offsets.reserve(positions.size() * real_bucket_size);
    valids.reserve(positions.size() * real_bucket_size);
    read_leaves.reserve(positions.size() * real_bucket_size);
    for (int pos : positions) {
        for (int offset = 0; offset < bucket_size; offset++) {
            if (metadata[pos].block_ids[offset] == -1 || metadata[pos].valids[offset] == 0) {
                continue;
            }
            if (offset >= static_cast<int>(metadata[pos].block_leaves.size()) ||
                    metadata[pos].block_leaves[offset] < 0 ||
                    metadata[pos].block_leaves[offset] >= num_leaves) {
                throw std::runtime_error("RingORAM metadata is missing a valid block leaf");
            }
            read_positions.push_back(pos);
            offsets.push_back(offset);
            valids.push_back(true);
            read_leaves.push_back(metadata[pos].block_leaves[offset]);
        }
    }

    const uint64_t cipher_block_bytes = ring_cipher_block_bytes(block_size);
    std::vector<Block*> read_blocks;
    if (!read_positions.empty()) {
        const uint64_t read_comm_bytes =
            ring_block_read_comm_bytes(read_positions.size(), cipher_block_bytes);
        stat_comm_bytes += read_comm_bytes;
        account_network_transfer(read_comm_bytes, 1, true);
        storage->ReadBlockBatchAsBlockRing(read_positions, offsets, read_blocks, valids, false);
        stat_server_cipher_block_reads += static_cast<uint64_t>(read_positions.size());
    }

    for (size_t i = 0; i < read_blocks.size(); i++) {
        Block* block = read_blocks[i];
        if (block->index == -1) {
            delete block;
            continue;
        }
        if (double_oram_leaf_cache.find(block->index) != double_oram_leaf_cache.end()) {
            delete block;
            continue;
        }

        const std::pair<int, int> key = make_pair(read_leaves[i], block->index);
        auto stash_it = mmstash.find(key);
        if (stash_it != mmstash.end()) {
            delete stash_it->second;
            stash_it->second = block;
        } else {
            mmstash[key] = block;
        }
    }

    for (int pos : positions) {
        metadata[pos].count = 0;
        metadata[pos].valids.set();
        metadata[pos].block_ids.assign(bucket_size, -1);
        metadata[pos].block_leaves.assign(bucket_size, -1);
    }

    std::map<int, vector<Block*>> evicted_storage;
    std::map<int, vector<int>> evicted_leaves;
    for (int pos : positions) {
        evicted_storage[pos] = {};
        evicted_leaves[pos] = {};
    }

    for (int l = num_levels - 1; l >= num_cached_levels; l--) {
        std::set<int> visited_at_level;
        for(int leaf :  leaves){
            int Pxl = P(leaf, l);
            if(!visited_at_level.insert(Pxl).second){
                continue;
            }
            auto storage_it = evicted_storage.find(Pxl);
            if (storage_it == evicted_storage.end()) {
                continue;
            }

            vector<Block*>& bkt = storage_it->second;
            vector<int>& bkt_leaves = evicted_leaves[Pxl];
            std::pair<int, int> leaf_range = Q(leaf, l);
            for (auto it = mmstash.lower_bound(make_pair(leaf_range.first, -1));
                    it != mmstash.end() &&
                    it->first.first < leaf_range.second &&
                    static_cast<int>(bkt.size()) < real_bucket_size;) {
                auto next_it = std::next(it);
                int block_leaf = it->first.first;

                if (Pxl != P(block_leaf, l)) {
                    it = next_it;
                    continue;
                }

                bkt.push_back(it->second);
                bkt_leaves.push_back(block_leaf);
                mmstash.erase(it);
                it = next_it;
            }
        }
    } 

    std::random_device rd;
    std::mt19937 g(rd());
    for (auto& pair : evicted_storage) {
        int pos = pair.first;
        vector<Block*>& bkt = pair.second;
        vector<int>& bkt_leaves = evicted_leaves[pos];
        while(static_cast<int>(bkt.size()) < bucket_size){
            bkt.push_back(new Block(block_size));
            bkt_leaves.push_back(-1);
        }

        std::vector<int> order(bkt.size());
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), g);
        std::vector<Block*> shuffled_bkt;
        std::vector<int> shuffled_leaves;
        shuffled_bkt.reserve(bkt.size());
        shuffled_leaves.reserve(bkt_leaves.size());
        for (int index : order) {
            shuffled_bkt.push_back(bkt[index]);
            shuffled_leaves.push_back(bkt_leaves[index]);
        }
        bkt.swap(shuffled_bkt);
        bkt_leaves.swap(shuffled_leaves);

        metadata[pos].count = 0;
        metadata[pos].valids.set();
        for(int i = 0; i < bucket_size; i++){
            metadata[pos].block_ids[i] = bkt[i]->index;
            metadata[pos].block_leaves[i] = bkt_leaves[i];
        }
    }

    const uint64_t write_comm_bytes = ring_bucket_write_comm_bytes(
        evicted_storage.size(),
        static_cast<size_t>(bucket_size),
        cipher_block_bytes);
    stat_comm_bytes += write_comm_bytes;
    account_network_transfer(write_comm_bytes, 1, true, true);
    storage->WriteBucketBatchMapAsBlockRing(evicted_storage, bucket_size, false);

    cnt_delayed_leaves -= num_paths * evict_rate;
    cnt_q ++;
    stat_max_stash_blocks_after_evict = std::max(
        stat_max_stash_blocks_after_evict,
        static_cast<uint64_t>(mmstash.size()));

    double effective_elapsed_sec = elapsed_sec_since(t_effective0);
    double elapsed_sec = elapsed_sec_since(t_ev0);
    if (!network_simulation_sleep_enabled()) {
        effective_elapsed_sec += stat_net_total_time_sec - net_effective_time0;
        elapsed_sec += stat_net_total_time_sec - net_time0;
    }
    stat_evict_effective_time_sec += effective_elapsed_sec;
    stat_evict_time_sec += elapsed_sec;

    // if(mmstash.size() != 0){
    //     cout << "evict mmsatsh size: " << mmstash.size() << " / " << num_cached_bucket * real_bucket_size << " resuffles: " << cnt_early_reshuffle*1.0 / cnt_q<< endl;
    // }
    // cnt_early_reshuffle = 0;
}

std::pair<int, int> RingORAM::Q(int leaf, int level){
    int l = num_levels - 1 - level;
    // cout << "Q l: " << l << endl;
    int left = leaf >> l;
    // cout << "Q left: " << left << endl;
    left = left << l;
    // cout << "Q left: " << left << endl;
    int right = left + (1 << l);
    // cout << "Q right: " << right << endl;
    return std::make_pair(left, right);
}

int RingORAM::P(int leaf, int level) {
    /*
    * This function should be deterministic. 
    * INPUT: leaf in range 0 to num_leaves - 1, level in range 0 to num_levels - 1. 
    * OUTPUT: Returns the location in the storage of the bucket which is at the input level and leaf.
    */
    return (1<<level) - 1 + (leaf >> (this->num_levels - level - 1));
}


void RingORAM::loadMetaDataFromFile(const char* fname) {
    FILE* f = fopen(fname, "r");
    if (!f) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }
    int s;
    fread(&s, 1, sizeof(int), f);
    vector<int> block_ids = std::vector<int>(s);
    fread(block_ids.data(), 1, s*sizeof(int), f);
    assert(s == num_buckets*bucket_size);
    for(int i = 0; i < num_buckets; i++){
        metadata[i].count = 0;
        metadata[i].valids.set();
        metadata[i].block_ids = vector<int>(
            block_ids.begin() + i * bucket_size,
            block_ids.begin() + (i+1) * bucket_size
        );
        metadata[i].block_leaves.assign(bucket_size, -1);

        // cout << "metadata for "<< i << " : ";
        // for(auto id : metadata[i].block_ids){
        //     cout << id << " ";
        // }
        // cout << endl;

        // cout << "metadata for "<< i << " : ";
        // for(auto id : metadata[i].block_ids){
        //     cout << id << " ";
        // }
        // cout << endl;
    }

}
    
int RingORAM::getNumLeaves() {
    return this->num_leaves;

}

int RingORAM::getNumLevels() {
    return this->num_levels;

}

int RingORAM::getNumBlocks() {
    return this->num_blocks;

}

int RingORAM::getNumBuckets() {
    return this->num_buckets;

}

} // double_oram 命名空间
