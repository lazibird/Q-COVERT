//
// RawOramBuilder: builds a Ring ORAM bucket file from arbitrary fixed-size
// int32 payloads. The on-disk format (buckets.bin / position_map.bin /
// metadata.bin / hash.bin) matches OramBuilder so the existing server/client
// loaders can consume it unchanged.
//

#ifndef RAW_ORAM_BUILDER_H
#define RAW_ORAM_BUILDER_H

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <cmath>

#include "Bucket.h"
#include "Block.h"
#include "OramInterface.h"
#include "RandForOramInterface.h"
#include "RandomForOram.h"

#include "evp.h"

class RawOramBuilder {
  public:
    const char *buckets_path;
    const char *position_map_path;
    const char *metadata_path;
    const char *hash_path;
    RingOramConfig *config;

    RawOramBuilder(const char *buckets_path, const char *position_map_path,
                   const char *metadata_path, const char *hash_path,
                   RingOramConfig *config)
        : buckets_path(buckets_path), position_map_path(position_map_path),
          metadata_path(metadata_path), hash_path(hash_path), config(config) {}

    // Returns a random leaf whose path crosses the given bucket.
    int P_inv(RandForOramInterface *rand, off_t bucket_pos) {
        int level = 0;
        for (; level < config->num_levels; ++level) {
            int start = (1 << level) - 1;
            int end = (1 << (level + 1)) - 1;
            if (bucket_pos >= start && bucket_pos < end)
                break;
        }
        int leaf_prefix = (int)(bucket_pos - ((1 << level) - 1));
        int suffix_bits = config->num_levels - level - 1;
        int random_suffix = (suffix_bits > 0) ? rand->getRandomLeafWithBound(1 << suffix_bits) : 0;
        int leaf = (leaf_prefix << suffix_bits) | random_suffix;
        assert(((1 << level) - 1 + (leaf >> (config->num_levels - level - 1))) == bucket_pos);
        return leaf;
    }

