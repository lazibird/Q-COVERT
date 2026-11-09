#pragma once
#include "bison/types.hpp"
#include "qcovert/topk.hpp"
namespace bison {
// Appendix F: L contains only unique, unvisited candidates. A pruned ID may
// be discovered again until its block is read and appended to V.
class candidate_list {
  std::vector<candidate> values_;
public:
  explicit candidate_list(std::size_t capacity) : values_(capacity) {
    if (!capacity) throw std::invalid_argument("empty BiSON candidate list");
  }
  void merge(std::span<const candidate> incoming, std::span<const std::uint64_t> visited) {
    struct record { candidate value{}; std::uint64_t visited = 0, source = invalid_id; };
    const auto candidates = add(values_.size(), incoming.size());
    std::vector<record> merged(add(candidates, visited.size()));
    for (std::size_t i = 0; i < values_.size(); ++i) merged[i] = {values_[i], 0, i};
    for (std::size_t i = 0; i < incoming.size(); ++i) merged[values_.size() + i] = {incoming[i], 0, values_.size() + i};
    for (std::size_t i = 0; i < visited.size(); ++i) merged[candidates + i] = {{visited[i]}, 1, i};
    // For each ID, a V record precedes all candidates; otherwise retain the
    // smallest PQ score, then public source. Sorting addresses are fixed.
    ringdoram::obliv::OSort(std::span<record>(merged), [](const record &r) { return r; },
        [](const record &a, const record &b) {
          const bool same = sn::obliv::ct_eq(a.value.id, b.value.id);
          const bool same_type = sn::obliv::ct_eq(a.visited, b.visited);
          const bool same_score = sn::obliv::ct_eq(score_key(a.value.distance), score_key(b.value.distance));
          return sn::obliv::ct_lt(a.value.id, b.value.id) |
              (same & (sn::obliv::ct_lt(b.visited, a.visited) |
                (same_type & (sn::obliv::ct_lt(score_key(a.value.distance), score_key(b.value.distance)) |
                  (same_score & sn::obliv::ct_lt(a.source, b.source))))));
        });
    std::uint64_t previous = invalid_id;
    std::vector<candidate> filtered(merged.size());
    for (std::size_t i = 0; i < merged.size(); ++i) {
      const auto &r = merged[i]; const auto id = r.value.id;
      const bool invalid = sn::obliv::ct_eq(id, invalid_id) | sn::obliv::ct_eq(id, previous) |
          sn::obliv::ct_eq(r.visited, std::uint64_t{1});
      filtered[i] = ringdoram::obliv::OSelect(candidate{}, r.value, invalid);
      previous = id;
    }
    qcovert::exact_topk(filtered, values_.size(), candidate{}, candidate_less);
    values_ = std::move(filtered);
  }
  // Removal occurs after the batch read, when returned IDs enter V.
  std::vector<candidate> select(std::size_t width) const {
    auto selected = values_;
    selected.resize(std::max(selected.size(), width));
    qcovert::exact_topk(selected, width, candidate{}, candidate_less);
    return selected;
  }
  const std::vector<candidate> &values() const { return values_; }
};
} // namespace bison
