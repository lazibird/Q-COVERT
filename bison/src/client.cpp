#include "defaults.h"
#include "ArgMapping.h"
#include "net_io_channel.h"
#include "evp.h"
#include "rapidjson/document.h"
#include <boost/program_options.hpp>
#include "io_utils.h"

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
#include <stdlib.h>

// oram
#include "macro.h"
#include "OramAPI.h"
#include "config_parser.h"

// diskann
#include "diskann_interface.h"
#include "tableprinter.h"

#include "OramBuilder.h"

using namespace std;
namespace po = boost::program_options;

int party = CLIENT_PARTY;
int port = CLIENT_PORT;
string address = CLIENT_IP;

int server_port = SERVER_PORT;
string server_address;

string dataset = "";
string config_path = "";
int iter, w, num_nodes_cache = -1, oram_cached_levels = -1;
string i_list, w_list;
bool integrity = true;
int mal = 1;

void setup_connection_client(int port, string server_address, NetIO*& io, NetIO*& bf_io) {
    cout << "Setting up connection..." << endl;
    cout << "connection inputs: " << server_address << " " << port << endl;

    io = new NetIO(server_address.c_str(), port, false, true);
    if (!io) {
        cerr << "Error: Failed to initialize NetIO for main connection." << endl;
        exit(EXIT_FAILURE);
    }

    bf_io = new NetIO(server_address.c_str(), port + 1, false, true);
    if (!bf_io) {
        cerr << "Error: Failed to initialize NetIO for backup connection." << endl;
        delete io;
        exit(EXIT_FAILURE);
    }

    cout << "\nNetwork connection established with server at " << server_address  << "at port " << port << endl;
}

void set_sbucket_cipher(int block_size_ints) {
    SBucket::resetState();
    SBucket::setCipherSize(compute_ctx_len((1 + block_size_ints) * (int)sizeof(int)));
}



std::vector<int> parse_int_list(const std::string &s) {
    std::vector<int> result;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        try {
            result.push_back(std::stoi(token));
        } catch (...) {
            std::cerr << "Error parsing integer from token: " << token << std::endl;
        }
    }
    return result;
}



