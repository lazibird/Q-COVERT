#include "defaults.h"
#include "ArgMapping.h"
#include "net_io_channel.h"
#include "evp.h"

// stl
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
#include "io_utils.h"

#include <optional>

// oram
#include "macro.h"
#include "OramAPI.h"
#include "config_parser.h"
#include "OramBuilder.h"

using namespace std;

int party = SERVER_PARTY;
int port = SERVER_PORT;
string address = SERVER_IP;

string client_address = CLIENT_IP;
int client_port = CLIENT_PORT;

string dataset = "";
string config_path = "";
string role = "graph";
bool in_memory = true;
int disk_type = -1;

int io_max = 1024;
string disk_path = "";


void setup_connection_server(int port, NetIO*& io, NetIO*& bf_io) {
    cout << "setting up connection..." << endl;

    io = new NetIO(nullptr, port, false, true);
    if (!io) {
        cerr << "Error: Failed to initialize NetIO for main connection." << endl;
        exit(EXIT_FAILURE);
    }

    bf_io = new NetIO(nullptr, port + 1, false, true);
    if (!bf_io) {
        cerr << "Error: Failed to initialize NetIO for backup connection." << endl;
        delete io;
        exit(EXIT_FAILURE);
    }

    cout << "Prepared own port at " << port << " for cnxn with client." << endl;
}

void setup_connection_server_aux(int port, NetIO*& io) {
    cout << "setting up auxiliary connection..." << endl;
    io = new NetIO(nullptr, port, false, true);
    if (!io) {
        cerr << "Error: Failed to initialize NetIO for auxiliary connection." << endl;
        exit(EXIT_FAILURE);
    }
    cout << "Prepared own port at " << port << " for auxiliary ORAM server." << endl;
}



