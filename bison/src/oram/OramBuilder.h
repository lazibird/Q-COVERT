#ifndef PORAM_BUILDER_H
#define PORAM_BUILDER_H

#include <iostream>
#include <iomanip>
#include <cmath>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <fstream>
#include <sys/time.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

//for diskannindexreader
#include <sys/types.h>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>


// diskann
// #include "oram_index.h"

// oram
#include "OramInterface.h"
#include "node.h"
#include "OramAPI.h"

#define NUM_THREADS 32

using namespace std;
// using namespace diskann;



class DiskANNIndexReader {
public:
    struct NodeData {
        std::vector<float>   embed;    // dim floats
        std::vector<int32_t> neighbors; // up to R neighbor IDs
        int32_t nbr_count;
    };

    DiskANNIndexReader(const std::string& index_path,
                       uint32_t dim,
                       uint32_t max_degree_R,
                       uint32_t coordinate_bytes)
        : path_(index_path),
          dim_(dim),
          R_(max_degree_R),
          coordinate_bytes_(coordinate_bytes),
          fd_(-1)
    {
        // 4KB sector, 1024 int32_t of header in sector 0
        static constexpr off_t SECTOR_BYTES  = 4096;
        static constexpr off_t HEADER_BYTES  = 4096; // we observed first 1024 ints as header

        fd_ = ::open(path_.c_str(), O_RDONLY);
        if (fd_ == -1) {
            throw std::runtime_error("Failed to open index file '" + path_ +
                                     "': " + std::strerror(errno));
        }

        sector_bytes_  = SECTOR_BYTES;
        header_bytes_  = HEADER_BYTES;

        if (coordinate_bytes_ != 1 && coordinate_bytes_ != 4)
            throw std::invalid_argument("Coordinate width must be 1 or 4 bytes");
        vector_bytes_   = static_cast<off_t>(dim_) * coordinate_bytes_;
        nbr_count_bytes_ = sizeof(int32_t);
        neighbors_bytes_ = static_cast<off_t>(R_)   * sizeof(int32_t);
        node_bytes_      = vector_bytes_ + nbr_count_bytes_ + neighbors_bytes_;

        if (node_bytes_ <= 0 || node_bytes_ > sector_bytes_) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("Invalid node_bytes; check dim and R vs 4096‑byte sector.");
        }

        nodes_per_sector_ = sector_bytes_ / node_bytes_;
        if (nodes_per_sector_ == 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("nodes_per_sector_ == 0; node record too large for sector.");
        }

        uint64_t metadata[5]{}; // npts, dimension, medoid, node bytes, nodes/sector
        if (::pread(fd_, metadata, sizeof(metadata), 8) != sizeof(metadata) ||
            metadata[0] == 0 || metadata[1] != dim_ ||
            metadata[3] != static_cast<uint64_t>(node_bytes_) ||
            metadata[4] != nodes_per_sector_) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("DiskANN index metadata disagrees with dimension, type or R");
        }
        index_point_count_ = metadata[0];

        // Debug summary
        std::cout << "DiskANNIndexReader\n"
                  << "  path            = " << path_ << "\n"
                  << "  dim             = " << dim_ << "\n"
                  << "  R               = " << R_ << "\n"
                  << "  vector_bytes    = " << vector_bytes_ << "\n"
                  << "  neighbors_bytes = " << neighbors_bytes_ << "\n"
                  << "  neighbor_count_bytes = " << nbr_count_bytes_ << "\n"
                  << "  node_bytes      = " << node_bytes_ << "\n"
                  << "  nodes_per_sector= " << nodes_per_sector_ << "\n"
                  << "  header_bytes    = " << header_bytes_ << "\n";
    }

    ~DiskANNIndexReader() {
        if (fd_ != -1) {
            ::close(fd_);
        }
    }

    // Read full-precision vector + neighbor IDs for node_id
    NodeData read_node(uint32_t node_id) const {
        if (fd_ == -1) {
            throw std::runtime_error("Index file not open");
        }
        if (node_id >= index_point_count_)
            throw std::out_of_range("Index node id exceeds point count");

        // Which sector (after header sector 0) and which slot inside that sector?
        uint64_t sector_idx_after_header = node_id / nodes_per_sector_;
        uint64_t slot_in_sector          = node_id % nodes_per_sector_;

        // Byte offset of start of that sector (sector 0 is header, so +1)
        off_t sector_offset = header_bytes_ +
                              static_cast<off_t>(sector_idx_after_header) * sector_bytes_;

        // Byte offset of this node within that sector
        off_t in_sector_offset = static_cast<off_t>(slot_in_sector) * node_bytes_;

        // Final file offset
        off_t node_offset = sector_offset + in_sector_offset;

        // Seek to node start
        off_t pos = ::lseek(fd_, node_offset, SEEK_SET);
        if (pos == (off_t)-1) {
            throw std::runtime_error("lseek failed for node " + std::to_string(node_id) +
                                     ": " + std::strerror(errno));
        }

        NodeData out;
        out.embed.resize(dim_);
        out.neighbors.resize(R_);

        // The on-disk vector has the dataset's native element width.
        std::vector<uint8_t> vector_data(vector_bytes_);
        char* buf = reinterpret_cast<char*>(vector_data.data());
        off_t to_read = vector_bytes_;
         while (to_read > 0) {
            ssize_t n = ::read(fd_, buf, static_cast<size_t>(to_read));
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
            }
            if (n == 0) {
                throw std::runtime_error("Unexpected EOF while reading index file");
            }
            buf     += n;
            to_read -= n;
        }

        if (coordinate_bytes_ == 4) {
            std::memcpy(out.embed.data(), vector_data.data(), vector_bytes_);
        } else {
            for (uint32_t i = 0; i < dim_; ++i) out.embed[i] = vector_data[i];
        }

        buf = reinterpret_cast<char*>(&out.nbr_count);
        to_read = nbr_count_bytes_;
         while (to_read > 0) {
            ssize_t n = ::read(fd_, buf, static_cast<size_t>(to_read));
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
            }
            if (n == 0) {
                throw std::runtime_error("Unexpected EOF while reading index file");
            }
            buf     += n;
            to_read -= n;
        }

        if (out.nbr_count < 0 || static_cast<uint32_t>(out.nbr_count) > R_)
            throw std::runtime_error("Invalid neighbor count in index node " + std::to_string(node_id));

        buf = reinterpret_cast<char*>(out.neighbors.data());
        to_read = neighbors_bytes_;
         while (to_read > 0) {
            ssize_t n = ::read(fd_, buf, static_cast<size_t>(to_read));
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
            }
            if (n == 0) {
                throw std::runtime_error("Unexpected EOF while reading index file");
            }
            buf     += n;
            to_read -= n;
        }

        return out;
    }

    // Just neighbors (no vector)
    std::vector<int32_t> read_neighbors(uint32_t node_id) const {
        NodeData d = read_node(node_id);
        return d.neighbors;
    }

    void printNodeData(NodeData node){
        cout << "Embeddings:" << endl;
        for(int i = 0; i< node.embed.size(); i++){
            cout << node.embed[i] << " ";
        }
        cout << endl;

        cout << "Nbr Count:" << node.nbr_count << endl;

        cout << "Nbrs:" << endl;
        for(int i = 0; i< node.neighbors.size(); i++){
            cout << (node_id_t)node.neighbors[i] << " ";
        }
        cout << endl;

    }

