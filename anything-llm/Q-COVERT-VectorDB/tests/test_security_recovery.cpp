#include "qcovert/collection_catalog.h"
#include "qcovert/collection_data_store.h"
#include "qcovert/generation_key_store.h"

#include "RemoteRing.h"
#include "evp.h"
#include "simple_json.h"
#include "ivf_rabitq_index.h"
#include "matrix.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>

namespace fs = std::filesystem;

namespace {

qcovert::VectorRecord record(const std::string& id, float value) {
    qcovert::VectorRecord output;
    output.id = id;
    output.values.assign(qcovert::kSupportedEmbeddingDimension, value);
    output.metadata_json = "{\"label\":\"" + id + "\"}";
    return output;
}

void test_authenticated_encryption() {
    AeadKey key{};
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<unsigned char>(i + 1);
    std::array<unsigned char, 37> plaintext{};
    for (size_t i = 0; i < plaintext.size(); ++i) plaintext[i] = static_cast<unsigned char>(i * 3);
    std::vector<unsigned char> ciphertext(compute_ctx_len(plaintext.size()));
    const int ciphertext_bytes = encrypt_wrapper(
        key, plaintext.data(), plaintext.size(), ciphertext.data());
    std::array<unsigned char, 37> decoded{};
    if (decrypt_wrapper(key, ciphertext.data(), ciphertext_bytes, decoded.data()) !=
            static_cast<int>(plaintext.size()) || decoded != plaintext) {
        throw std::runtime_error("AES-GCM round trip failed");
    }
    ciphertext.back() ^= 1U;
    bool rejected = false;
    try {
        decrypt_wrapper(key, ciphertext.data(), ciphertext_bytes, decoded.data());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected) throw std::runtime_error("AES-GCM accepted a modified authentication tag");

    std::array<unsigned char, kAeadNonceBytes> nonce_a{};
    std::array<unsigned char, kAeadNonceBytes> nonce_b{};
    nonce_b.fill(7);
    std::array<unsigned char, 37> dummy{};
    std::vector<unsigned char> real_body(plaintext.size() + kAeadTagBytes);
    std::vector<unsigned char> dummy_body(dummy.size() + kAeadTagBytes);
    encrypt_wrapper_with_iv(key, nonce_a.data(), plaintext.data(), plaintext.size(), real_body.data());
    encrypt_wrapper_with_iv(key, nonce_b.data(), dummy.data(), dummy.size(), dummy_body.data());
    std::vector<unsigned char> xor_body(real_body.size());
    for (size_t i = 0; i < xor_body.size(); ++i) {
        xor_body[i] = real_body[i] ^ dummy_body[i];
        xor_body[i] ^= dummy_body[i];
    }
    if (decrypt_wrapper_with_iv(key, nonce_a.data(), xor_body.data(), xor_body.size(),
                                decoded.data()) != static_cast<int>(plaintext.size()) ||
        decoded != plaintext) {
        throw std::runtime_error("authenticated ciphertext no longer supports XOR recovery");
    }
}

void test_key_store(const fs::path& root, qcovert::CollectionCatalog& catalog) {
    catalog.create_collection("keys", 384, "l2", "test");
    qcovert::GenerationKeyStore keys(root);
    const AeadKey created = keys.create("keys", "generation_1");
    const AeadKey loaded = keys.load("keys", "generation_1");
    if (created != loaded) throw std::runtime_error("persisted generation key changed");
    struct stat info{};
    if (stat(keys.key_path("keys", "generation_1").c_str(), &info) != 0 ||
        (info.st_mode & 0777) != 0600) {
        throw std::runtime_error("generation key is not restricted to mode 0600");
    }
}

void test_log_recovery(const fs::path& root, qcovert::CollectionCatalog& catalog) {
    catalog.create_collection("recover", 384, "l2", "test");
    {
        qcovert::CollectionDataStore data(catalog);
        data.upsert("recover", {record("a", 1.0f), record("b", 2.0f)});
    }
    const fs::path log = root / "recover" / "vectors.log";
    const uint64_t committed_size = fs::file_size(log);
    {
        std::ofstream tail(log, std::ios::binary | std::ios::app);
        const uint32_t partial_batch_magic = 0x32424351U;
        tail.write(reinterpret_cast<const char*>(&partial_batch_magic), sizeof(partial_batch_magic));
        tail << "partial";
    }
    catalog.set_vector_count("recover", 99);
    {
        qcovert::CollectionDataStore recovered(catalog);
        if (recovered.vector_count("recover") != 2 || fs::file_size(log) != committed_size ||
            catalog.get_collection("recover").vector_count != 2) {
            throw std::runtime_error("vector log tail recovery or manifest reconciliation failed");
        }
        recovered.upsert("recover", {record("a", 4.0f), record("c", 5.0f)});
        const std::vector<qcovert::VectorDelta> deltas = recovered.read_latest_after("recover", 2);
        if (deltas.size() != 2 || deltas[0].sequence != 3 || deltas[1].sequence != 4) {
            throw std::runtime_error("post-snapshot append replay returned the wrong deltas");
        }
    }

    catalog.create_collection("corrupt", 384, "l2", "test");
    {
        qcovert::CollectionDataStore data(catalog);
        data.upsert("corrupt", {record("x", 3.0f)});
    }
    const fs::path corrupt_log = root / "corrupt" / "vectors.log";
    {
        std::fstream file(corrupt_log, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(72);
        char byte = 0;
        file.read(&byte, 1);
        byte ^= 0x40;
        file.seekp(72);
        file.write(&byte, 1);
    }
    bool rejected = false;
    try {
        qcovert::CollectionDataStore corrupted(catalog);
        corrupted.vector_count("corrupt");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected) throw std::runtime_error("committed vector log corruption was accepted");

    catalog.create_collection("concurrent", 384, "l2", "test");
    qcovert::CollectionDataStore concurrent(catalog);
    std::vector<std::thread> writers;
    for (int worker = 0; worker < 4; ++worker) {
        writers.emplace_back([worker, &concurrent] {
            for (int i = 0; i < 10; ++i) {
                concurrent.upsert("concurrent", {
                    record("worker-" + std::to_string(worker) + "-" + std::to_string(i),
                           static_cast<float>(worker * 10 + i))});
            }
        });
    }
    for (std::thread& writer : writers) writer.join();
    if (concurrent.vector_count("concurrent") != 40) {
        throw std::runtime_error("concurrent upserts lost committed records");
    }
}

void test_unicode_json() {
    const simple_json::Value value = simple_json::parse_text(
        "{\"bmp\":\"\\u4e2d\\u6587\",\"supplementary\":\"\\ud83d\\ude00\"}");
    if (value.get("bmp")->string_value != "\xE4\xB8\xAD\xE6\x96\x87" ||
        value.get("supplementary")->string_value != "\xF0\x9F\x98\x80") {
        throw std::runtime_error("JSON unicode escapes were not decoded as UTF-8");
    }
    bool rejected = false;
    try {
        simple_json::parse_text("{\"bad\":\"\\ud800x\"}");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected) throw std::runtime_error("JSON parser accepted an unmatched surrogate");
}

void test_remote_state_errors(const fs::path& root) {
    const fs::path state = root / "remote" / "ring.ring";
    fs::create_directories(state.parent_path());
    double_oram::RingOramConfig config(1, 1, 1, 1, 1, 2, 0);
    double_oram::RemoteRing server(nullptr, config, true, true, false);
    server.save_server_state(state.c_str());
    double_oram::RemoteRing loaded(nullptr, config, true, true, false);
    loaded.load_server_state(state.c_str());
    fs::resize_file(state, fs::file_size(state) - 1);
    bool rejected = false;
    try {
        loaded.load_server_state(state.c_str());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected) throw std::runtime_error("truncated RemoteRing state was accepted");
}

void test_multi_cluster_index(const fs::path& root) {
    constexpr uint32_t count = 64;
    constexpr uint32_t clusters = 4;
    Matrix<float> vectors(count, 384);
    Matrix<float> randomized_centroids(clusters, 384);
    Matrix<float> distances(count, 1);
    Matrix<float> x0(count, 1);
    Matrix<uint32_t> assignments(count, 1);
    Matrix<uint64_t> binary(count, 384 / 64);
    for (uint32_t i = 0; i < count; ++i) {
        assignments.values[i] = i % clusters;
        distances.values[i] = static_cast<float>(i % 7) * 0.1f;
        x0.values[i] = 0.8f;
        for (uint32_t d = 0; d < 384; ++d) {
            vectors.row(i)[d] = static_cast<float>((i % clusters) * 10 + (d % 3));
        }
        for (uint32_t word = 0; word < 384 / 64; ++word) {
            binary.row(i)[word] = (static_cast<uint64_t>(i) << 32U) | word;
        }
    }
    IvfRabitQIndex<384, 384, 4> index(
        vectors, randomized_centroids, distances, x0, assignments, binary);
    if (index.num_clusters != clusters) {
        throw std::runtime_error("multi-cluster index lost its cluster count");
    }
    for (uint32_t cluster = 0; cluster < clusters; ++cluster) {
        if (index.len[cluster] != count / clusters || index.cluster_num_blocks(cluster) != 1) {
            throw std::runtime_error("multi-cluster index produced the wrong cluster layout");
        }
    }
    const fs::path path = root / "multi-cluster.index";
    index.save(path.string());
    IvfRabitQIndex<384, 384, 4> reloaded;
    reloaded.load(path.string());
    if (reloaded.num_clusters != clusters || reloaded.n != count || reloaded.len != index.len) {
        throw std::runtime_error("multi-cluster index persistence changed its layout");
    }
}

} // namespace

int main() {
    const fs::path root = "/tmp/qcovert-security-recovery-test";
    std::error_code error;
    fs::remove_all(root, error);
    try {
        test_authenticated_encryption();
        qcovert::CollectionCatalog catalog(root);
        test_key_store(root, catalog);
        test_log_recovery(root, catalog);
        test_unicode_json();
        test_remote_state_errors(root);
        test_multi_cluster_index(root);
        fs::remove_all(root);
        std::cout << "test_security_recovery passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "test_security_recovery failed: " << exception.what() << "\n";
        return 1;
    }
}
