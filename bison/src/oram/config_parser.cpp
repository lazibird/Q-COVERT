#include "config_parser.h"

// stl
#include <iostream>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <filesystem>


// json
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/writer.h"
#include "rapidjson/error/en.h"



int parseJson(string config_path, Metadata &md, string dataset){
    // Open the JSON file
    ifstream file(config_path);
    if (!file.is_open()) {
        cerr << "Failed to open file." << endl;
        return 1;
    }

    // Read the entire file into a buffer
    string jsonContent;
    string line;
    while (getline(file, line)) {
        jsonContent += line + "\n";
    }
    file.close();

    // Parse the JSON content
    rapidjson::Document document;
    document.Parse(jsonContent.c_str());

    // Accessing values
    if (document.HasParseError()) {
        cerr << "Parse error: " << rapidjson::GetParseError_En(document.GetParseError()) << endl;
        return 1;
    }

    for (auto dataset_it = document.MemberBegin(); dataset_it != document.MemberEnd(); ++dataset_it) {
        if (!dataset_it->value.IsObject()) continue;
        for (auto it = dataset_it->value.MemberBegin(); it != dataset_it->value.MemberEnd(); ++it) {
            std::string key = it->name.GetString();
            if (!it->value.IsString()) continue;
            if (key.find("path") == std::string::npos && key != "--query_file" && key != "--gt_file") continue;
            std::filesystem::path path(it->value.GetString());
            if (!path.empty() && path.is_relative()) {
                std::string resolved = (std::filesystem::path(BISON_REPOSITORY_DIR) / path).lexically_normal().string();
                it->value.SetString(resolved.c_str(), document.GetAllocator());
            }
        }
    }

    // Extracting data
    if (document.HasMember(dataset) && document[dataset].IsObject()) {
        const rapidjson::Value& val = document[dataset];

        if (val.HasMember("--data_type") && val["--data_type"].IsString())
            md.data_type = val["--data_type"].GetString();
        else
            md.data_type = "float";
        if (md.data_type != "float" && md.data_type != "uint8") {
            cerr << "Unsupported ORAM data type: " << md.data_type << endl;
            return 1;
        }

        if(val.HasMember("integrity") && val["integrity"].IsBool()){
            md.integrity = val["integrity"].GetBool();
        } else{
            cerr << "Parse error: integrity not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("real_bucket_size") && val["real_bucket_size"].IsInt()){
            md.real_bucket_size = val["real_bucket_size"].GetInt();
        } else{
            cerr << "Parse error: real_bucket_size not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("dummy_size") && val["dummy_size"].IsInt()){
            md.dummy_size = val["dummy_size"].GetInt();
        } else{
            cerr << "Parse error: dummy_size not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("evict_rate") && val["evict_rate"].IsInt()){
            md.evict_rate = val["evict_rate"].GetInt();
        } else{
            cerr << "Parse error: evict_rate not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("num_levels") && val["num_levels"].IsInt()){
            md.num_levels = val["num_levels"].GetInt();
        } else{
            cerr << "Parse error: num_levels not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("oram_cached_levels") && val["oram_cached_levels"].IsInt()){
            md.oram_cached_levels = val["oram_cached_levels"].GetInt();
        } else{
            cerr << "Parse error: oram_cached_levels not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("large_number") && val["large_number"].IsInt()){
            md.large_number = val["large_number"].GetInt();
        } else{
            cerr << "Parse error: large_number not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("use_oram") && val["use_oram"].IsBool()){
            md.use_oram = val["use_oram"].GetBool();
        } else{
            cerr << "Parse error: use_oram not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("debug") && val["debug"].IsBool()){
            md.debug = val["debug"].GetBool();
            // cout << "Debug mode: " << (md.debug ? "ON" : "OFF") << endl;
        } else{
            cerr << "Parse error: debug not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("block_size") && val["block_size"].IsInt()){
            md.block_size = val["block_size"].GetInt();
        } else{
            cerr << "Parse error: block_size not found or invalid format" << endl;
            return 1;
        } 

        if(val.HasMember("dim") && val["dim"].IsInt()){
            md.dim = val["dim"].GetInt();
        } else{
            cerr << "Parse error: dim not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("R") && val["R"].IsInt()){
            md.R = val["R"].GetInt();
        } else{
            cerr << "Parse error: R not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("base_size") && val["base_size"].IsInt()){
            md.base_size = val["base_size"].GetInt();
        } else{
            cerr << "Parse error: base_size not found or invalid format" << endl;
            return 1;
        }

        // if(val.HasMember("ef_search") && val["ef_search"].IsInt()){
        //     md.ef_search = val["ef_search"].GetInt();
        // } else{
        //     cerr << "Parse error: ef_search not found or invalid format" << endl;
        //     return 1;
        // }

        if(val.HasMember("-L") && val["-L"].IsInt()){
            md.ef_spec = val["-L"].GetInt();
        } else{
            cerr << "Parse error: L not found or invalid format" << endl;
            return 1;
        }

        // if(val.HasMember("ef_spec") && val["ef_spec"].IsInt()){
        //     md.ef_spec = val["ef_spec"].GetInt();
        // } else{
        //     cerr << "Parse error: ef_spec not found or invalid format" << endl;
        //     return 1;
        // }

        // if(val.HasMember("ef_neighbor") && val["ef_neighbor"].IsInt()){
        //     md.ef_neighbor = val["ef_neighbor"].GetInt();
        // } else{
        //     cerr << "Parse error: ef_neighbor not found or invalid format" << endl;
        //     return 1;
        // }

        // if(val.HasMember("ef_lowest_cached_layer") && val["ef_lowest_cached_layer"].IsInt()){
        //     md.ef_lowest_cached_layer = val["ef_lowest_cached_layer"].GetInt();
        // } else{
        //     cerr << "Parse error: ef_lowest_cached_layer not found or invalid format" << endl;
        //     return 1;
        // }

        // if(val.HasMember("ef_upper_steps") && val["ef_upper_steps"].IsInt()){
        //     md.ef_upper_steps = val["ef_upper_steps"].GetInt();
        // } else{
        //     cerr << "Parse error: ef_upper_steps not found or invalid format" << endl;
        //     return 1;
        // }

        // if(val.HasMember("index_path") && val["index_path"].IsString()){
        //     md.index_path = val["index_path"].GetString();
        // } else{
        //     cerr << "Parse error: index_path not found or invalid format" << endl;
        //     return 1;
        // }

        // if(val.HasMember("pq_path") && val["pq_path"].IsString()){
        //     md.pq_path = val["pq_path"].GetString();
        // } else{
        //     cerr << "Parse error: pq_path not found or invalid format" << endl;
        //     return 1;
        // } 

        if(val.HasMember("base_path") && val["base_path"].IsString()){
            md.base_path = val["base_path"].GetString();
        } else{
            cerr << "Parse error: base_path not found or invalid format" << endl;
            return 1;
        }

        // if(val.HasMember("query_path") && val["query_path"].IsString()){
        //     md.query_path = val["query_path"].GetString();
        // } else{
        //     cerr << "Parse error: query_path not found or invalid format" << endl;
        //     return 1;
        // }

        // if(val.HasMember("gt_path") && val["gt_path"].IsString()){
        //     md.gt_path = val["gt_path"].GetString();
        // } else{
        //     cerr << "Parse error: gt_path not found or invalid format" << endl;
        //     return 1;
        // }

        if(val.HasMember("--index_path_prefix") && val["--index_path_prefix"].IsString()){
            md.index_path_prefix = val["--index_path_prefix"].GetString();
        } else{
            cerr << "Parse error: --index_path_prefix not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("buckets_path") && val["buckets_path"].IsString()){
            md.buckets_path = val["buckets_path"].GetString();
        } else{
            cerr << "Parse error: buckets_path not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("pos_map_path") && val["pos_map_path"].IsString()){
            md.pos_map_path = val["pos_map_path"].GetString();
        } else{
            cerr << "Parse error: pos_map_path not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("block_map_path") && val["block_map_path"].IsString()){
            md.block_map_path = val["block_map_path"].GetString();
        } else{
            cerr << "Parse error: block_map_path not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("metadata_path") && val["metadata_path"].IsString()){
            md.metadata_path = val["metadata_path"].GetString();
        } else{
            cerr << "Parse error: metadata_path not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("graph_cache_path") && val["graph_cache_path"].IsString()){
            md.graph_cache_path = val["graph_cache_path"].GetString();
        } else{
            cerr << "Parse error: graph_cache_path not found or invalid format" << endl;
            return 1;
        }

        if(val.HasMember("hash_path") && val["hash_path"].IsString()){
            md.hash_path = val["hash_path"].GetString();
        } else{
            cerr << "Parse error: hash_path not found or invalid format" << endl;
            return 1;
        }

        
        // ---- low-client-storage ORAM path (all optional) ----
        md.low_client_storage = false;
        if (val.HasMember("low_client_storage") && val["low_client_storage"].IsBool())
            md.low_client_storage = val["low_client_storage"].GetBool();

        md.recursive_posmap = true;
        if (val.HasMember("recursive_posmap") && val["recursive_posmap"].IsBool())
            md.recursive_posmap = val["recursive_posmap"].GetBool();

        md.pq_bytes = 0;
        if (val.HasMember("pq_bytes") && val["pq_bytes"].IsInt())
            md.pq_bytes = val["pq_bytes"].GetInt();

        md.pq_oram_buckets_path.clear();
        if (val.HasMember("pq_oram_buckets_path") && val["pq_oram_buckets_path"].IsString())
            md.pq_oram_buckets_path = val["pq_oram_buckets_path"].GetString();
        md.pq_oram_pos_map_path.clear();
        if (val.HasMember("pq_oram_pos_map_path") && val["pq_oram_pos_map_path"].IsString())
            md.pq_oram_pos_map_path = val["pq_oram_pos_map_path"].GetString();
        md.pq_oram_metadata_path.clear();
        if (val.HasMember("pq_oram_metadata_path") && val["pq_oram_metadata_path"].IsString())
            md.pq_oram_metadata_path = val["pq_oram_metadata_path"].GetString();
        md.pq_oram_hash_path.clear();
        if (val.HasMember("pq_oram_hash_path") && val["pq_oram_hash_path"].IsString())
            md.pq_oram_hash_path = val["pq_oram_hash_path"].GetString();
        md.pq_oram_block_size_bytes = 0;
        if (val.HasMember("pq_oram_block_size_bytes") && val["pq_oram_block_size_bytes"].IsInt())
            md.pq_oram_block_size_bytes = val["pq_oram_block_size_bytes"].GetInt();
        md.pq_oram_num_levels = 0;
        if (val.HasMember("pq_oram_num_levels") && val["pq_oram_num_levels"].IsInt())
            md.pq_oram_num_levels = val["pq_oram_num_levels"].GetInt();
        md.pq_oram_num_blocks = 0;
        if (val.HasMember("pq_oram_num_blocks") && val["pq_oram_num_blocks"].IsInt())
            md.pq_oram_num_blocks = val["pq_oram_num_blocks"].GetInt();

        md.posmap_oram_buckets_path.clear();
        if (val.HasMember("posmap_oram_buckets_path") && val["posmap_oram_buckets_path"].IsString())
            md.posmap_oram_buckets_path = val["posmap_oram_buckets_path"].GetString();
        md.posmap_oram_pos_map_path.clear();
        if (val.HasMember("posmap_oram_pos_map_path") && val["posmap_oram_pos_map_path"].IsString())
            md.posmap_oram_pos_map_path = val["posmap_oram_pos_map_path"].GetString();
        md.posmap_oram_metadata_path.clear();
        if (val.HasMember("posmap_oram_metadata_path") && val["posmap_oram_metadata_path"].IsString())
            md.posmap_oram_metadata_path = val["posmap_oram_metadata_path"].GetString();
        md.posmap_oram_hash_path.clear();
        if (val.HasMember("posmap_oram_hash_path") && val["posmap_oram_hash_path"].IsString())
            md.posmap_oram_hash_path = val["posmap_oram_hash_path"].GetString();
        md.posmap_oram_block_size_bytes = 0;
        if (val.HasMember("posmap_oram_block_size_bytes") && val["posmap_oram_block_size_bytes"].IsInt())
            md.posmap_oram_block_size_bytes = val["posmap_oram_block_size_bytes"].GetInt();
        md.posmap_oram_num_levels = 0;
        if (val.HasMember("posmap_oram_num_levels") && val["posmap_oram_num_levels"].IsInt())
            md.posmap_oram_num_levels = val["posmap_oram_num_levels"].GetInt();
        md.posmap_oram_num_blocks = 0;
        if (val.HasMember("posmap_oram_num_blocks") && val["posmap_oram_num_blocks"].IsInt())
            md.posmap_oram_num_blocks = val["posmap_oram_num_blocks"].GetInt();

        if(val.HasMember("--query_nums") && val["--query_nums"].IsInt64()){
            md.num_queries = val["--query_nums"].GetInt64();
        } else{
            cerr << "Parse error: --query_nums not found or invalid format" << endl;
            return 1;
        }
    }

    return 0;

}