private:
    void read_fully(char* buf, off_t nbytes) const {
        off_t to_read = nbytes;
        while (to_read > 0) {
            ssize_t n = ::read(fd_, buf, static_cast<size_t>(to_read));
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
            }
            if (n == 0) {
                throw std::runtime_error("Unexpected EOF while reading index file");
            }
            buf     += n;
            to_read -= n;
        }
    }

    std::string path_;
    uint32_t    dim_;
    uint32_t    R_;
    uint32_t    coordinate_bytes_;
    uint64_t    index_point_count_;

    int   fd_;
    off_t sector_bytes_;
    off_t header_bytes_;
    off_t vector_bytes_;
    off_t neighbors_bytes_;
    off_t nbr_count_bytes_;
    off_t node_bytes_;
    uint32_t nodes_per_sector_;
};

template<typename T, typename LabelT = node_id_t>
class OramBuilder {

    public:

    float large_number = 1;

    const char* buckets_path;
    const char* block_map_path;
    const char* position_map_path;
    const char* metadata_path;

    const char* hash_path;
    // bool integrity;
    // this->integrity is always considered true during ORAM init. so removing flag

    RingOramConfig* config;
    RandForOramInterface* random;
    FakeBlockFetcherRing<T, LabelT>* block_fetcher;

    map<node_id_t, vector<node_id_t>> graph;

    OramBuilder(
        const char* buckets_path,
        const char* block_map_path,
        const char* position_map_path,
        const char* metadata_path,
        const char* hash_path,
        node_id_t num_points,
        const float large_number,
        RingOramConfig* config,
        int build_type
    ) {
        this->buckets_path = buckets_path;
        this->block_map_path = block_map_path;
        this->position_map_path = position_map_path;
        this->metadata_path = metadata_path;
        
        this-> hash_path = hash_path;
        // this->integrity is always considered true during build

        this->large_number = large_number;

        this->config = config;
        this->random = new RandomForOram();
        this->block_fetcher = new FakeBlockFetcherRing<T, LabelT>(num_points, *config, this->random, build_type);
    }

    void load_graph(const char* graph_path) {
        int fd = open(graph_path, O_RDONLY);
        if (fd == -1) {
            perror("Failed to open file");
            abort();
        }

        // Get file size
        off_t file_size = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);

        cout << "file size of database from base path: " << file_size << endl;

        //load into memory
        char* data = static_cast<char*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
        if (data == MAP_FAILED) {
            perror("mmap failed");
            close(fd);
            abort();
        }

        // Find line offsets
        std::vector<size_t> line_starts;
        line_starts.push_back(0);
        for (size_t i = 0; i < file_size; ++i) {
            if (data[i] == '\n') {
                if (i + 1 < file_size)
                    line_starts.push_back(i + 1);
            }
        }

        cout << "lines: " << line_starts.size() << endl;
        graph.clear();

        // Parallel parse each line
        #pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < line_starts.size(); ++i) {
            const char* line = &data[line_starts[i]];
            const char* end = (i + 1 < line_starts.size()) ? &data[line_starts[i + 1] - 1] : &data[file_size];

            std::string_view line_view(line, end - line);

            std::istringstream iss{std::string(line_view)};
            int node_id, nbr_cnt;
            iss >> node_id >> nbr_cnt;  //each line starts with node_id nbr_count
            // cout << "node_id and nbr count: " << node_id << " " << nbr_cnt << endl;

            std::vector<node_id_t> neighbors(DiskANNNode<T, LabelT>::n_neighbors, -1);
            for (int j = 0; j < nbr_cnt && j < neighbors.size(); ++j) {
                int nbr_id;
                iss >> nbr_id;
                neighbors[j] = nbr_id;
            }
            
            // if(node_id ==0 ){
            //     for(int i=0; i< neighbors.size(); i++){ cout << neighbors[i] << " ";}
                
            // }
            //NEIGHBORS ARE LOADED CORRECTLY 
            // SINCE node_id to node_id
            // cout << "done" << endl;
            // abort();
            
            #pragma omp critical
            {
                graph[node_id] = std::move(neighbors);
            }

