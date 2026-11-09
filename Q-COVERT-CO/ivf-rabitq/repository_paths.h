#pragma once

#include "simple_json.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <utility>

#ifndef QCOVERT_CO_SOURCE_DIR
#error "Build with the supplied CMake project to define QCOVERT_CO_SOURCE_DIR"
#endif

namespace repository_paths {
// Raw filenames follow the PS profiles; custom filenames are relative to --source.
struct DatasetFiles {
    std::string base, query, groundtruth;
    void configure(const simple_json::Value& value) {
        if (!value.is_object()) throw std::runtime_error("files must be an object");
        for (auto field : {std::make_pair("base", &base), std::make_pair("query", &query),
                           std::make_pair("groundtruth", &groundtruth)}) {
            if (const auto* item = value.get(field.first))
                *field.second = simple_json::as_string(*item, std::string("files.") + field.first);
        }
    }
    std::string path(const std::string& source, std::string dataset, const std::string& role) const {
        const std::string& configured = role == "base" ? base : role == "query" ? query : groundtruth;
        if (!configured.empty()) return (std::filesystem::path(source) / configured).string();
        std::transform(dataset.begin(), dataset.end(), dataset.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool bin_base = dataset == "sift10m" || dataset == "sift100m" ||
                              dataset == "deep1m" || dataset == "msmarco1m" || dataset == "sift1b";
        if (role == "base" && bin_base) return (std::filesystem::path(source) / "base.bin").string();
        if (role != "base" && (dataset == "msmarco1m" || dataset == "sift1b"))
            return (std::filesystem::path(source) / (role == "query" ? "query.bin" : "gt.bin")).string();
        return (std::filesystem::path(source) / (dataset + "_" +
                (role == "groundtruth" ? "groundtruth.ivecs" : role + ".fvecs"))).string();
    }
};

inline std::string default_config() {
    return (std::filesystem::path(QCOVERT_CO_SOURCE_DIR) / "config.json").string();
}
inline std::string dataset(const std::string& name) {
    return (std::filesystem::path(QCOVERT_CO_SOURCE_DIR).parent_path() / "data" / name).string();
}
// Explicit CLI paths remain relative to the caller's working directory.
inline void resolve_config_source(std::string& source, const std::string& config,
                                  bool cli_source) {
    if (!cli_source && !source.empty()) {
        source = (std::filesystem::absolute(config).parent_path() / source).lexically_normal().string();
    }
}
}
