#include "qcovert/index.hpp"
#include "qcovert/provider.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include <iostream>
int main(int argc, char **argv) {
  try {
    using L = qcovert::layout<3, std::uint8_t>;
    L::dataset prepared;
    if (argc == 2)
      prepared =
          qcovert::decode_dataset<3, std::uint8_t>(qcovert::load_file(argv[1], 16 * 1024 * 1024), 1000, 100);
    else {
      std::vector<L::vector> vectors{{0, 0, 0}, {1, 2, 3}, {7, 8, 9}, {250, 251, 252}};
      std::vector<std::uint64_t> labels{0, 0, 1, 1};
      std::vector<std::array<float, 3>> centers{{.5f, 1, 1.5f}, {128.5f, 129.5f, 130.5f}};
      prepared = qcovert::prepare_dataset<3, std::uint8_t>(vectors, labels, centers, 3, 3);
    }
    sn::threads::pthread_thread_pool pool(3);
    sn::threads::thread_team workers(pool.pool(), 4);
    // Public parameters for a tiny correctness example, not SIFT experiment defaults.
    const auto candidates = prepared.centroids.size() * ((prepared.tau + 1) / 2) * 2;
    qcovert::index<3, std::uint8_t, 2> index(prepared, {prepared.centroids.size(), candidates, 2}, workers);
    for (auto r : index.search({0, 0, 0}))
      std::cout << "id=" << r.id << " squared_l2=" << r.distance << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
