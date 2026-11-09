#include "node.h"

int OptNode::n_neighbors = -1;
int OptNode::dim = -1;


BlockFetcher::BlockFetcher(offset_id_t nb, int max_level)
    : node_to_block(nb, vector<block_id_t>(max_level, 0)) {
}




OptNode* BlockFetcher::get_block(node_id_t node_id, int l){
    assert(0);
    return NULL;
}

vector<OptNode> BlockFetcher::get_blocks(vector<node_id_t> node_ids, vector<int> ls, int pad_l){
    assert(0);
    return {};
}

