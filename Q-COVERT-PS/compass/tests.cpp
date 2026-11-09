#ifdef NDEBUG
#undef NDEBUG
#endif
#include <sys/stat.h>
#include "compass/index.hpp"
#include "qcovert/snapshot_file.hpp"
#include "compass/codec.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include <cassert>
#include <random>
#include <set>
#include <iostream>
#include <map>
using namespace compass;
struct image_buffer {
  std::vector<std::uint8_t> bytes;
  std::size_t offset = 0;
  static std::size_t read(void *context, void *out, std::size_t size) {
    auto &self = *static_cast<image_buffer *>(context);
    const auto count = std::min(size, self.bytes.size() - self.offset);
    std::memcpy(out, self.bytes.data() + self.offset, count);
    self.offset += count;
    return count;
  }
  static void write(void *context, const void *data, std::size_t size) {
    auto &self = *static_cast<image_buffer *>(context);
    const auto *p = static_cast<const std::uint8_t *>(data);
    self.bytes.insert(self.bytes.end(), p, p + size);
  }
  qcovert::snapshot::transport transport() {
    return {this, read, write, nullptr, qcovert::snapshot::raw_protect, qcovert::snapshot::raw_unprotect};
  }
};

template<class L> typename L::dataset fixture(int level, metric m) {
  typename L::dataset d; d.distance = m; d.nodes.resize(32);
  for (std::size_t p = 0; p < L::subspaces; ++p)
    for (std::size_t c = 0; c < L::centroids; ++c)
      for (std::size_t j = 0; j < L::subdim; ++j) d.book[(p * L::centroids + c) * L::subdim + j] = float(c);
  for (std::size_t i = 0; i < d.nodes.size(); ++i) {
    auto &v = d.nodes[i]; v.id = i;
    for (std::size_t j = 0; j < L::dim; ++j) v.embedding[j] = typename L::scalar((i + j) % 7);
    for (std::size_t j = 0; j < L::degree; ++j) {
      auto id = (i + j + 1) % d.nodes.size();
      v.neighbors[j].id = id;
      for (std::size_t p = 0; p < L::subspaces; ++p) v.neighbors[j].pq[p] = (id + p) % L::centroids;
    }
  }
  auto upper = [&](std::size_t id) {
    typename L::upper_node n; n.id = id; n.embedding = d.nodes[id].embedding;
    for (std::size_t j = 0; j < n.neighbors.size(); ++j) {
      n.neighbors[j].id = ((id / 8 + j + 1) % 4) * 8;
      for (std::size_t p = 0; p < L::subspaces; ++p) n.neighbors[j].pq[p] = (n.neighbors[j].id + p) % L::centroids;
    }
    return n;
  };
  d.scan_level = level;
  if (level) for (std::size_t i = 0; i < 32; i += 8) d.upper_nodes.push_back(upper(i));
  if (level == 2) d.scan_nodes = {upper(0), upper(16)};
  if (level == 1) d.scan_nodes = d.upper_nodes;
  if (level == 0) { typename L::upper_node n; n.id = 0; n.embedding = d.nodes[0].embedding; d.scan_nodes = {n}; }
  return d;
}
// Independent direct-address oracle: unbounded C, ordinary sort and sets.
template<class L> std::vector<result> plain(const typename L::dataset &d, const typename L::query &q,
                                           parameters p, diagnostics &trace) {
  const auto table = L::make_table(q, d.book, d.distance);
  auto exact = [&](const auto &n) { return L::exact(q, n.embedding, d.distance); };
  auto rank = [&](const auto &edges, const std::set<std::uint64_t> &visited, std::size_t count) {
    std::map<std::uint64_t, double> codes;
    for (const auto &e : edges) if (e.id != invalid_id && !visited.count(e.id)) codes.emplace(e.id, L::approximate(e.pq, table));
    std::vector<result> ranked;
    for (auto [id, score] : codes) ranked.push_back({id, score});
    std::sort(ranked.begin(), ranked.end(), [](auto a, auto b) { return nearer(a.distance, a.id, b.distance, b.id); });
    ranked.resize(std::min(count, ranked.size()));
    std::vector<std::uint64_t> ids(count, invalid_id);
    for (std::size_t i = 0; i < ranked.size(); ++i) ids[i] = ranked[i].id;
    return ids;
  };
  auto nearest = *std::min_element(d.scan_nodes.begin(), d.scan_nodes.end(), [&](auto a, auto b) { return nearer(exact(a), a.id, exact(b), b.id); });
  trace.scan_entry = nearest.id;
  if (!d.upper_nodes.empty()) {
    trace.upper_expanded.push_back(nearest.id);
    auto ids = rank(nearest.neighbors, {nearest.id}, p.ef_n); trace.upper_fetched = ids;
    for (auto id : ids) if (id != invalid_id) {
      auto n = *std::find_if(d.upper_nodes.begin(), d.upper_nodes.end(), [&](auto n) { return n.id == id; });
      if (nearer(exact(n), n.id, exact(nearest), nearest.id)) nearest = n;
    }
  }
  const auto entry = nearest.id; trace.bottom_entry = entry; trace.fetched.push_back(entry);
  std::set<std::uint64_t> visited{entry}; std::map<std::uint64_t, double> candidates{{entry, exact(d.nodes[entry])}};
  std::vector<result> best{{entry, exact(d.nodes[entry])}};
  auto less = [](auto a, auto b) { return nearer(a.distance, a.id, b.distance, b.id); };
  for (std::size_t round = 0; round < (p.ef + p.ef_spec - 1) / p.ef_spec; ++round) {
    std::vector<result> selected;
    for (auto [id, score] : candidates) selected.push_back({id, score});
    std::sort(selected.begin(), selected.end(), less); selected.resize(std::min(selected.size(), p.ef_spec));
    std::vector<typename L::neighbor> edges;
    for (auto s : selected) { candidates.erase(s.id); trace.expanded.push_back(s.id); edges.insert(edges.end(), d.nodes[s.id].neighbors.begin(), d.nodes[s.id].neighbors.end()); }
    for (std::size_t i = selected.size(); i < p.ef_spec; ++i) trace.expanded.push_back(invalid_id);
    auto ids = rank(edges, visited, p.ef_spec * p.ef_n);
    trace.fetched.insert(trace.fetched.end(), ids.begin(), ids.end());
    for (auto id : ids) if (id != invalid_id) { visited.insert(id); candidates[id] = exact(d.nodes[id]); best.push_back({id, exact(d.nodes[id])}); }
    std::sort(best.begin(), best.end(), less); best.resize(std::min(best.size(), p.ef));
    trace.pending_candidates.push_back(candidates.size());
  }
  best.resize(p.k); return best;
}
template<class L> void check(sn::threads::thread_team &team) {
  for (auto metric : {metric::l2, metric::inner_product}) for (int level : {0, 1, 2}) {
    auto d = fixture<L>(level, metric); auto encoded = encode<L>(d); d = decode<L>(encoded, 32);
    parameters p{11, 3, 4, 5, 1}; compass::index<L> idx(d, p, team);
    assert(idx.rounds() == 4 && idx.batch_size() == 12 && idx.capacity() == 49);
    assert(idx.logical_reads() == 49 + (level ? 4 : 0));
    assert(idx.position_map_bytes() == (32 + (level ? 4 : 0)) * 8);
    assert(idx.upper_storage_bytes() == 0 || level != 0);
    image_buffer buffer; auto io = buffer.transport(); qcovert::snapshot::identity identity{};
    { qcovert::snapshot::writer out(io, identity); idx.save_initial(out); out.finish(); }
    qcovert::snapshot::reader in(io, identity); compass::index<L> restored(d, p, team, {}, nullptr, &in);
    for (auto ef : {11UL, 5UL, 8UL, 11UL}) {
      p.ef = ef; idx.configure_ef(ef); restored.configure_ef(ef);
      for (int qi = 0; qi < 6; ++qi) {
        typename L::query q; for (std::size_t j = 0; j < L::dim; ++j) q[j] = float(qi - 2) + float(j) / 4;
        diagnostics expected, actual; auto want = plain<L>(d, q, p, expected); auto got = idx.search(q, &actual); auto loaded = restored.search(q);
        assert(got.size() == p.k);
        for (std::size_t j = 0; j < got.size(); ++j) { assert(got[j].id == want[j].id && got[j].distance == want[j].distance); assert(loaded[j].id == got[j].id && loaded[j].distance == got[j].distance); }
        assert(actual.logical_reads == idx.logical_reads()); assert(actual.expanded == expected.expanded); assert(actual.fetched == expected.fetched);
        assert(actual.upper_fetched == expected.upper_fetched); assert(actual.bottom_entry == expected.bottom_entry); assert(actual.pending_candidates == expected.pending_candidates);
        std::set<std::uint64_t> fetched; for (auto id : actual.fetched) if (id != invalid_id) assert(fetched.insert(id).second);
        std::set<std::uint64_t> expanded; for (auto id : actual.expanded) if (id != invalid_id) assert(expanded.insert(id).second);
      }
    }
    bool invalid = false; try { idx.configure_ef(0); } catch (const std::invalid_argument &) { invalid = true; } assert(invalid);
    invalid = false; try { qcovert::snapshot::writer out(io, identity); idx.save_initial(out); } catch (const std::logic_error &) { invalid = true; } assert(invalid);
  }
}
int main() {
  sn::threads::pthread_thread_pool pool(1); sn::threads::thread_team team(pool.pool(), 2);
  using F = layout<4, 8, 2, 4>; using U = layout<4, 8, 2, 4, std::uint8_t>;
  check<F>(team); check<U>(team);
  auto d = fixture<F>(0, metric::l2);
  auto standard = fixture<F>(2, metric::l2);
  compass::index<F> default_budget(standard, {}, team);
  assert(default_budget.rounds() == 5 && default_budget.logical_reads() == 253 && default_budget.capacity() == 241);
  // Fetch eight neighbors with ef=5: C must keep all eight, not ef entries.
  compass::index<F> idx(d, {5, 2, 4, 2, 1}, team); diagnostics trace; F::query q{}; idx.search(q, &trace);
  assert(trace.pending_candidates[0] == 8 && trace.pending_candidates[0] > 5);
  // Only the last half of the bottom entry has a useful edge. Scan records
  // intentionally have no bottom hints; the explicit L0 read must find it.
  auto tail = fixture<F>(2, metric::l2);
  for (auto &n : tail.nodes) { n.embedding.fill(10); n.neighbors = {}; }
  for (auto &n : tail.upper_nodes) { n.embedding.fill(10); n.neighbors = {}; }
  tail.scan_nodes = {tail.upper_nodes[0]}; tail.nodes[31].embedding.fill(0);
  tail.nodes[0].neighbors[7] = {31, {0, 0}};
  compass::index<F> full(tail, {4, 4, 2, 1, 1}, team); auto result = full.search(q, &trace);
  assert(result[0].id == 31 && trace.bottom_entry == 0 && trace.logical_reads == 11);
  assert(std::count(trace.expanded.begin(), trace.expanded.end(), invalid_id) == 3);
  auto exhausted = fixture<F>(0, metric::l2); for (auto &n : exhausted.nodes) n.neighbors = {};
  compass::index<F> empty(exhausted, {5, 2, 4, 2, 1}, team); result = empty.search(q, &trace);
  assert(trace.logical_reads == 25 && result[1].id == invalid_id);
  assert(std::count(trace.expanded.begin(), trace.expanded.end(), invalid_id) == 5);
  bool invalid = false; try { compass::index<F> bad(d, {5, 2, 4, 2, 2}, team); } catch (const std::invalid_argument &) { invalid = true; } assert(invalid);
  std::cout << "Compass fixed C, exact oracle, layer maps, snapshots and public budgets passed\n";
}
