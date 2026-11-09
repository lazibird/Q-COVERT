#include <sys/stat.h>
#include "compass/benchmark.hpp"
#include "compass/progress.hpp"
#include "qcovert/snapshot_file.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
namespace {
std::uint64_t clock_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void progress(void *, const char *stage, std::uint64_t done, std::uint64_t total) {
  compass::host_progress().update(stage, done, total);
}
}
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::strcmp(argv[1], "--describe-layout") == 0) {
      using L = compass::benchmark::L;
      std::cout << "{\"dimension\":" << L::dim << ",\"degree\":" << L::degree
                << ",\"pq_subspaces\":" << L::subspaces << ",\"pq_centroids\":" << L::centroids
                << ",\"scalar\":\"" << (sizeof(L::scalar) == 1 ? "uint8" : "float32")
                << "\",\"mode\":\"native\",\"snapshot_protocol\":4,\"search_protocol\":5,\"prepared_protocol\":3}\n";
      return 0;
    }
    if (argc != 3 && argc != 4)
      throw std::invalid_argument("usage: compass_benchmark request.bin report.bin [initial.state] (see compass/benchmark.py)");
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in || in.tellg() < 0) throw std::runtime_error("cannot open Compass request");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(in.tellg())); in.seekg(0);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) throw std::runtime_error("cannot read request");
    const auto req = compass::benchmark::parse(bytes);
    sn::threads::pthread_thread_pool pool(req.threads - 1);
    sn::threads::thread_team team(pool.pool(), req.threads);
    std::unique_ptr<qcovert::snapshot::file_transport> image_file;
    qcovert::snapshot::transport image_io{};
    compass::benchmark::snapshot_status image_status = compass::benchmark::snapshot_status::none;
    if (argc == 4) {
      image_file = std::make_unique<qcovert::snapshot::file_transport>(argv[3]);
      image_io = {image_file.get(), qcovert::snapshot::file_transport::read_callback,
                  qcovert::snapshot::file_transport::write_callback,
                  qcovert::snapshot::file_transport::commit_callback,
                  qcovert::snapshot::raw_protect, qcovert::snapshot::raw_unprotect};
    }
    const auto report = compass::benchmark::run(bytes, team, clock_ns, progress, nullptr,
                                               image_file ? &image_io : nullptr, &image_status);
    std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
    if (!out.write(reinterpret_cast<const char *>(report.data()), report.size())) throw std::runtime_error("cannot write report");
    rusage usage{}; getrusage(RUSAGE_SELF, &usage);
    std::cout << "oram_snapshot_status=" << (image_status == compass::benchmark::snapshot_status::loaded ? "loaded" :
                 image_status == compass::benchmark::snapshot_status::built ? "built" : "none") << '\n';
    std::cout << "peak_host_rss_bytes=" << std::uint64_t(usage.ru_maxrss) * 1024 << '\n';
    return 0;
  } catch (const std::exception &e) {
    compass::host_progress().finish();
    std::cerr << e.what() << '\n'; return 1;
  }
}
