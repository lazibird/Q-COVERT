#include "rapidjson/document.h"
#include <boost/program_options.hpp>
#include "net_io_channel.h"
#include <sys/resource.h>

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
#include <filesystem>
#include <sys/time.h>
#include <stdlib.h>

// diskann
#include "linux_aligned_file_reader.h"
#include "program_options_utils.hpp"
#include "oram_index.h"
#include "pq_flash_index.h"
#include "index.h"
#include "timer.h"

#include "OramAPI.h"

using namespace std;

namespace po = boost::program_options;

struct DiskANNInterface { 
    int num_diskann_args;
    vector<string> diskann_args;
    const char* diskann_config_file;
    NetIO* io;
    bool use_oram;

    void print_stats(std::string category, std::vector<float> percentiles, std::vector<float> results)
    {
        diskann::cout << std::setw(20) << category << ": " << std::flush;
        for (uint32_t s = 0; s < percentiles.size(); s++)
        {
            diskann::cout << std::setw(8) << percentiles[s] << "%";
        }
        diskann::cout << std::endl;
        diskann::cout << std::setw(22) << " " << std::flush;
        for (uint32_t s = 0; s < percentiles.size(); s++)
        {
            diskann::cout << std::setw(9) << results[s];
        }
        diskann::cout << std::endl;
    }


    DiskANNInterface(const char* diskann_config_file, NetIO* io, bool use_oram, bool debug){
        this->diskann_config_file = diskann_config_file;
        this->io = io;
        this->use_oram = use_oram;
        diskann::debug = debug;
    }

