#include "utils/config_parser.h"
#include "rapidjson/document.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

int main() {
    namespace fs = std::filesystem;
    try {
        std::ifstream input(fs::path(COMPASS_REPOSITORY_DIR) / "Q-COVERT-PS/config/dataset_defaults.json");
        std::stringstream text; text << input.rdbuf();
        rapidjson::Document ps; ps.Parse(text.str().c_str());
        if (ps.HasParseError() || !ps.IsObject()) throw std::runtime_error("invalid PS profiles");
        const char* names[] = {"sift", "sift10M", "sift100M", "deep1M", "msmarco1m"};
        const char* profiles[] = {"SIFT", "SIFT10M", "SIFT100M", "DEEP1M", "MSMARCO1M"};
        for (int i = 0; i < 5; ++i) {
            Metadata md{};
            if (parseJson(compass_default_config(), md, names[i])) throw std::runtime_error("cannot parse Compass config");
            const auto& files = ps["datasets"][profiles[i]]["files"];
            auto expect = [&](const std::string& actual, const char* role) {
                auto expected = (fs::path(COMPASS_REPOSITORY_DIR) / files[role].GetString()).lexically_normal();
                if (fs::path(actual) != expected) throw std::runtime_error(std::string(names[i]) + ": wrong " + role);
            };
            expect(md.base_path, "base"); expect(md.query_path, "query"); expect(md.gt_path, "groundtruth");
            for (const auto* path : {&md.index_path, &md.pq_path, &md.buckets_path, &md.pos_map_path})
                if (path->find("data/prepared/compass/") == std::string::npos)
                    throw std::runtime_error("Compass artifacts must use their own directory");
        }
        Metadata md{};
        if (parseJson(compass_default_config(), md, "missing-dataset") == 0)
            throw std::runtime_error("unknown dataset accepted");
        std::cout << "Compass shared input paths and artifact isolation passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