int main(int argc, char** argv) {
    cout << "Starting server..." << endl;
    
    ArgMapping amap;
    amap.arg("d", dataset, "Dataset: [sift, trip, msmarco, laion]");
    amap.arg("c", config_path, "Path to config file");
    amap.arg("role", role, "Server role: graph, pq, posmap");

    amap.arg("p", port, "Port Number");
    
    amap.arg("m", in_memory, "server type: 1:in-memory  0:on-disk");
    amap.arg("t", disk_type, "server type: 1:mmap   2:iouring");  
    amap.arg("io", io_max, "MAX IO EVENTS");
    
    amap.arg("dp", disk_path, "Path to disk oram bucket (SSD storage)");
    amap.parse(argc, argv);
    // CLI arguments

    
    bool in_mem = (in_memory == 1)?true : false;
    
    ///CONNECTING TO CLIENT
    bool aux = (role != "graph");

    Metadata md;
    if (parseJson(config_path, md, dataset) != 0) {
        cerr << "Error: Failed to parse JSON configuration." << endl;
        return EXIT_FAILURE;
    }
    cout << "Metadata md loaded successfully." << endl;

    if(!md.use_oram){
        return 0;
    }

    NetIO* io = nullptr;
    NetIO* bf_io = nullptr;
    if (aux) {
        setup_connection_server_aux(port, io);
    } else {
        setup_connection_server(port, io, bf_io);
    }

    int num_expts = 1;
    bool integrity = md.integrity;
    if (aux) {
        io->recv_data(&num_expts, sizeof(int));
    } else {
        bf_io->recv_data(&num_expts, sizeof(int));
        bf_io->recv_data(&integrity, sizeof(bool));
        overwrite_json_nested_field_bool(config_path, dataset, "integrity", integrity);
    }

    int role_block_size = md.block_size;
    int role_num_levels = md.num_levels;
    int role_num_blocks = md.base_size;
    int role_cached_levels = aux ? 0 : md.oram_cached_levels;
    string role_buckets_path = md.buckets_path;
    string role_hash_path = md.hash_path;

    if (role == "pq") {
        role_block_size = md.pq_oram_block_size_bytes / 4;
        role_num_levels = md.pq_oram_num_levels;
        role_num_blocks = md.pq_oram_num_blocks;
        role_buckets_path = md.pq_oram_buckets_path;
        role_hash_path = md.pq_oram_hash_path;
    } else if (role == "posmap") {
        role_block_size = md.posmap_oram_block_size_bytes / 4;
        role_num_levels = md.posmap_oram_num_levels;
        role_num_blocks = md.posmap_oram_num_blocks;
        role_buckets_path = md.posmap_oram_buckets_path;
        role_hash_path = md.posmap_oram_hash_path;
    }
    
    struct RingOramConfig config(
        role_block_size, 
        md.real_bucket_size, 
        md.dummy_size, 
        md.evict_rate, 
        role_num_blocks,
        role_num_levels,
        role_cached_levels
    );
    cout << "Configuration loaded successfully." << endl;


    DiskANNNode<float, node_id_t>::n_neighbors = md.R;
    DiskANNNode<float, node_id_t>::dim = md.dim;
    Bucket::setMaxSize(1);
    SBucket::setCipherSize(compute_ctx_len((1 + role_block_size) * sizeof(int)));

    RandForOramInterface* random = new RandomForOram();
    if (!random) {
        cerr << "Error: Failed to initialize RandomForOram." << endl;
        delete io;
        if (bf_io) delete bf_io;
        return EXIT_FAILURE;
    }
    
    // bool in_memory;  //first use of in memory
    bool is_server = true;
    
    
    // // std::optional<RemoteRing> ground_server;
    // RemoteRing ground_server = RemoteRing(io, config, is_server, true, md.integrity, disk_type, "");
    // cout << "ground server init" << endl;

    // if(in_mem){
        
       
    //     auto s = std::chrono::high_resolution_clock::now();
    //     // load buckets
    //     ground_server.load_server_state(md.buckets_path.c_str());
    //     // load hashes
    //     if(md.integrity){
    //         ground_server.load_server_hash(md.hash_path.c_str());
    //         // ground_server->sync_roots();
    //     }

    //     auto e = std::chrono::high_resolution_clock::now();
    //     std::chrono::duration<double> elapsed = e - s;
    //     std::cout << "Time to load ground state MEMORY: " << elapsed.count() << "s" << std::endl;
    // }

    








    for(int expt=0; expt < num_expts; expt++){

        cout << "Loop " << expt << endl;

        RemoteRing server = RemoteRing(io, config, true, in_mem, md.integrity, disk_type, disk_path);
        

        if (!aux) {
            bool ready = true;
            try {
                bf_io->send_data(&ready, sizeof(bool)); 
            } 
            catch (const std::exception& e) {
                cerr << "Error: Failed to send ready signal. Exception: " << e.what() << endl;
                delete random;
                delete io;
                if (bf_io) delete bf_io;
                return EXIT_FAILURE;
            }
        }
        




        try {

            auto start = std::chrono::high_resolution_clock::now();
            if(in_mem){
                //load the buckets into memory
                // server.load_from_ground(ground_server);
                server.load_server_state(role_buckets_path.c_str());
                if(md.integrity){
                    server.load_server_hash(role_hash_path.c_str());
                    server.sync_roots();
                }
                
            } else{
                //will do disk copy from ground state buckets.bin to temp file
                
                if(md.integrity){
                    server.load_server_hash(role_hash_path.c_str());
                    server.sync_roots();
                }
                server.load_server_state(role_buckets_path.c_str());

            }
            
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            std::cout << "Time to load server state: " << elapsed.count() << "s" << std::endl;            
     
        } catch (const std::exception& e) {
            cerr << "Error: Failed to Build buckets. Exception: " << e.what() << endl;
            delete random;
            delete io;
            delete bf_io;
            return EXIT_FAILURE;
        }
        
    
        if(md.use_oram && !aux){
            long long* dummy_data = new long long[1000000]; 
            long comm = io->counter;
        
            for(int i = 0; i < 50; i++){
                io->recv_data(dummy_data, 1000000 * sizeof(long long));
                cout << "\rDummy " << i+1 << " sent: " << (io->counter - comm)*1.0/(1024*1024) << " MB"  << std::flush;
            }
            cout << "\n";
            io->counter = comm;  
        }
        
        // double t0 = elapsed();
        // tprint("Starting remote server... \n", t0);
        
        long comm = io->counter, round = io->num_rounds;
        
        try {
            server.start_comm = comm;
            server.start_rounds = round;
            server.run_server(io_max);
        } catch (const std::exception& e) {
            cerr << "Error: Exception occurred while running the server. Exception: " << e.what() << endl;
        }

        comm = io->counter - comm;
        round = io->num_rounds - round;

        //used in "print_communication metrics" function
        io->send_data(&comm, sizeof(long));
        io->send_data(&round, sizeof(long));


        //clear server data
        server.clear_data();
        
    }
    // ground_server.clear_data();
    delete random;
    delete io;
    delete bf_io;
    
    cout << "\nServer terminated successfully." << endl;
    return EXIT_SUCCESS;
}




/*


int check;

int NUM_ROUNDS = 3;
size_t PAYLOAD_SIZE = 3616 * 37;
size_t HASH_SIZE = 116 * 1024;

int main(int argc, char** argv){

    // mimic MS Marco

    // num round trips = 3
    // per round payload = 3616*37 Bytes
    // per round hash = 116 * 1024 Bytes

    ArgMapping amap;
    amap.arg("i", check, "iasdfkjasldfj");
    amap.parse(argc, argv);



    NetIO* io = nullptr;
    NetIO* bf_io = nullptr;
    setup_connection_server(8000, io, bf_io);




    unsigned char* payload = new unsigned char[PAYLOAD_SIZE];
    unsigned char* hash = new unsigned char[HASH_SIZE];

    for(int r=0; r < NUM_ROUNDS; r++){
        std::cout << "Server: sending round " << r << "\n";

        std::memset(payload, r, PAYLOAD_SIZE);
        io->send_data(payload, PAYLOAD_SIZE);
      
        std::memset(hash, r, HASH_SIZE);
        io->send_data(hash, HASH_SIZE);
        
    }

    delete[] payload;
    delete io;
    delete bf_io;

    return 0;
    
}


*/
