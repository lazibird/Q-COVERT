#include "ArgMapping.h"
#include "net_io_channel.h"

// stl
#include <iostream>
#include <iomanip>
#include <cmath>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <set>
#include <fstream>

// oram
// #include "RemoteServerStorage.h"
// #include "RemoteServerRing.h"
// #include "RemoteLeak.h"
#include "RandForOramInterface.h"
#include "RandomForOram.h"
#include "OramLeak.h"

// graph
#include "../../src/cluster.h"
#include "../../src/faiss/impl/HNSW.h"
#include "../../src/faiss/impl/ProductQuantizer.h"
#include "../../src/faiss/utils/io.h"
#include "../../src/faiss/index_io.h"

// json
#include "utils/config_parser.h"

using namespace std;


double elapsed() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}


// ground truth labels @gt (gt_size NNs per query), results @I (nq * k).
// Prints only R@k (standard recall@k, 0..1) and returns that value.
float compute_recall(faiss::idx_t* gt, int gt_size, faiss::idx_t* I, int nq, int k) {
    const int eval_k = std::min(k, gt_size);
    long hits = 0;
    for (int i = 0; i < nq; i++) { // loop over all queries
        faiss::idx_t* row = gt + i * gt_size;
        std::set<faiss::idx_t> gt_nns(row, row + eval_k);
        for (int j = 0; j < k; j++) { // iterate over returned nn results
            if (gt_nns.count(I[i * k + j]) != 0) {
                hits++;
            }
        }
    }
    const float recall = hits / (float)(nq * eval_k);
    printf("* Base HNSW accuracy relative to exact search:\n");
    printf("\tR@%d = %.4f\n", k, recall);
    return recall;
}

inline void tprint(string s, double t0){
    cout << "[" << std::fixed << std::setprecision(3) << (elapsed() - t0) * 1000.0 << " ms] " << s;
}


int party = 0;
int port = 8000;
string address = "127.0.0.1";
string dataset = "";
int n = -1;

int efspec = -1;
int efn = -1;
int ef_search_arg = -1;
int topk_o = -1;
bool batching = true;
bool lazy_evict = true;
string f_latency = "";
string f_accuracy = "";

