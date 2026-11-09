#ifdef NDEBUG
#undef NDEBUG
#endif
#include "bison/index.hpp"
#include "bison/codec.hpp"
#include <map>
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include <cassert>
#include <set>
#include <random>
#include <iostream>
using namespace bison;
namespace {
void queue_test() {
  std::mt19937 rng(42);
  for (std::size_t capacity : {1, 3, 12, 31}) {
    candidate_list actual(capacity);
    std::map<std::uint64_t, float> expected;
    std::set<std::uint64_t> visited_set;
    std::vector<std::uint64_t> visited(64, invalid_id);
    auto merge = [&](const std::vector<candidate> &items) {
      for (auto v : items) if (v.id != invalid_id && !visited_set.count(v.id)) {
        auto it = expected.find(v.id);
        if (it == expected.end() || v.distance < it->second) expected[v.id] = v.distance;
      }
      for (auto id : visited_set) expected.erase(id);
      std::vector<candidate> sorted;
      for (auto [id, score] : expected) sorted.push_back({id, score});
      std::sort(sorted.begin(), sorted.end(), candidate_less);
      sorted.resize(std::min(sorted.size(), capacity));
      expected.clear(); for (auto v : sorted) expected[v.id] = v.distance;
      actual.merge(items, visited);
      for (std::size_t i = 0; i < capacity; ++i) {
        const auto v = actual.values()[i];
        if (i < sorted.size()) assert(v.id == sorted[i].id && v.distance == sorted[i].distance);
        else assert(v.id == invalid_id && std::isinf(v.distance));
      }
    };
    for (std::size_t operation = 0; operation < 150; ++operation) {
      std::vector<candidate> incoming;
      for (std::size_t j = 0, count = 1 + rng() % 8; j < count; ++j)
        incoming.push_back({rng() % 48, float(rng() % 23)});
      incoming.push_back({}); merge(incoming);
      const auto width = 1 + rng() % 10; const auto selected = actual.select(width);
      std::vector<candidate> sorted;
      for (auto [id, score] : expected) sorted.push_back({id, score});
      std::sort(sorted.begin(), sorted.end(), candidate_less);
      sorted.resize(width);
      for (std::size_t i = 0; i < width; ++i) {
        assert(selected[i].id == sorted[i].id && selected[i].distance == sorted[i].distance);
        if (selected[i].id != invalid_id) visited_set.insert(selected[i].id);
      }
      std::fill(visited.begin(), visited.end(), invalid_id);
      std::copy(visited_set.begin(), visited_set.end(), visited.begin());
      merge({});
    }
  }
}
template <class L> typename L::dataset fixture(std::size_t prefix) {
  typename L::dataset d;
  d.book.offsets = {0, 2, 4, 7};
  for (std::size_t dim = 0; dim < L::dim; ++dim)
    for (std::size_t c = 0; c < 256; ++c) d.book.pivots[dim * 256 + c] = float(c % 16) / 4 + float(dim) / 7;
  d.book.mean.fill(0.125f);
  d.nodes.resize(32);
  for (std::size_t i = 0; i < d.nodes.size(); ++i) {
    auto &v = d.nodes[i]; v.id = i; v.degree = i % (L::degree + 1);
    for (std::size_t j = 0; j < L::dim; ++j) {
      if constexpr (sizeof(typename L::scalar) == 1) v.embedding[j] = (i * 7 + j) % 32;
      else v.embedding[j] = float((i * 7 + j) % 32) / 4 - 2.125f;
    }
    for (std::size_t j = 0; j < L::subspaces; ++j) v.pq[j] = (i + 3 * j) % 256;
  }
  for (std::size_t i = 0; i < d.nodes.size(); ++i)
    for (std::size_t j = 0; j < d.nodes[i].degree; ++j) {
      const auto id = (i + j + 1) % d.nodes.size();
      d.nodes[i].neighbors[j] = {id, d.nodes[id].pq};
    }
  for (std::size_t i : {1, 17}) {
    typename L::medoid m; m.id = i; m.pq = d.nodes[i].pq;
    for (std::size_t j = 0; j < L::dim; ++j) m.centroid[j] = float(d.nodes[i].embedding[j]);
    d.medoids.push_back(m);
  }
  std::set<std::size_t> ids, frontier{1, 17};
  for (std::size_t level = 0; level < prefix; ++level) {
    ids.insert(frontier.begin(), frontier.end()); std::set<std::size_t> next;
    for (auto i : frontier)
      for (std::size_t j = 0; j < d.nodes[i].degree; ++j) {
        const auto id = d.nodes[i].neighbors[j].id;
        if (!ids.count(id)) next.insert(id);
      }
    frontier = next;
  }
  for (auto i : ids) d.cache.push_back(d.nodes[i]);
  d.cached_rounds = prefix;
  return d;
}
template <class L> std::vector<result> oracle(const typename L::dataset &data, const typename L::query &q, parameters p, diagnostics &trace) {
  const auto table = L::make_table(q, data.book);
  auto approximate = [&](const auto &code) {
    float sum = 0;
    for (std::size_t j = 0; j < L::subspaces; ++j) sum += table[j * 256 + code[j]];
    return sum;
  };
  const typename L::medoid *entry = &data.medoids[0];
  for (const auto &m : data.medoids)
    if (L::centroid_distance(q, m.centroid) < L::centroid_distance(q, entry->centroid)) entry = &m;
  trace.entry = entry->id;
  std::map<std::uint64_t, float> candidates{{entry->id, approximate(entry->pq)}};
  std::set<std::uint64_t> visited;
  std::vector<std::uint64_t> retrieved;
  for (std::size_t t = 0; t < p.iterations; ++t) {
    std::vector<candidate> ranked;
    for (auto [id, score] : candidates) ranked.push_back({id, score});
    std::sort(ranked.begin(), ranked.end(), candidate_less);
    ranked.resize(std::min(ranked.size(), p.beamwidth));
    std::vector<std::uint64_t> beam(p.beamwidth, invalid_id);
    for (std::size_t i = 0; i < ranked.size(); ++i) { beam[i] = ranked[i].id; visited.insert(beam[i]); }
    trace.fetched.insert(trace.fetched.end(), beam.begin(), beam.end());
    trace.expanded.insert(trace.expanded.end(), beam.begin(), beam.end());
    for (auto id : beam) if (id != invalid_id) {
      retrieved.push_back(id); const auto &v = data.nodes[id];
      for (std::size_t j = 0; j < v.degree; ++j) {
        const auto &edge = v.neighbors[j];
        if (!visited.count(edge.id)) {
          const auto score = approximate(edge.pq);
          auto it = candidates.find(edge.id);
          if (it == candidates.end() || score < it->second) candidates[edge.id] = score;
        }
      }
    }
    for (auto id : visited) candidates.erase(id);
    ranked.clear(); for (auto [id, score] : candidates) ranked.push_back({id, score});
    std::sort(ranked.begin(), ranked.end(), candidate_less);
    ranked.resize(std::min(ranked.size(), p.l_search));
    candidates.clear(); for (auto v : ranked) candidates[v.id] = v.distance;
    ranked.resize(p.l_search); trace.candidates.push_back(ranked);
  }
  std::vector<result> full;
  for (auto id : retrieved) full.push_back({id, L::exact(q, data.nodes[id].embedding)});
  std::sort(full.begin(), full.end(), [](const result &a, const result &b) { return a.distance < b.distance || (a.distance == b.distance && a.id < b.id); });
  full.resize(p.k);
  return full;
}
template <class Scalar> void index_test() {
  using L = layout<7, 5, 3, Scalar>; // unequal chunks; D is not divisible by P
  sn::threads::pthread_thread_pool pool(1);
  sn::threads::thread_team team(pool.pool(), 2);
  for (std::size_t prefix : {0, 2, 20}) {
    auto data = fixture<L>(prefix);
    if (prefix == 0) data.cache.push_back(data.nodes[1]); // Legacy cache must not affect reads.
    const auto bytes = encode<L>(data); data = decode<L>(bytes, 100);
    bison::index<L> actual(data, {6, 4, 12, 5}, team, {8, 16, 4, 1, 0}, nullptr, nullptr, 80, 12);
    assert(actual.cached_rounds() == 0 && actual.cache_count() == 0);
    assert(data.cache.empty()); // Decoder skips legacy cache payloads.
    for (parameters p : {parameters{6,4,12,5}, parameters{2,9,8,5}, parameters{4,2,16,5}}) {
      actual.configure(p);
      for (std::size_t qi : {1, 17, 31}) {
        typename L::query q{};
        for (std::size_t j = 0; j < L::dim; ++j) q[j] = float(data.nodes[qi].embedding[j]) + 0.0625f;
        diagnostics ref, observed;
        const auto expected = oracle<L>(data, q, p, ref), found = actual.search(q, &observed);
        assert(found.size() == expected.size() && ref.entry == observed.entry);
        for (std::size_t j = 0; j < found.size(); ++j) assert(found[j].id == expected[j].id && found[j].distance == expected[j].distance);
        assert(ref.expanded == observed.expanded && ref.fetched == observed.fetched);
        assert(observed.fetched.size() == actual.logical_reads());
        std::set<std::uint64_t> unique;
        for (auto id : observed.fetched) if (id != invalid_id) assert(unique.insert(id).second);
        for (std::size_t t = 0; t < p.iterations; ++t)
          for (std::size_t j = 0; j < p.l_search; ++j) {
            const auto a = ref.candidates[t][j], b = observed.candidates[t][j];
            assert(a.id == b.id && a.distance == b.distance && a.reserved == 0 && b.reserved == 0);
          }
      }
    }
    bool rejected = false;
    try { actual.configure({0,4,12,5}); } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { actual.configure({6,13,12,5}); } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
  }
  auto invalid = fixture<L>(1); invalid.book.offsets[1] = 0;
  bool rejected = false;
  try { bison::index<L> bad(invalid, {2,2,8,5}, team); } catch (const std::invalid_argument &) { rejected = true; }
  assert(rejected);
  // Even an incomplete legacy BFS cache is ignored by the query core.
  auto legacy = fixture<L>(2); legacy.cache.resize(2);
  bison::index<L> ignored(legacy, {3,4,12,5}, team); typename L::query q{};
  diagnostics trace; ignored.search(q, &trace); assert(trace.fetched.size() == 12);

}
void appendix_semantics_test() {
  using L = layout<1, 3, 1, float>;
  typename L::dataset d; d.book.offsets = {0, 1};
  for (std::size_t c = 0; c < 256; ++c) d.book.pivots[c] = float(c);
  d.nodes.resize(4);
  for (std::size_t i = 0; i < 4; ++i) { d.nodes[i].id = i; d.nodes[i].pq[0] = i; d.nodes[i].embedding[0] = float(10 - i); }
  d.nodes[3].embedding[0] = 0;
  d.nodes[0].degree = 3;
  for (std::size_t i = 0; i < 3; ++i) d.nodes[0].neighbors[i] = {i + 1, {std::uint8_t(i + 1)}};
  d.nodes[2].degree = 1; d.nodes[2].neighbors[0] = {3, {3}};
  typename L::medoid m; m.id = 0; m.pq = {0}; d.medoids = {m};
  sn::threads::pthread_thread_pool pool(1); sn::threads::thread_team team(pool.pool(), 2);
  bison::index<L> idx(d, {4,1,2,1}, team); typename L::query q{}; diagnostics trace;
  auto best = idx.search(q, &trace);
  // 3 is pruned after round 0, then discovered again via 2. Expanded seed
  // cannot occupy L. The exact winner does not influence PQ search order.
  assert((trace.expanded == std::vector<std::uint64_t>{0,1,2,3}));
  assert(trace.candidates[0][0].id == 1 && trace.candidates[0][1].id == 2);
  assert(trace.candidates[2][0].id == 3 && best[0].id == 3 && best[0].distance == 0);
  for (auto &v : d.nodes) { v.degree = 0; v.neighbors = {}; }
  bison::index<L> empty(d, {5,3,5,5}, team); best = empty.search(q, &trace);
  assert(trace.fetched.size() == 15 && trace.expanded[0] == 0);
  assert(std::count(trace.expanded.begin(), trace.expanded.end(), invalid_id) == 14);
  for (std::size_t i = 1; i < best.size(); ++i) assert(best[i].id == invalid_id && std::isinf(best[i].distance));
  bison::index<L> short_run(d, {1,1,5,5}, team); best = short_run.search(q);
  assert(best[0].id == 0 && best[4].id == invalid_id); // k > TW is padded.
}

void pq_test() {
  using L = layout<7,5,3,float>;
  auto data = fixture<L>(0); data.book.rotated = 1;
  for (std::size_t j = 0; j < L::dim; ++j) data.book.rotation[j*L::dim+j] = 1;
  data.book.rotation[0] = 0; data.book.rotation[L::dim+1] = 0;
  data.book.rotation[1] = 1; data.book.rotation[L::dim] = -1;
  typename L::query q{}; q.fill(0.75f);
  const auto table = L::make_table(q,data.book);
  // Independent, explicit centered/rotated coordinates, from the DiskANN
  // q[j]*rotation[j*D+d] convention, distinguish matrix transposition.
  typename L::query transformed{}; transformed.fill(0.625f); transformed[0] = -0.625f;
  typename L::distance_table expected{};
  for (std::size_t p = 0; p < L::subspaces; ++p)
    for (std::size_t d = data.book.offsets[p]; d < data.book.offsets[p+1]; ++d)
      for (std::size_t c = 0; c < 256; ++c) {
        const double diff = data.book.pivots[d*256+c] - transformed[d];
        expected[p*256+c] += float(diff*diff);
      }
  assert(table == expected);
  std::mt19937 rng(13);
  for (int i = 0; i < 100; ++i) {
    typename L::code code{}; for (auto &x : code) x = rng() % 256;
    float sum = 0; for (std::size_t p = 0; p < L::subspaces; ++p) sum += table[p*256+code[p]];
    assert(sum == L::approximate(code,table));
  }
}
}
int main() {
  queue_test(); pq_test(); appendix_semantics_test(); index_test<std::uint8_t>(); index_test<float>();
  std::cout << "BiSON Appendix F list/V/OSort/rediscovery/final-rerank oracles passed\n";
}
