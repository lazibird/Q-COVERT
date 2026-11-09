#pragma once
#include "qcovert/encrypted_store.hpp"
#include "qcovert/quantization.hpp"
#include "qcovert/snapshot.hpp"
#include "qcovert/topk.hpp"
#include "ringdoram/oram.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <memory>
namespace qcovert {
// evict_batch/routing_depth apply to the quantized (Qu) ORAM; fu_evict_batch/
// fu_routing_depth override them for the full-vector (Fu) ORAM.
struct oram_parameters {
  std::uint32_t z = 8, s = 16, a = 4, evict_batch = 1;
  std::uint64_t stash = 0;
  std::uint32_t routing_depth = 0;
  std::uint32_t fu_evict_batch = 1, fu_routing_depth = 0;
};
// Non-reentrant: the owner serializes setup/query/finish. Workers may execute
// disjoint ReadPaths concurrently inside a single query.
template <std::size_t D, class Scalar = float, std::size_t B = 8> class index {
  static_assert(B > 0);

public:
  using L = layout<D, Scalar>;
  using dataset = typename L::dataset;
  using vector = typename L::vector;
  using QE = typename L::quantized_entry;
  using FB = typename L::full_block;
  struct alignas(8) QB {
    std::array<QE, B> entries{};
  };
  using QC = ringdoram::client<
      ringdoram::traits<sizeof(QB), ringdoram::epoch_mode::disjoint_epoch, encrypted_store>>;
  using FC = ringdoram::client<
      ringdoram::traits<sizeof(FB), ringdoram::epoch_mode::disjoint_epoch, encrypted_store>>;

private:
  struct CM {
    typename L::centroid c{};
    std::uint64_t cid = invalid_id, counter = 0;
    double score = infinity;
  };
  struct candidate {
    QE entry{};
    double score = infinity;
  };
  struct placement {
    QE entry{};
    std::uint64_t cid = invalid_id, position = invalid_id;
  };
  std::vector<CM> centroids_;
  compact_topk_workspace<CM> centroid_workspace_;
  // The optimized fixed-size rotation uses packed SSE memory operands. Keep
  // the matrix aligned even when it follows the 24-byte centroid vector.
  alignas(16) std::array<float, D * L::padded> rotation_{};
  std::array<float, L::padded> offsets_{};
  std::size_t n_ = 0, nb_ = 0;
  query_options query_;
  key128 leaf_key_{};
  std::unique_ptr<QC> qu_;
  std::unique_ptr<FC> fu_;
  ringdoram::batch_workspace<QC> qw_;
  ringdoram::batch_workspace<FC> fw_;
  sn::threads::thread_team &workers_;
  ringdoram::obliv::secure_random random_;
  bool failed_ = false;
  bool pristine_ = true;
  query_stats stats_{};
  measurement *measurement_ = nullptr;
  void attach_counters() {
    if (!measurement_) return;
    for (auto &bucket : qu_->state_ref().storage().data())
      if (bucket.configured()) bucket.block_store().attach_counters(measurement_->qu_storage);
    for (auto &bucket : fu_->state_ref().storage().data())
      if (bucket.configured()) bucket.block_store().attach_counters(measurement_->fu_storage);
  }
  static std::size_t multiply(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b)
      throw std::length_error("Q-COVERT size overflow");
    return a * b;
  }
  static bool safe_float(float x) { return std::isfinite(x) & (std::abs(x) <= 1.e10f); }
  std::int64_t random_leaf(std::uint64_t leaves) {
    std::uint64_t x;
    random_.random_bytes(reinterpret_cast<std::uint8_t *>(&x), sizeof(x));
    return x & (leaves - 1);
  }
  std::int64_t qu_leaf(std::uint64_t cid, std::uint64_t ctr, std::uint64_t offset) {
    return prf(leaf_key_, cid, ctr, offset) & (qu_->shape().leaf_count - 1);
  }
  static std::uint64_t ordered_score(double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    // Canonicalize -0 and +0; no NaNs reach a valid query network.
    bits = ringdoram::obliv::OSelect(std::uint64_t{0}, bits, (bits << 1) == 0);
    return bits ^ ((std::uint64_t{0} - (bits >> 63)) | (std::uint64_t{1} << 63));
  }
  static bool less(double a, std::uint64_t aid, double b, std::uint64_t bid) {
    const auto ka = ordered_score(a), kb = ordered_score(b);
    return sn::obliv::ct_lt(ka, kb) | (sn::obliv::ct_eq(ka, kb) & sn::obliv::ct_lt(aid, bid));
  }
  ringdoram::options options(std::size_t n, std::size_t reads, const oram_parameters &p) {
    ringdoram::options o{};
    o.block_count = std::max(n, std::size_t{2});
    o.bucket_real_size = p.z;
    o.bucket_dummy_size = p.s;
    o.eviction_rate = p.a;
    o.evict_batch = p.evict_batch;
    o.stash_bound = p.stash;
    if (p.routing_depth >= 32)
      throw std::invalid_argument("ORAM routing depth exceeds subtree count representation");
    o.routing_depth = p.routing_depth;
    o.evict_backend = ringdoram::eviction_backend::shared_top;
    o.access_concurrency = workers_.logical_threads();
    const auto chunk = multiply(multiply(p.a, p.evict_batch), std::size_t{1} << p.routing_depth);
    if (chunk == 0 || reads > std::numeric_limits<std::size_t>::max() - chunk)
      throw std::invalid_argument("invalid ORAM batch");
    o.disjoint_epoch_window = ((reads + chunk - 1) / chunk) * chunk;
    return o;
  }
  sn::threads::thread_team eviction_team(const oram_parameters &p) {
    if (p.routing_depth >= 32)
      throw std::invalid_argument("invalid ORAM routing depth");
    // Both the worker count and subtree count are powers of two, so the
    // public subtree partition always divides evenly, even for odd access T.
    const auto count = std::min<std::size_t>(std::size_t{1} << p.routing_depth, workers_.logical_threads());
    return workers_.limited_to(std::bit_floor(count));
  }
  template <class Client, class Payload>
  void read_batch(Client &client, const std::vector<std::int64_t> &addresses,
                  const std::vector<std::int64_t> &oldleaf, const std::vector<std::int64_t> &newleaf,
                  std::vector<Payload> &outputs, ringdoram::batch_workspace<Client> &workspace) {
    std::vector<std::array<std::uint8_t, sizeof(Payload)>> in(addresses.size()), out(addresses.size());
    std::vector<ringdoram::access_request> requests(addresses.size());
    for (std::size_t i = 0; i < requests.size(); ++i)
      requests[i] = {addresses[i],
                     oldleaf[i],
                     newleaf[i],
                     false,
                     {in[i].data(), in[i].size()},
                     {out[i].data(), out[i].size()}};
    ringdoram::AccessBatch(client, requests, workspace, workers_);
    outputs.resize(requests.size());
    for (std::size_t i = 0; i < outputs.size(); ++i)
      outputs[i] = from_bytes<Payload>(out[i]);
  }

public:
  index(const dataset &data, query_options q, sn::threads::thread_team &workers, oram_parameters p = {},
        measurement *measured = nullptr, snapshot::reader *image = nullptr)
      : n_(data.entries.size()), query_(q), workers_(workers), measurement_(measured) {
    if (measured && !measured->clock)
      throw std::invalid_argument("measurement clock missing");
#if !defined(SONIC_ORAM_METRICS)
    if (measured)
      throw std::invalid_argument("benchmark requires SONIC_ORAM_METRICS");
#endif
    const auto setup_begin = measured ? measured->clock() : 0;
    using ringdoram::obliv::OSelect;
    const auto nc = data.centroids.size();
    if (!n_ || !nc || !data.tau || data.tau > std::numeric_limits<std::size_t>::max() - B || !q.nprobe ||
        q.nprobe > nc || !q.k || q.k > q.k2)
      throw std::invalid_argument("invalid dataset/query dimensions");
    nb_ = (data.tau + B - 1) / B;
    const auto region = multiply(nb_, B), capacity = multiply(nc, region), qr = multiply(q.nprobe, nb_);
    if (n_ > capacity || q.k2 > multiply(qr, B) ||
        capacity > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
      throw std::invalid_argument("dataset/candidate capacity exceeded");
    if (!q.rank_partitions.empty()) {
      if (q.rank_partitions.size() != q.nprobe)
        throw std::invalid_argument("one partition quota is required per probed-cluster rank");
      std::size_t representatives = 0;
      for (auto quota : q.rank_partitions) {
        if (quota > region || quota > std::numeric_limits<std::size_t>::max() - representatives)
          throw std::invalid_argument("partition quota exceeds cluster capacity");
        representatives += quota;
      }
      if (representatives < q.k2)
        throw std::invalid_argument("partition representatives fewer than k2");
    }
    const auto progress_step = std::max<std::size_t>(1, n_ / 100);
    const auto progress = [&](const char *stage, std::size_t completed, std::size_t total) {
      if (measured)
        measured->report_progress(stage, completed, total);
    };
    bool invalid = false;
    progress("setup/validate_vectors", 0, n_);
    for (std::size_t i = 0; i < n_; ++i) {
      const auto &e = data.entries[i];
      invalid |= e.cid >= nc;
      for (auto x : e.values)
        invalid |= !safe_float(float(x));
      invalid |= (!safe_float(e.quantized.norm)) | (!safe_float(e.quantized.x0)) | (e.quantized.norm < 0) |
                 (e.quantized.x0 < 0) | (e.quantized.x0 > 1.0001f) |
                 ((e.quantized.norm > 0) & (e.quantized.x0 < 0.5f / std::sqrt(float(L::padded))));
      if ((i + 1) % progress_step == 0 || i + 1 == n_)
        progress("setup/validate_vectors", i + 1, n_);
    }
    for (const auto &c : data.centroids) {
      for (auto x : c.values)
        invalid |= !safe_float(x);
      for (auto x : c.rotated)
        invalid |= !safe_float(x);
      invalid |= c.valid > 1;
    }
    for (auto x : data.rotation)
      invalid |= (!std::isfinite(x)) | (std::abs(x) > 1.0001f);
    for (auto x : data.offsets)
      invalid |= (!std::isfinite(x)) | (x < 0) | (x >= 1);
    if (invalid)
      throw std::invalid_argument("malformed/nonfinite prepared dataset");
    std::copy(data.rotation.begin(), data.rotation.end(), rotation_.begin());
    offsets_ = data.offsets;
    centroids_.resize(nc);
    for (std::size_t i = 0; i < nc; ++i) {
      centroids_[i].c = data.centroids[i];
      centroids_[i].cid = i;
    }
    oram_parameters fp = p;
    fp.evict_batch = p.fu_evict_batch;
    fp.routing_depth = p.fu_routing_depth;
    qu_ = std::make_unique<QC>(options(nc * nb_, qr, p), eviction_team(p));
    fu_ = std::make_unique<FC>(options(n_, q.k2, fp), eviction_team(fp));
    if (image) {
      const std::array<std::uint64_t, 17> expected{
          snapshot::index_format_version, n_, nc, data.tau, D, B, sizeof(Scalar),
          sizeof(FB), sizeof(QB), p.z, p.s, p.a, p.evict_batch, p.routing_depth, p.fu_evict_batch,
          p.fu_routing_depth, p.stash};
      std::array<std::uint64_t, 17> found{};
      image->read(found.data(), sizeof(found));
      if (found != expected)
        throw std::invalid_argument("ORAM snapshot parameters mismatch");
      image->read(leaf_key_.data(), leaf_key_.size());
      progress("setup/restore_fu", 0, 1);
      const auto fu_begin = measured ? measured->clock() : 0;
      fu_->load_initial(*image);
      if (measured) measured->fu_setup_ns = measured->clock() - fu_begin;
      progress("setup/restore_fu", 1, 1);
      progress("setup/restore_qu", 0, 1);
      const auto qu_begin = measured ? measured->clock() : 0;
      qu_->load_initial(*image);
      if (measured) measured->qu_setup_ns = measured->clock() - qu_begin;
      progress("setup/restore_qu", 1, 1);
      image->finish();
      attach_counters();
      progress("setup/query_workspace", 0, 1);
      qw_.configure(*qu_, qr);
      fw_.configure(*fu_, q.k2);
      progress("setup/query_workspace", 1, 1);
      if (measured) measured->setup_ns = measured->clock() - setup_begin;
      return;
    }
    random_.random_bytes(leaf_key_.data(), leaf_key_.size());
    progress("setup/prepare_records", 0, n_);
    std::vector<ringdoram::bulk_record<sizeof(FB)>> full(n_);
    std::vector<placement> placed(padded_size(capacity));
    for (std::size_t i = 0; i < n_; ++i) {
      const auto leaf = random_leaf(fu_->shape().leaf_count);
      FB fb{};
      fb.values = data.entries[i].values;
      full[i] = {static_cast<std::int64_t>(i), leaf, bytes(fb)};
      placed[i] = {{i, leaf, data.entries[i].quantized}, data.entries[i].cid, invalid_id};
      if ((i + 1) % progress_step == 0 || i + 1 == n_)
        progress("setup/prepare_records", i + 1, n_);
    }
    {
      progress("setup/fu_bulk_load", 0, 1);
      const auto start = measured ? measured->clock() : 0;
      fu_->bulk_setup(full);
      if (measured)
        measured->fu_setup_ns = measured->clock() - start;
      progress("setup/fu_bulk_load", 1, 1);
    }
    const auto qu_setup_begin = measured ? measured->clock() : 0;
    progress("setup/qu_sort", 0, 1);
    ringdoram::obliv::OSort(
        std::span<placement>(placed), [](const placement &p) { return p.cid; },
        [](std::uint64_t a, std::uint64_t b) { return sn::obliv::ct_lt(a, b); });
    progress("setup/qu_sort", 1, 1);
    std::uint64_t previous = invalid_id, rank = 0;
    invalid = false;
    for (auto &x : placed) {
      rank = OSelect(rank + 1, std::uint64_t{0}, x.cid == previous);
      previous = x.cid;
      invalid |= (x.entry.id != invalid_id) & (rank >= data.tau);
      x.position = OSelect(x.cid * region + rank, invalid_id, x.entry.id != invalid_id);
    }
    if (invalid)
      throw std::invalid_argument("cluster exceeds tau");
    progress("setup/qu_place", 0, 1);
    ringdoram::obliv::OPlace(
        std::span<placement>(placed), [](const placement &p) { return p.position; }, invalid_id);
    progress("setup/qu_place", 1, 1);
    ringdoram::obliv::shuffle_scratch scratch;
    {
      progress("setup/qu_cluster_shuffle", 0, nc);
      for (std::size_t cid = 0; cid < nc; ++cid) {
        // A fixed shuffle includes padded entries. Otherwise adjacent entries
        // from the offline cluster order would share the same partition.
        ringdoram::obliv::OShuffleBitonic(std::span<placement>(placed.data() + cid * region, region),
                                         scratch, random_);
        progress("setup/qu_cluster_shuffle", cid + 1, nc);
      }
    }
    progress("setup/qu_pack", 0, nc * nb_);
    std::vector<ringdoram::bulk_record<sizeof(QB)>> quantized(nc * nb_);
    const auto pack_step = std::max<std::size_t>(1, quantized.size() / 100);
    for (std::size_t cid = 0; cid < nc; ++cid)
      for (std::size_t off = 0; off < nb_; ++off) {
        QB block{};
        for (std::size_t j = 0; j < B; ++j)
          block.entries[j] = placed[(cid * nb_ + off) * B + j].entry;
        quantized[cid * nb_ + off] = {static_cast<std::int64_t>(cid * nb_ + off), qu_leaf(cid, 0, off),
                                      bytes(block)};
        const auto completed = cid * nb_ + off + 1;
        if (completed % pack_step == 0 || completed == quantized.size())
          progress("setup/qu_pack", completed, quantized.size());
      }
    progress("setup/qu_shuffle", 0, 1);
    ringdoram::obliv::OShuffleBitonic(std::span(quantized), scratch, random_);
    progress("setup/qu_shuffle", 1, 1);
    progress("setup/qu_bulk_load", 0, 1);
    qu_->bulk_setup(quantized);
    progress("setup/qu_bulk_load", 1, 1);
    if (measured)
      measured->qu_setup_ns = measured->clock() - qu_setup_begin;
    attach_counters();
    progress("setup/query_workspace", 0, 1);
    qw_.configure(*qu_, qr);
    fw_.configure(*fu_, q.k2);
    progress("setup/query_workspace", 1, 1);
    if (measured)
      measured->setup_ns = measured->clock() - setup_begin;
  }
  index(const index &) = delete;
  index &operator=(const index &) = delete;
  // Re-point this index at new query options (nprobe/k2/partitions) without
  // touching the ORAMs; client windows were sized for the largest job at setup.
  void configure_query(query_options q) {
    const auto region = multiply(nb_, B), qr = multiply(q.nprobe, nb_);
    if (!q.nprobe || q.nprobe > centroids_.size() || !q.k || q.k > q.k2 || q.k2 > multiply(qr, B))
      throw std::invalid_argument("invalid query dimensions");
    if (!q.rank_partitions.empty()) {
      if (q.rank_partitions.size() != q.nprobe)
        throw std::invalid_argument("one partition quota is required per probed-cluster rank");
      std::size_t representatives = 0;
      for (auto quota : q.rank_partitions) {
        if (quota > region || quota > std::numeric_limits<std::size_t>::max() - representatives)
          throw std::invalid_argument("partition quota exceeds cluster capacity");
        representatives += quota;
      }
      if (representatives < q.k2)
        throw std::invalid_argument("partition representatives fewer than k2");
    }
    query_ = std::move(q);
    qw_.configure(*qu_, qr);
    fw_.configure(*fu_, query_.k2);
  }
  void save_initial(snapshot::writer &image, std::uint64_t tau) const {
    if (!pristine_ || failed_ || qu_->pending_epoch_accesses() || fu_->pending_epoch_accesses())
      throw std::logic_error("ORAM snapshot requires idle initial index");
    const std::array<std::uint64_t, 17> header{
        snapshot::index_format_version, n_, centroids_.size(), tau, D, B, sizeof(Scalar),
        sizeof(FB), sizeof(QB),
        fu_->options().bucket_real_size, fu_->options().bucket_dummy_size, fu_->options().eviction_rate,
        qu_->options().evict_batch, qu_->options().routing_depth,
        fu_->options().evict_batch, fu_->options().routing_depth, fu_->options().stash_bound};
    image.write(header.data(), sizeof(header));
    image.write(leaf_key_.data(), leaf_key_.size());
    fu_->save_initial(image);
    qu_->save_initial(image);
  }
  void finish() {
    if (failed_)
      throw std::logic_error("index unusable after failed query");
    failed_ = true;
    auto *measured = measurement_ && measurement_->active ? measurement_ : nullptr;
    {
      phase_timer timer(measured, phase::qu_evict);
      qu_->flush_epoch();
    }
    {
      phase_timer timer(measured, phase::fu_evict);
      fu_->flush_epoch();
    }
    if (measured) {
      measured->stop_query();
      auto qm = qu_->stash_metrics_snapshot(), fm = fu_->stash_metrics_snapshot();
      measured->query.qu_stash_peak = qm.maxima.subtree_stash_deferred_real_max;
      measured->query.fu_stash_peak = fm.maxima.subtree_stash_deferred_real_max;
      measured->query.qu_stash_total_peak = qm.maxima.subtree_stash_real_max;
      measured->query.fu_stash_total_peak = fm.maxima.subtree_stash_real_max;
      measured->query.qu_stash_capacity = qm.subtree_stash_overflow_capacity * qu_->shape().subtree_count;
      measured->query.fu_stash_capacity = fm.subtree_stash_overflow_capacity * fu_->shape().subtree_count;
    }
    failed_ = false;
  }
  query_stats stats() const noexcept { return stats_; }
  const QC &quantized_oram() const noexcept { return *qu_; }
  const FC &full_oram() const noexcept { return *fu_; }
  std::vector<result> search(const vector &query, bool defer_eviction = false) {
    using ringdoram::obliv::OSelect;
    bool invalid = false;
    for (auto x : query)
      invalid |= !safe_float(float(x));
    if (invalid)
      throw std::invalid_argument("query must be finite and within coordinate bound");
    pristine_ = false;
    finish();
    if (measurement_) {
      qu_->reset_stash_metrics();
      fu_->reset_stash_metrics();
#if defined(SONIC_ORAM_METRICS)
      auto &qs = qu_->state_ref().stash().state;
      auto &fs = fu_->state_ref().stash().state;
      sn::oram::stash::forestzing::pipeline::observe_forest_stash_sections(qs);
      sn::oram::stash::forestzing::pipeline::observe_forest_stash_sections(fs);
#endif
      measurement_->start_query();
    }
    failed_ = true;
    auto selected = centroids_;
    {
      phase_timer timer(measurement_, phase::centroid_distance);
      for (auto &c : selected) {
        c.score = 0;
        for (std::size_t d = 0; d < D; ++d) {
          const double delta = double(query[d]) - c.c.values[d];
          c.score += delta * delta;
        }
        c.score = OSelect(c.score, infinity, c.c.valid != 0);
      }
    }
    {
      phase_timer timer(measurement_, phase::centroid_topk);
      exact_topk_compact(selected, query_.nprobe, CM{},
                        [](const CM &c) { return std::array<std::uint64_t, 2>{ordered_score(c.score), c.cid}; },
                        [](const auto &a, const auto &b) {
                          return sn::obliv::ct_lt(a[0], b[0]) |
                                 (sn::obliv::ct_eq(a[0], b[0]) & sn::obliv::ct_lt(a[1], b[1]));
                        }, centroid_workspace_);
    }
    if (measurement_)
      for (const auto &c : selected)
        measurement_->selected_clusters.push_back(c.cid);
    bool exhausted = false;
    for (const auto &c : selected)
      exhausted |= c.counter == invalid_id;
    if (exhausted)
      throw std::overflow_error("cluster counter exhausted");
    const auto qr = query_.nprobe * nb_;
    std::vector<std::int64_t> addresses(qr), oldleaf(qr), newleaf(qr);
    for (std::size_t i = 0; i < query_.nprobe; ++i)
      for (std::size_t off = 0; off < nb_; ++off) {
        const auto pos = i * nb_ + off;
        const auto &c = selected[i];
        addresses[pos] = c.cid * nb_ + off;
        oldleaf[pos] = qu_leaf(c.cid, c.counter, off);
        newleaf[pos] = qu_leaf(c.cid, c.counter + 1, off);
      }
    std::vector<QB> blocks;
    {
      phase_timer timer(measurement_, phase::qu_read);
      read_batch(*qu_, addresses, oldleaf, newleaf, blocks, qw_);
    }
    if (measurement_)
      measurement_->query.qu_reshuffles =
          measurement_->qu_storage.rebuilds.load() - measurement_->traffic_start[4];
    for (auto &c : centroids_)
      for (const auto &chosen : selected)
        c.counter += std::uint64_t(c.cid == chosen.cid);
    std::array<float, L::padded> rotated{};
    {
      phase_timer timer(measurement_, phase::query_rotation);
      for (std::size_t j = 0; j < L::padded; ++j)
        for (std::size_t d = 0; d < D; ++d)
          rotated[j] += float(query[d]) * rotation_[d * L::padded + j];
    }
    std::vector<candidate> candidates(qr * B);
    {
      phase_timer timer(measurement_, phase::quantized_distance);
      for (std::size_t i = 0; i < query_.nprobe; ++i) {
        const auto qq = prepare_query<L>(rotated, selected[i].c, offsets_);
        for (std::size_t off = 0; off < nb_; ++off)
          for (std::size_t j = 0; j < B; ++j) {
            const auto pos = (i * nb_ + off) * B + j;
            auto e = blocks[i * nb_ + off].entries[j];
            const double score = estimate<L>(qq, e.quantized);
            candidates[pos] = {e, OSelect(score, infinity, e.id != invalid_id)};
          }
      }
    }
    if (measurement_)
      for (const auto &candidate : candidates)
        measurement_->query.valid_quantized += candidate.entry.id != invalid_id;
    {
      phase_timer timer(measurement_, phase::candidate_topk);
      const auto candidate_less = [](const candidate &a, const candidate &b) {
        return less(a.score, a.entry.id, b.score, b.entry.id);
      };
      if (query_.rank_partitions.empty())
        exact_topk(candidates, query_.k2, candidate{}, candidate_less);
      else
        partition_topk(candidates, query_.k2, nb_ * B, query_.rank_partitions, candidate{}, candidate_less);
    }
    if (measurement_)
      for (const auto &c : candidates)
        measurement_->selected_candidates.push_back(c.entry.id);
    addresses.resize(query_.k2);
    oldleaf.resize(query_.k2);
    newleaf.resize(query_.k2);
    for (std::size_t i = 0; i < query_.k2; ++i) {
      const auto &e = candidates[i].entry;
      const auto dummy = random_leaf(fu_->shape().leaf_count);
      addresses[i] = OSelect(static_cast<std::int64_t>(e.id), std::int64_t{-1}, e.id != invalid_id);
      oldleaf[i] = OSelect(e.leaf, dummy, e.id != invalid_id);
      newleaf[i] = random_leaf(fu_->shape().leaf_count);
    }
    std::vector<FB> full;
    {
      phase_timer timer(measurement_, phase::fu_read);
      read_batch(*fu_, addresses, oldleaf, newleaf, full, fw_);
    }
    if (measurement_)
      measurement_->query.fu_reshuffles =
          measurement_->fu_storage.rebuilds.load() - measurement_->traffic_start[5];
    // Mutate the actual deferred QuORAM blocks. Slot order is deliberately not
    // assumed to match concurrent request order; visit every entry/candidate.
    {
      phase_timer timer(measurement_, phase::fu_leaf_update);
      qu_->mutate_pending_payloads([&](auto &payload) {
        QB block = from_bytes<QB>(payload);
        for (auto &e : block.entries)
          for (std::size_t i = 0; i < candidates.size(); ++i)
            e.leaf = OSelect(newleaf[i], e.leaf, (e.id == candidates[i].entry.id) & (e.id != invalid_id));
        payload = bytes(block);
      });
    }
    std::vector<result> results(query_.k2);
    {
      phase_timer timer(measurement_, phase::exact_distance);
      for (std::size_t i = 0; i < query_.k2; ++i) {
        double distance = 0;
        for (std::size_t d = 0; d < D; ++d) {
          const double delta = double(query[d]) - double(full[i].values[d]);
          distance += delta * delta;
        }
        results[i] = {candidates[i].entry.id,
                      OSelect(distance, infinity, candidates[i].entry.id != invalid_id)};
      }
    }
    {
      phase_timer timer(measurement_, phase::rerank_topk);
      exact_topk(results, query_.k, result{},
                 [](const result &a, const result &b) { return less(a.distance, a.id, b.distance, b.id); });
    }
    stats_ = {qr, query_.k2, qr * B};
    if (measurement_) {
      measurement_->query.online_ns = measurement_->clock() - measurement_->begin;
      measurement_->query.qu_pending_slots = qu_->pending_epoch_accesses();
      measurement_->query.fu_pending_slots = fu_->pending_epoch_accesses();
      for (const auto &candidate : candidates)
        measurement_->query.fu_pending_real += candidate.entry.id != invalid_id;
    }
    failed_ = false;
    if (!defer_eviction)
      finish();
    return results;
  }
};
} // namespace qcovert
