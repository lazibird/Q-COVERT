#pragma once
#include "compass/types.hpp"
#include "qcovert/encrypted_store.hpp"
#include "qcovert/snapshot.hpp"
#include "qcovert/topk.hpp"
#include "ringdoram/oram.hpp"
#include <memory>
namespace compass {
// All query workspaces have public sizes. Logical IDs and selected cache
// slots are resolved by full scans; they never address trusted memory.
template <class L> class index {
  using node = typename L::node;
  using upper_node = typename L::upper_node;
  struct key { std::uint64_t id = invalid_id, slot = invalid_id; double score = infinity; };
  struct hint { typename L::neighbor value{}; double score = infinity; };
  struct route { std::uint64_t id, address; };
  struct budget_t { std::size_t rounds, batch, capacity, bottom_reads; };
  template<class N> struct layer {
    using client = ringdoram::client<ringdoram::traits<sizeof(N),
        ringdoram::epoch_mode::disjoint_epoch, qcovert::encrypted_store>>;
    std::unique_ptr<client> tree;
    std::vector<std::int64_t> positions;
    ringdoram::batch_workspace<client> workspace;
  };
  parameters p_;
  metric metric_;
  std::uint64_t entry_;
  budget_t budget_{};
  std::size_t scan_level_;
  std::vector<route> upper_routes_;
  std::vector<upper_node> scan_nodes_;
  typename L::codebook book_;
  layer<node> bottom_;
  layer<upper_node> upper_;
  sn::threads::thread_team &workers_;
  ringdoram::obliv::secure_random random_;
  bool failed_ = false, pristine_ = true;
  measurement *measured_;
  static budget_t budget(parameters p) {
    if (!p.ef || !p.ef_spec || !p.ef_n || !p.k || p.upper_steps != 1)
      throw std::invalid_argument("Compass requires positive ef/ef_spec/ef_n/k and upper_steps=1");
    const auto rounds = p.ef / p.ef_spec + std::size_t(p.ef % p.ef_spec != 0);
    const auto batch = multiply(p.ef_spec, p.ef_n), added = multiply(rounds, batch);
    if (added == SIZE_MAX || p.ef_n > SIZE_MAX - (1 + added)) throw std::length_error("Compass capacity overflow");
    multiply(p.ef_spec, L::degree);
    return {rounds, batch, 1 + added, 1 + added};
  }
  std::uint64_t now() const { return measured_ ? measured_->clock() : 0; }
  void elapsed(std::uint64_t measurement::*field, std::uint64_t start) {
    if (measured_) measured_->*field += now() - start;
  }
  static bool key_less(const key &a, const key &b) { return nearer(a.score, a.id, b.score, b.id); }
  static bool result_less(const result &a, const result &b) { return nearer(a.distance, a.id, b.distance, b.id); }
  template<class N> std::int64_t random_leaf(const layer<N> &state) {
    std::uint64_t x;
    random_.random_bytes(reinterpret_cast<std::uint8_t *>(&x), sizeof(x));
    return static_cast<std::int64_t>(x & (state.tree->shape().leaf_count - 1));
  }
  template<class N> void setup(layer<N> &state, const std::vector<N> &values,
                               std::size_t reads, oram_parameters o) {
    ringdoram::options opts{};
    opts.block_count = std::max<std::size_t>(2, values.size());
    opts.bucket_real_size = o.z; opts.bucket_dummy_size = o.s;
    opts.eviction_rate = o.a; opts.evict_batch = o.evict_batch; opts.stash_bound = o.stash;
    opts.routing_depth = 0; opts.evict_backend = ringdoram::eviction_backend::shared_top;
    opts.access_concurrency = workers_.logical_threads();
    const auto chunk = multiply(o.a, o.evict_batch);
    if (!chunk || reads > SIZE_MAX - (chunk - 1)) throw std::invalid_argument("invalid Compass ORAM window");
    opts.disjoint_epoch_window = ((reads + chunk - 1) / chunk) * chunk;
    state.tree = std::make_unique<typename layer<N>::client>(opts, workers_.limited_to(1));
    state.positions.resize(values.size());
    state.workspace.configure(*state.tree, std::max(budget_.batch, p_.ef_n));
  }
  template<class N> void bulk(layer<N> &state, const std::vector<N> &values) {
    std::vector<ringdoram::bulk_record<sizeof(N)>> records(values.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
      state.positions[i] = random_leaf(state);
      records[i] = {static_cast<std::int64_t>(i), state.positions[i], qcovert::bytes(values[i])};
    }
    state.tree->bulk_setup(records);
  }
  template<class N> std::vector<N> read(layer<N> &state, std::span<const std::uint64_t> addresses) {
    const auto position_begin = now();
    std::vector<std::int64_t> old(addresses.size()), fresh(addresses.size());
    for (std::size_t lane = 0; lane < addresses.size(); ++lane) {
      old[lane] = random_leaf(state); fresh[lane] = random_leaf(state);
      for (std::size_t i = 0; i < state.positions.size(); ++i) {
        const bool match = sn::obliv::ct_eq(addresses[lane], std::uint64_t(i));
        old[lane] = ringdoram::obliv::OSelect(state.positions[i], old[lane], match);
        state.positions[i] = ringdoram::obliv::OSelect(fresh[lane], state.positions[i], match);
      }
    }
    elapsed(&measurement::position_ns, position_begin);
    std::vector<std::array<std::uint8_t, sizeof(N)>> in(addresses.size()), out(addresses.size());
    std::vector<ringdoram::access_request> requests(addresses.size());
    for (std::size_t i = 0; i < addresses.size(); ++i)
      requests[i] = {ringdoram::obliv::OSelect(static_cast<std::int64_t>(addresses[i]), std::int64_t{-1},
                          addresses[i] != invalid_id), old[i], fresh[i], false,
                          {in[i].data(), in[i].size()}, {out[i].data(), out[i].size()}};
    const auto start = now();
    ringdoram::AccessBatch(*state.tree, requests, state.workspace, workers_);
    elapsed(&measurement::oram_ns, start);
    std::vector<N> nodes(addresses.size());
    for (std::size_t i = 0; i < addresses.size(); ++i)
      nodes[i] = ringdoram::obliv::OSelect(qcovert::from_bytes<N>(out[i]), N{}, addresses[i] != invalid_id);
    return nodes;
  }
  template<class N> static std::size_t storage(const layer<N> &s) {
    if (!s.tree) return 0; // Public hierarchy shape.
    return multiply(multiply(s.tree->shape().node_count, s.tree->shape().bucket_total_size),
                    sizeof(typename layer<N>::client::block_t) + 16);
  }
  std::array<std::uint64_t, 24> snapshot_header() const {
    const auto &o = bottom_.tree->options();
    return {0x313049504d435343ULL, 3, qcovert::snapshot::index_format_version,
      bottom_.positions.size(), upper_.positions.size(), L::dim, L::degree, L::subspaces,
      L::centroids, sizeof(typename L::scalar), sizeof(node), sizeof(upper_node),
      std::uint64_t(metric_), entry_, o.bucket_real_size, o.bucket_dummy_size,
      o.eviction_rate, o.evict_batch, o.stash_bound, 2, scan_nodes_.size(), scan_level_,
      sizeof(typename layer<node>::client::block_t), sizeof(typename layer<upper_node>::client::block_t)};
  }
  template<class N> void restore(layer<N> &s, qcovert::snapshot::reader &image) {
    image.read(s.positions.data(), multiply(s.positions.size(), sizeof(std::int64_t)));
    bool bad = false;
    for (auto leaf : s.positions) bad |= (leaf < 0) | (std::uint64_t(leaf) >= s.tree->shape().leaf_count);
    if (bad) throw std::invalid_argument("Compass snapshot position map invalid");
    s.tree->load_initial(image);
  }
  // Sort V + hints by ID, put V first, suppress repeated IDs, then restore
  // public hint order by source. No secret source is used as an array index.
  void exclude(std::vector<hint> &hints, const std::vector<std::uint64_t> &visited) {
    struct record { typename L::neighbor value{}; std::uint64_t source = invalid_id, visited = 0; };
    const auto count = hints.size();
    std::vector<record> merged(count + visited.size());
    for (std::size_t i = 0; i < count; ++i) merged[i] = {hints[i].value, i, 0};
    for (std::size_t i = 0; i < visited.size(); ++i) merged[count + i] = {{visited[i], {}}, invalid_id, 1};
    qcovert::exact_topk(merged, merged.size(), record{}, [](const record &a, const record &b) {
      const bool same = sn::obliv::ct_eq(a.value.id, b.value.id);
      return sn::obliv::ct_lt(a.value.id, b.value.id) |
        (same & (sn::obliv::ct_lt(b.visited, a.visited) |
          (sn::obliv::ct_eq(a.visited, b.visited) & sn::obliv::ct_lt(a.source, b.source))));
    });
    std::uint64_t previous = invalid_id;
    for (auto &r : merged) {
      const auto id = r.value.id;
      const bool remove = sn::obliv::ct_eq(id, previous) | sn::obliv::ct_eq(id, invalid_id);
      r.value.id = ringdoram::obliv::OSelect(invalid_id, id, remove);
      previous = id;
    }
    // A real hint has a finite public source, V/padding uses UINT64_MAX.
    qcovert::exact_topk(merged, count, record{}, [](const record &a, const record &b) {
      return sn::obliv::ct_lt(a.source, b.source);
    });
    for (std::size_t i = 0; i < count; ++i) hints[i].value = merged[i].value;
  }
  std::vector<hint> rank_hints(std::vector<hint> hints, std::size_t count,
                               const typename L::distance_table &table,
                               const std::vector<std::uint64_t> &visited) {
    const auto sets = now(); exclude(hints, visited); elapsed(&measurement::sets_ns, sets);
    const auto start = now();
    for (auto &h : hints) {
      const double score = L::approximate(h.value.pq, table);
      h.score = ringdoram::obliv::OSelect(score, infinity, h.value.id != invalid_id);
    }
    hints.resize(std::max(hints.size(), count));
    qcovert::exact_topk(hints, count, hint{}, [](const hint &a, const hint &b) {
      return nearer(a.score, a.value.id, b.score, b.value.id);
    });
    elapsed(&measurement::pq_ns, start);
    return hints;
  }
public:
  index(const typename L::dataset &data, parameters p, sn::threads::thread_team &workers,
        oram_parameters o = {}, measurement *measured = nullptr, qcovert::snapshot::reader *image = nullptr)
      : p_(p), metric_(data.distance), entry_(data.entry), scan_level_(data.scan_level),
        scan_nodes_(data.scan_nodes), book_(data.book), workers_(workers), measured_(measured) {
    if (measured && !measured->clock) throw std::invalid_argument("Compass clock missing");
    budget_ = budget(p);
    const auto n = data.nodes.size();
    if (!n || n > std::size_t(INT64_MAX) || entry_ >= n || L::degree % 2 ||
        data.scan_nodes.empty() || data.scan_nodes.size() > n || data.upper_nodes.size() > n || scan_level_ > 2 ||
        (scan_level_ == 0 && (!data.upper_nodes.empty() || data.scan_nodes.size() != 1)) ||
        (scan_level_ != 0 && data.upper_nodes.empty()) ||
        (metric_ != metric::l2 && metric_ != metric::inner_product))
      throw std::invalid_argument("invalid Compass hierarchy");
    bool invalid = false;
    auto validate = [&](const auto &v) {
      invalid |= v.id >= n;
      for (auto x : v.embedding) invalid |= !std::isfinite(float(x)) | (std::abs(float(x)) > 1.e10f);
      for (const auto &e : v.neighbors) {
        invalid |= (e.id != invalid_id) & (e.id >= n);
        for (auto c : e.pq) invalid |= std::size_t(c) >= L::centroids;
      }
    };
    for (std::size_t i = 0; i < n; ++i) { validate(data.nodes[i]); invalid |= data.nodes[i].id != i; }
    for (std::size_t i = 0; i < data.upper_nodes.size(); ++i) {
      validate(data.upper_nodes[i]);
      if (i) invalid |= data.upper_nodes[i - 1].id >= data.upper_nodes[i].id;
      upper_routes_.push_back({data.upper_nodes[i].id, i});
    }
    // Setup checks are over public input, outside the query path.
    auto has_upper = [&](std::uint64_t id) {
      auto it = std::lower_bound(upper_routes_.begin(), upper_routes_.end(), id,
          [](const route &r, std::uint64_t x) { return r.id < x; });
      return it != upper_routes_.end() && it->id == id;
    };
    for (const auto &v : data.upper_nodes)
      for (const auto &e : v.neighbors) if (e.id != invalid_id) invalid |= !has_upper(e.id);
    for (const auto &v : data.scan_nodes) {
      validate(v);
      if (scan_level_ != 0) {
        invalid |= !has_upper(v.id);
        for (const auto &e : v.neighbors) if (e.id != invalid_id) invalid |= !has_upper(e.id);
      }
    }
    for (auto x : book_) invalid |= !std::isfinite(x) | (std::abs(x) > 1.e10f);
    if (invalid) throw std::invalid_argument("malformed Compass dataset");
    setup(bottom_, data.nodes, budget_.bottom_reads, o);
    if (!data.upper_nodes.empty()) setup(upper_, data.upper_nodes, p_.ef_n, o);
    if (image) {
      std::array<std::uint64_t, 24> found{}; image->read(found.data(), sizeof(found));
      if (found != snapshot_header()) throw std::invalid_argument("Compass snapshot parameters/version mismatch");
      restore(bottom_, *image);
      if (upper_.tree) restore(upper_, *image);
      image->finish();
    } else {
      bulk(bottom_, data.nodes);
      if (upper_.tree) bulk(upper_, data.upper_nodes);
    }
  }
  void configure_ef(std::size_t ef) {
    if (failed_ || bottom_.tree->pending_epoch_accesses() || (upper_.tree && upper_.tree->pending_epoch_accesses()))
      throw std::logic_error("Compass ef change requires idle healthy trees");
    auto p = p_; p.ef = ef; const auto b = budget(p);
    if (b.bottom_reads > bottom_.tree->options().disjoint_epoch_window)
      throw std::invalid_argument("Compass ef exceeds the initialized ORAM window");
    p_ = p; budget_ = b;
  }
  void save_initial(qcovert::snapshot::writer &image) const {
    if (!pristine_ || failed_ || bottom_.tree->pending_epoch_accesses() || (upper_.tree && upper_.tree->pending_epoch_accesses()))
      throw std::logic_error("Compass snapshot requires idle initial trees");
    const auto header = snapshot_header(); image.write(header.data(), sizeof(header));
    auto save = [&](const auto &s) {
      image.write(s.positions.data(), multiply(s.positions.size(), sizeof(std::int64_t)));
      s.tree->save_initial(image);
    };
    save(bottom_); if (upper_.tree) save(upper_);
  }
  std::size_t rounds() const { return budget_.rounds; }
  std::size_t batch_size() const { return budget_.batch; }
  std::size_t capacity() const { return budget_.capacity; }
  std::size_t logical_reads() const { return budget_.bottom_reads + upper_reads(); }
  std::size_t upper_reads() const { return upper_.tree ? p_.ef_n : 0; }
  std::size_t scan_count() const { return scan_nodes_.size(); }
  std::size_t routing_bytes() const { return multiply(scan_nodes_.size(), sizeof(upper_node)) + multiply(upper_routes_.size(), sizeof(route)); }
  static constexpr std::size_t block_bytes() { return sizeof(node); }
  static constexpr std::size_t upper_block_bytes() { return sizeof(upper_node); }
  std::size_t encrypted_storage_bytes() const { return storage(bottom_) + storage(upper_); }
  std::size_t position_map_bytes() const { return multiply(bottom_.positions.size() + upper_.positions.size(), sizeof(std::int64_t)); }
  std::size_t upper_storage_bytes() const { return storage(upper_); }
  std::size_t upper_map_bytes() const { return multiply(upper_.positions.size(), sizeof(std::int64_t)); }
  std::vector<result> search(const typename L::query &query, diagnostics *trace = nullptr) {
    if (failed_) throw std::logic_error("Compass index failed; rebuild before reuse");
    bool invalid = false;
    for (auto x : query) invalid |= !std::isfinite(x) | (std::abs(x) > 1.e10f);
    if (invalid) throw std::invalid_argument("invalid Compass query");
    if (measured_) { auto clock = measured_->clock; *measured_ = {}; measured_->clock = clock; }
    const auto total = now();
    if (trace) { *trace = {}; trace->rounds = rounds(); trace->batch_size = batch_size(); }
    failed_ = true; pristine_ = false;
    const auto table_begin = now(); const auto table = L::make_table(query, book_, metric_);
    elapsed(&measurement::pq_ns, table_begin);
    const auto scan_begin = now();
    upper_node nearest{}; double nearest_score = infinity;
    for (const auto &v : scan_nodes_) {
      const double score = L::exact(query, v.embedding, metric_);
      const bool better = nearer(score, v.id, nearest_score, nearest.id);
      nearest = ringdoram::obliv::OSelect(v, nearest, better);
      nearest_score = ringdoram::obliv::OSelect(score, nearest_score, better);
    }
    elapsed(&measurement::upper_scan_ns, scan_begin);
    if (trace) trace->scan_entry = nearest.id;
    if (upper_.tree) { // Public layer existence; exactly one L1 step.
      if (trace) trace->upper_expanded.push_back(nearest.id);
      std::vector<hint> hints(L::degree / 2);
      for (std::size_t i = 0; i < hints.size(); ++i) hints[i].value = nearest.neighbors[i];
      hints = rank_hints(std::move(hints), p_.ef_n, table, {nearest.id});
      const auto routing = now();
      std::vector<std::uint64_t> addresses(p_.ef_n, invalid_id);
      for (std::size_t i = 0; i < p_.ef_n; ++i)
        for (const auto &r : upper_routes_)
          addresses[i] = ringdoram::obliv::OSelect(r.address, addresses[i], sn::obliv::ct_eq(hints[i].value.id, r.id));
      elapsed(&measurement::sets_ns, routing);
      const auto fetched = read(upper_, addresses);
      const auto exact_begin = now();
      for (const auto &v : fetched) {
        const double score = L::exact(query, v.embedding, metric_);
        const bool better = (v.id != invalid_id) & nearer(score, v.id, nearest_score, nearest.id);
        nearest = ringdoram::obliv::OSelect(v, nearest, better);
        nearest_score = ringdoram::obliv::OSelect(score, nearest_score, better);
      }
      elapsed(&measurement::exact_ns, exact_begin);
      if (trace) {
        trace->logical_reads += p_.ef_n;
        for (const auto &h : hints) trace->upper_fetched.push_back(h.value.id);
      }
    }
    // The lower entry always comes from its complete layer-0 block.
    const std::array<std::uint64_t, 1> entry_ids{nearest.id};
    const auto entry = read(bottom_, entry_ids)[0];
    const auto exact_begin = now(); const auto entry_score = L::exact(query, entry.embedding, metric_);
    elapsed(&measurement::exact_ns, exact_begin);
    if (trace) { trace->bottom_entry = entry.id; ++trace->logical_reads; trace->fetched.push_back(entry.id); }
    std::vector<key> candidates(capacity());
    std::vector<node> cache(capacity());
    std::vector<std::uint64_t> visited(capacity(), invalid_id);
    candidates[0] = {entry.id, 0, entry_score}; cache[0] = entry; visited[0] = entry.id;
    const auto window = std::max(p_.ef, p_.k);
    std::vector<result> best(window); best[0] = {entry.id, entry_score};
    for (std::size_t round = 0; round < rounds(); ++round) {
      const auto sets = now();
      auto selected = candidates;
      selected.resize(std::max(selected.size(), p_.ef_spec));
      qcovert::exact_topk(selected, p_.ef_spec, key{}, key_less);
      // Delete only expanded candidates. Every other fetched node remains.
      for (std::size_t j = 0; j < candidates.size(); ++j) {
        bool expanded = false;
        for (const auto &s : selected) expanded |= (s.id != invalid_id) & sn::obliv::ct_eq(s.slot, std::uint64_t(j));
        candidates[j] = ringdoram::obliv::OSelect(key{}, candidates[j], expanded);
      }
      std::vector<hint> hints(multiply(p_.ef_spec, L::degree));
      for (std::size_t i = 0; i < p_.ef_spec; ++i) {
        node value{};
        for (std::size_t j = 0; j < cache.size(); ++j)
          value = ringdoram::obliv::OSelect(cache[j], value,
              (selected[i].id != invalid_id) & sn::obliv::ct_eq(selected[i].slot, std::uint64_t(j)));
        if (trace) trace->expanded.push_back(value.id);
        for (std::size_t j = 0; j < L::degree; ++j) hints[i * L::degree + j].value = value.neighbors[j];
      }
      elapsed(&measurement::sets_ns, sets);
      hints = rank_hints(std::move(hints), batch_size(), table, visited);
      std::vector<std::uint64_t> ids(batch_size());
      for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = hints[i].value.id;
      const auto fetched = read(bottom_, ids);
      const auto exact = now();
      std::vector<double> scores(batch_size());
      for (std::size_t i = 0; i < scores.size(); ++i)
        scores[i] = ringdoram::obliv::OSelect(L::exact(query, fetched[i].embedding, metric_), infinity, ids[i] != invalid_id);
      elapsed(&measurement::exact_ns, exact);
      const auto update = now();
      const auto offset = 1 + round * batch_size(); // Public append slots.
      for (std::size_t i = 0; i < ids.size(); ++i) {
        visited[offset + i] = ids[i]; cache[offset + i] = fetched[i];
        candidates[offset + i] = ringdoram::obliv::OSelect(key{ids[i], offset + i, scores[i]}, key{}, ids[i] != invalid_id);
        best.push_back({ids[i], scores[i]});
      }
      qcovert::exact_topk(best, window, result{}, result_less); // One batch update of W.
      elapsed(&measurement::sets_ns, update);
      if (trace) {
        trace->logical_reads += batch_size(); trace->fetched.insert(trace->fetched.end(), ids.begin(), ids.end());
        std::size_t pending = 0; for (const auto &c : candidates) pending += c.id != invalid_id;
        trace->pending_candidates.push_back(pending);
      }
    }
    const auto finish = now(); qcovert::exact_topk(best, p_.k, result{}, result_less);
    elapsed(&measurement::sets_ns, finish);
    const auto evict = now();
    if (upper_.tree) upper_.tree->flush_epoch();
    bottom_.tree->flush_epoch();
    elapsed(&measurement::eviction_ns, evict); elapsed(&measurement::total_ns, total);
    failed_ = false;
    return best;
  }
};
} // namespace compass
