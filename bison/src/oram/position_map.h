//
// Position-map abstraction for OramRing.
//   LocalPositionMap     : plaintext vector of N leaf labels (native path)
//   RecursivePositionMap : stores the N labels inside a second Ring ORAM
//                          (one-level recursion), keeping only O(N/B) client
//                          state in the sub-ORAM's own local position map.
//

#ifndef PORAM_POSITION_MAP_H
#define PORAM_POSITION_MAP_H

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

class OramRing;

class IPositionMap {
  public:
    virtual ~IPositionMap() = default;
    virtual int read(int block_id) = 0;
    virtual void write(int block_id, int leaf) = 0;
    virtual void read_batch(const std::vector<int> &ids, std::vector<int> &leaves) = 0;
    virtual void write_batch(const std::vector<int> &ids, const std::vector<int> &leaves) = 0;
    virtual int *data() { return nullptr; }
    virtual size_t size() const = 0;
};

class LocalPositionMap : public IPositionMap {
  public:
    std::vector<int> pm;

    int read(int block_id) override { return pm[block_id]; }
    void write(int block_id, int leaf) override { pm[block_id] = leaf; }
    void read_batch(const std::vector<int> &ids, std::vector<int> &leaves) override {
        leaves.clear();
        leaves.reserve(ids.size());
        for (int id : ids)
            leaves.push_back(pm[id]);
    }
    void write_batch(const std::vector<int> &ids, const std::vector<int> &leaves) override {
        for (size_t i = 0; i < ids.size(); i++)
            pm[ids[i]] = leaves[i];
    }
    int *data() override { return pm.data(); }
    size_t size() const override { return pm.size(); }
    void resize(int n) { pm.resize(n); }

    void loadFromFile(const char *fname) {
        FILE *f = fopen(fname, "r");
        if (!f) { perror(fname); abort(); }
        int s;
        fread(&s, 1, sizeof(int), f);
        pm.resize(s);
        fread(pm.data(), 1, (size_t)s * sizeof(int), f);
        fclose(f);
    }
};

// One-level recursive position map: the main ORAM's leaf labels are packed
// into fixed-size sub-blocks stored in a second Ring ORAM.
class RecursivePositionMap : public IPositionMap {
  public:
    OramRing *posmap_oram;
    int labels_per_block;

    RecursivePositionMap(OramRing *oram, int labels_per_block)
        : posmap_oram(oram), labels_per_block(labels_per_block) {}

    int read(int block_id) override {
        std::vector<int> ids{block_id}, leaves;
        read_batch(ids, leaves);
        return leaves[0];
    }
    void write(int block_id, int leaf) override {
        std::vector<int> ids{block_id}, leaves{leaf};
        write_batch(ids, leaves);
    }
    void read_batch(const std::vector<int> &ids, std::vector<int> &leaves) override;
    void write_batch(const std::vector<int> &ids, const std::vector<int> &leaves) override;
    size_t size() const override { return 0; }
};

#endif