int main(int argc, char **argv) {
    cout << "Starting client..." << endl;

    ArgMapping amap;
    amap.arg("d", dataset, "Dataset: [sift, trip, msmarco, laion]");
    amap.arg("c", config_path, "Path to config file");

    amap.arg("p", server_port, "Port Number");
    amap.arg("sip", server_address, "server IP");
    
    amap.arg("i_list", i_list, "corr list of iterations");
    amap.arg("w_list", w_list, "corr list of beamwidths");
    amap.arg("ocl", oram_cached_levels, "oram cached levels");
    amap.arg("nc", num_nodes_cache, "numcache");
    amap.arg("mal", mal, "malicious security" );
    amap.parse(argc, argv);
    //CLI Arguments

    
    NetIO* io = nullptr;
    NetIO* bf_io = nullptr;
    setup_connection_client(server_port, server_address, io, bf_io);
    // CONNECTION MADE WITH SERVER


    if (oram_cached_levels >= 0)
        overwrite_json_nested_field(config_path, dataset, "oram_cached_levels", oram_cached_levels);

    integrity = (mal == 1)?true:false;
    overwrite_json_nested_field_bool(config_path, dataset, "integrity", integrity);
    
    vector<int> iter_list = parse_int_list(i_list);
    vector<int> beam_list = parse_int_list(w_list);
    assert(iter_list.size() == beam_list.size());
    

    //SENDING NUM EXPTS TO SERVER 
    int num_expts = iter_list.size();
    try{
        bf_io->send_data(&num_expts, sizeof(int));
        bf_io->send_data(&integrity, sizeof(bool));
    } catch (const std::exception& e){
        delete io;
        delete bf_io;
        return EXIT_FAILURE;
    }

    Metadata md;
    if (parseJson(config_path, md, dataset) != 0) {
        cerr << "Error: Failed to parse JSON configuration." << endl;
        return EXIT_FAILURE;
    }
    cout << "Metadata md loaded successfully." << endl;

    
    if(!md.use_oram){
        return 0;
    }

    struct RingOramConfig config(
        md.block_size, 
        md.real_bucket_size, 
        md.dummy_size, 
        md.evict_rate, 
        md.base_size,
        md.num_levels,
        md.oram_cached_levels
    );
    cout << "Configuration loaded successfully." << endl;


    DiskANNNode<float, node_id_t>::n_neighbors = md.R;
    DiskANNNode<float, node_id_t>::dim = md.dim;
    DiskANNNode<uint8_t, node_id_t>::n_neighbors = md.R;
    DiskANNNode<uint8_t, node_id_t>::dim = md.dim;
    Bucket::setMaxSize(1);

    RandForOramInterface* random = new RandomForOram();
    if (!random) {
        cerr << "Error: Failed to initialize RandomForOram." << endl;
        delete io;
        delete bf_io;
        return EXIT_FAILURE;
    }

    NetIO* pq_io = nullptr;
    NetIO* posmap_io = nullptr;
    if (md.low_client_storage) {
        pq_io = new NetIO(server_address.c_str(), server_port + 2, false, true);
        if (md.recursive_posmap)
            posmap_io = new NetIO(server_address.c_str(), server_port + 3, false, true);
        if (!pq_io || (md.recursive_posmap && !posmap_io)) {
            cerr << "Error: Failed to initialize auxiliary ORAM connections." << endl;
            delete random;
            delete io;
            delete bf_io;
            return EXIT_FAILURE;
        }
        pq_io->send_data(&num_expts, sizeof(int));
        if (md.recursive_posmap)
            posmap_io->send_data(&num_expts, sizeof(int));
    }
    
    
    
    
    
    for(int expt=0; expt < num_expts; expt++){
        
        cout << "Loop " << expt << endl;
        
        bool is_server = false;
        bool in_memory = true;
        set_sbucket_cipher(md.block_size);
        RemoteRing* rss = new RemoteRing(io, config, is_server, in_memory, md.integrity, -1, "");
        if (!rss) {
            cerr << "Error: Failed to initialize RemoteServerStorage." << endl;
            delete random;
            delete io;
            delete bf_io;
            return EXIT_FAILURE;
        }

        RemoteRing* pq_rss = nullptr;
        RemoteRing* posmap_rss = nullptr;
        if (md.low_client_storage) {
            RingOramConfig pq_config(
                md.pq_oram_block_size_bytes / 4, md.real_bucket_size, md.dummy_size, md.evict_rate,
                md.pq_oram_num_blocks, md.pq_oram_num_levels, 0);
            set_sbucket_cipher(md.pq_oram_block_size_bytes / 4);
            pq_rss = new RemoteRing(pq_io, pq_config, false, true, md.integrity, -1, "");

            if (md.recursive_posmap) {
                RingOramConfig posmap_config(
                    md.posmap_oram_block_size_bytes / 4, md.real_bucket_size, md.dummy_size, md.evict_rate,
                    md.posmap_oram_num_blocks, md.posmap_oram_num_levels, 0);
                set_sbucket_cipher(md.posmap_oram_block_size_bytes / 4);
                posmap_rss = new RemoteRing(posmap_io, posmap_config, false, true, md.integrity, -1, "");
            }
        }
 

        OramAPI* oram_api = nullptr;
        if (md.use_oram){
            oram_api = new OramAPI(
                rss,
                random,
                config.num_blocks,
                md.num_levels,
                md.oram_cached_levels,
                config.block_size,
                config.real_bucket_size,
                config.dummy_size,
                config.evict_rate,
                md.block_map_path,
                md.metadata_path,
                md.pos_map_path,
                md.debug,
                md.large_number,
                config,
                md.low_client_storage,
                md.recursive_posmap
            );
            if (md.low_client_storage) {
                oram_api->setup_low_client(
                    pq_rss, posmap_rss, random,
                    md.recursive_posmap,
                    md.pq_oram_metadata_path, md.pq_oram_pos_map_path,
                    md.pq_oram_num_levels, md.pq_oram_num_blocks,
                    md.posmap_oram_metadata_path, md.posmap_oram_pos_map_path,
                    md.posmap_oram_num_levels, md.posmap_oram_num_blocks,
                    md.pq_bytes,
                    md.pq_oram_block_size_bytes / 4,
                    md.posmap_oram_block_size_bytes / 4,
                    md.real_bucket_size,
                    md.dummy_size, md.evict_rate);
            }
        }
        if (md.use_oram && !oram_api) {
            cerr << "Error: Failed to initialize OramAPI." << endl;
            delete random;
            delete rss;
            delete io;
            delete bf_io;
            return EXIT_FAILURE;
        }
        cout << "OramAPI initialized successfully." << endl;
        
        
        iter = iter_list[expt];
        w = beam_list[expt];
        overwrite_json_nested_field(config_path, dataset, "--search_iter_limit",iter);
        overwrite_json_nested_field(config_path, dataset, "-W", w);
        if (num_nodes_cache >= 0)
            overwrite_json_nested_field(config_path, dataset, "--num_nodes_to_cache", num_nodes_cache);
        


        DiskANNInterface diskann(config_path.c_str(), io, md.use_oram, md.debug);
        diskann.read_diskann_args(dataset);
        
        long comm = io->counter, rounds = io->num_rounds;
        long pq_comm_start = 0, pq_rounds_start = 0;
        long posmap_comm_start = 0, posmap_rounds_start = 0;
        if (md.low_client_storage) {
            pq_comm_start = pq_io->counter;
            pq_rounds_start = pq_io->num_rounds;
            if (md.recursive_posmap) {
                posmap_comm_start = posmap_io->counter;
                posmap_rounds_start = posmap_io->num_rounds;
            }
        }





        cout << "hello3" << endl;







        bool ready;
        bf_io->recv_data(&ready, sizeof(bool)); //waits for ready signal from server
        

        if(md.integrity){
            rss->sync_roots();
        }



        diskann.initiate_and_run_diskann(oram_api);
        
        comm = io->counter - comm;
        rounds = io->num_rounds - rounds;

        rss->close_server();

        long graph_s2c_comm = 0, graph_s2c_round = 0;
        io->recv_data(&graph_s2c_comm, sizeof(long));
        io->recv_data(&graph_s2c_round, sizeof(long));

        long pq_comm = 0, pq_rounds = 0, pq_s2c_comm = 0, pq_s2c_round = 0;
        long posmap_comm = 0, posmap_rounds = 0, posmap_s2c_comm = 0, posmap_s2c_round = 0;

        if (md.low_client_storage) {
            pq_rss->close_server();
            pq_comm = pq_io->counter - pq_comm_start;
            pq_rounds = pq_io->num_rounds - pq_rounds_start;
            pq_io->recv_data(&pq_s2c_comm, sizeof(long));
            pq_io->recv_data(&pq_s2c_round, sizeof(long));

            if (md.recursive_posmap) {
                posmap_rss->close_server();
                posmap_comm = posmap_io->counter - posmap_comm_start;
                posmap_rounds = posmap_io->num_rounds - posmap_rounds_start;
                posmap_io->recv_data(&posmap_s2c_comm, sizeof(long));
                posmap_io->recv_data(&posmap_s2c_round, sizeof(long));
            }
        }

        if (md.use_oram) {
            long total_c2s_comm = comm + pq_comm + posmap_comm;
            long total_s2c_comm = graph_s2c_comm + pq_s2c_comm + posmap_s2c_comm;
            long total_direction_changes = rounds + pq_rounds + posmap_rounds;
            print_communication_metrics(md.num_queries, total_c2s_comm, total_s2c_comm, total_direction_changes,
                                         rss, rss->rounds_for_oram_access, rss->rounds_for_reshuffles, rss->rounds_for_evictions);
            if (md.low_client_storage) {
                // Communication bytes are data-only. Bucket metadata that is
                // logically stored on the server adds simulated RTT (not
                // bytes), matching the Compass network-accounting model.
                auto semantic_rtt = [](RemoteRing* r, bool account_metadata) {
                    long long bma = r->semantic_bma_invocations;
                    long long early = r->semantic_early_reshuffle_invocations;
                    long long evict = r->semantic_effective_evict_invocations;
                    long long data_rtt = bma + 2 * early + 2 * evict;
                    if (account_metadata)
                        data_rtt += bma + early + evict;
                    return data_rtt;
                };

                long long graph_rtt = semantic_rtt(rss, true);
                long long pq_rtt = semantic_rtt(pq_rss, true);
                long long posmap_rtt = md.recursive_posmap ? semantic_rtt(posmap_rss, true) : 0;
                long long total_rtt = graph_rtt + pq_rtt + posmap_rtt;
                cout << "LOW_NET graph_bytes=" << comm
                     << " pq_bytes=" << pq_comm
                     << " posmap_bytes=" << posmap_comm
                     << " total_bytes=" << total_c2s_comm
                     << " graph_rtt=" << graph_rtt
                     << " pq_rtt=" << pq_rtt
                     << " posmap_rtt=" << posmap_rtt
                     << " total_rtt=" << total_rtt << endl;
            }
        }
        
        delete oram_api;
        if (md.low_client_storage) {
            delete pq_rss;
            if (md.recursive_posmap)
                delete posmap_rss;
        }
        delete rss;
    } // num expt loop
    

    delete random;
    if (pq_io) delete pq_io;
    if (posmap_io) delete posmap_io;
    delete io;
    delete bf_io;
    cout << "\nClient terminated successfully." << endl;
    return EXIT_SUCCESS;
}
    
    
   