            if (node_id % 100000 == 0) {
                #pragma omp critical
                std::cout << "-> Done loading graph[node_id] till node " << node_id << std::endl;
            }
        }
        cout << "\ngraph loaded size: " << graph.size() << endl;
        
        munmap(data, file_size);
        close(fd);
    }


    // void load_graph(const char* graph_path){
    //     ifstream infile(graph_path);  // Replace with your filename
    //     if (!infile.is_open()) {
    //         cerr << "could not open file " << graph_path << endl;
    //         perror("");
    //         abort();
    //     }

    //     string line;
    //     int i = 0; 
    //     while (getline(infile, line)) {
    //         // if(i < 60000000){
    //         //     i++;
    //         //     continue;
    //         // }

    //         istringstream iss(line);

    //         string node_id;
    //         iss >> node_id;
    //         graph[(node_id_t) stoi(node_id)] = vector<node_id_t>(DiskANNNode<T, LabelT>::n_neighbors, -1);
            
    //         string nbr_cnt_s;
    //         iss >> nbr_cnt_s;
    //         int nbr_cnt = stoi(nbr_cnt_s);

    //         for(int i = 0; i < nbr_cnt; i++){
    //             string nbr_id;
    //             iss >> nbr_id;
    //             graph[stoi(node_id)][i] = ((node_id_t) stoi(nbr_id));
    //         }

    //         if(stoi(node_id) % 100000 == 0){
    //             cout << "-> Done loading graph till node " << node_id << endl;
    //         }
    //         // if(i == 70000000){
    //         //     break;
    //         // }
    //         i++;
    //     }

    //     infile.close();
    // }

    void load_index(const char* index_file_path){
        // wait for Sandhya
        load_graph(index_file_path);
    }   

    void insert_real_blocks(T* database){   
        std::cout << "-> Start clustering: " << std::endl;
        node_id_t num_points = block_fetcher->num_points;

        int dim = DiskANNNode<T, LabelT>::dim;

        cout << "num points: " << num_points << endl;
        for(size_t node_id = 0; node_id < num_points; node_id++){
            DiskANNNode<T, LabelT>* oram_node = new DiskANNNode<T, LabelT>();

            //set ID
            oram_node->set_id(node_id);
            
            //set NBRs
            oram_node->set_neighbors(graph[node_id]);
            graph[node_id].clear();
    
            //set COORDs
            vector<T> coords;
            for(size_t offset = node_id*dim; offset < (node_id+1)*dim; offset++){
                coords.push_back(database[offset]*large_number);
            }
            oram_node->set_coords(coords);

            // oram_node->print_node_info_and_Bytes();
            

            block_id_t block_id = block_fetcher->add_block_with_list(oram_node, {node_id});
            
            // cout << "Hello7\n";
            
            if(node_id % 100000 == 0){
                std::cout << "-> Done clustering: " << node_id << std::endl;
            }
            
            delete oram_node;
        }
        cout << "Real blocks insertion completed!" << endl;
    }


    void disk_initiate_and_write_blocks(string index_path){


        /*
        fbuckets
        fbids
        fhash
        */


        // fbuckets HEADERS
        FILE* fbuckets = fopen(buckets_path, "w");
        if (!fbuckets) {
            fprintf(stderr, "could not open %s\n", buckets_path);
            perror("");
            abort();
        }
        fwrite(&(config->num_buckets), 1, sizeof(int), fbuckets);
        int sb_size = SBucket::getCipherSize();
        fwrite(&sb_size, 1, sizeof(int), fbuckets);
        bool mal_security = true;   // considered true during ORAM init
        fwrite(&mal_security, 1, sizeof(bool), fbuckets);   

        // fbids HEADERS
        FILE* fbids = fopen(metadata_path, "w");
        if (!fbids) {
            fprintf(stderr, "could not open %s\n", metadata_path);
            perror("");
            abort();
        }
        int s = config->num_buckets * config->bucket_size;
        fwrite(&s, 1, sizeof(int), fbids);
        
        // fhash HEADERS
        FILE* fhash = fopen(hash_path, "w");
        if (!fhash) {
            fprintf(stderr, "could not open %s\n", hash_path);
            perror("");
            abort();
        }
        
            

        
        //common params
        const int per_block_size =  (1 + config->block_size) * sizeof(int);
        const size_t dim = DiskANNNode<T, LabelT>::dim;
        const size_t R = DiskANNNode<T, LabelT>::n_neighbors;
        const node_id_t total_nodes = block_fetcher->num_points;


        // per bucket structures
        std::vector<SBucket*> sbuckets;
        sbuckets.resize(config->bucket_size, nullptr);
        for(int x=0; x < config->bucket_size; x++){
            sbuckets[x] = new SBucket(true);
            // cout << "init sbkt ptr @" << sbuckets[x] << endl;
        }

        std::vector<Bucket*> buckets;
        buckets.resize(config->real_bucket_size, nullptr);
        for(int x=0; x < config->real_bucket_size; x++){
            buckets[x] = new Bucket();
            // cout << "init bkt ptr @" << buckets[x] << endl;
        }

        vector<block_id_t> block_ids;
        block_ids.resize(config->bucket_size);


        //malicious security structures

        // hash of each blocks in a bucket
        uint8_t* per_block_hash = new uint8_t[SHA256_DIGEST_LENGTH * config->bucket_size];  // for all blocks in each bucket

        
        size_t bucket_tree_height = ceil(log10(config->bucket_size) / log10(2)) + 1;
        size_t bucket_num_hashes = pow(2, bucket_tree_height) - 1;
        size_t sec_MT_size = SHA256_DIGEST_LENGTH * bucket_num_hashes;
        // secondary merkle tree for a bucket
        uint8_t* sec_MT = new uint8_t[sec_MT_size];
        memset(sec_MT, 0, sec_MT_size);

        // needed to construct primary merkle tree
        uint8_t* sec_MT_roots = new uint8_t[config->num_buckets * SHA256_DIGEST_LENGTH];

        // primary merkle tree for the ORAM tree
        uint8_t* prim_MT = new uint8_t[config->num_buckets * SHA256_DIGEST_LENGTH];



        // loop variables
        node_id_t global_node_id = 0;
        off_t cur_bucket = 0;
        off_t cur_bucket_size = 0;
        DiskANNIndexReader reader(index_path.c_str(), dim, R, sizeof(T));


        while(global_node_id < total_nodes){


            // CONSTRUCTING BLOCK FOR EACH NODE
            {
                DiskANNNode<T, LabelT>* oram_node = new DiskANNNode<T, LabelT>();
                auto node = reader.read_node(global_node_id);
                
                std::vector<node_id_t> oram_neighbors;
                std::vector<T> oram_coords;
                
                for(size_t x = 0; x < node.nbr_count; x++){
                    oram_neighbors.push_back(node.neighbors[x]);
                }
                for(size_t x = node.nbr_count; x < R; x++){
                    oram_neighbors.push_back(-1);
                }
                for(size_t x = 0; x < dim; x++){
                    if constexpr (sizeof(T) == 1) oram_coords.push_back(static_cast<T>(node.embed[x]));
                    else oram_coords.push_back(node.embed[x]*large_number);
                }
                
                oram_node->set_id(global_node_id);
                oram_node->set_coords(oram_coords);
                oram_node->set_neighbors(oram_neighbors);
                
                block_id_t block_id = block_fetcher->add_block_with_list_for_bucket(oram_node, global_node_id, cur_bucket, buckets[cur_bucket_size]);
                
                delete oram_node;
                cur_bucket_size++;   
            }
            

            if(cur_bucket_size >= config->real_bucket_size){
                // buckets is filled
                assert(cur_bucket_size == config->real_bucket_size);

                //fill sbuckets with real data blocks
                for(int j=0; j < config->real_bucket_size; j++){
                    SBucket* sbkt = sbuckets[j];
                    Bucket* bkt = buckets[j];

                    unsigned char* payload = new unsigned char[SBucket::getCipherSize()];

                    Block rrb = bkt->getRRBlock();
                    rrb.to_ptr(payload);
                    block_ids[j] = rrb.index;

                    int ctx_len = encrypt_wrapper(payload, per_block_size*Bucket::getMaxSize(), sbkt->data);
                    assert(ctx_len == SBucket::getCipherSize());
                    
                    delete [] payload;
                }
                for(int j= config->real_bucket_size; j < config->bucket_size; j++){
                    SBucket* sbkt = sbuckets[j];

                    unsigned char* payload = new unsigned char[SBucket::getCipherSize()];
                    Block dummy = Block(config->block_size);
                    dummy.to_ptr(payload);

                    block_ids[j] = -1;
                    int ctx_len = encrypt_wrapper(payload, per_block_size*Bucket::getMaxSize(), sbkt->data);
                    assert(ctx_len == SBucket::getCipherSize());

                    delete [] payload;
                }

                //integrity check
                // compute hash for each block in this bucket
                for(size_t j = 0; j < config->bucket_size; j++){
                    sha256_wrapper(
                        sbuckets[j]->data, 
                        SBucket::getCipherSize(), 
                        per_block_hash + j*SHA256_DIGEST_LENGTH);
                }


                // compute secondary merkle tree for this bucket
                for(size_t j=bucket_num_hashes; j>= 1; j--){
                    
                    size_t id = j-1;
                    size_t lid = 2*j-1;
                    size_t rid = 2*j;

                    if(lid > bucket_num_hashes - 1){
                        // id is leaf node
                        // leaf of sec-merkle-tree is just hash of all blocks
                        size_t offset = j - (bucket_num_hashes + 1)/2;  // position of block's hash
                        if(offset < config->bucket_size){
                            uint8_t* src = per_block_hash + offset * SHA256_DIGEST_LENGTH;
                            uint8_t* dst = sec_MT + id * SHA256_DIGEST_LENGTH;
                            memcpy(dst, src, SHA256_DIGEST_LENGTH);
                        }
                    }
                    else {
                        uint8_t* dst = sec_MT +  id * SHA256_DIGEST_LENGTH;
                        uint8_t* lsrc = sec_MT +  lid * SHA256_DIGEST_LENGTH;
                        uint8_t* rsrc = sec_MT +  rid * SHA256_DIGEST_LENGTH;

                        sha256_twin_input_wrapper(
                            lsrc, SHA256_DIGEST_LENGTH,
                            rsrc, SHA256_DIGEST_LENGTH,
                            dst
                        );

                    }
                }   
                //store root of secondary merkle tree
                uint8_t* src = sec_MT + 0 * SHA256_DIGEST_LENGTH;
                uint8_t* dst = sec_MT_roots + cur_bucket * SHA256_DIGEST_LENGTH;
                memcpy(dst, src, SHA256_DIGEST_LENGTH);




                //writing sbuckets into file
                for(size_t i=0; i < config->bucket_size; i++){
                    size_t total_written = 0;
                    size_t bytes_to_write = SBucket::getCipherSize();

                    while (total_written < bytes_to_write) {
                        size_t written = fwrite(sbuckets[i]->data, 1, bytes_to_write - total_written, fbuckets);
                        if (written == 0) {
                            if (ferror(fbuckets)) {
                                perror("Write error");
                                break;
                            }
                        }
                        total_written += written;
                    }
                }

                //writing block_ids into fbids file
                fwrite(block_ids.data(), 1, block_ids.size()*sizeof(int), fbids);

                //writing sec merkle tree into fhash file
                size_t total_written = 0;
                size_t bytes_to_write = sec_MT_size;
                while (total_written < bytes_to_write) {
                    size_t written = fwrite(sec_MT + total_written, 1, bytes_to_write - total_written, fhash);
                    if (written == 0) {
                        if (ferror(fhash)) {
                            perror("Write error");
                            break;
                        }
                    }
                    total_written += written;
                }
                
                

                cur_bucket_size = 0;
                cur_bucket ++;

                if(cur_bucket %10000 == 0){
                    cout << "done writing bucket " << cur_bucket << " out of " << config->num_buckets << endl;
                }
            }

            global_node_id ++;
        }
        cout << "All nodes done. current bucket: " << cur_bucket << " @offset: " << cur_bucket_size << endl;
        while(cur_bucket < config->num_buckets){

            //there is at most one semi filled bucket
            if(cur_bucket_size > 0){
                //encrypt and fill real blocks in sbuckets
                for(int j=0; j < cur_bucket_size; j++){
                    SBucket* sbkt = sbuckets[j];
                    Bucket* bkt = buckets[j];

                    unsigned char* payload = new unsigned char[SBucket::getCipherSize()];

                    Block rrb = bkt->getRRBlock();
                    rrb.to_ptr(payload);
                    block_ids[j] = rrb.index;

                    int ctx_len = encrypt_wrapper(payload, per_block_size*Bucket::getMaxSize(), sbkt->data);
                    assert(ctx_len == SBucket::getCipherSize());
                    
                    delete [] payload;
                }
            }

            // fill dummies in sbuckets
            for(int j= cur_bucket_size; j < config->bucket_size; j++){
                SBucket* sbkt = sbuckets[j];

                unsigned char* payload = new unsigned char[SBucket::getCipherSize()];
                Block dummy = Block(config->block_size);
                dummy.to_ptr(payload);

                block_ids[j] = -1;
                int ctx_len = encrypt_wrapper(payload, per_block_size*Bucket::getMaxSize(), sbkt->data);
                assert(ctx_len == SBucket::getCipherSize());

                delete [] payload;
            }


            // compute hash for each block in this bucket
            for(size_t j = 0; j < config->bucket_size; j++){
                sha256_wrapper(
                    sbuckets[j]->data, 
                    SBucket::getCipherSize(), 
                    per_block_hash + j*SHA256_DIGEST_LENGTH);
            }

            // compute secondary merkle tree for this bucket
            for(size_t j=bucket_num_hashes; j>= 1; j--){
                
                size_t id = j-1;
                size_t lid = 2*j-1;
                size_t rid = 2*j;

                if(lid > bucket_num_hashes - 1){
                    // id is leaf node
                    // leaf of sec-merkle-tree is just hash of all blocks
                    size_t offset = j - (bucket_num_hashes + 1)/2;  // position of block's hash
                    if(offset < config->bucket_size){
                        uint8_t* src = per_block_hash + offset * SHA256_DIGEST_LENGTH;
                        uint8_t* dst = sec_MT + id * SHA256_DIGEST_LENGTH;
                        memcpy(dst, src, SHA256_DIGEST_LENGTH);
                    }
                }
                else {
                    uint8_t* dst = sec_MT +  id * SHA256_DIGEST_LENGTH;
                    uint8_t* lsrc = sec_MT +  lid * SHA256_DIGEST_LENGTH;
                    uint8_t* rsrc = sec_MT +  rid * SHA256_DIGEST_LENGTH;

                    sha256_twin_input_wrapper(
                        lsrc, SHA256_DIGEST_LENGTH,
                        rsrc, SHA256_DIGEST_LENGTH,
                        dst
                    );

                }
            }   
            //store root of secondary merkle tree
            uint8_t* src = sec_MT + 0 * SHA256_DIGEST_LENGTH;
            uint8_t* dst = sec_MT_roots + cur_bucket * SHA256_DIGEST_LENGTH;
            memcpy(dst, src, SHA256_DIGEST_LENGTH);


            //WRITE to fbuckets
            for(size_t i=0; i < config->bucket_size; i++){
                size_t total_written = 0;
                size_t bytes_to_write = SBucket::getCipherSize();

                while (total_written < bytes_to_write) {
                    size_t written = fwrite(sbuckets[i]->data, 1, bytes_to_write - total_written, fbuckets);
                    if (written == 0) {
                        if (ferror(fbuckets)) {
                            perror("Write error");
                            break;
                        }
                    }
                    total_written += written;
                }
            }

            //writing block_ids into fbids file
            fwrite(block_ids.data(), 1, block_ids.size()*sizeof(int), fbids);

            //writing sec merkle tree into fhash file
            size_t total_written = 0;
            size_t bytes_to_write = sec_MT_size;
            while (total_written < bytes_to_write) {
                size_t written = fwrite(sec_MT + total_written, 1, bytes_to_write - total_written, fhash);
                if (written == 0) {
                    if (ferror(fhash)) {
                        perror("Write error");
                        break;
                    }
                }
                total_written += written;
            }
            
            

            cur_bucket_size = 0;
            cur_bucket ++;

            if(cur_bucket %10000 == 0){
                cout << "done writing bucket " << cur_bucket << " out of " << config->num_buckets << endl;
            }
        }

        // primary merkle tree construction
        for(size_t j = config->num_buckets; j >= 1; j--){
            size_t id = j-1;
            size_t lid = 2*j-1;
            size_t rid = 2*j;

            if(lid > config->num_buckets - 1){
                // leaf
                memcpy(
                    prim_MT + id * SHA256_DIGEST_LENGTH,
                    sec_MT_roots + id * SHA256_DIGEST_LENGTH,
                    SHA256_DIGEST_LENGTH
                );
            } else{
                // internal node
                sha256_trib_input_wrapper(
                    sec_MT_roots + id * SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH,
                    prim_MT + lid * SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH,
                    prim_MT + rid * SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH,
                    prim_MT + id * SHA256_DIGEST_LENGTH
                );
            }
        }


        size_t total_written = 0;
        size_t bytes_to_write = config->num_buckets * SHA256_DIGEST_LENGTH;
        while (total_written < bytes_to_write) {
            size_t written = fwrite(prim_MT + total_written, 1, bytes_to_write - total_written, fhash);
            if (written == 0) {
                if (ferror(fhash)) {
                    perror("Write error");
                    break;
                }
            }
            total_written += written;
        }


        delete[] per_block_hash;
        delete[] sec_MT;
        delete[] sec_MT_roots;
        delete[] prim_MT;




        //save_block_mapping()          //node_to_block. [size = #nodes][which node is at which block]
        cout << "Saving block mapping..." << endl;
        block_fetcher->save_block_mapping(block_map_path);

        //save_position_map()           // [size=#nodes][which node at which path]
        cout << "Saving position mapping..." << endl;
        block_fetcher->save_position_map(position_map_path);
        

        cout << "closing files" << endl;
        fclose(fbuckets);
        fclose(fbids);
        fclose(fhash);
    }





























    void type1_initiate_and_write_blocks(string index_path) {


        // fbuckets HEADERS
        FILE* fbuckets = fopen(buckets_path, "w");
        if (!fbuckets) {
            fprintf(stderr, "could not open %s\n", buckets_path);
            perror("");
            abort();
        }
        fwrite(&(config->num_buckets), 1, sizeof(int), fbuckets);
        int sb_size = SBucket::getCipherSize();
        fwrite(&sb_size, 1, sizeof(int), fbuckets);
        bool mal_security = true;   // ALWAYS considered true during ORAM init. at Search we can switch either ON/OFF
        fwrite(&mal_security, 1, sizeof(bool), fbuckets);   

        // fbids HEADERS
        FILE* fbids = fopen(metadata_path, "w");
        if (!fbids) {
            fprintf(stderr, "could not open %s\n", metadata_path);
            perror("");
            abort();
        }
        int s = config->num_buckets * config->bucket_size;
        fwrite(&s, 1, sizeof(int), fbids);
        
        // fhash HEADERS
        FILE* fhash = fopen(hash_path, "w");
        if (!fhash) {
            fprintf(stderr, "could not open %s\n", hash_path);
            perror("");
            abort();
        }
      

        
        //common params 
        int per_block_size = (1 + config->block_size) * sizeof(int);
        const size_t dim = DiskANNNode<T, LabelT>::dim;
        const size_t R = DiskANNNode<T, LabelT>::n_neighbors;
        node_id_t total_nodes = block_fetcher->num_points;
        bool integrity = true; //always assumed true for oram init
        

        

        cout << "inserting real blocks..." << endl;
        // insert_real_blocks(database);
        {
            DiskANNIndexReader reader(index_path.c_str(), dim, R, sizeof(T));
            
            cout << "Adding all num points: " << total_nodes << " into ORAM Tree structure" << endl;
            for(size_t node_id = 0; node_id < total_nodes; node_id++){
                
                DiskANNNode<T, LabelT>* oram_node = new DiskANNNode<T, LabelT>();
                auto node = reader.read_node(node_id);

                std::vector<node_id_t> oram_neighbors;
                std::vector<T> oram_coords;
                
                for(size_t x = 0; x < node.nbr_count; x++){
                    oram_neighbors.push_back(node.neighbors[x]);
                }
                for(size_t x = node.nbr_count; x < R; x++){
                    oram_neighbors.push_back(-1);
                }
                for(size_t x = 0; x < dim; x++){
                    if constexpr (sizeof(T) == 1) oram_coords.push_back(static_cast<T>(node.embed[x]));
                    else oram_coords.push_back(node.embed[x]*large_number);
                }
                
                oram_node->set_id(node_id);
                oram_node->set_coords(oram_coords);
                oram_node->set_neighbors(oram_neighbors);

                // MAIN : insertion into ORAM tree
                block_id_t block_id = block_fetcher->insert_block_with_list(oram_node, node_id );

            
                if(node_id % 100000 == 0){
                    std::cout << "-> Done clustering: " << node_id << std::endl;
                }
                delete oram_node;            
            }
            cout << "Real blocks insertion completed!" << endl;
        }
        
        

        // datastructures for the whole oram tree
        std::vector<SBucket*> sbuckets;
        sbuckets.resize(config->num_buckets * config->bucket_size);

        vector<block_id_t> block_ids;
        block_ids.resize(config->num_buckets * config->bucket_size);


        cout << "-> Encrypting ORAM Tree..." << endl;
        // #pragma omp parallel for num_threads(96)
        for(int i = 0; i < config->num_buckets; i++){
            // fill in the real blocks
            for(int j = 0; j < config->real_bucket_size; j++){
                SBucket* sbkt = new SBucket(integrity);
                Bucket* bkt = block_fetcher->bkts[i*config->real_bucket_size + j];

                unsigned char* payload = new unsigned char[SBucket::getCipherSize()];

                vector<Block> blocks = bkt->getBlocks(); // will only give 1 block. since bkt has only 1
                assert(blocks.size() == 1);
                
                blocks[0].to_ptr(payload);
                block_ids[i*config->bucket_size + j] = blocks[0].index;

                // Encrypt and write
                int ctx_len = encrypt_wrapper(payload, per_block_size*Bucket::getMaxSize(), sbkt->data);
                
                sbuckets[i*config->bucket_size + j] = sbkt;
                assert(ctx_len == SBucket::getCipherSize());

                // cout << "bkt ptr @" << bkt << endl;
                delete bkt;
                delete[] payload;
            }
            // cout << "done1" << endl;

            // fill in the dummy blocks
            for(int j = config->real_bucket_size; j < config->bucket_size; j++){
                SBucket* sbkt = new SBucket(integrity);
                unsigned char* payload = new unsigned char[SBucket::getCipherSize()];
                Block dummy = Block(config->block_size);
                dummy.to_ptr(payload);
                
                block_ids[i*config->bucket_size + j] = -1;
                int ctx_len = encrypt_wrapper(payload, per_block_size*Bucket::getMaxSize(), sbkt->data);
            
                sbuckets[i*config->bucket_size + j] = sbkt;
                assert(ctx_len == SBucket::getCipherSize());

                // cout << "here 1" << endl;
                delete[] payload;
                // cout << "here 2" << endl;
            }
            // cout << "done2" << endl;
            // assert(0);
        }

        assert(block_ids.size() == config->num_buckets * config->bucket_size);

        
        
        
        
        cout << "Constructing Merkle Tree ..." << endl;

        size_t num_buckets = config->num_buckets;
        size_t bucket_size = config->bucket_size;
        size_t num_total_blocks = config->num_buckets * config->bucket_size;
        
        size_t bucket_tree_height = ceil(log10(config->bucket_size) / log10(2)) + 1;
        size_t bucket_num_hashes = pow(2, bucket_tree_height) - 1;
        
        cout << "bucket tree height: " << bucket_tree_height << endl;

        cout << "->Computing hash for each block" << endl;
        uint8_t* per_block_hash = new uint8_t[SHA256_DIGEST_LENGTH * num_total_blocks];

        #pragma omp parallel for num_threads(96)
        for(size_t i = 0; i < num_total_blocks; i++){
            sha256_wrapper(sbuckets[i]->data, SBucket::getCipherSize(), per_block_hash + i*SHA256_DIGEST_LENGTH);
        }


        cout << "->Constructing Secondary MT for each bucket" << endl;
        // secondary merkle tree for all buckets
        size_t sec_MT_size = SHA256_DIGEST_LENGTH * bucket_num_hashes * config->num_buckets;
        uint8_t* sec_MT = new uint8_t[sec_MT_size];
        memset(sec_MT, 0, sec_MT_size);


        for(size_t i = 0; i < num_buckets; i++){
            
            for(size_t j = bucket_num_hashes; j >= 1; j--){
                size_t id = j-1;
                size_t lid = 2*j-1;
                size_t rid = 2*j;
                if(lid > bucket_num_hashes - 1){
                    // leaf
                    size_t offset = j - (bucket_num_hashes + 1) / 2;
                    if(offset < bucket_size){
                        uint8_t* src = per_block_hash + i * bucket_size * SHA256_DIGEST_LENGTH + offset * SHA256_DIGEST_LENGTH;
                        uint8_t* dst = sec_MT + i * bucket_num_hashes * SHA256_DIGEST_LENGTH + id * SHA256_DIGEST_LENGTH;
                        memcpy(dst, src, SHA256_DIGEST_LENGTH);
                    }
                } else{
                    // internal node
                    uint8_t* dst =  sec_MT + i * bucket_num_hashes * SHA256_DIGEST_LENGTH + id * SHA256_DIGEST_LENGTH;
                    uint8_t* lsrc = sec_MT + i * bucket_num_hashes * SHA256_DIGEST_LENGTH + lid * SHA256_DIGEST_LENGTH;
                    uint8_t* rsrc = sec_MT + i * bucket_num_hashes * SHA256_DIGEST_LENGTH + rid * SHA256_DIGEST_LENGTH;

                    sha256_twin_input_wrapper(
                        lsrc, SHA256_DIGEST_LENGTH,
                        rsrc, SHA256_DIGEST_LENGTH,
                        dst
                    );
                }
            }
        }

        cout << "->Constructing Primary MT for each bucket" << endl;
        uint8_t* sec_MT_roots = new uint8_t[config->num_buckets * SHA256_DIGEST_LENGTH];
        uint8_t* prim_MT  = new uint8_t[config->num_buckets * SHA256_DIGEST_LENGTH];

        for(size_t j =0; j < config->num_buckets; j++){
            uint8_t* src = sec_MT + j * bucket_num_hashes * SHA256_DIGEST_LENGTH + 0;
            uint8_t* dst = sec_MT_roots + j * SHA256_DIGEST_LENGTH;
            memcpy(dst, src, SHA256_DIGEST_LENGTH);
        }

        for(size_t j = num_buckets; j >= 1; j--){

            size_t id = j-1;
            size_t lid = 2*j-1;
            size_t rid = 2*j;

            if(lid > config->num_buckets - 1){
                // leaf
                memcpy(
                    prim_MT + id * SHA256_DIGEST_LENGTH,
                    sec_MT_roots + id * SHA256_DIGEST_LENGTH,
                    SHA256_DIGEST_LENGTH
                );
            } else{
                // internal node
                sha256_trib_input_wrapper(
                    sec_MT_roots + id * SHA256_DIGEST_LENGTH , SHA256_DIGEST_LENGTH,
                    prim_MT + lid * SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH,
                    prim_MT + rid * SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH,
                    prim_MT + id * SHA256_DIGEST_LENGTH
                );
            }
        }
        


        //saving Hash file
        cout << "Writing to Hash file..." << endl;
        size_t total_written = 0;
        size_t bytes_to_write = sec_MT_size;
        while (total_written < bytes_to_write) {
            size_t written = fwrite(sec_MT + total_written, 1, bytes_to_write - total_written, fhash);
            if (written == 0) {
                if (ferror(fhash)) {
                    perror("Write error");
                    break;
                }
            }
            total_written += written;
        }

        total_written = 0;
        bytes_to_write = config->num_buckets * SHA256_DIGEST_LENGTH;
        while (total_written < bytes_to_write) {
            size_t written = fwrite(prim_MT + total_written, 1, bytes_to_write - total_written, fhash);
            if (written == 0) {
                if (ferror(fhash)) {
                    perror("Write error");
                    break;
                }
            }
            total_written += written;
        }
        fclose(fhash);
            


        cout << "Saving block mapping..." << endl;
        block_fetcher->save_block_mapping(block_map_path);

        cout << "Saving position mapping..." << endl;
        block_fetcher->save_position_map(position_map_path);


        // Save metadata
        cout << "Saving metadata..." << endl;
        fwrite(block_ids.data(), 1, block_ids.size()*sizeof(int), fbids);
        fclose(fbids);
        

        // Save sbuckets
        cout << "Saving sbuckets..." << endl;            

        for (size_t i = 0; i < config->num_buckets * config->bucket_size; i++){
            size_t total_written = 0;
            size_t bytes_to_write = SBucket::getCipherSize();
            while (total_written < bytes_to_write) {
                size_t written = fwrite(sbuckets[i]->data, 1, bytes_to_write - total_written, fbuckets);
                if (written == 0) {
                    if (ferror(fbuckets)) {
                        perror("Write error");
                        break;
                    }
                }
                total_written += written;
            }
        }
        fclose(fbuckets);
    }

};

