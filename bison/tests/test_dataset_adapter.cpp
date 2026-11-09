#include "OramBuilder.h"

// Standalone adapter test does not link the full ORAM runtime.
template<> int DiskANNNode<uint8_t, node_id_t>::dim = -1;
template<> int DiskANNNode<uint8_t, node_id_t>::n_neighbors = -1;

#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

template<typename T>
void check_index_node(int dim) {
    auto path = std::filesystem::temp_directory_path() /
                ("bison_adapter_" + std::to_string(dim) + "_" + std::to_string(sizeof(T)) + ".index");
    std::vector<T> coords(dim);
    for (int i = 0; i < dim; ++i) coords[i] = static_cast<T>(i % 251);
    uint32_t count = 2;
    std::array<uint32_t, 2> neighbors{7, 9};
    {
        std::ofstream file(path, std::ios::binary);
        std::array<char, 4096> header{};
        std::array<uint64_t, 5> metadata{1, static_cast<uint64_t>(dim), 0,
                                          static_cast<uint64_t>(dim * sizeof(T) + 12),
                                          static_cast<uint64_t>(4096 / (dim * sizeof(T) + 12))};
        std::memcpy(header.data() + 8, metadata.data(), sizeof(metadata));
        file.write(header.data(), header.size());
        file.write(reinterpret_cast<const char*>(coords.data()), dim * sizeof(T));
        file.write(reinterpret_cast<const char*>(&count), 4);
        file.write(reinterpret_cast<const char*>(neighbors.data()), 8);
    }
    DiskANNIndexReader reader(path.string(), dim, 2, sizeof(T));
    auto node = reader.read_node(0);
    assert(node.nbr_count == 2 && node.neighbors == std::vector<int32_t>({7, 9}));
    for (int i = 0; i < dim; ++i) assert(node.embed[i] == static_cast<float>(coords[i]));
    bool rejected_mismatch = false;
    try {
        DiskANNIndexReader wrong(path.string(), dim + 1, 2, sizeof(T));
    } catch (const std::runtime_error&) {
        rejected_mismatch = true;
    }
    assert(rejected_mismatch);
    std::filesystem::remove(path);
}

int main() {
    check_index_node<float>(96);
    check_index_node<float>(128);
    check_index_node<float>(768);
    check_index_node<uint8_t>(128);

    DiskANNNode<uint8_t, node_id_t>::dim = 128;
    DiskANNNode<uint8_t, node_id_t>::n_neighbors = 4;
    std::vector<uint8_t> coords(128);
    for (int i = 0; i < 128; ++i) coords[i] = static_cast<uint8_t>(i);
    std::vector<node_id_t> neighbors{1234567, 42, -1, -1};
    DiskANNNode<uint8_t, node_id_t> built;
    built.set_id(1000);
    built.set_coords(coords);
    built.set_neighbors(neighbors);
    DiskANNNode<uint8_t, node_id_t> fetched(built.get_int_data(), 1);
    assert(fetched.get_id() == 1000);
    auto* bytes = reinterpret_cast<uint8_t*>(fetched.get_diskann_aligned_data());
    for (int i = 0; i < 128; ++i) assert(bytes[i] == coords[i]);
    uint32_t count;
    std::memcpy(&count, bytes + 128, 4);
    assert(count == 2);
    uint32_t first, second;
    std::memcpy(&first, bytes + 132, 4);
    std::memcpy(&second, bytes + 136, 4);
    assert(first == 1234567 && second == 42);
}
