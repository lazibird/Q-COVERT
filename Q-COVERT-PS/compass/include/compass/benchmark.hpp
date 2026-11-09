#pragma once
#include "compass/codec.hpp"
#include "compass/index.hpp"
namespace compass::benchmark {
#if COMPASS_SCALAR_ID == 2
using scalar = std::uint8_t;
#else
using scalar = float;
#endif
using L = layout<COMPASS_DIM, COMPASS_DEGREE, COMPASS_PQ_SUBSPACES, COMPASS_PQ_CENTROIDS, scalar>;
inline constexpr std::uint64_t request_magic = 0x3530305152504d43ULL; // CMPRQ005
inline constexpr std::uint64_t report_magic = 0x3530305452504d43ULL; // CMPRT005
enum class snapshot_status : std::uint32_t { none = 0, loaded = 1, built = 2 };
struct request {
  parameters p; // ef is the largest job, used only for initial window sizing.
  std::vector<std::size_t> efs;
  oram_parameters o;
  std::size_t threads = 1, warmup = 0, count = 1, repeats = 1;
  std::uint64_t snapshot_flags = 0; // bit 0: enabled; bit 1: force rebuild
  qcovert::snapshot::identity image_id{};
  std::span<const std::uint8_t> dataset, queries;
};
inline request parse(std::span<const std::uint8_t> bytes) {
  qcovert::wire::reader r{bytes};
  const auto magic = r.get<std::uint64_t>();
  if (magic != request_magic)
    throw std::invalid_argument("Compass request version mismatch; rebuild the Compass runner");
  request req;
  req.p.ef = r.get<std::uint64_t>(); req.p.ef_spec = r.get<std::uint64_t>();
  req.p.ef_n = r.get<std::uint64_t>(); req.p.k = r.get<std::uint64_t>();
  auto u32 = [&] { const auto x = r.get<std::uint64_t>(); if (x > UINT32_MAX) throw std::invalid_argument("ORAM parameter overflow"); return std::uint32_t(x); };
  req.o.z = u32(); req.o.s = u32(); req.o.a = u32(); req.o.evict_batch = u32(); req.o.stash = r.get<std::uint64_t>();
  req.threads = r.get<std::uint64_t>(); req.warmup = r.get<std::uint64_t>();
  req.count = r.get<std::uint64_t>(); req.repeats = r.get<std::uint64_t>();
  const auto size = r.get<std::uint64_t>();
  req.p.upper_steps = r.get<std::uint64_t>();
  req.snapshot_flags = r.get<std::uint64_t>();
  req.image_id = r.get<qcovert::snapshot::identity>();
  if (req.snapshot_flags != 0 && req.snapshot_flags != 1 && req.snapshot_flags != 3)
    throw std::invalid_argument("invalid Compass snapshot flags");
  const auto jobs = r.get<std::uint64_t>();
  if (!jobs || jobs > 1024 || jobs > (bytes.size() - r.pos) / sizeof(std::uint64_t))
    throw std::invalid_argument("Compass ef job count/length mismatch");
  std::size_t maximum = 0;
  for (std::size_t i = 0; i < jobs; ++i) {
    const auto ef = r.get<std::uint64_t>();
    if (!ef || ef > SIZE_MAX) throw std::invalid_argument("invalid Compass ef");
    req.efs.push_back(ef); maximum = std::max(maximum, std::size_t(ef));
  }
  if (req.p.ef != maximum || !req.p.ef_spec || !req.p.ef_n || !req.p.k || req.p.upper_steps != 1 ||
      !req.threads || req.threads > 256 || !req.count || !req.repeats || req.repeats > 10000 ||
      req.count > (1000000 / jobs) / req.repeats || req.warmup > 1000000 / jobs || size > bytes.size() - r.pos)
    throw std::invalid_argument("Compass request limits exceeded");
  req.dataset = bytes.subspan(r.pos, size); r.pos += size;
  if (multiply(multiply(req.count, L::dim), sizeof(float)) != bytes.size() - r.pos)
    throw std::invalid_argument("Compass query payload mismatch");
  req.queries = bytes.subspan(r.pos);
  return req;
}
using progress_fn = void (*)(void *, const char *, std::uint64_t, std::uint64_t);
inline std::vector<std::uint8_t> run(std::span<const std::uint8_t> bytes, sn::threads::thread_team &team,
                                    measurement::clock_fn clock, progress_fn progress = nullptr, void *host = nullptr,
                                    qcovert::snapshot::transport *image_io = nullptr,
                                    snapshot_status *image_status = nullptr) {
  const auto req = parse(bytes);
  if (req.threads != team.logical_threads()) throw std::invalid_argument("Compass thread count mismatch");
  if (image_status) *image_status = snapshot_status::none;
  if (!(req.snapshot_flags & 1)) image_io = nullptr;
  if ((req.snapshot_flags & 1) && !image_io)
    throw std::invalid_argument("Compass snapshot transport missing");
  auto report_progress = [&](const char *stage, std::size_t done, std::size_t total) { if (progress) progress(host, stage, done, total); };
  report_progress("compass/setup", 0, 1);
  measurement measured; measured.clock = clock;
  auto data = decode<L>(req.dataset, 100000000);
  const auto setup_begin = clock();
  std::unique_ptr<index<L>> instance;
  if (image_io && image_io->read && !(req.snapshot_flags & 2)) {
    report_progress("compass/restore", 0, 1);
    try {
      qcovert::snapshot::reader image(*image_io, req.image_id);
      instance = std::make_unique<index<L>>(data, req.p, team, req.o, &measured, &image);
      if (image_status) *image_status = snapshot_status::loaded;
      report_progress("compass/restore", 1, 1);
    } catch (const std::exception &) {
      report_progress("compass/snapshot_invalid", 1, 1);
    }
  }
  if (!instance) {
    report_progress("compass/bulk_setup", 0, 1);
    instance = std::make_unique<index<L>>(data, req.p, team, req.o, &measured);
    report_progress("compass/bulk_setup", 1, 1);
    if (image_io && image_io->write) {
      report_progress("compass/snapshot_save", 0, 1);
      qcovert::snapshot::writer image(*image_io, req.image_id);
      instance->save_initial(image);
      image.finish();
      if (image_io->commit) image_io->commit(image_io->context);
      if (image_status) *image_status = snapshot_status::built;
      report_progress("compass/snapshot_save", 1, 1);
    }
  }
  auto &searcher = *instance;
  const auto setup_ns = clock() - setup_begin;
  // Release plaintext ORAM input blocks. The searcher owns the routing cache.
  data.nodes.clear(); data.nodes.shrink_to_fit();
  data.upper_nodes.clear(); data.upper_nodes.shrink_to_fit();
  data.scan_nodes.clear(); data.scan_nodes.shrink_to_fit();
  report_progress("compass/setup", 1, 1);
  auto query = [&](std::size_t i) { typename L::query q{}; std::memcpy(q.data(), req.queries.data() + i * sizeof(q), sizeof(q)); return q; };
  qcovert::wire::writer w;
  for (auto ef : req.efs) {
    searcher.configure_ef(ef);
    const auto suffix = "/ef=" + std::to_string(ef);
    const auto warmup_stage = "compass/warmup" + suffix;
    const auto query_stage = "compass/query" + suffix;
    for (std::size_t i = 0; i < req.warmup; ++i) {
      searcher.search(query(i % req.count)); report_progress(warmup_stage.c_str(), i + 1, req.warmup);
    }
    // Each ef has a self-describing report frame. Setup statistics describe
    // the one shared initialization, repeated as metadata rather than work.
    for (auto x : std::array<std::uint64_t, 18>{report_magic, req.count * req.repeats, req.p.k,
        setup_ns, searcher.rounds(), searcher.batch_size(), searcher.block_bytes(),
        searcher.encrypted_storage_bytes(), searcher.position_map_bytes(), searcher.logical_reads(),
        searcher.upper_reads(), searcher.scan_count(), searcher.routing_bytes(), ef, searcher.upper_block_bytes(), searcher.upper_storage_bytes(),
        searcher.upper_map_bytes(), searcher.capacity()}) w.put(x);
    for (std::size_t repeat = 0; repeat < req.repeats; ++repeat)
      for (std::size_t i = 0; i < req.count; ++i) {
        const auto result = searcher.search(query(i));
        for (auto x : std::array<std::uint64_t, 9>{measured.total_ns, measured.oram_ns, measured.eviction_ns,
            measured.sets_ns, measured.pq_ns, measured.exact_ns, measured.position_ns, searcher.logical_reads(), measured.upper_scan_ns}) w.put(x);
        for (const auto &r : result) { w.put(r.id); w.put(r.distance); }
        report_progress(query_stage.c_str(), repeat * req.count + i + 1, req.count * req.repeats);
      }
  }
  return std::move(w.bytes);
}
} // namespace compass::benchmark