template<typename T, typename LabelT>
class OramBuilderDisk : public OramBuilder<T, LabelT> {

    public:
        const char* built_buckets_path;
        
    OramBuilderDisk(
        const char* buckets_path,
        const char* block_map_path,
        const char* position_map_path,
        const char* metadata_path,
        node_id_t num_points,
        const float large_number,
        RingOramConfig* config,
        const char* built_buckets_path

    ) : OramBuilder<T, LabelT>(buckets_path, block_map_path, position_map_path, metadata_path, num_points, large_number, config) {
        // Initialize the block fetcher with disk access
        cout << "Initiating on-disk ORAM" << endl;
        this->block_fetcher = new FakeBlockFetcherRing<T, LabelT>(num_points, *config, this->random);

        this->built_buckets_path = built_buckets_path;
    }


    void initiate_and_write_blocks(T* database, int block_size) {
        cout << "Using on-disk initiation\n";

        this->insert_real_blocks(database);
        delete[] database;

        int per_block_size = (1 + this->config->block_size) * sizeof(int);

        bool integrity = false;

        std::vector<SBucket*> buckets;
        buckets.resize(this->config->num_buckets * this->config->bucket_size);

        vector<block_id_t> block_ids;
        block_ids.resize(this->config->num_buckets * this->config->bucket_size);

        cout << "-> Encrypting ORAM Tree..." << endl;
        #pragma omp parallel for num_threads(NUM_THREADS)
        for(int i = 0; i < this->config->num_buckets; i++){
            // fill in the real blocks
            for(int j = 0; j < this->config->real_bucket_size; j++){
                SBucket* sbkt = new SBucket(integrity);
                Bucket* bkt = this->block_fetcher->bkts[i*this->config->real_bucket_size + j];

                unsigned char* payload = new unsigned char[SBucket::getCipherSize()];

                vector<Block> blocks = bkt->getBlocks();
                assert(blocks.size() == 1);
                
                blocks[0].to_ptr(payload);
                block_ids[i*this->config->bucket_size + j] = blocks[0].index;

                // Encrypt and write
                int ctx_len = encrypt_wrapper(payload, per_block_size*Bucket::getMaxSize(), sbkt->data);
                
                buckets[i*this->config->bucket_size + j] = sbkt;
                assert(ctx_len == SBucket::getCipherSize());
                delete bkt;
                delete[] payload;
            }

            // fill in the dummy blocks
            for(int j = this->config->real_bucket_size; j < this->config->bucket_size; j++){
                SBucket* sbkt = new SBucket(integrity);
                unsigned char* payload = new unsigned char[SBucket::getCipherSize()];
                Block dummy = Block(block_size);
                dummy.to_ptr(payload);
                
                block_ids[i*this->config->bucket_size + j] = -1;
                int ctx_len = encrypt_wrapper(payload, per_block_size*Bucket::getMaxSize(), sbkt->data);
            
                buckets[i*this->config->bucket_size + j] = sbkt;
                assert(ctx_len == SBucket::getCipherSize());
                delete[] payload;
            }
        }

        assert(block_ids.size() == this->config->num_buckets * this->config->bucket_size);

        cout << "Saving block mapping..." << endl;
        this->block_fetcher->save_block_mapping(this->block_map_path);

        cout << "Saving position mapping..." << endl;
        this->block_fetcher->save_position_map(this->position_map_path);


        // Save metadata
        {
            cout << "Saving metadata..." << endl;
            int s = block_ids.size();
            FILE* f = fopen(this->metadata_path, "w");
            if (!f) {
                fprintf(stderr, "could not open %s\n", this->metadata_path);
                perror("");
                abort();
            }
            fwrite(&s, 1, sizeof(int), f);
            fwrite(block_ids.data(), 1, block_ids.size()*sizeof(int), f);
            fclose(f);
        }

        // Save buckets, one file per path
        {
            for(size_t i = 0; i < this->config->num_buckets; i++){
                // cout << "Saving bucket " << i << endl;

                string bucket_file_path = std::string(this->built_buckets_path) +  "/bucket_" + std::to_string(i) + ".bin";

                FILE* f = fopen(bucket_file_path.c_str(), "w");
                if (!f) {
                    fprintf(stderr, "could not open %s\n", bucket_file_path.c_str());
                    perror("");
                    abort();
                }

                // Write capacity
                // fwrite(&(this->config->num_buckets), 1, sizeof(int), f);

                int sb_size = SBucket::getCipherSize();
                // fwrite(&sb_size, 1, sizeof(int), f);

                // fwrite(&integrity, 1, sizeof(bool), f);       // not using integrity

                for(size_t j = 0; j < this->config->bucket_size; j++){
                    size_t total_written = 0;
                    size_t bytes_to_write = SBucket::getCipherSize();
                    while (total_written < bytes_to_write) {
                        size_t written = fwrite(buckets[i*this->config->bucket_size + j]->data, 1, bytes_to_write - total_written, f);
                        if (written == 0) {
                            if (ferror(f)) {
                                perror("Write error");
                                break;
                            }
                        }
                        total_written += written;
                    }
                }
                fclose(f);
            }
        }

    }

};






#endif