bool overwrite_json_nested_field(const std::string &filename,
                                 const std::string &dataset,
                                 const std::string &field,
                                 int new_value) {
    FILE* fp = fopen(filename.c_str(), "rb");
    if (!fp) {
        std::cerr << "Error: Cannot open file " << filename << std::endl;
        return false;
    }

    char readBuffer[65536];
    rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
    rapidjson::Document doc;
    doc.ParseStream(is);
    fclose(fp);

    if (!doc.IsObject() || !doc.HasMember(dataset.c_str())) {
        std::cerr << "Error: Dataset " << dataset << " not found in config." << std::endl;
        return false;
    }

    rapidjson::Value& ds = doc[dataset.c_str()];
    if (!ds.IsObject()) {
        std::cerr << "Error: Dataset section is not an object." << std::endl;
        return false;
    }

    rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();
    if (ds.HasMember(field.c_str())) {
        ds[field.c_str()].SetInt(new_value);
    } else {
        ds.AddMember(rapidjson::Value().SetString(field.c_str(), allocator),
                     rapidjson::Value().SetInt(new_value),
                     allocator);
    }

    // Write back to file
    fp = fopen(filename.c_str(), "wb");
    if (!fp) {
        std::cerr << "Error: Cannot write file " << filename << std::endl;
        return false;
    }

    char writeBuffer[65536];
    rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
    rapidjson::Writer<rapidjson::FileWriteStream> writer(os);
    doc.Accept(writer);
    fclose(fp);

    return true;
}


