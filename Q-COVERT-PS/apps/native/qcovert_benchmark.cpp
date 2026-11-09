#include <sys/stat.h>
#include "qcovert/benchmark.hpp"
#include "qcovert/snapshot_file.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
namespace {
std::uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void report_progress(void *, const char *stage, std::uint64_t completed, std::uint64_t total) {
  std::cerr << "QCOVERT_PROGRESS\t" << stage << '\t' << completed << '\t' << total << '\n' << std::flush;
}
// Append each finished job's report block so the driver can record per-job
// results while the runner is still working on later jobs.
struct report_file {
  std::ofstream stream;
  static void sink(void *context, const std::uint8_t *bytes, std::size_t size) {
    auto *self = static_cast<report_file *>(context);
    self->stream.write(reinterpret_cast<const char *>(bytes), size);
    self->stream.flush();
    if (!self->stream)
      throw std::runtime_error("cannot stream benchmark report");
  }
};
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 3 && argc != 4) {
      std::cerr << "Use tools/benchmark.py (runner arguments: input.bin output.bin [initial.state])\n";
      return 2;
    }
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input)
      throw std::runtime_error("cannot open request");
    auto length = input.tellg();
    if (length < 0)
      throw std::runtime_error("bad request length");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
      throw std::runtime_error("cannot read request");
    auto request = qcovert::benchmark::parse(bytes);
    sn::threads::pthread_thread_pool pool(request.threads - 1);
    sn::threads::thread_team workers(pool.pool(), request.threads);
    std::unique_ptr<qcovert::snapshot::file_transport> image_file;
    qcovert::snapshot::transport image_io{};
    qcovert::benchmark::snapshot_status image_status = qcovert::benchmark::snapshot_status::none;
    if (argc == 4) {
      image_file = std::make_unique<qcovert::snapshot::file_transport>(argv[3]);
      image_io = {image_file.get(), qcovert::snapshot::file_transport::read_callback,
                  qcovert::snapshot::file_transport::write_callback,
                  qcovert::snapshot::file_transport::commit_callback,
                  qcovert::snapshot::raw_protect, qcovert::snapshot::raw_unprotect};
    }
    report_file output{std::ofstream(argv[2], std::ios::binary | std::ios::trunc)};
    if (!output.stream)
      throw std::runtime_error("cannot create report file");
    qcovert::benchmark::run(bytes, workers, now, report_progress, nullptr,
                            image_file ? &image_io : nullptr, &image_status, &report_file::sink, &output);
    std::cout << "oram_snapshot_status="
              << (image_status == qcovert::benchmark::snapshot_status::loaded ? "loaded" :
                  image_status == qcovert::benchmark::snapshot_status::built ? "built" : "none") << '\n';
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    std::cout << "peak_host_rss_bytes=" << std::uint64_t(usage.ru_maxrss) * 1024 << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
