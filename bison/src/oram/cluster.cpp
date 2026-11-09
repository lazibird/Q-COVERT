#include "cluster.h"
#include <iostream>
#include <algorithm>
#include <random>
#include <fstream>
#include <set>

void to_metis_graph_weightless(faiss::IndexHNSW* index){
    node_id_t ntotal = index->hnsw.levels.size();
    int l = 0;

    std::string fname = "SIFT10K_weightless.graph";
    std::ofstream ofile(fname);

    std::vector<std::set<node_id_t>> edges(ntotal);


    for(node_id_t nid = 0; nid < ntotal; nid++){
        size_t begin, end;
        index->hnsw.neighbor_range(nid, l, &begin, &end);
        for (size_t j = begin; j < end; ++j) {
            node_id_t v1 = index->hnsw.neighbors[j];
            if (v1 < 0) {
                break;
            }
            edges[nid].insert(v1);
            edges[v1].insert(nid);
        }
        if(nid % 1000 == 0){
            std::cout << "1: " << nid << std::endl;
        }
    }

    int n_edges = 0;
    for(node_id_t nid = 0; nid < ntotal; nid++){
        n_edges += edges[nid].size();
    }

    n_edges = n_edges / 2;
    ofile << ntotal << " " <<  n_edges << std::endl;

    for(node_id_t nid = 0; nid < ntotal; nid++){
        for(node_id_t v: edges[nid]){
            ofile << v + 1<< " ";
        }
        ofile << std::endl;

        if(nid % 1000 == 0){
            std::cout << "2: " << nid << std::endl;
        }
    }

    ofile.close();
    std::cout << "Vertices: " << ntotal << " Edges: " <<  n_edges << std::endl;

}

std::set<node_id_t> get_node_list_by_distance(faiss::IndexHNSW* index, node_id_t entry, int level, int dis){
    std::set<node_id_t> res;
    res.insert(entry);

    if(dis > 0){
        size_t begin, end;
        index->hnsw.neighbor_range(entry, level, &begin, &end);
        for (size_t j = begin; j < end; ++j) {
            node_id_t v1 = index->hnsw.neighbors[j];
            if (v1 < 0) {
                break;
            }
            std::set<node_id_t> neighbors = get_node_list_by_distance(index, v1, level, dis - 1);
            res.insert(neighbors.begin(), neighbors.end());
        }
    }

    return res;

}

Cluster* get_cluster_by_distance(faiss::IndexHNSW* index, float* xb, node_id_t entry, int level, int dis){
    Cluster* b = new Cluster(level);
    std::set<node_id_t> cluster = get_node_list_by_distance(index, entry, level, dis);
    // std::cout << "Cluster size: " << cluster.size() << std::endl;
    int cnt = 0;
    for(node_id_t nid: cluster){
        // Pushing info to block
        b->ids.push_back(nid);

        size_t begin, end;
        index->hnsw.neighbor_range(nid, level, &begin, &end);
        std::vector<node_id_t> nb_list(
            index->hnsw.neighbors.begin() + begin,
            index->hnsw.neighbors.begin() + end
        );
        b->neighbors.push_back(nb_list);

        int dim = index-> d;

        // for(size_t offset = nid*dim; offset < (nid+1)*dim; offset++){
        //     b->coords.push_back(xb[offset]);
        // }

        cnt++;
    }

    return b;
}

// void naive_cluster(BlockFetcher* bf, faiss::IndexHNSW* index, float* xb){
//     // Only for last layer
//     node_id_t ntotal = index->hnsw.levels.size();

//     int l = 0;

//     for(node_id_t nid = 0; nid < ntotal; nid++){
//         Cluster* b = new Cluster(l);
//         b->ids.push_back(nid);

//         size_t begin, end;
//         index->hnsw.neighbor_range(nid, l, &begin, &end);
//         std::vector<node_id_t> nb_list(
//             index->hnsw.neighbors.begin() + begin,
//             index->hnsw.neighbors.begin() + end
//         );
//         b->neighbors.push_back(nb_list);

//         int dim = index-> d;

//         for(size_t offset = nid*dim; offset < (nid+1)*dim; offset++){
//             b->coords.push_back(xb[offset]);
//         }

//         bf->add_block(b);
//     }
// }

void fully_naive_cluster(BlockFetcher* bf, faiss::IndexHNSW* index, float* xb, std::vector<uint8_t> codes){
    std::cout << "-> Start clustering: " << std::endl;
    node_id_t ntotal = index->hnsw.levels.size();
    int d = index->d;

    // number of subvectors for pq
    int M = codes.size() / ntotal;

    // add graph
    for(node_id_t nid = 0; nid < ntotal; nid++){
        for(int l = 0; l < index->hnsw.levels[nid]; l++){

            OptNode* on = new OptNode();
            on->set_id(nid);
            on->set_layer(l);

            size_t begin, end;
            index->hnsw.neighbor_range(nid, l, &begin, &end);
            std::vector<node_id_t> nb_list(
                index->hnsw.neighbors.begin() + begin,
                index->hnsw.neighbors.begin() + end
            );

            on->set_neighbors(nb_list);

            if(l >= 1){
                size_t begin, end;
                index->hnsw.neighbor_range(nid, l - 1, &begin, &end);
                std::vector<node_id_t> next_layer_nb_list(
                    index->hnsw.neighbors.begin() + begin,
                    index->hnsw.neighbors.begin() + end
                );

                if(l == 1){
                    // truncate the last layer neighbors
                    next_layer_nb_list.resize(OptNode::n_neighbors/2);
                } 
                on->set_next_layer_neighbors(next_layer_nb_list);
            }


            size_t long_d = d;
            size_t long_nid = nid;

            vector<float> coords;
            for(size_t offset = long_nid*long_d; offset < (long_nid+1) *long_d; offset++){
                coords.push_back(xb[offset]);
            }

            on->set_coord(coords);

            bf->add_block_with_list(on, {nid});

            // assert(0);

            delete on;

        }
        if(nid % 10000 == 0){
            std::cout << "-> Done clustering: " << nid << std::endl;
        }
    }


}