bool overwrite_json_nested_field_bool(const std::string &filename,
                                 const std::string &dataset,
                                 const std::string &field,
                                 bool new_value) {
    FILE* fp = fopen(filename.c_str(), "rb");
    if (!fp) {
        std::cerr << "Error: Cannot open file " << filename << std::endl;
        return false;
    }

    char readBuffer[65536];
    rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
    rapidjson::Document doc;
    doc.ParseStream(is);
    fclose(fp);

    if (!doc.IsObject() || !doc.HasMember(dataset.c_str())) {
        std::cerr << "Error: Dataset " << dataset << " not found in config." << std::endl;
        return false;
    }

    rapidjson::Value& ds = doc[dataset.c_str()];
    if (!ds.IsObject()) {
        std::cerr << "Error: Dataset section is not an object." << std::endl;
        return false;
    }

    rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();
    if (ds.HasMember(field.c_str())) {
        ds[field.c_str()].SetBool(new_value);
    } else {
        ds.AddMember(rapidjson::Value().SetString(field.c_str(), allocator),
                     rapidjson::Value().SetBool(new_value),
                     allocator);
    }

    // Write back to file
    fp = fopen(filename.c_str(), "wb");
    if (!fp) {
        std::cerr << "Error: Cannot write file " << filename << std::endl;
        return false;
    }

    char writeBuffer[65536];
    rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
    rapidjson::Writer<rapidjson::FileWriteStream> writer(os);
    doc.Accept(writer);
    fclose(fp);

    return true;
}
