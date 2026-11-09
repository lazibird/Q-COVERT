#pragma once
#include "bison/candidate_list.hpp"
#include "qcovert/encrypted_store.hpp"
#include "qcovert/snapshot.hpp"
#include "ringdoram/oram.hpp"
#include <memory>
namespace bison {
// Prepared graph/PQ come from the trusted offline provider. Public
// benchmarks use plaintext inputs; private services authenticate this manifest.
template <class L> class index {
  using node = typename L::node;
  using client = ringdoram::client<ringdoram::traits<sizeof(node),
      ringdoram::epoch_mode::disjoint_epoch, qcovert::encrypted_store>>;
  parameters p_;
  typename L::model book_;
  std::vector<typename L::medoid> medoids_;
  // Preserve physical initial snapshots; these two fields are metadata only.
  std::size_t legacy_cache_count_, legacy_cached_rounds_, maximum_reads_, maximum_width_;
  struct retrieved { std::uint64_t id = invalid_id; typename L::vector embedding{}; };
  std::vector<std::int64_t> positions_;
  std::unique_ptr<client> oram_;
  ringdoram::batch_workspace<client> workspace_;
  sn::threads::thread_team &workers_;
  ringdoram::obliv::secure_random random_;
  measurement *measured_;
  bool failed_ = false, pristine_ = true;
  std::uint64_t now() const { return measured_ ? measured_->clock() : 0; }
  void elapsed(std::uint64_t measurement::*field, std::uint64_t start) {
    if (measured_) measured_->*field += now() - start;
  }
  static void validate(parameters p) {
    if (!p.iterations || !p.beamwidth || !p.k || p.l_search < p.k || p.k > 65536)
      throw std::invalid_argument("invalid BiSON T/W/L/k");
    const auto reads = multiply(p.iterations, p.beamwidth);
    multiply(reads, sizeof(retrieved));
    add(add(p.l_search, multiply(p.beamwidth, L::degree)), reads);
  }
  std::int64_t random_leaf() {
    std::uint64_t x;
    random_.random_bytes(reinterpret_cast<std::uint8_t *>(&x), sizeof(x));
    return static_cast<std::int64_t>(x & (oram_->shape().leaf_count - 1));
  }
  std::array<std::uint64_t, 17> snapshot_header() const {
    const auto &o = oram_->options();
    return {0x3130494e4f534942ULL, 1, qcovert::snapshot::index_format_version, positions_.size(),
        L::dim, L::degree, L::subspaces, sizeof(typename L::scalar), sizeof(node),
        sizeof(typename client::block_t), o.bucket_real_size, o.bucket_dummy_size,
        o.eviction_rate, o.evict_batch, o.stash_bound, legacy_cache_count_, legacy_cached_rounds_};
  }
  std::vector<node> read(std::span<const std::uint64_t> ids) {
    const auto position_begin = now();
    std::vector<std::int64_t> old(ids.size()), fresh(ids.size());
    for (std::size_t lane = 0; lane < ids.size(); ++lane) {
      old[lane] = random_leaf();
      fresh[lane] = random_leaf();
      for (std::size_t i = 0; i < positions_.size(); ++i) {
        const bool match = sn::obliv::ct_eq(ids[lane], std::uint64_t(i));
        old[lane] = ringdoram::obliv::OSelect(positions_[i], old[lane], match);
        positions_[i] = ringdoram::obliv::OSelect(fresh[lane], positions_[i], match);
      }
    }
    elapsed(&measurement::position_ns, position_begin);
    std::vector<std::array<std::uint8_t, sizeof(node)>> in(ids.size()), out(ids.size());
    std::vector<ringdoram::access_request> requests(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i)
      requests[i] = {ringdoram::obliv::OSelect(static_cast<std::int64_t>(ids[i]), std::int64_t{-1},
                          ids[i] != invalid_id), old[i], fresh[i], false,
                          {in[i].data(), in[i].size()}, {out[i].data(), out[i].size()}};
    const auto start = now();
    ringdoram::AccessBatch(*oram_, requests, workspace_, workers_);
    elapsed(&measurement::oram_ns, start);
    std::vector<node> nodes(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const auto value = qcovert::from_bytes<node>(out[i]);
      nodes[i] = ringdoram::obliv::OSelect(value, node{}, ids[i] != invalid_id);
    }
    return nodes;
  }
public:
  index(const typename L::dataset &data, parameters p, sn::threads::thread_team &workers,
        oram_parameters o = {}, measurement *measured = nullptr,
        qcovert::snapshot::reader *image = nullptr, std::size_t maximum_reads = 0,
        std::size_t maximum_width = 0)
      : p_(p), book_(data.book), medoids_(data.medoids),
        legacy_cache_count_(std::max<std::size_t>(data.legacy_cache_count, data.cache.size())),
        legacy_cached_rounds_(data.cached_rounds), workers_(workers), measured_(measured) {
    validate(p);
    if (measured_ && !measured_->clock) throw std::invalid_argument("missing BiSON clock");
    if (data.nodes.empty() || data.nodes.size() > std::size_t(INT64_MAX) || medoids_.empty())
      throw std::invalid_argument("invalid BiSON dataset");
    bool bad = book_.rotated > 1 || book_.offsets[0] != 0 || book_.offsets.back() != L::dim;
    for (std::size_t p = 0; p < L::subspaces; ++p)
      bad |= book_.offsets[p] >= book_.offsets[p + 1] || book_.offsets[p + 1] > L::dim;
    auto finite = [&](float x) { bad |= !std::isfinite(x) | (std::abs(x) > 1.e4f); };
    for (auto x : book_.pivots) finite(x);
    for (auto x : book_.mean) finite(x);
    for (auto x : book_.rotation) finite(x);
    auto check_node = [&](const node &v) {
      bad |= v.id >= data.nodes.size() || v.degree > L::degree || v.reserved != 0;
      for (auto x : v.embedding) finite(float(x));
      for (std::size_t j = 0; j < L::degree; ++j) {
        const auto id = v.neighbors[j].id;
        bad |= (j < v.degree) & (id >= data.nodes.size());
        bad |= (j >= v.degree) & (id != invalid_id);
      }
    };
    for (std::size_t i = 0; i < data.nodes.size(); ++i) {
      bad |= data.nodes[i].id != i; check_node(data.nodes[i]);
    }
    for (const auto &m : medoids_) {
      bad |= m.id >= data.nodes.size();
      for (auto x : m.centroid) finite(x);
    }
    if (bad) throw std::invalid_argument("malformed BiSON dataset/model");
    maximum_reads_ = std::max(maximum_reads, logical_reads());
    maximum_width_ = std::max(maximum_width, p.beamwidth);
    ringdoram::options opts{};
    opts.block_count = std::max<std::size_t>(2, data.nodes.size());
    opts.bucket_real_size = o.z; opts.bucket_dummy_size = o.s;
    opts.eviction_rate = o.a; opts.evict_batch = o.evict_batch; opts.stash_bound = o.stash;
    opts.routing_depth = 0; opts.evict_backend = ringdoram::eviction_backend::shared_top;
    opts.access_concurrency = workers.logical_threads();
    const auto chunk = multiply(o.a, o.evict_batch);
    if (!chunk) throw std::invalid_argument("invalid BiSON ORAM chunk");
    opts.disjoint_epoch_window = std::max(chunk, multiply(add(maximum_reads_, chunk - 1) / chunk, chunk));
    oram_ = std::make_unique<client>(opts, workers.limited_to(1));
    positions_.resize(data.nodes.size());
    if (image) {
      std::array<std::uint64_t, 17> found{};
      image->read(found.data(), sizeof(found));
      if (found != snapshot_header()) throw std::invalid_argument("BiSON snapshot parameters mismatch");
      image->read(positions_.data(), multiply(positions_.size(), sizeof(std::int64_t)));
      bool invalid = false;
      for (auto leaf : positions_) invalid |= (leaf < 0) | (std::uint64_t(leaf) >= oram_->shape().leaf_count);
      if (invalid) throw std::invalid_argument("BiSON snapshot invalid leaf");
      oram_->load_initial(*image); image->finish();
    } else {
      std::vector<ringdoram::bulk_record<sizeof(node)>> records(data.nodes.size());
      for (std::size_t i = 0; i < records.size(); ++i) {
        positions_[i] = random_leaf();
        records[i] = {std::int64_t(i), positions_[i], qcovert::bytes(data.nodes[i])};
      }
      oram_->bulk_setup(records);
    }
    workspace_.configure(*oram_, maximum_width_);
  }
  void configure(parameters p) {
    validate(p);
    if (failed_ || oram_->pending_epoch_accesses()) throw std::logic_error("BiSON requires idle healthy index");
    const auto reads = multiply(p.iterations, p.beamwidth);
    if (reads > maximum_reads_ || p.beamwidth > maximum_width_)
      throw std::invalid_argument("BiSON job exceeds initialized budget");
    p_ = p;
  }
  void save_initial(qcovert::snapshot::writer &image) const {
    if (!pristine_ || failed_ || oram_->pending_epoch_accesses()) throw std::logic_error("BiSON snapshot requires pristine index");
    const auto header = snapshot_header(); image.write(header.data(), sizeof(header));
    image.write(positions_.data(), multiply(positions_.size(), sizeof(std::int64_t)));
    oram_->save_initial(image);
  }
  std::size_t logical_reads() const { return multiply(p_.iterations, p_.beamwidth); }
  std::size_t cached_rounds() const { return 0; }
  std::size_t cache_count() const { return 0; }
  std::size_t cache_bytes() const { return 0; }
  static constexpr std::size_t block_bytes() { return sizeof(node); }
  std::size_t position_map_bytes() const { return positions_.size() * sizeof(std::int64_t); }
  std::size_t encrypted_storage_bytes() const {
    return multiply(multiply(oram_->shape().node_count, oram_->shape().bucket_total_size), sizeof(typename client::block_t) + 16);
  }
  std::vector<result> search(const typename L::query &query, diagnostics *trace = nullptr) {
    if (failed_) throw std::logic_error("BiSON failed; rebuild index");
    bool invalid = false;
    for (auto x : query) invalid |= !std::isfinite(x) | (std::abs(x) > 1.e4f);
    if (invalid) throw std::invalid_argument("invalid BiSON query");
    if (measured_) { const auto clock = measured_->clock; *measured_ = {}; measured_->clock = clock; }
    if (trace) *trace = {};
    failed_ = true; pristine_ = false;
    const auto total = now(), pq_begin = now();
    const auto table = L::make_table(query, book_);
    elapsed(&measurement::pq_ns, pq_begin);
    typename L::medoid entry{};
    float distance = std::numeric_limits<float>::infinity();
    for (const auto &m : medoids_) {
      const auto score = L::centroid_distance(query, m.centroid);
      const bool better = sn::obliv::ct_lt(score_key(score), score_key(distance));
      entry = ringdoram::obliv::OSelect(m, entry, better);
      distance = ringdoram::obliv::OSelect(score, distance, better);
    }
    if (trace) trace->entry = entry.id;
    candidate_list c(p_.l_search);
    const candidate first{entry.id, L::approximate(entry.pq, table)};
    c.merge(std::span<const candidate>(&first, 1), {});
    const auto neighbors = multiply(p_.beamwidth, L::degree);
    std::vector<std::uint64_t> visited(logical_reads(), invalid_id);
    std::vector<retrieved> full(logical_reads());
    for (std::size_t round = 0; round < p_.iterations; ++round) {
      const auto sets = now();
      const auto beam = c.select(p_.beamwidth);
      std::vector<std::uint64_t> ids(p_.beamwidth);
      for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = beam[i].id;
      elapsed(&measurement::sets_ns, sets);
      // Exactly W accesses per iteration, including the entry and all dummies.
      const auto nodes = read(ids);
      const auto append = now();
      for (std::size_t i = 0; i < p_.beamwidth; ++i) {
        const auto offset = round * p_.beamwidth + i; // Public slot, never an ID.
        visited[offset] = nodes[i].id;
        full[offset] = {nodes[i].id, nodes[i].embedding};
      }
      elapsed(&measurement::sets_ns, append);
      const auto pq_start = now();
      std::vector<candidate> incoming(neighbors);
      for (std::size_t j = 0; j < neighbors; ++j) {
        const auto &parent = nodes[j / L::degree];
        const auto &edge = parent.neighbors[j % L::degree];
        const float score = L::approximate(edge.pq, table);
        const bool valid = (parent.id != invalid_id) & (edge.id != invalid_id) &
            sn::obliv::ct_lt(std::uint64_t(j % L::degree), std::uint64_t(parent.degree));
        incoming[j] = ringdoram::obliv::OSelect(candidate{edge.id, score}, candidate{}, valid);
      }
      elapsed(&measurement::pq_ns, pq_start);
      const auto merge_begin = now();
      c.merge(incoming, visited); // OSort IDs, invalidate V/duplicates, exact TopL.
      elapsed(&measurement::sets_ns, merge_begin);
      if (trace) {
        trace->fetched.insert(trace->fetched.end(), ids.begin(), ids.end());
        for (const auto &v : nodes) trace->expanded.push_back(v.id);
        trace->candidates.push_back(c.values());
      }
    }
    // The TW vectors never feed exact distances back into PQ candidate ranking.
    const auto exact_begin = now();
    std::vector<result> best(std::max(full.size(), p_.k));
    for (std::size_t i = 0; i < full.size(); ++i) {
      const float score = L::exact(query, full[i].embedding);
      best[i] = ringdoram::obliv::OSelect(result{full[i].id, double(score)}, result{}, full[i].id != invalid_id);
    }
    elapsed(&measurement::exact_ns, exact_begin);
    const auto topk_begin = now();
    qcovert::exact_topk(best, p_.k, result{}, [](const result &a, const result &b) {
      return nearer(float(a.distance), a.id, float(b.distance), b.id);
    });
    elapsed(&measurement::sets_ns, topk_begin);
    const auto evict = now(); oram_->flush_epoch(); elapsed(&measurement::eviction_ns, evict);
    elapsed(&measurement::total_ns, total);
    failed_ = false;
    return best;
  }
};
} // namespace bison
