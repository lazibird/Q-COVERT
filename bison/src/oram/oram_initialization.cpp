#include "ArgMapping.h"
#include "defaults.h"

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

// oram
#include "macro.h"
#include "OramAPI.h"
#include "config_parser.h"
#include "OramBuilder.h"
#include "io_utils.h"

using namespace std;

string dataset;
string config_path;

string degree;
string efc;
string pq_bytes;

string graph_path;

int build_type = 2;

int main(int argc, char** argv) {
    // this code should be run on the server
    ArgMapping amap;
    amap.arg("d", dataset, "Dataset: [sift, trip, msmarco, laion]");
    amap.arg("c", config_path, "Path to config file");
    
    amap.arg("type", build_type, "build_type= [1 suff RAM][2 sequential bkts]");

    amap.arg("gp", graph_path, "graph path");   //for build type 1
    amap.parse(argc, argv);

    
    cout << "Starting oram initialization..." << endl;

    Metadata md;

    config_path = config_path.size() == 0 ? (CONFIG_DIR) + "config_" + dataset + ".json" : config_path;
    cout << "config path: " << config_path << "\n";
    if (parseJson(config_path, md, dataset) != 0) {
        cerr << "Error: Failed to parse JSON configuration." << endl;
        return EXIT_FAILURE;
    }

    cout << "Configuration loaded successfully." << endl;

    DiskANNNode<float, node_id_t>::dim = md.dim;
    DiskANNNode<float, node_id_t>::n_neighbors = md.R;
    DiskANNNode<uint8_t, node_id_t>::dim = md.dim;
    DiskANNNode<uint8_t, node_id_t>::n_neighbors = md.R;

    struct RingOramConfig config(
        md.block_size, 
        md.real_bucket_size, 
        md.dummy_size, 
        md.evict_rate, 
        md.base_size,
        md.num_levels,
        md.oram_cached_levels
    );

    Bucket::setMaxSize(1);   // each Bucket* has 1 block in it (weird implementation ikr) there are multiple Bucket* in each (acutal logical) bucket
    SBucket::setCipherSize(compute_ctx_len((1 + md.block_size) * sizeof(int)));

    auto initialize = [&]<typename T>() {
    OramBuilder<T, node_id_t> *oram_builder = new OramBuilder<T, node_id_t>(
        md.buckets_path.c_str(),
        md.block_map_path.c_str(),
        md.pos_map_path.c_str(),
        md.metadata_path.c_str(),
        md.hash_path.c_str(),
        md.base_size,
        md.large_number,
        &config,
        build_type
    );

    if(build_type==1){
        // load database
        size_t nb, dim;
        // cout << "reading base fvecs file..." << endl;
        // float* database = fvecs_read(md.base_path.c_str(), &dim, &nb);  //returns a mem pointer to float vecs of all points
        // cout << "num_points in base: " << nb << " dimensions: " << dim << endl;
    
        // cout << "base path: " << md.base_path << endl;
    
        // cout << "Initializing ORAM from " << graph_path << "\n";
        
        // oram_builder->load_index(graph_path.c_str());
        //sets graph[node_id] datastructure to give neighbors for each nodeid
        // cout << "oram_builder->load_index() done" << endl;

        
        string index_path = md.index_path_prefix + "_disk.index";
        cout << "Building oram from index: " << index_path << endl;
        
        oram_builder->type1_initiate_and_write_blocks(index_path);
    
        cout << "Oram initialization done!" << endl;

    }
    else if(build_type ==2){

        cout << "\nDISK BASED ORAM INIT" << endl;

        string index_path = md.index_path_prefix + "_disk.index";
        cout << "Building oram from index: " << index_path << endl;

        // int dim, R;
        // dim = md.dim;
        // R = md.R;

        // DiskANNIndexReader reader(index_path.c_str(), dim, R);

        // for(int i=0; i< 10; i++){
        //     auto node = reader.read_node(i);
        //     reader.printNodeData(node);
        // }



        oram_builder->disk_initiate_and_write_blocks(index_path);

    }
    else{
        cerr << "Unusual Build type. Can be either 1[sufficient RAM] or 2[Sequential Build, Insufficient RAM]" << endl;
        abort();
    }


    delete oram_builder;
    };
    if (md.data_type == "uint8") initialize.template operator()<uint8_t>();
    else initialize.template operator()<float>();
    return 0;
}