/*
int check;

int NUM_ROUNDS = 3;
size_t PAYLOAD_SIZE = 3616 * 37;
size_t HASH_SIZE = 116 * 1024;

double NETWORK_us = 0;

int main(int argc, char **argv) {
    

    ArgMapping amap;
    amap.arg("i", check, "r");
    amap.parse(argc, argv);
    
    NetIO* io = nullptr;
    NetIO* bf_io = nullptr;
    setup_connection_client(8000, "10.209.224.140", io, bf_io);

    unsigned char* payload = new unsigned char[PAYLOAD_SIZE];
    unsigned char* hash = new unsigned char[HASH_SIZE];
    

    for (int r = 0; r < NUM_ROUNDS; r++) {
        std::cout << "Client: receiving round " << r << "\n";

        // PAYLOAD
        diskann::Timer network;
        io->recv_data(payload, PAYLOAD_SIZE);
        NETWORK_us += network.elapsed();



        // Dummy computation
        uint64_t acc = 0;
        for (size_t i = 0; i < PAYLOAD_SIZE; i += 64) {
            acc += payload[i];
        }
        size_t x = 0;
        for(size_t i=0; i< 10000; i++){
            x += i;
        }


        // HASH
        diskann::Timer network_hash;
        io->recv_data(hash, HASH_SIZE);
        NETWORK_us += network_hash.elapsed();


        x = 0;
        for(size_t i=0; i< 10000; i++){
            x += i;
        }
        
    }



    uint64_t dummy = 0;
    for (uint64_t i = 0; i < 100000000; i++) {
        dummy += i;
    }
    

    cout << "Network time: " << NETWORK_us/1000 << " ms"  << endl;

    
    
    delete[] payload;
    delete io;
    delete bf_io;
    
    
    return 0;  

}

*/