    template <typename T, typename LabelT = node_id_t>
    int search_disk_index(
        diskann::Metric &metric, const std::string &index_path_prefix,
        const std::string &result_output_prefix, const std::string &query_file, std::string &gt_file,
        const uint32_t num_threads, const uint32_t recall_at, const uint32_t beamwidth,
        const uint32_t num_nodes_to_cache, const uint32_t search_io_limit, const uint32_t search_iter_limit,
        const std::vector<uint32_t> &Lvec, const float fail_if_recall_below,
        const std::vector<std::string> &query_filters, const bool use_reorder_data = false,
        const std::set<size_t> &query_nums = std::set<size_t>(),
        OramAPI* oram_api = nullptr
    ){
        cout << "SEARCH PARAMS: " << search_io_limit << " " << search_iter_limit << endl;
        
        std::string warmup_query_file = index_path_prefix + "_sample_data.bin";

        // load query bin
        T *query = nullptr;
        uint32_t *gt_ids = nullptr;
        float *gt_dists = nullptr;
        size_t query_num, query_dim, query_aligned_dim, gt_num, gt_dim;
        diskann::load_aligned_bin<T>(query_file, query, query_num, query_dim, query_aligned_dim);

        diskann::load_truthset(gt_file, gt_ids, gt_dists, gt_num, gt_dim);

        std::shared_ptr<AlignedFileReader> reader = nullptr;
        reader.reset(new LinuxAlignedFileReader());

        std::unique_ptr<diskann::OramIndex<T, LabelT>> _pFlashIndex(new diskann::OramIndex<T, LabelT>(reader, metric));

        if (oram_api && oram_api->low_client_storage)
            _pFlashIndex->low_client_storage = true;

        int err = _pFlashIndex->load(num_threads, index_path_prefix.c_str());
        if (err != 0){
            cout << "Could not load index file...exiting!" << endl;
            return err;
        }

        if (oram_api && oram_api->low_client_storage) {
            _pFlashIndex->pq_read = [oram_api](const uint32_t *ids, uint64_t n_ids, uint8_t *out) {
                std::vector<uint32_t> ids_vec(ids, ids + n_ids);
                oram_api->read_pq(ids_vec, out);
            };
        }

        std::vector<uint32_t> node_list;
        diskann::cout << "Caching " << num_nodes_to_cache << " nodes around medoid(s)" << std::endl;
        _pFlashIndex->cache_bfs_levels(num_nodes_to_cache, node_list);
        _pFlashIndex->load_cache_list(node_list);
        node_list.clear();
        node_list.shrink_to_fit();

        diskann::cout << "Caching done." << std::endl;
        
        omp_set_num_threads(num_threads);

        diskann::cout.setf(std::ios_base::fixed, std::ios_base::floatfield);
        diskann::cout.precision(2);

        // set<size_t> query_nums{10, 100, 500, 1000, 5000, 10000};

        query_num = *query_nums.begin();
        cout << "\n\nRunning " << (use_oram ? "SECURE " : "INSECURE ") << "search for ";
                {
                    int i = 0;
                    for(auto itr : query_nums){
                        if(i == 0){
                            cout << itr;
                        }else if(query_nums.size() > 1 && i == query_nums.size()-1){
                            cout << " and " << itr;
                            break;
                        }else{
                            cout << ", " << itr;
                        }
                        i++;
                    }
                }
        cout << " queries." << endl; 

        std::string recall_string = "Recall@" + std::to_string(recall_at);

        // print_search_results(vector<double>(), true);

        std::vector<std::vector<uint32_t>> query_result_ids(Lvec.size());
        std::vector<std::vector<float>> query_result_dists(Lvec.size());

        uint32_t optimized_beamwidth = 2;

        double best_recall = 0.0, best_mrr = 0.0;

        // int op = -2;
        // io->send_data(&op, sizeof(int));
        if(use_oram){
            const long long* dummy_data = new long long[1000000]; 
            long comm = io->counter;
            auto start = std::chrono::high_resolution_clock::now();
            for(int i = 0; i < 50; i++){
                io->send_data(dummy_data, 1000000 * sizeof(long long));
                cout << "\rDummy " << i+1 << " sent: " << (io->counter - comm)*1.0/(1024*1024) << " MB"  << std::flush;
            }
            auto end = std::chrono::high_resolution_clock::now();
            double seconds = std::chrono::duration<double>(end-start).count();
            io->counter = comm;
            cout << endl;

            double totalDummy = 500*1000000 * sizeof(long long)* 8 * 1.0 / (1024*1024*1024);
            double bandwidth = totalDummy/seconds;

            cout << "Dummy bandwidth observerd: " << bandwidth << "Gbps in time: " << seconds << "s" << endl;


        }   

        //oram caching
        oram_api->init_cache_top();
        // ((OramRing*) oram)->init_cache_top();



        auto total_local_compute_time = 0;
        auto total_oram_local_time = 0;
        auto total_oram_wait_time = 0;
        auto total_user_perceived_time = 0;
        auto total_e2e_time = 0;

        map<string, double> results;

        for (uint32_t test_id = 0; test_id < Lvec.size(); test_id++)
        {
            uint32_t L = Lvec[test_id];
            optimized_beamwidth = beamwidth;

            query_result_ids[test_id].resize(recall_at * query_num);
            query_result_dists[test_id].resize(recall_at * query_num);

            auto stats = new diskann::QueryStats[query_num];

            std::vector<uint64_t> query_result_ids_64(recall_at * query_num);

            long before_query_comm = 0, before_query_rounds = 0;
            before_query_comm = io->counter;
            before_query_rounds = io->num_rounds;

            int query_nums_done = 0;

            auto s = std::chrono::high_resolution_clock::now();            
            // #pragma omp parallel for schedule(dynamic, 1)
            for (int64_t i = 0; i < (int64_t)query_num; i++)
            {
                if(use_oram){
                    diskann::Timer query_timer;
                    // auto st_q = std::chrono::high_resolution_clock::now();

                    _pFlashIndex->cached_beam_search_with_oram(
                        query + (i * query_aligned_dim),
                        recall_at, 
                        L,
                        query_result_ids_64.data() + (i * recall_at),
                        query_result_dists[test_id].data() + (i * recall_at),
                        optimized_beamwidth, 
                        false,
                        0,
                        search_io_limit, 
                        search_iter_limit,
                        use_reorder_data, 
                        stats + i,
                        oram_api
                    );

                    (stats + i)->user_time_us = query_timer.elapsed();

                    // Eviction
                    // auto st_ev = std::chrono::high_resolution_clock::now();
                    
                    // (stats + i)->user_perceived_time += (st_ev - st_q);

                    oram_api->evict_and_write_back();
                    // auto en_ev = std::chrono::high_resolution_clock::now();

                    // (stats + i)->e2e_time += (en_ev - st_q);
                    (stats + i)->total_time_us = query_timer.elapsed();

                    // cout << "UserLat: " << (stats + i)->user_time_us << " TotalLat: " << (stats + i)->total_time_us << endl;


                } else {
                    diskann::Timer query_timer;

                    _pFlashIndex->cached_beam_search(
                        query + (i * query_aligned_dim),
                        recall_at, 
                        L,
                        query_result_ids_64.data() + (i * recall_at),
                        query_result_dists[test_id].data() + (i * recall_at),
                        optimized_beamwidth, 
                        search_io_limit, 
                        use_reorder_data, 
                        stats + i
                    );

                    (stats + i)->user_time_us = query_timer.elapsed();
                }

                cout << "\rQuery " << i + 1 << " done." << std::flush;

                if(query_nums.count(i+1)){
                    //after last query is searched
                    cout << endl << endl;
                    
                    long after_query_comm = io->counter;
                    long after_query_rounds = io->num_rounds;

                    size_t query_num = i+1;

                    auto e = std::chrono::high_resolution_clock::now();
                    std::chrono::duration<double> diff = e - s;
                    double qps = (1.0 * query_num) / (1.0 * diff.count());  //num queries answered per second
        
                    diskann::convert_types<uint64_t, uint32_t>(query_result_ids_64.data(), query_result_ids[test_id].data(),
                                                            query_num, recall_at);
        
                    auto mean_latency = diskann::get_mean_stats<float>(
                        stats, query_num, [](const diskann::QueryStats &stats) { return stats.total_us; });
        
                    auto latency_999 = diskann::get_percentile_stats<float>(
                        stats, query_num, 0.999, [](const diskann::QueryStats &stats) { return stats.total_us; });
        
                    auto mean_ios = diskann::get_mean_stats<uint32_t>(stats, query_num,
                                                                    [](const diskann::QueryStats &stats) { return stats.n_ios; });
        
                    auto mean_hops = diskann::get_mean_stats<uint32_t>(stats, query_num,
                                                                    [](const diskann::QueryStats &stats) { return stats.n_hops; });
                    auto mean_actual_hops = diskann::get_mean_stats<uint32_t>(stats, query_num,
                                                                    [](const diskann::QueryStats &stats) { return stats.n_actualhops; });
        
                    
                    //only the time taken in PQ distance calc of neighbours
                    auto mean_cpuus = diskann::get_mean_stats<float>(stats, query_num,
                                                                    [](const diskann::QueryStats &stats) { return stats.cpu_us; });


                    //only time around oram_read functions
                    auto mean_io_us = diskann::get_mean_stats<float>(stats, query_num,
                                                                    [](const diskann::QueryStats &stats) { return stats.io_us; });

                    //number of node comparisons: around computte_dists(node_nbrs, nnbrs, dist_scratch)
                    auto mean_n_cmps = diskann::get_mean_stats<float>(stats, query_num,
                                                                    [](const diskann::QueryStats &stats) { return stats.n_cmps; });
        
                    //number of beamwidth reads from the oram server
                    auto mean_n_fullvectorreads = diskann::get_mean_stats<float>(stats, query_num,
                                                                    [](const diskann::QueryStats &stats) { return stats.n_fullvectorreads; });                                                         
                    //number of search iterations
                    auto mean_n_search_iterations = diskann::get_mean_stats<float>(stats, query_num,
                                                                    [](const diskann::QueryStats &stats) { return stats.num_search_iterations; });   
                    // for(int i=0; i< query_num; i++){cout << stats[i].num_search_iterations << " " ;}
                                                                    
        
                    auto mean_communcation = (after_query_comm - before_query_comm)*1.0 / (query_num * 1024 * 1024);   

                    auto total_communication_time = diskann::get_total_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.communication_time
                                                                                            .count(); });                                                         
                            
                    auto total_latency = diskann::get_total_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.total_us; });                                                         


