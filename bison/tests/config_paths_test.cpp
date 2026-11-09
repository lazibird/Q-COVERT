#include "config_parser.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
int main() {
    try {
        for (const auto* name : {"sift", "sift10m", "deep1m", "msmarco1m"}) {
            for (bool low : {false, true}) {
                Metadata md{};
                auto file = std::filesystem::path(BISON_SOURCE_DIR_PATH) / "config" /
                    (std::string(low ? "bench_low_" : "bench_") + name + ".json");
                if (parseJson(file.string(), md, name)) throw std::runtime_error("config failed: " + file.string());
                if (md.low_client_storage != low) throw std::runtime_error("storage mode changed");
                for (const auto* path : {&md.buckets_path, &md.pos_map_path, &md.index_path_prefix})
                    if (!std::filesystem::path(*path).is_absolute() || path->find("data/prepared/bison/") == std::string::npos)
                        throw std::runtime_error("nonportable artifact path");
                if (low && md.oram_cached_levels != 0) throw std::runtime_error("low-storage cache changed");
            }
        }
        std::cout << "BiSON native/low configuration modes and paths passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
