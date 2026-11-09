#pragma once

#include <algorithm>
#include <functional>
#include <limits>

#include "sonic/util/profiling.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/threads/sync.hpp"

namespace sn::oram::stash::forestzing {

namespace obliv = sn::obliv;

template <typename Block>
Block scan_subtree_resident_sections(
    stash<Block>& st, std::uint32_t subtree_ix, std::int64_t address, const obliv::choice& gstash_read_dummy,
    obliv::choice& already_found, obliv::choice& found_in_local, obliv::choice& local_hand_full
) {
  sn_prof_zone("forestzing.stash.scan_subtree");
  auto& subtree = st.state.subtrees[subtree_ix];
  Block result = Block::make_dummy(st.state.uid());

  using section = typename subtree_storage<Block>::section;

  // scan subtree-local stash
  // the regions of interest here are
  // - local_deferred: deferred blocks belonging to this subtree
  // - treetop: extra space for evict blocks
  // - overlap_region: extra space for the overlapping levels in the subtree
  auto scan_section = [&](const section& sec) {
    sn_prof_zone("forestzing.stash.scan_section");
    const obliv::choice should_remove = !already_found;
    // use linear storage helper to read and remove from section
    Block candidate = subtree.storage.read(address, should_remove, sec);
    const obliv::choice candidate_is_real = candidate.is_real();
    const obliv::choice take_candidate = candidate_is_real && !local_hand_full && !already_found;

#if defined(ORAM_DEBUG)
    if (candidate_is_real.unwrap()) {
      log::ensure(!found_in_local.unwrap(), "found duplicate matching block");
      log::ensure(gstash_read_dummy.unwrap(), "found block in both global stash and treetop");
      log::ensure(!local_hand_full.unwrap(), "multiple local blocks selected");
    }
#endif

    // conditionally move the candidate block into the local hand
    obliv::ct_set_data(&result, candidate, take_candidate.unwrap());

    found_in_local = found_in_local || candidate_is_real;
    already_found = already_found || candidate_is_real;
    local_hand_full = local_hand_full || take_candidate;
  };

  scan_section(subtree.treetop);
  scan_section(subtree.overlap_region);
  scan_section(subtree.local_deferred);

  return result;
}

template <typename Block, typename ScanFn>
Block scan_subtree_resident_sections_serialized(stash<Block>& st, std::uint32_t subtree_ix, ScanFn&& scan_fn) {
  if (st.cfg.single_thread_access) {
    return scan_fn();
  }

  sn::threads::lock_guard guard(st.state.subtree_extract_mutex(subtree_ix));
  return scan_fn();
}

// add block to lock-free global stash
template <typename Block> void add_global(stash<Block>& st, const Block& block) {
  sn_prof_zone("forestzing.stash.add_global");
  st.state.global_stash->add(&block);
}

template <typename Block> void insert_pathread(stash<Block>& st, const Block& block) {
  sn_prof_zone("forestzing.stash.insert_pathread");
#if defined(ORAM_DEBUG)
  st.state.log.dbgf(
      "insert_pathread: address=$%08x real=%d", static_cast<std::uint64_t>(block.address), block.is_real().unwrap()
  );
#endif
  if (st.cfg.disjoint_epoch_mode) {
    return;
  }
  add_global(st, block);
}

template <typename Block> void insert_pathread_batch(stash<Block>& st, sn::util::span<const Block> blocks) {
  sn_prof_zone("forestzing.stash.insert_pathread_batch");
#if defined(ORAM_DEBUG)
  st.state.log.dbgf("insert_pathread_batch: count=%d", static_cast<std::uint64_t>(blocks.size()));
#endif
  auto& runtime = st.state.runtime;
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(!runtime.snapshot_prefilled, "forestzing::stash: snapshot already prefilled");
#endif

  auto snapshot_span = runtime.snapshot_span();
  sn::util::log::ensure(
      blocks.size() <= snapshot_span.size(), "forestzing::stash: batch size exceeds snapshot capacity"
  );

  sn::obliv::copy(blocks.begin(), blocks.end(), snapshot_span.begin());
  for (std::size_t ix = blocks.size(); ix < snapshot_span.size(); ++ix) {
    snapshot_span[ix].set_dummy(st.state.uid());
  }

  runtime.snapshot_count = blocks.size();
  runtime.snapshot_prefilled = true;
}

// extract block from stash by address
template <typename Block> Block extract(stash<Block>& st, std::int64_t address, std::uint32_t subtree_ix) {
  sn_prof_zone("forestzing.stash.extract");

  // first, we will check the global stash
  // the global stash contains pathreads from this epoch
  // then we will check all the subtree stashes

  Block out_block = Block::make_dummy(st.state.uid());

#if defined(ORAM_DEBUG)
  st.state.log.dbgf("extract: address=$%08x, subtree_ix=%d", static_cast<std::uint64_t>(address), subtree_ix);
#endif

  obliv::choice gstash_read_dummy = obliv::choice::true_value();
  if (!st.cfg.disjoint_epoch_mode) {
    // read and remove from global stash
    st.state.global_stash->read(&out_block, address, obliv::choice(true));
    // whether we got a dummy block out of the gstash (wasn't there)
    gstash_read_dummy = !out_block.is_real();
  }

  // check the subtree stashes
  Block local_stash_out_block = Block::make_dummy(st.state.uid());
  obliv::choice found_in_local_stash = obliv::choice::false_value();
  obliv::choice already_found = !gstash_read_dummy;
  obliv::choice local_hand_full = obliv::choice::false_value();

#if defined(ORAM_DEBUG)
  const std::uint32_t subtree_count = st.cfg.tree.subtree_count;
  log::ensure(subtree_ix < subtree_count, "extract: subtree index out of range");
#endif

  auto scan_local = [&]() {
    return scan_subtree_resident_sections(
        st, subtree_ix, address, gstash_read_dummy, already_found, found_in_local_stash, local_hand_full
    );
  };

  Block candidate = scan_subtree_resident_sections_serialized(st, subtree_ix, scan_local);
  // consolidate the per-subtree candidate into the shared hand
  const obliv::choice take_candidate = candidate.is_real();
  obliv::ct_set_data(&local_stash_out_block, candidate, take_candidate.unwrap());
#if defined(ORAM_DEBUG)
  if (take_candidate.unwrap()) {
    log::ensure(local_hand_full.unwrap(), "extract: candidate real but local_hand_full not set by scan");
  }
#endif

#if defined(ORAM_DEBUG)
  if (local_hand_full.unwrap()) {
    log::ensure(gstash_read_dummy.unwrap(), "local and global stash both provided real block");
  }
#endif

  // if we read a dummy from the stash, copy local stash found (which could be dummy or real)
  obliv::ct_set_data(&out_block, local_stash_out_block, gstash_read_dummy.unwrap());

#if defined(ORAM_DEBUG)
  const char* origin = "dummy";
  if (!gstash_read_dummy.unwrap()) {
    origin = "global";
  } else if (local_hand_full.unwrap()) {
    origin = "local";
  }
  st.state.log.dbgf(
      "extract: address=$%08x result_origin=%s real=%d", static_cast<std::uint64_t>(address), origin,
      out_block.is_real().unwrap()
  );
#endif

  return out_block;
}

} // namespace sn::oram::stash::forestzing
