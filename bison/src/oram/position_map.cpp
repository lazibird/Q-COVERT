#include "position_map.h"
#include "OramRing.h"
#include "OramInterface.h"

#include <map>

void RecursivePositionMap::read_batch(const std::vector<int> &ids, std::vector<int> &leaves) {
    leaves.clear();
    leaves.reserve(ids.size());

    std::vector<int> sub_blocks;
    std::map<int, int> sub_to_idx;
    for (int id : ids) {
        int sub = id / labels_per_block;
        if (sub_to_idx.find(sub) == sub_to_idx.end()) {
            sub_to_idx[sub] = (int)sub_blocks.size();
            sub_blocks.push_back(sub);
        }
    }

    if (sub_blocks.empty())
        return;

    std::vector<int *> fetched = posmap_oram->batch_multi_access_swap_ro_templated<int>(
        std::vector<OramInterface::Operation>(sub_blocks.size(), OramInterface::Operation::READ),
        sub_blocks,
        std::vector<int *>(sub_blocks.size(), nullptr),
        (int)sub_blocks.size());

    for (int id : ids) {
        int sub = id / labels_per_block;
        int off = id % labels_per_block;
        leaves.push_back(fetched[sub_to_idx[sub]][off]);
    }

    for (int *p : fetched)
        delete[] p;
}

void RecursivePositionMap::write_batch(const std::vector<int> &ids, const std::vector<int> &leaves) {
    if (ids.empty())
        return;

    std::vector<int> sub_blocks;
    std::map<int, int> sub_to_idx;
    for (int id : ids) {
        int sub = id / labels_per_block;
        if (sub_to_idx.find(sub) == sub_to_idx.end()) {
            sub_to_idx[sub] = (int)sub_blocks.size();
            sub_blocks.push_back(sub);
        }
    }

    // Pull each touched sub-block into the posmap ORAM stash and re-randomize
    // its leaf label (the normal Ring ORAM read side effect).
    std::vector<int *> fetched = posmap_oram->batch_multi_access_swap_ro_templated<int>(
        std::vector<OramInterface::Operation>(sub_blocks.size(), OramInterface::Operation::READ),
        sub_blocks,
        std::vector<int *>(sub_blocks.size(), nullptr),
        (int)sub_blocks.size());

    // Build updated payloads per sub-block, then push them back into the stash
    // so the next eviction writes the new contents to the server.
    std::map<int, std::vector<int>> updates;
    for (size_t i = 0; i < ids.size(); i++) {
        int sub = ids[i] / labels_per_block;
        int off = ids[i] % labels_per_block;
        if (updates.find(sub) == updates.end()) {
            int idx = sub_to_idx[sub];
            updates[sub] = std::vector<int>(fetched[idx], fetched[idx] + labels_per_block);
        }
        updates[sub][off] = leaves[i];
    }

    for (int *p : fetched)
        delete[] p;

    for (auto &kv : updates) {
        posmap_oram->update_stash_block(kv.first, kv.second);
    }
}
