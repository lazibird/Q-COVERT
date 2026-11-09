#pragma once
// Benchmark instrumentation: intentionally not oblivious. Never enable for private service queries.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace qcovert {
enum class phase : std::size_t {
  centroid_distance,
  centroid_topk,
  qu_read,
  query_rotation,
  quantized_distance,
  candidate_topk,
  fu_read,
  fu_leaf_update,
  exact_distance,
  rerank_topk,
  qu_evict,
  fu_evict,
  count
};
inline constexpr std::array<const char *, static_cast<std::size_t>(phase::count)> phase_names{
    "centroid_distance_ns",  "centroid_topk_ns",  "qu_read_ns",  "query_rotation_ns",
    "quantized_distance_ns", "candidate_topk_ns", "fu_read_ns",  "fu_leaf_update_ns",
    "exact_distance_ns",     "rerank_topk_ns",    "qu_evict_ns", "fu_evict_ns"};
struct storage_counters {
  std::atomic<std::uint64_t> read_bytes{0}, write_bytes{0}, allocated_bytes{0}, rebuilds{0};
};
struct query_measurement {
  std::array<std::uint64_t, static_cast<std::size_t>(phase::count)> ns{};
  std::uint64_t online_ns = 0, total_ns = 0;
  std::uint64_t qu_stash_peak = 0, fu_stash_peak = 0, qu_stash_total_peak = 0, fu_stash_total_peak = 0;
  std::uint64_t qu_stash_capacity = 0, fu_stash_capacity = 0;
  std::uint64_t qu_pending_slots = 0, fu_pending_slots = 0, fu_pending_real = 0, valid_quantized = 0;
  std::uint64_t qu_reshuffles = 0, fu_reshuffles = 0;
  std::uint64_t qu_read_bytes = 0, qu_write_bytes = 0, fu_read_bytes = 0, fu_write_bytes = 0;
};
struct measurement {
  using clock_fn = std::uint64_t (*)();
  using progress_fn = void (*)(void *, const char *, std::uint64_t, std::uint64_t);
  clock_fn clock = nullptr;
  progress_fn progress = nullptr;
  void *progress_context = nullptr;
  query_measurement query{};
  std::vector<std::uint64_t> selected_clusters, selected_candidates;
  storage_counters qu_storage, fu_storage;
  std::uint64_t begin = 0, setup_ns = 0, fu_setup_ns = 0, qu_setup_ns = 0;
  std::array<std::uint64_t, 6> traffic_start{};
  bool active = false;
  void report_progress(const char *stage, std::uint64_t completed, std::uint64_t total) const {
    if (progress)
      progress(progress_context, stage, completed, total);
  }
  void start_query() {
    query = {};
    selected_clusters.clear();
    selected_candidates.clear();
    traffic_start = {qu_storage.read_bytes.load(), qu_storage.write_bytes.load(),
                     fu_storage.read_bytes.load(), fu_storage.write_bytes.load(),
                     qu_storage.rebuilds.load(),   fu_storage.rebuilds.load()};
    begin = clock();
    active = true;
  }
  void stop_query() {
    query.total_ns = clock() - begin;
    query.qu_read_bytes = qu_storage.read_bytes.load() - traffic_start[0];
    query.qu_write_bytes = qu_storage.write_bytes.load() - traffic_start[1];
    query.fu_read_bytes = fu_storage.read_bytes.load() - traffic_start[2];
    query.fu_write_bytes = fu_storage.write_bytes.load() - traffic_start[3];
    active = false;
  }
};
class phase_timer {
  measurement *m_;
  phase phase_;
  std::uint64_t start_ = 0;

public:
  phase_timer(measurement *m, phase p) : m_(m), phase_(p) {
    if (m_)
      start_ = m_->clock();
  }
  ~phase_timer() {
    if (m_)
      m_->query.ns[static_cast<std::size_t>(phase_)] += m_->clock() - start_;
  }
};
} // namespace qcovert
