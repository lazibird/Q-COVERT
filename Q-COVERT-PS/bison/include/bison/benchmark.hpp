#pragma once
#include "bison/codec.hpp"
#include "bison/index.hpp"
namespace bison::benchmark {
#if BISON_SCALAR_ID == 2
using scalar = std::uint8_t;
#else
using scalar = float;
#endif
using L = layout<BISON_DIM, BISON_DEGREE, BISON_PQ_BYTES, scalar>;
inline constexpr std::uint64_t request_magic = 0x3230305152534942ULL; // BISRQ002
inline constexpr std::uint64_t report_magic = 0x3230305452534942ULL; // BISRT002
enum class snapshot_status : std::uint32_t { none = 0, loaded = 1, built = 2 };
struct request {
  std::vector<parameters> jobs;
  oram_parameters o;
  std::size_t threads = 1, warmup = 0, count = 1, repeats = 1;
  std::uint64_t snapshot_flags = 0;
  qcovert::snapshot::identity image_id{};
  std::span<const std::uint8_t> dataset, queries;
};
inline request parse(std::span<const std::uint8_t> bytes) {
  qcovert::wire::reader r{bytes};
  if (r.get<std::uint64_t>() != request_magic) throw std::invalid_argument("BiSON request version mismatch");
  std::array<std::uint64_t, 3> maxima{};
  for (auto &x : maxima) x = r.get<std::uint64_t>();
  const auto k = r.get<std::uint64_t>();
  request req;
  auto u32 = [&] { const auto x = r.get<std::uint64_t>(); if (x > UINT32_MAX) throw std::invalid_argument("BiSON ORAM parameter overflow"); return std::uint32_t(x); };
  req.o.z = u32(); req.o.s = u32(); req.o.a = u32(); req.o.evict_batch = u32(); req.o.stash = r.get<std::uint64_t>();
  req.threads = r.get<std::uint64_t>(); req.warmup = r.get<std::uint64_t>();
  const auto size = r.get<std::uint64_t>(), reserved = r.get<std::uint64_t>();
  req.count = r.get<std::uint64_t>(); req.repeats = r.get<std::uint64_t>();
  req.snapshot_flags = r.get<std::uint64_t>(); req.image_id = r.get<qcovert::snapshot::identity>();
  const auto count = r.get<std::uint64_t>();
  if (!count || count > 1024 || !k || k > 65536 || reserved || !req.threads || req.threads > 256 ||
      !req.count || !req.repeats || req.repeats > 10000 || req.count > (1000000 / count) / req.repeats ||
      req.warmup > 1000000 / count || (req.snapshot_flags != 0 && req.snapshot_flags != 1 && req.snapshot_flags != 3))
    throw std::invalid_argument("BiSON request limits exceeded");
  std::array<std::uint64_t, 3> actual{};
  for (std::size_t i = 0; i < count; ++i) {
    const auto t = r.get<std::uint64_t>(), w = r.get<std::uint64_t>(), l = r.get<std::uint64_t>();
    if (!t || !w || l < k) throw std::invalid_argument("invalid BiSON parameter job");
    add(add(l, multiply(w, L::degree)), multiply(t, w));
    req.jobs.push_back({std::size_t(t), std::size_t(w), std::size_t(l), std::size_t(k)});
    actual[0] = std::max(actual[0], t); actual[1] = std::max(actual[1], w); actual[2] = std::max(actual[2], l);
  }
  if (actual != maxima || size > bytes.size() - r.pos) throw std::invalid_argument("BiSON job/dataset size mismatch");
  req.dataset = bytes.subspan(r.pos, size); r.pos += size;
  if (multiply(multiply(req.count, L::dim), sizeof(float)) != bytes.size() - r.pos)
    throw std::invalid_argument("BiSON query payload mismatch");
  req.queries = bytes.subspan(r.pos);
  return req;
}
using progress_fn = void (*)(void *, const char *, std::uint64_t, std::uint64_t);
using report_flush_fn = void (*)(void *, const std::uint8_t *, std::size_t);
inline std::vector<std::uint8_t> run(std::span<const std::uint8_t> bytes, sn::threads::thread_team &team,
    measurement::clock_fn clock, progress_fn progress = nullptr, void *host = nullptr,
    qcovert::snapshot::transport *image_io = nullptr, snapshot_status *image_status = nullptr,
    report_flush_fn flush = nullptr, void *flush_ctx = nullptr) {
  const auto req = parse(bytes);
  if (req.threads != team.logical_threads()) throw std::invalid_argument("BiSON thread count mismatch");
  if (image_status) *image_status = snapshot_status::none;
  if (!(req.snapshot_flags & 1)) image_io = nullptr;
  if ((req.snapshot_flags & 1) && !image_io) throw std::invalid_argument("BiSON snapshot transport missing");
  auto emit = [&](const char *stage, std::size_t done, std::size_t total) { if (progress) progress(host, stage, done, total); };
  emit("bison/setup", 0, 1);
  measurement measured; measured.clock = clock;
  auto data = decode<L>(req.dataset, 100000000);
  std::size_t maximum_reads = 0, maximum_width = 0;
  for (auto p : req.jobs) {
    maximum_reads = std::max(maximum_reads, multiply(p.iterations, p.beamwidth));
    maximum_width = std::max(maximum_width, p.beamwidth);
  }
  const auto setup_begin = clock();
  std::unique_ptr<index<L>> instance;
  if (image_io && image_io->read && !(req.snapshot_flags & 2)) {
    emit("bison/restore", 0, 1);
    try {
      qcovert::snapshot::reader image(*image_io, req.image_id);
      instance = std::make_unique<index<L>>(data, req.jobs[0], team, req.o, &measured, &image, maximum_reads, maximum_width);
      if (image_status) *image_status = snapshot_status::loaded;
      emit("bison/restore", 1, 1);
    } catch (const std::exception &) { emit("bison/snapshot_invalid", 1, 1); }
  }
  if (!instance) {
    emit("bison/bulk_setup", 0, 1);
    instance = std::make_unique<index<L>>(data, req.jobs[0], team, req.o, &measured, nullptr, maximum_reads, maximum_width);
    emit("bison/bulk_setup", 1, 1);
    if (image_io && image_io->write) {
      qcovert::snapshot::writer image(*image_io, req.image_id);
      instance->save_initial(image); image.finish();
      if (image_io->commit) image_io->commit(image_io->context);
      if (image_status) *image_status = snapshot_status::built;
    }
  }
  const auto setup_ns = clock() - setup_begin;
  data.nodes.clear(); data.nodes.shrink_to_fit(); data.cache.clear(); data.cache.shrink_to_fit();
  emit("bison/setup", 1, 1);
  auto query = [&](std::size_t i) { typename L::query q{}; std::memcpy(q.data(), req.queries.data() + i * sizeof(q), sizeof(q)); return q; };
  qcovert::wire::writer report;
  std::size_t flushed = 0;
  for (auto p : req.jobs) {
    auto &searcher = *instance; searcher.configure(p);
    const auto suffix = "/T=" + std::to_string(p.iterations) + "/W=" + std::to_string(p.beamwidth) + "/L=" + std::to_string(p.l_search);
    const auto warmup_stage = "bison/warmup" + suffix, query_stage = "bison/query" + suffix;
    for (std::size_t i = 0; i < req.warmup; ++i) {
      searcher.search(query(i % req.count)); emit(warmup_stage.c_str(), i + 1, req.warmup);
    }
    for (auto x : std::array<std::uint64_t, 14>{report_magic, req.count * req.repeats, p.k, setup_ns,
        p.iterations, p.beamwidth, searcher.block_bytes(), searcher.encrypted_storage_bytes(),
        searcher.position_map_bytes(), searcher.logical_reads(), searcher.cached_rounds(),
        searcher.cache_count(), searcher.cache_bytes(), p.l_search}) report.put(x);
    for (std::size_t repeat = 0; repeat < req.repeats; ++repeat)
      for (std::size_t i = 0; i < req.count; ++i) {
        const auto results = searcher.search(query(i));
        for (auto x : std::array<std::uint64_t, 9>{measured.total_ns, measured.oram_ns, measured.eviction_ns,
            measured.sets_ns, measured.pq_ns, measured.exact_ns, measured.position_ns,
            searcher.logical_reads(), measured.cache_ns}) report.put(x);
        for (const auto &r : results) { report.put(r.id); report.put(r.distance); }
        emit(query_stage.c_str(), repeat * req.count + i + 1, req.count * req.repeats);
      }
    if (flush && report.bytes.size() > flushed) {
      flush(flush_ctx, report.bytes.data() + flushed, report.bytes.size() - flushed);
      flushed = report.bytes.size();
    }
  }
  return std::move(report.bytes);
}
} // namespace bison::benchmark
