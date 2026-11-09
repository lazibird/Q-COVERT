#pragma once
// Experiment-only wire protocol and common native/SGX benchmark runner.
#include "qcovert/codec.hpp"
#include "qcovert/index.hpp"
#include <string>
namespace qcovert::benchmark {
inline constexpr std::uint64_t request_magic = 0x354e555243565151ULL, report_magic = 0x3150455243565151ULL;
enum class snapshot_status : std::uint32_t { none = 0, loaded = 1, built = 2 };
// Invoked with one complete report block each time a (nprobe, k2) job finishes,
// so hosts can persist per-job results while later jobs are still running.
using report_sink_fn = void (*)(void *context, const std::uint8_t *bytes, std::size_t size);
struct job {
  std::uint64_t nprobe = 0, k2 = 0;
  std::vector<std::size_t> rank_partitions;
};
struct request {
  std::uint64_t scalar = 0, dim = 0, k = 0, threads = 0, z = 0, s = 0, a = 0, e = 0, routing_depth = 0,
                fu_e = 0, fu_routing_depth = 0, stash = 0,
                warmup = 0, count = 0, repeats = 0, force_rebuild = 0;
  std::vector<job> jobs;
  snapshot::identity image_id{};
  std::span<const std::uint8_t> prepared, queries;
};
inline request parse(std::span<const std::uint8_t> bytes) {
  wire::reader r{bytes};
  if (r.get<std::uint64_t>() != request_magic)
    throw std::invalid_argument("bad benchmark request");
  request q;
  std::uint64_t *fields[] = {&q.scalar, &q.dim, &q.k,  &q.threads, &q.z,      &q.s,
                             &q.a,      &q.e,   &q.routing_depth, &q.fu_e, &q.fu_routing_depth,
                             &q.stash, &q.warmup, &q.count, &q.repeats};
  for (auto *field : fields)
    *field = r.get<std::uint64_t>();
  const auto pair_count = r.get<std::uint64_t>();
  const auto size = r.get<std::uint64_t>();
  q.force_rebuild = r.get<std::uint64_t>();
  q.image_id = r.get<snapshot::identity>();
  if ((q.scalar != 1 && q.scalar != 2) || (q.dim != 96 && q.dim != 128 && q.dim != 768) || !q.k || q.k > 256 || !q.count || !q.repeats ||
      !q.threads || q.threads > 256 || q.count > 1000000 || q.repeats > 10000 || q.warmup > 1000000 ||
      q.force_rebuild > 1 || q.routing_depth >= 32 || q.fu_routing_depth >= 32 || !q.z || !q.s || !q.a || !q.e ||
      (q.e & (q.e - 1)) || !q.fu_e || (q.fu_e & (q.fu_e - 1)))
    throw std::invalid_argument("invalid benchmark dimensions (require 1 <= k <= 256)");
  if (!pair_count || pair_count > 4096 || pair_count > (bytes.size() - r.pos) / 24)
    throw std::invalid_argument("invalid benchmark job count");
  for (std::uint64_t p = 0; p < pair_count; ++p) {
    job j;
    j.nprobe = r.get<std::uint64_t>();
    j.k2 = r.get<std::uint64_t>();
    const auto partition_count = r.get<std::uint64_t>();
    if (!j.nprobe || j.k2 < q.k || partition_count > j.nprobe || (partition_count && partition_count != j.nprobe))
      throw std::invalid_argument("invalid benchmark job (nprobe/k2/partitions)");
    if (partition_count > (bytes.size() - r.pos) / sizeof(std::uint64_t))
      throw std::invalid_argument("truncated partition quotas");
    std::size_t quota_sum = 0;
    for (std::size_t i = 0; i < partition_count; ++i) {
      const auto quota = r.get<std::uint64_t>();
      if (quota > std::numeric_limits<std::size_t>::max() - quota_sum)
        throw std::invalid_argument("partition quota overflow");
      quota_sum += quota;
      j.rank_partitions.push_back(quota);
    }
    if (partition_count && quota_sum < j.k2)
      throw std::invalid_argument("partition representatives fewer than k2");
    q.jobs.push_back(std::move(j));
  }
  for (auto v : {q.z, q.s, q.a, q.e, q.fu_e})
    if (v > UINT32_MAX)
      throw std::invalid_argument("ORAM argument overflow");
  if (size > bytes.size() - r.pos)
    throw std::invalid_argument("truncated prepared dataset");
  q.prepared = bytes.subspan(r.pos, size);
  r.pos += size;
  q.queries = bytes.subspan(r.pos);
  const auto row_bytes = q.dim * (q.scalar == 1 ? 4 : 1) + q.k * 8;
  if (q.queries.size() != q.count * row_bytes || q.count > 1000000 / q.repeats)
    throw std::invalid_argument("benchmark query/report limit");
  return q;
}
inline std::vector<std::string> fields() {
  std::vector<std::string> names{"repetition", "query_index"};
  for (auto *n : phase_names)
    names.push_back(n);
  for (auto *n : {"online_latency_ns",
                  "total_latency_ns",
                  "call_wall_ns",
                  "qu_stash_peak",
                  "fu_stash_peak",
                  "qu_stash_total_peak",
                  "fu_stash_total_peak",
                  "qu_stash_capacity",
                  "fu_stash_capacity",
                  "qu_pending_slots",
                  "fu_pending_slots",
                  "fu_pending_real",
                  "valid_quantized_candidates",
                  "qu_read_bytes",
                  "qu_write_bytes",
                  "fu_read_bytes",
                  "fu_write_bytes",
                  "recall_hits",
                  "cluster_recall_hits",
                  "candidate_recall_hits",
                  "prepared_decode_ns",
                  "setup_total_ns",
                  "fu_setup_ns",
                  "qu_setup_ns",
                  "qu_capacity",
                  "fu_capacity",
                  "qu_storage_bytes",
                  "fu_storage_bytes",
                  "qu_window",
                  "fu_window",
                  "qu_eviction_chunks",
                  "fu_eviction_chunks",
                  "qu_evicted_paths",
                  "fu_evicted_paths",
                  "qu_padding_blocks",
                  "fu_padding_blocks",
                  "qu_reshuffles",
                  "fu_reshuffles",
                  "measured_loop_ns"})
    names.emplace_back(n);
  return names;
}
template <std::size_t D, class T>
std::vector<std::uint8_t> run_typed(const request &req, sn::threads::thread_team &workers,
                                    measurement::clock_fn clock, measurement::progress_fn progress,
                                    void *progress_context, snapshot::transport *image_io,
                                    snapshot_status *image_status, report_sink_fn report_sink = nullptr,
                                    void *report_sink_context = nullptr) {
  using L = layout<D, T>;
  if (progress)
    progress(progress_context, "runner/decode", 0, 1);
  const auto decode_start = clock();
  auto data = decode_dataset<D, T>(req.prepared, 100000000, 1000000);
  const auto decode_ns = clock() - decode_start;
  if (progress)
    progress(progress_context, "runner/decode", 1, 1);
  wire::reader reader{req.queries};
  std::vector<typename L::vector> queries(req.count);
  const auto k = static_cast<std::size_t>(req.k);
  std::vector<std::vector<std::uint64_t>> truth(req.count, std::vector<std::uint64_t>(k));
  for (std::size_t i = 0; i < req.count; ++i) {
    queries[i] = reader.get<typename L::vector>();
    for (std::size_t j = 0; j < k; ++j)
      truth[i][j] = reader.get<std::uint64_t>();
    for (std::size_t j = 0; j < k; ++j) {
      if (truth[i][j] >= data.entries.size())
        throw std::invalid_argument("groundtruth ID out of range");
      for (std::size_t j2 = 0; j2 < j; ++j2)
        if (truth[i][j] == truth[i][j2])
          throw std::invalid_argument("duplicate groundtruth ID");
    }
  }
  measurement m;
  m.clock = clock;
  m.progress = progress;
  m.progress_context = progress_context;
  using index_t = index<D, T, 32>;
  const oram_parameters params{std::uint32_t(req.z), std::uint32_t(req.s), std::uint32_t(req.a),
                               std::uint32_t(req.e), req.stash, std::uint32_t(req.routing_depth),
                               std::uint32_t(req.fu_e), std::uint32_t(req.fu_routing_depth)};
  // One ORAM initialization serves every job: client windows are sized by the
  // largest job, and each job reconfigures only its query workspace/options.
  std::size_t max_nprobe = 0, max_k2 = 0;
  for (const auto &j : req.jobs) {
    max_nprobe = std::max(max_nprobe, std::size_t(j.nprobe));
    max_k2 = std::max(max_k2, std::size_t(j.k2));
  }
  const query_options initial{max_nprobe, max_k2, req.k, {}};
  std::unique_ptr<index_t> instance;
  if (image_io && image_io->read && !req.force_rebuild) {
    try {
      snapshot::reader image(*image_io, req.image_id);
      instance = std::make_unique<index_t>(data, initial, workers, params, &m, &image);
      if (image_status) *image_status = snapshot_status::loaded;
    } catch (const std::bad_alloc &) {
      // Out of enclave heap is not a snapshot problem; rebuilding would fail
      // the same way after wasting a full Setup, so fail fast instead.
      throw;
    } catch (const std::exception &) {
      m.report_progress("setup/snapshot_invalid", 1, 1);
    }
  }
  if (!instance) {
    instance = std::make_unique<index_t>(data, initial, workers, params, &m);
    if (image_io && image_io->write) {
      m.report_progress("setup/snapshot_save", 0, 1);
      snapshot::writer image(*image_io, req.image_id);
      instance->save_initial(image, data.tau);
      image.finish();
      if (image_io->commit) image_io->commit(image_io->context);
      m.report_progress("setup/snapshot_save", 1, 1);
      if (image_status) *image_status = snapshot_status::built;
    }
  }
  auto &idx = *instance;
  auto names = fields();
  wire::writer report;
  const auto total_queries = req.count * req.repeats;
  const auto report_step = std::max<std::uint64_t>(1, total_queries / 100);
  for (const auto &job : req.jobs) {
    idx.configure_query({job.nprobe, job.k2, req.k, job.rank_partitions});
    m.report_progress("runner/warmup", 0, req.warmup ? req.warmup : 1);
    for (std::size_t i = 0; i < req.warmup; ++i) {
      (void)idx.search(queries[i % req.count]);
      m.report_progress("runner/warmup", i + 1, req.warmup);
    }
    if (!req.warmup)
      m.report_progress("runner/warmup", 1, 1);
    const auto block_begin = report.bytes.size();
    report.put(report_magic);
    report.put(std::uint64_t(names.size()));
    report.put(total_queries);
    for (const auto &name : names) {
      report.put(std::uint64_t(name.size()));
      for (char c : name)
        report.put(c);
    }
    const auto row_data_begin = report.bytes.size();
    const auto loop_begin = clock();
    m.report_progress("runner/queries", 0, total_queries);
    for (std::size_t rep = 0; rep < req.repeats; ++rep)
      for (std::size_t i = 0; i < req.count; ++i) {
        const auto call_begin = clock();
        auto results =
            idx.search(queries[i]); // Includes both flushes; next query cannot inherit this eviction.
        const auto call_wall = clock() - call_begin;
        const auto &q = m.query;
        std::uint64_t hits = 0, chits = 0, khits = 0;
        for (auto id : truth[i]) {
          hits += std::any_of(results.begin(), results.end(), [&](auto r) { return r.id == id; });
          chits += std::find(m.selected_clusters.begin(), m.selected_clusters.end(), data.entries[id].cid) !=
                   m.selected_clusters.end();
          khits += std::find(m.selected_candidates.begin(), m.selected_candidates.end(), id) !=
                   m.selected_candidates.end();
        }
        std::vector<std::uint64_t> row{rep, i};
        row.insert(row.end(), q.ns.begin(), q.ns.end());
        auto qu_window = idx.quantized_oram().options().disjoint_epoch_window,
             fu_window = idx.full_oram().options().disjoint_epoch_window;
        const std::uint64_t qu_paths = req.e * (std::uint64_t{1} << req.routing_depth),
                            fu_paths = req.fu_e * (std::uint64_t{1} << req.fu_routing_depth);
        const auto qu_chunk = req.a * qu_paths, fu_chunk = req.a * fu_paths;
        for (auto value : {q.online_ns,
                           q.total_ns,
                           call_wall,
                           q.qu_stash_peak,
                           q.fu_stash_peak,
                           q.qu_stash_total_peak,
                           q.fu_stash_total_peak,
                           q.qu_stash_capacity,
                           q.fu_stash_capacity,
                           q.qu_pending_slots,
                           q.fu_pending_slots,
                           q.fu_pending_real,
                           q.valid_quantized,
                           q.qu_read_bytes,
                           q.qu_write_bytes,
                           q.fu_read_bytes,
                           q.fu_write_bytes,
                           hits,
                           chits,
                           khits,
                           decode_ns,
                           m.setup_ns,
                           m.fu_setup_ns,
                           m.qu_setup_ns,
                           idx.quantized_oram().options().block_count,
                           idx.full_oram().options().block_count,
                           m.qu_storage.allocated_bytes.load(),
                           m.fu_storage.allocated_bytes.load(),
                           qu_window,
                           fu_window,
                           qu_window / qu_chunk,
                           fu_window / fu_chunk,
                           qu_window / qu_chunk * qu_paths,
                           fu_window / fu_chunk * fu_paths,
                           qu_window - q.qu_pending_slots,
                           fu_window - q.fu_pending_slots,
                           q.qu_reshuffles,
                           q.fu_reshuffles,
                           std::uint64_t{0}})
          row.push_back(value);
        if (row.size() != names.size())
          throw std::logic_error("benchmark report schema mismatch");
        for (auto value : row)
          report.put(value);
        const auto completed = rep * req.count + i + 1;
        if (completed % report_step == 0 || completed == total_queries)
          m.report_progress("runner/queries", completed, total_queries);
      }
    const auto loop_ns = clock() - loop_begin;
    for (std::size_t i = 0; i < total_queries; ++i)
      std::memcpy(report.bytes.data() + row_data_begin + (i * names.size() + names.size() - 1) * 8, &loop_ns, 8);
    if (report_sink)
      report_sink(report_sink_context, report.bytes.data() + block_begin, report.bytes.size() - block_begin);
  }
  return std::move(report.bytes);
}
inline std::vector<std::uint8_t> run(std::span<const std::uint8_t> input, sn::threads::thread_team &workers,
                                     measurement::clock_fn clock, measurement::progress_fn progress = nullptr,
                                     void *progress_context = nullptr, snapshot::transport *image_io = nullptr,
                                     snapshot_status *image_status = nullptr, report_sink_fn report_sink = nullptr,
                                     void *report_sink_context = nullptr) {
  auto q = parse(input);
  if (q.threads != workers.logical_threads())
    throw std::invalid_argument("thread count mismatch");
  if (q.dim == 96)
    return q.scalar == 1 ? run_typed<96, float>(q, workers, clock, progress, progress_context, image_io,
                                                image_status, report_sink, report_sink_context)
                         : run_typed<96, std::uint8_t>(q, workers, clock, progress, progress_context, image_io,
                                                       image_status, report_sink, report_sink_context);
  if (q.dim == 128)
    return q.scalar == 1 ? run_typed<128, float>(q, workers, clock, progress, progress_context, image_io,
                                                 image_status, report_sink, report_sink_context)
                         : run_typed<128, std::uint8_t>(q, workers, clock, progress, progress_context, image_io,
                                                        image_status, report_sink, report_sink_context);
  if (q.scalar != 1)
    throw std::invalid_argument("768-dimensional benchmark requires float32 storage");
  return run_typed<768, float>(q, workers, clock, progress, progress_context, image_io, image_status,
                               report_sink, report_sink_context);
}
} // namespace qcovert::benchmark