                    total_user_perceived_time = diskann::get_total_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.user_perceived_time.count(); });                                                         


                    total_e2e_time = diskann::get_total_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.e2e_time.count(); });                                                         

                    total_local_compute_time = diskann::get_total_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.local_compute_time.count(); });  
                

                    total_oram_wait_time = diskann::get_total_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.oram_wait_time.count(); });  
                
                    total_oram_local_time = diskann::get_total_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return (stats.oram_total_time.count() - stats.oram_wait_time.count()); });  

                    auto mean_diskann_local_compute = diskann::get_mean_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.diskann_compute_time; });  

                    auto mean_oram_total_time = diskann::get_mean_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.oram_total_time_us; });  

                    auto mean_total_time = diskann::get_mean_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.total_time_us; });                                                                  

                    auto mean_user_time = diskann::get_mean_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.user_time_us; });    
                                                                                        
                    auto mean_oram_wait_time = diskann::get_mean_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.oram_wait_time_us; }); 
                    
                    auto median_oram_wait_time = diskann::get_median_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.oram_wait_time_us; });

                    
                    auto mean_oram_client_time = diskann::get_mean_stats<float>(stats, query_num,
                                                                                        [](const diskann::QueryStats &stats) { return stats.oram_client_time_us; }); 
                    

                    double recall = 0, mrr = 0;
                    recall = diskann::calculate_recall((uint32_t)query_num, gt_ids, gt_dists, (uint32_t)gt_dim,
                                                        query_result_ids[test_id].data(), recall_at, recall_at);
                    best_recall = std::max(recall, best_recall);
        
                    if (index_path_prefix.find("trip") != std::string::npos)
                    {
                        mrr = diskann::calculate_mrr_text(
                            (uint32_t)query_num, query_result_ids[test_id].data(), recall_at,
                            "./data/trip_distilbert/benchmark_tsv/documents/docs.tsv",
                            "./data/trip_distilbert/benchmark_tsv/topics/topics.head.val.tsv",
                            "./data/trip_distilbert/benchmark_tsv/qrels/qrels.dctr.head.val.tsv");
                    }
                    else if (index_path_prefix.find("marco") != std::string::npos &&
                             index_path_prefix.find("msmarco1m") == std::string::npos)
                    {
                        mrr = diskann::calculate_mrr_text(
                            (uint32_t)query_num, query_result_ids[test_id].data(), recall_at,
                            "./data/msmarco_bert/passages/collection.tsv",
                            "./data/msmarco_bert/passages/queries.dev.small.tsv",
                            "./data/msmarco_bert/passages/qrels.dev.small.tsv");
                    }
                    else
                    {
                        mrr = diskann::calculate_mrr((uint32_t)query_num, gt_ids, gt_dists, (uint32_t)gt_dim,
                                                        query_result_ids[test_id].data(), recall_at);
                    }
                    best_mrr = std::max(mrr, best_mrr); 
                    
                    // fetch server-to-client metrics
                    if(use_oram){
                        int req = -1;
                        io->send_data(&req, sizeof(int));
                        io->counter -= sizeof(int);
                    }

                    long server_comm, server_rounds;
                    double total_server_local_time;
                    if(use_oram){
                        io->recv_data(&server_comm, sizeof(long));
                        io->recv_data(&server_rounds, sizeof(long));
                        io->recv_data(&total_server_local_time, sizeof(double));
                        io->num_rounds--;
                    }


                    // server_rounds = 2*(((OramRing*) oram_api->oram)->rounds_for_early_reshuffle + ((OramRing*) oram_api->oram)->rounds_for_eviction)/3 + ((OramRing*) oram_api->oram)->rounds_for_oram_access/2;
                    if (oram_api == nullptr){
                        server_rounds = 0;
                    } else {
                        server_rounds = 2*(((OramRing*) oram_api->oram)->storage->rounds_for_reshuffles + ((OramRing*) oram_api->oram)->storage->rounds_for_evictions)/3 + ((OramRing*) oram_api->oram)->storage->rounds_for_oram_access/2;
                    }

                    long num_queries = i+1;
                    auto mean_network_time = mean_oram_wait_time - (total_server_local_time*1000000.0/num_queries);
                    auto median_network_time = median_oram_wait_time - (total_server_local_time*1000000.0/num_queries);
                    
                    // cout << "Completed search for " << *query_nums.rbegin() << " queries." << endl;
                    if(use_oram){
                        cout << "Executed " << ((OramRing*)oram_api->oram)->num_reshuffles << " early reshuffles." << endl;
                        cout << "Executed " << oram_api->oram_calls << " oram calls." << endl;
                    }
                    
                    cout << "Latency ms = " << mean_latency/1000 << "\n"; 
                    cout << "CPU ms = " << mean_cpuus/1000 << "\n"; 
                    cout << "Diskann ms = " << mean_diskann_local_compute/1000 << "\n"; 
                    cout << "ORAM Total ms = " << mean_oram_total_time/1000 << "\n"; 
                    cout << "ORAM Wait ms = " << mean_oram_wait_time/1000 << "\n"; 
                    cout << "ORAM Client ms = " << mean_oram_client_time/1000 << "\n"; 
                    cout << "User ms = " << mean_user_time/1000 << "\n"; 
                    cout << "Total ms = " << mean_total_time/1000 << "\n"; 

                    results["Queue Size"] = L;
                    results["Iterations"] = mean_n_search_iterations;
                    results["Beamwidth"] = optimized_beamwidth;
                    results[recall_string] = recall;
                    results["MRR@" + std::to_string(recall_at)] = mrr;

                    // results["Total Latency"] = (total_e2e_time*1000.0)/num_queries;
                    // results["User Latency"] = (total_user_perceived_time*1000.0)/num_queries;
                    // results["ORAM Server Local Time"] = (total_server_local_time*1000.0)/num_queries;
                    // results["ORAM Client Local Time"] = (total_oram_local_time*1000.0)/num_queries;
                    // results["Network Time"] = ((total_oram_wait_time - total_server_local_time)*1000.0)/num_queries;
                    // results["DiskANN Local Time"] = (total_local_compute_time*1000.0)/num_queries;

                    cout << "Query num: " << query_num << "; Num Queries: " << num_queries << endl;

                    results["Total Latency"] = (mean_total_time/1000.0);
                    results["User Latency"] = (mean_user_time/1000.0);

                    results["ORAM Server Local Time"] = (total_server_local_time*1000.0)/num_queries;
                    results["ORAM Client Local Time"] = (mean_oram_client_time/1000.0);
                    results["Mean Network Time"] = mean_network_time/1000.0;
                    results["Median Network Time"] = median_network_time/1000.0;
                    results["DiskANN Local Time"] = (mean_diskann_local_compute/1000.0);

                    print_search_results_untabulated(results, recall_at);

                    query_nums_done++;
                }


                // if(query_nums_done == query_nums.size()){
                //     break;
                // }
            }
            auto end = std::chrono::high_resolution_clock::now();
            // cout << "Time: " << (end - s).count()*1000 << " ms\n";

                
            delete[] stats;
        }
        // cout << "===================================================================================================================================================================================" << endl << endl << endl;

        if (diskann::debug) diskann::cout << "Done searching. Now saving results " << std::endl;
        uint64_t test_id = 0;
        for (auto L : Lvec)
        {
            if (L < recall_at)
                continue;

            std::string cur_result_path = result_output_prefix + "_" + std::to_string(L) + "_idx_uint32.bin";
            diskann::save_bin<uint32_t>(cur_result_path, query_result_ids[test_id].data(), query_num, recall_at);

            cur_result_path = result_output_prefix + "_" + std::to_string(L) + "_dists_float.bin";
            diskann::save_bin<float>(cur_result_path, query_result_dists[test_id++].data(), query_num, recall_at);
        }

        diskann::aligned_free(query);

        // if(use_oram){
        //     cout << "\n\n";
        //     cout << "E2E Time: " << (total_e2e_time*1000.0)/(*query_nums.rbegin()) << " ms\n";
        //     cout << "Online Time: " << (total_user_perceived_time*1000.0)/(*query_nums.rbegin()) << " ms\n";
        //     cout << "Local Search Time: " << (total_local_compute_time*1000.0)/(*query_nums.rbegin()) << " ms (" << total_local_compute_time*100.0/total_user_perceived_time << " %)\n";
        //     cout << "ORAM Wait Time: " << (total_oram_wait_time*1000.0)/(*query_nums.rbegin()) << " ms (" << total_oram_wait_time*100.0/total_user_perceived_time << " %)\n";
        //     cout << "ORAM Local Time: " << (total_oram_local_time*1000.0)/(*query_nums.rbegin()) << " ms (" << total_oram_local_time*100.0/total_user_perceived_time << " %)\n";
        // }

        return best_recall >= fail_if_recall_below ? 0 : -1;
    }


    int initiate_and_run_diskann(OramAPI* oram_api = nullptr){
        std::string data_type, 
                    dist_fn, 
                    index_path_prefix, 
                    result_path_prefix, 
                    query_file, 
                    gt_file, 
                    filter_label,
                    label_type, 
                    query_filters_file;
        uint32_t num_threads, K, W, num_nodes_to_cache, search_io_limit, search_iter_limit;
        std::vector<uint32_t> Lvec;
        std::vector<size_t> query_nums_v;
        bool use_reorder_data = false;
        float fail_if_recall_below = 0.0f;

        po::options_description desc{
            program_options_utils::make_program_description("search_disk_index", "Searches on-disk DiskANN indexes")
        };
        
        try {
            desc.add_options()("help,h", "Print information on arguments");

            // Required parameters
            po::options_description required_configs("Required");
            required_configs.add_options()("data_type", po::value<std::string>(&data_type)->required(),
                                        program_options_utils::DATA_TYPE_DESCRIPTION);
            required_configs.add_options()("dist_fn", po::value<std::string>(&dist_fn)->required(),
                                        program_options_utils::DISTANCE_FUNCTION_DESCRIPTION);
            required_configs.add_options()("index_path_prefix", po::value<std::string>(&index_path_prefix)->required(),
                                        program_options_utils::INDEX_PATH_PREFIX_DESCRIPTION);
            required_configs.add_options()("result_path", po::value<std::string>(&result_path_prefix)->required(),
                                        program_options_utils::RESULT_PATH_DESCRIPTION);
            required_configs.add_options()("query_file", po::value<std::string>(&query_file)->required(),
                                        program_options_utils::QUERY_FILE_DESCRIPTION);
            required_configs.add_options()("recall_at,K", po::value<uint32_t>(&K)->required(),
                                        program_options_utils::NUMBER_OF_RESULTS_DESCRIPTION);
            required_configs.add_options()("search_list,L",
                                        po::value<std::vector<uint32_t>>(&Lvec)->multitoken()->required(),
                                        program_options_utils::SEARCH_LIST_DESCRIPTION);
            required_configs.add_options()("query_nums", po::value<std::vector<size_t>>(&query_nums_v)->multitoken()->required(),
                                        program_options_utils::QUERY_NUMS_DESCRIPTION);

            // Optional parameters
            po::options_description optional_configs("Optional");
            optional_configs.add_options()("gt_file", po::value<std::string>(&gt_file)->default_value(std::string("null")),
                                        program_options_utils::GROUND_TRUTH_FILE_DESCRIPTION);
            optional_configs.add_options()("beamwidth,W", po::value<uint32_t>(&W)->default_value(2),
                                        program_options_utils::BEAMWIDTH);
            optional_configs.add_options()("num_nodes_to_cache", po::value<uint32_t>(&num_nodes_to_cache)->default_value(0),
                                        program_options_utils::NUMBER_OF_NODES_TO_CACHE);
            optional_configs.add_options()(
                "search_io_limit",
                po::value<uint32_t>(&search_io_limit)->default_value(std::numeric_limits<uint32_t>::max()),
                "Max #IOs for search.  Default value: uint32::max()");

            optional_configs.add_options()(
                "search_iter_limit",
                po::value<uint32_t>(&search_iter_limit)->default_value(std::numeric_limits<uint32_t>::max()),
                "Max #iterations for search.  Default value: uint32::max()");


            optional_configs.add_options()("num_threads,T",
                                        po::value<uint32_t>(&num_threads)->default_value(omp_get_num_procs()),
                                        program_options_utils::NUMBER_THREADS_DESCRIPTION);
            optional_configs.add_options()("use_reorder_data", po::bool_switch()->default_value(false),
                                        "Include full precision data in the index. Use only in "
                                        "conjuction with compressed data on SSD.  Default value: false");
            optional_configs.add_options()("label_type", po::value<std::string>(&label_type)->default_value("uint"),
                                        program_options_utils::LABEL_TYPE_DESCRIPTION);

            // Merge required and optional parameters
            desc.add(required_configs).add(optional_configs);


            po::variables_map vm;

            std::vector<char*> cstrings;
            for (const auto& s : diskann_args) {
                cstrings.push_back(const_cast<char*>(s.c_str()));
            }
            char** parsed_args = (char**) malloc(num_diskann_args * sizeof(char*));
            parsed_args = cstrings.data();

            po::store(po::parse_command_line(num_diskann_args, parsed_args, desc), vm);
            if (vm.count("help"))
            {
                std::cout << desc;
                return 0;
            }
            po::notify(vm);
            if (vm["use_reorder_data"].as<bool>())
                use_reorder_data = true;
        } catch (const std::exception &ex) {
            std::cerr << ex.what() << '\n';
            return -1;
        }

        diskann::Metric metric;
        if (dist_fn == "mips")
            metric = diskann::Metric::INNER_PRODUCT;
        else if (dist_fn == "cosine")
            metric = diskann::Metric::COSINE;
        else
            metric = diskann::Metric::L2;
        std::cout << "Using distance function: " << dist_fn << std::endl;
        std::vector<std::string> query_filters;  
        std::set<size_t> query_nums(query_nums_v.begin(), query_nums_v.end());    

        cout << "number of threads: " << num_threads << endl;


        try {
            if (data_type == std::string("float")){
                cout << "data type float" << endl;
                return search_disk_index<float>(metric, index_path_prefix, result_path_prefix, query_file, gt_file,
                                                num_threads, K, W, num_nodes_to_cache, search_io_limit, search_iter_limit, Lvec,
                                                fail_if_recall_below, query_filters, use_reorder_data, query_nums, oram_api);
            }
            else if (data_type == std::string("int8"))
                return search_disk_index<int8_t>(metric, index_path_prefix, result_path_prefix, query_file, gt_file,
                                                    num_threads, K, W, num_nodes_to_cache, search_io_limit, search_iter_limit, Lvec,
                                                    fail_if_recall_below, query_filters, use_reorder_data, query_nums, oram_api);
            else if (data_type == std::string("uint8"))
                return search_disk_index<uint8_t>(metric, index_path_prefix, result_path_prefix, query_file, gt_file,
                                                    num_threads, K, W, num_nodes_to_cache, search_io_limit, search_iter_limit, Lvec,
                                                    fail_if_recall_below, query_filters, use_reorder_data, query_nums, oram_api);
            else {
                std::cerr << "Unsupported data type. Use float or int8 or uint8" << std::endl;
                return -1;
            }    
        } catch (const std::exception &e) {
            std::cout << std::string(e.what()) << std::endl;
            diskann::cerr << "Index search failed." << std::endl;
            return -1;
        }
    }


    void read_diskann_args(string dataset){
        // vector<string> diskann_args;
        diskann_args.push_back("dummy_diskann_executable");
        set<string> valid_diskann_args{
            "--data_type",
            "--dist_fn",
            "-W",
            "-K",
            "-L",
            "-T",
            "--search_io_limit",
            "--search_iter_limit",
            "--num_nodes_to_cache",
            "--index_path_prefix",
            "--query_file",
            "--gt_file",
            "--result_path",
            "--query_nums"
        };

        std::ifstream file(diskann_config_file);  // Open file for reading
        string json_str;
        string line;

        while (std::getline(file, line)) {
            json_str += line + "\n";
        }
        file.close();

        rapidjson::Document diskann_config;
        diskann_config.Parse(json_str.c_str());
        assert(diskann_config.IsObject());

        if (diskann_config.HasParseError()) {
            std::cerr << "Parse error: " << diskann_config.GetParseError() << "\n";
        }
        
        if (!diskann_config.IsObject()) {
            std::cerr << "Parsed value is not an object\n";
        }

        for (rapidjson::Value::ConstMemberIterator dataset_config = diskann_config.MemberBegin(); dataset_config != diskann_config.MemberEnd(); ++dataset_config){
            if(dataset_config->name.GetString() == dataset){
                for (rapidjson::Value::ConstMemberIterator itr = dataset_config->value.MemberBegin(); itr != dataset_config->value.MemberEnd(); ++itr){
                    if(valid_diskann_args.count(itr->name.GetString())){
                        diskann_args.push_back(itr->name.GetString());
                        if(itr->value.IsString() || itr->value.IsBool()){
                            std::string value = itr->value.GetString();
                            std::string key = itr->name.GetString();
                            if (!value.empty() && (key.find("path") != std::string::npos || key == "--query_file" || key == "--gt_file") && std::filesystem::path(value).is_relative())
                                value = (std::filesystem::path(BISON_REPOSITORY_DIR) / value).lexically_normal().string();
                            diskann_args.push_back(value);
                        } else if (itr->value.IsArray()) {
                            std::string s = "";
                            const rapidjson::Value& arr = itr->value;
                            for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
                                s += to_string(arr[i].GetInt()) + " ";
                            }    
                            s.pop_back();
                            // s.pop_back();

                            diskann_args.push_back(s);                    
                        } else {
                            diskann_args.push_back(std::to_string(itr->value.GetInt()));                        
                        }
                    }
                }
            }
        }

        num_diskann_args = (int) diskann_args.size();

        // return diskann_args;
    }
};
