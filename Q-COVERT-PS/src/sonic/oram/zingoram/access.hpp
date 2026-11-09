#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/struct_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/span.hpp"

#include "sonic/oram/core/access_ops.hpp"
#include "sonic/oram/tree/path_buffer.hpp"

#include "sonic/oram/zingoram/access_bucket.hpp"
#include "sonic/oram/zingoram/state.hpp"

namespace sn::oram::zingoram {

namespace fz_stash = sn::oram::stash::forestzing;

template <typename Traits>
typename Traits::block_t read_path(
    state<Traits>& st, std::uint64_t leaf_ix, const sn::oram::access_request& req,
    typename state<Traits>::access_scratch& scratch
) {
  sn_prof_zone("zingoram.path.read");
  using block_t = typename Traits::block_t;

  auto& topo = st.topology();
  auto& storage = st.storage();
  auto& uid_gen = st.uid_gen();
  auto& prng = st.prng();
  auto& geom = st.shape();
  const auto& derived = st.derived();
  const auto mode = derived.single_thread_access ? bucket_access_mode::serial : bucket_access_mode::concurrent;
  auto& epochs = st.epoch_states();

  auto& buffer = scratch.path;
  auto view = buffer.view();
  auto node_ids = view.node_ids();
  topo.path_to_leaf(leaf_ix, node_ids);

  // storage prefetch hint
  for (std::size_t i = 0; i < node_ids.size(); ++i) {
    storage[node_ids[i]].prefetch();
  }

#if defined(ORAM_DEBUG)
  st.log().dbgf(
      "zingoram::read_path: reading path to leaf_ix=%d (address=$%08x), nodes=%s", leaf_ix, req.address,
      sn::util::format::format_vec(node_ids)
  );
#endif

  // we expect at most one real block along the path
  block_t hand = block_t::make_dummy(uid_gen);

  // read buckets from leaf to root
  // skip "nonexistent" buckets above routing depth
  std::int64_t start_depth = static_cast<std::int64_t>(geom.routing_depth);
  for (std::int64_t depth = static_cast<std::int64_t>(geom.height); depth >= start_depth; --depth) {
    const std::uint64_t node_id = node_ids[static_cast<std::size_t>(depth)];
    auto& bucket = storage[node_id];
    auto& epoch = epochs[node_id];

    // guarded access to bucket
    auto [bucket_block, block_is_real] =
        guarded_bucket_access<Traits>(bucket, epoch, scratch, uid_gen, prng, req.address, mode);

    // take block if it's real and our hand is empty
    auto have_hand = hand.is_real();
    auto take_block = block_is_real && !have_hand;
    sn::obliv::ct_set_data(&hand, bucket_block, take_block.unwrap());

#if defined(ORAM_DEBUG)
    // ensure at most one real block is found
    if (have_hand.unwrap() && block_is_real.unwrap()) {
      sn::util::log::failf(
          "zingoram::read_path: multiple real blocks found on path to leaf_ix=%d: existing hand block#%d "
          "(addr=$%08x leaf=%d), bucket block#%d (addr=$%08x leaf=%d)",
          leaf_ix, hand.uid, hand.address, hand.leaf_ix, bucket_block.uid, bucket_block.address, bucket_block.leaf_ix
      );
    }
#endif
  }

#if defined(ORAM_DEBUG)
  std::ostringstream hand_ss;
  if (hand.is_real().unwrap()) {
    hand_ss << pfm::format("real block#%d (addr=$%08x leaf=%d)", hand.uid, hand.address, hand.leaf_ix);
  } else {
    hand_ss << pfm::format("dummy block#%d", hand.uid);
  }
  st.log().dbgf(
      "zingoram::read_path: fetched path leaf_ix=%d, address=$%08x: got %s", leaf_ix, req.address, hand_ss.str()
  );
#endif

  // if there was a real block, it is now in hand
  return hand;
}

template <typename Traits, typename Mutator>
typename Traits::block_t access_path(
    state<Traits>& st, const sn::oram::access_request& req, typename state<Traits>::access_scratch& scratch,
    Mutator&& mutator
) {
  sn_prof_zone("zingoram.path.access");
#if defined(ORAM_DEBUG)
  st.log().trcf(
      "access_path: %s($%08x) leaf_ix=%d new_leaf_ix=%d", req.is_write ? "write" : "read", req.address, req.cur_leaf,
      req.new_leaf
  );
#endif

  const std::uint64_t bucket_size = st.bucket_total_size();

  auto& stash = st.stash();
  auto& uid_gen = st.uid_gen();
  auto& ctx = scratch;
  auto& buffer = ctx.path;
  using block_t = typename Traits::block_t;
  const sn::obliv::choice request_is_real = sn::obliv::choice(req.address >= 0);

#if defined(ORAM_DEBUG)
  const auto& shape = st.shape();
  sn::oram::validate_access<block_t>(
      static_cast<std::int64_t>(st.options().block_count), static_cast<std::int64_t>(shape.leaf_count), req
  );
  sn::util::log::ensure(
      buffer.height() == shape.height && buffer.bucket_size() == bucket_size,
      "zingoram::access_path: access buffer shape mismatch"
  );
#endif

  auto path_block = read_path<Traits>(st, static_cast<std::uint64_t>(req.cur_leaf), req, ctx);

  // determine which subtree this path enters
  const auto subtree_ix =
      static_cast<std::uint32_t>(st.forest_topology().leaf_ix_to_subtree_ix(static_cast<std::uint64_t>(req.cur_leaf)));

  // try to extract matching block from stash
  block_t block;
  {
    sn_prof_zone("zingoram.access.extract");
    block = fz_stash::extract(stash, req.address, subtree_ix);
  }
  const sn::obliv::choice stash_had_dummy = block.is_dummy();
  const sn::obliv::choice path_had_dummy = path_block.is_dummy();

  // if not found in stash, the path read must have the target block (or it's unmaterialized)
  sn::obliv::ct_set_data(&block, path_block, stash_had_dummy.unwrap());

#if defined(ORAM_DEBUG)
  const sn::obliv::choice both_real = (!stash_had_dummy) && (!path_had_dummy);
  if (both_real.unwrap()) {
    sn::util::log::failf(
        "zingoram::access_path: block found in both stash and path: stash block#%d (addr=$%08x leaf=%d), path "
        "block#%d (addr=$%08x leaf=%d)",
        block.uid, block.address, block.leaf_ix, path_block.uid, path_block.address, path_block.leaf_ix
    );
  }
#endif

  const sn::obliv::choice need_materialize = request_is_real && stash_had_dummy && path_had_dummy;

#if defined(ORAM_DEBUG)
  if (request_is_real.unwrap()) {
    sn::oram::access_debug::verify_materialization_consistency(
        st.assigned_blocks(), static_cast<std::uint64_t>(req.address), need_materialize, "zingoram::access_path"
    );
  }
#endif

  {
    sn_prof_zone("zingoram.path.materialize");
    sn::oram::materialize_block_cond(block, req.address, uid_gen, need_materialize);
  }

#if defined(ORAM_DEBUG)
  if (request_is_real.unwrap()) {
    sn::oram::access_debug::record_materialization(
        st.assigned_blocks(), static_cast<std::uint64_t>(req.address), block, "zingoram::access_path"
    );
  }
#endif

  {
    sn_prof_zone("zingoram.path.apply");
    std::forward<Mutator>(mutator)(block, req);
    block.leaf_ix = req.new_leaf;
  }
  return block;
}

} // namespace sn::oram::zingoram