int main(int argc, char **argv) {
    std::string config_path = compass_default_config();
    ArgMapping amap;
    amap.arg("c", config_path, "Configuration JSON (default: tests/config/config_ring.json)");
    amap.arg("r", party, "Role of party: Server = 1; Client = 2");
    amap.arg("p", port, "Port Number");
    amap.arg("d", dataset, "Dataset: [sift, trip, msmarco, laion]");
    amap.arg("n", n, "# queries");
    amap.arg("ip", address, "IP Address of server");
    amap.arg("efspec", efspec, "Size of speculation set");
    amap.arg("efn", efn, "Size of directional filter");
    amap.arg("ef_search", ef_search_arg, "Override efSearch (-1: use config)");
    amap.arg("topk", topk_o, "Top-k neighbors to retrieve (-1: 10; use 100 for R@100 eval)");
    amap.arg("batch", batching, "Disable batching");
    amap.arg("lazy", lazy_evict, "Disable lazy eviction");
    amap.arg("f_latency", f_latency, "Save latency");
    amap.arg("f_accuracy", f_accuracy, "Save accuracy");
    amap.parse(argc, argv);

    cout << ">>> Setting up..." << endl;
    cout << "-> Role: " << party << endl;
    cout << "-> Address: " << address << endl;
    cout << "-> Port: " << port << endl;
    cout << "<<<" << endl << endl;

    bool isServer = party == 1;
    NetIO* io = new NetIO(isServer ? nullptr : address.c_str(), port);
    NetIO* bf_io = new NetIO(isServer ? nullptr : address.c_str(), port+1);

    Metadata md;
    
    cout << "-> Parsing json..." << endl;
    if(parseJson(config_path, md, dataset) != 0) {
        cerr << "Early exit..." << endl;
        return 0;
    }

     // ORAM for graph
    struct RingOramConfig config(
        md.block_size, 
        md.real_bucket_size, 
        md.dummy_size, 
        md.evict_rate, 
        md.base_size,
        md.num_levels,
        md.oram_cached_levels
    );

    OptNode::n_neighbors = 2*md.M;
    OptNode::dim = md.dim;

    Bucket::setMaxSize(1);
    SBucket::setCipherSize(compute_ctx_len((1 + md.block_size)*sizeof(int)));

    // cout << "block_size: " << md.block_size << endl;
    // cout << "bucket_size: " << md.bucket_size << endl;
    // cout << "sbucket_size: " << SBucket::getCipherSize() << endl;

    // RemoteServerStorage* rss = new RemoteServerStorage(config.block_size, io, isServer, config.num_levels, md.integrity, config);

    RandForOramInterface* random = new RandomForOram();

    bool in_memory = true;
    RemoteLeak* remote_storage = new RemoteLeak(io, config, isServer, in_memory, md.integrity);

    long comm;
    long round;

    double t0 = elapsed();

    if(isServer){
        // Config remote storage server
        tprint("Starting remote server... \n", t0);
        
        bool in_memory = true;

        tprint("Faster initialization...\n", t0);

        remote_storage->load_server_state(md.buckets_path.c_str());
        
        bool ready;
        bf_io->send_data(&ready, sizeof(bool));


        tprint("Run server...\n", t0);

        comm = io->counter;
        round = io->num_rounds;
        
		remote_storage->run_server();
    } else{
        // Client
        tprint("Initializing ORAM... \n", t0);

        OramInterface* oram = new OramLeak(
            remote_storage, 
            random, 
            config,
            md.num_levels,
            md.oram_cached_levels,
            batching,
            lazy_evict
        );

        tprint("Loading ORAM metadata\n", t0);
        ((OramLeak*)oram)->loadMetaDataFromFile(md.metadata_path.c_str());

        // # search result
        const size_t k = (topk_o >= 1) ? (size_t)topk_o : 10;

        tprint("Loading prebuilt index\n", t0);

        faiss::IndexHNSW* index = faiss::read_index(md.index_path.c_str(), 0);

        tprint("Loading pq index\n", t0);

        faiss::ProductQuantizer* pq = faiss::read_pq(md.pq_path.c_str(), 0);
        pq->print_stats();

        tprint("Clustering\n", t0);

        LeakBlockFetcherRing* obf = new LeakBlockFetcherRing(
            md.base_size, 
            index->hnsw.max_level + 1 ,
            oram
        );

        // bool tmp;
        // io->recv_data(&tmp, 1);        

        // Read client states
        {
            obf->read_block_mapping(md.block_map_path.c_str());

            // stash
            ((OramLeak*)oram)->loadPositionMapFromFile(md.pos_map_path.c_str());
            
            bool ready;
            bf_io->recv_data(&ready, sizeof(bool));

            // Graph cache
            index->set_graph_cache(md.ef_lowest_cached_layer, obf, md.graph_cache_path.c_str());

            // ORAM cache
            ((OramLeak*)oram)->init_cache_top();
        }

        tprint("Loading queries\n", t0);
        size_t nq;
        float* xq;

        {
            size_t d2;
            xq = fvecs_read(md.query_path.c_str(), &d2, &nq);
            n = n == -1 ? nq : n;
            assert(md.dim == d2 || !"query does not have same dimension as train set");
            assert(n <= nq || !"# queries larger than query set");
        }

        tprint("Loading GT\n", t0);

        faiss::idx_t* gt; // nq * gt_size matrix of ground-truth nearest-neighbors
        size_t gt_size;

        {

            tprint("Loading ground truth for " + to_string(nq) + "queries\n", t0);

            // load ground-truth and convert int to long
            size_t nq2;
            int* gt_int = ivecs_read(md.gt_path.c_str(), &gt_size, &nq2);
            assert(nq2 == nq || !"incorrect nb of ground truth entries");
            // assert(k == k1 || !"incorrect k");

            gt = new faiss::idx_t[gt_size * nq];
            for (int i = 0; i < gt_size * nq; i++) {
                gt[i] = gt_int[i];
            }
            delete[] gt_int;
        }

        // bool tmp;
        // io->recv_data(&tmp, 1);

        nq = n;

        comm = io->counter;
        round = io->num_rounds;

        faiss::SearchParametersHNSW* params = new faiss::SearchParametersHNSW();
        params->efSpec = md.ef_spec;
        params->efNeighbor = md.ef_neighbor;
        params->efLayer = md.ef_lowest_cached_layer;
        params->efUpperSteps = md.ef_upper_steps;
        params->efSearch = md.ef_search;

        if(efn != -1){
            params->efNeighbor = efn;
        }

        if(efspec != -1){
            params->efSpec = efspec;
        }

        if(ef_search_arg != -1){
            params->efSearch = ef_search_arg;
        }

        // output buffers
        faiss::idx_t* I = new faiss::idx_t[nq * k];
        float* D = new float[nq * k];

        tprint("Searching with efs: " + to_string(params->efSearch) + "\n", t0);

        double t1 = elapsed();

        faiss::SearchStats s_stats = index->oblivious_search(
            nq,
            xq,
            k,
            D,
            I,
            pq,
            params
        );


        unsigned long total = 0;
        for(int nrt : s_stats.nsteps_map[0]){
            total += nrt;
        }

        printf("Since t0: %.3f ms | Search: %.3f ms\n", (elapsed() - t0) * 1000.0, (elapsed() - t1) * 1000.0);

        printf("[%.3f ms] Computing average round trip: %ld\n", (elapsed() - t0) * 1000.0, total / nq);

        printf("[%.3f ms] Compute recalls\n", (elapsed() - t0) * 1000.0);

        compute_recall(
            gt, 
            gt_size,
            I,
            nq,
            k
        );

        int* result = new int[nq * k];

        for(int i = 0; i < nq * k; i++){
            result[i] = I[i];
        }

        if(f_accuracy != ""){
            // write result if search over the whole query set
            // string result_path = dataset + ".ivecs";
            ivecs_write(
                f_accuracy.c_str(), 
                result, 
                k,
                nq
            );
        }

        if(f_latency != ""){
            // string perceived_latency_path = "perceived_latency_" + dataset + ".bin";
            // string full_latency_path = "full_latency_" + dataset + ".bin";

            vector<float> latency = s_stats.perceived_latency;
            latency.insert(latency.end(), s_stats.full_latency.begin(), s_stats.full_latency.end());

            fvecs_write(
                f_latency.c_str(),
                latency.data(),
                latency.size(),
                1
            );

        }

        delete params;

        delete[] result;
        delete[] I;
        delete[] D;
        delete[] gt;
        

        remote_storage->close_server();
    }

    // delete io;
    // delete bf_io;

    std::cout << "Communication cost: " << io->counter - comm << std::endl;
    std::cout << "Round: " << io->num_rounds - round << std::endl;

    return 0;
}