    // payloads[i] has length <= config->block_size (in int32 words).
    void build(const std::vector<std::vector<int>> &payloads) {
        const int per_block_size = (1 + config->block_size) * (int)sizeof(int);

        FILE *fbuckets = fopen(buckets_path, "w");
        if (!fbuckets) { perror(buckets_path); abort(); }
        fwrite(&(config->num_buckets), 1, sizeof(int), fbuckets);
        int sb_size = SBucket::getCipherSize();
        fwrite(&sb_size, 1, sizeof(int), fbuckets);
        bool mal_security = true;
        fwrite(&mal_security, 1, sizeof(bool), fbuckets);

        FILE *fbids = fopen(metadata_path, "w");
        if (!fbids) { perror(metadata_path); abort(); }
        int s = config->num_buckets * config->bucket_size;
        fwrite(&s, 1, sizeof(int), fbids);

        FILE *fhash = fopen(hash_path, "w");
        if (!fhash) { perror(hash_path); abort(); }

        // merkle tree scratch
        size_t bucket_tree_height = (size_t)ceil(log10(config->bucket_size) / log10(2)) + 1;
        size_t bucket_num_hashes = (1UL << bucket_tree_height) - 1;
        size_t sec_MT_size = SHA256_DIGEST_LENGTH * bucket_num_hashes;
        uint8_t *per_block_hash = new uint8_t[SHA256_DIGEST_LENGTH * config->bucket_size];
        uint8_t *sec_MT = new uint8_t[sec_MT_size];
        memset(sec_MT, 0, sec_MT_size);

        RandForOramInterface *random = new RandomForOram();
        random->setBound(config->num_leaves);

        std::vector<int> graph_position_map;
        graph_position_map.reserve(config->num_blocks);

        std::vector<SBucket *> sbuckets(config->bucket_size, nullptr);
        for (int x = 0; x < config->bucket_size; x++)
            sbuckets[x] = new SBucket(true);
        std::vector<Bucket *> buckets(config->real_bucket_size, nullptr);
        for (int x = 0; x < config->real_bucket_size; x++)
            buckets[x] = new Bucket();
        std::vector<int> block_ids(config->bucket_size);

        auto write_bucket = [&](off_t cur_bucket, int cur_bucket_size) {
            (void)cur_bucket;
            for (int j = 0; j < cur_bucket_size; j++) {
                unsigned char *payload = new unsigned char[SBucket::getCipherSize()];
                Block rrb = buckets[j]->getRRBlock();
                rrb.to_ptr(payload);
                block_ids[j] = rrb.index;
                int ctx_len = encrypt_wrapper(payload, per_block_size * Bucket::getMaxSize(), sbuckets[j]->data);
                assert(ctx_len == SBucket::getCipherSize());
                delete[] payload;
            }
            for (int j = cur_bucket_size; j < config->bucket_size; j++) {
                unsigned char *payload = new unsigned char[SBucket::getCipherSize()];
                Block dummy = Block(config->block_size);
                dummy.to_ptr(payload);
                block_ids[j] = -1;
                int ctx_len = encrypt_wrapper(payload, per_block_size * Bucket::getMaxSize(), sbuckets[j]->data);
                assert(ctx_len == SBucket::getCipherSize());
                delete[] payload;
            }

            for (size_t j = 0; j < (size_t)config->bucket_size; j++)
                sha256_wrapper(sbuckets[j]->data, SBucket::getCipherSize(), per_block_hash + j * SHA256_DIGEST_LENGTH);

            for (size_t j = bucket_num_hashes; j >= 1; j--) {
                size_t id = j - 1;
                size_t lid = 2 * j - 1;
                size_t rid = 2 * j;
                if (lid > bucket_num_hashes - 1) {
                    size_t offset = j - (bucket_num_hashes + 1) / 2;
                    if (offset < (size_t)config->bucket_size)
                        memcpy(sec_MT + id * SHA256_DIGEST_LENGTH,
                               per_block_hash + offset * SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH);
                } else {
                    sha256_twin_input_wrapper(sec_MT + lid * SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH,
                                              sec_MT + rid * SHA256_DIGEST_LENGTH, SHA256_DIGEST_LENGTH,
                                              sec_MT + id * SHA256_DIGEST_LENGTH);
                }
            }

            for (size_t i = 0; i < (size_t)config->bucket_size; i++)
                fwrite(sbuckets[i]->data, 1, SBucket::getCipherSize(), fbuckets);
            fwrite(block_ids.data(), 1, block_ids.size() * sizeof(int), fbids);
            fwrite(sec_MT, 1, sec_MT_size, fhash);
        };

        off_t cur_bucket = 0;
        int cur_bucket_size = 0;
        for (size_t block_id = 0; block_id < payloads.size(); block_id++) {
            Block b(config->block_size);
            b.index = (int)block_id;
            assert((int)payloads[block_id].size() <= config->block_size);
            memcpy(b.data.data(), payloads[block_id].data(), payloads[block_id].size() * sizeof(int));
            buckets[cur_bucket_size]->addRRBlock(b);
            graph_position_map.push_back(P_inv(random, cur_bucket));
            cur_bucket_size++;
            if (cur_bucket_size >= config->real_bucket_size) {
                assert(cur_bucket_size == config->real_bucket_size);
                write_bucket(cur_bucket, cur_bucket_size);
                cur_bucket_size = 0;
                cur_bucket++;
                for (int x = 0; x < config->real_bucket_size; x++)
                    buckets[x] = new Bucket();
            }
        }

        while (cur_bucket < config->num_buckets) {
            write_bucket(cur_bucket, cur_bucket_size);
            cur_bucket_size = 0;
            cur_bucket++;
        }

        FILE *fpos = fopen(position_map_path, "w");
        if (!fpos) { perror(position_map_path); abort(); }
        int ps = (int)graph_position_map.size();
        fwrite(&ps, 1, sizeof(int), fpos);
        fwrite(graph_position_map.data(), 1, graph_position_map.size() * sizeof(int), fpos);

        fclose(fpos);
        fclose(fbuckets);
        fclose(fbids);
        fclose(fhash);
        for (auto *b : buckets) delete b;
        for (auto *sb : sbuckets) delete sb;
        delete[] per_block_hash;
        delete[] sec_MT;
        ((RandomForOram *)random)->resetState();
        delete random;
    }
};

#endif
