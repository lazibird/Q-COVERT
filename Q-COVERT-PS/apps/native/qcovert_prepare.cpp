#include "qcovert/provider.hpp"
#include <iostream>
// Dimension-generic offline converter; the online index is template-specialized.
int main(int argc, char **argv) {
  try {
    if (argc != 3 && argc != 4) {
      std::cerr << "usage: qcovert_prepare clustered.raw prepared.bin [16-byte-key-file]\n";
      return 2;
    }
    auto bytes = qcovert::load_file(argv[1], std::numeric_limits<std::size_t>::max() / 2);
    qcovert::wire::reader r{bytes};
    if (r.get<std::uint64_t>() != 0x31534C4356435151ULL)
      throw std::invalid_argument("bad clustered input version");
    auto d = r.get<std::uint64_t>(), type = r.get<std::uint64_t>(), n = r.get<std::uint64_t>(),
         nc = r.get<std::uint64_t>(), tau = r.get<std::uint64_t>(), pnc = r.get<std::uint64_t>();
    if (!d || d > 65536 || !n || !nc || nc > n || !tau || pnc < nc || (type != 1 && type != 2))
      throw std::invalid_argument("bad clustered dimensions");
    const std::size_t scalar = type == 1 ? 4 : 1;
    std::size_t remaining = bytes.size() - r.pos;
    if (nc > remaining / (d * 4))
      throw std::invalid_argument("truncated centroids");
    remaining -= nc * d * 4;
    if (n > remaining / (8 + d * scalar) || n * (8 + d * scalar) != remaining)
      throw std::invalid_argument("clustered length mismatch");
    // Do not allow a malicious offline file to request unbounded dummy padding.
    if (pnc > std::max<std::uint64_t>(n, 1024) * 2)
      throw std::invalid_argument("excessive public padding");
    std::vector<float> centers(nc * d);
    for (auto &x : centers)
      x = r.get<float>();
    std::vector<std::uint64_t> labels(n), counts(nc);
    for (auto &cid : labels) {
      cid = r.get<std::uint64_t>();
      if (cid >= nc || ++counts[cid] > tau)
        throw std::invalid_argument("cluster capacity/label mismatch");
    }
    ivf_rabitq::RabitQ quantizer(d, 4, 42);
    const auto padded = quantizer.padded_dim();
    qcovert::wire::writer w;
    w.put(qcovert::wire::magic);
    w.put(d);
    w.put(type);
    w.put(n);
    w.put(pnc);
    w.put(tau);
    for (std::size_t i = 0; i < d; ++i)
      for (std::size_t j = 0; j < padded; ++j)
        w.put(quantizer.rotation_matrix()(i, j));
    for (auto x : quantizer.query_offsets())
      w.put(x);
    for (std::size_t c = 0; c < pnc; ++c) {
      w.put(std::uint64_t(c < nc && counts[c] != 0));
      if (c < nc) {
        for (std::size_t j = 0; j < d; ++j)
          w.put(centers[c * d + j]);
        for (auto x : quantizer.prepare(centers.data() + c * d).values)
          w.put(x);
      } else {
        for (std::size_t j = 0; j < d + padded; ++j)
          w.put(0.0f);
      }
    }
    std::vector<float> vector(d);
    for (std::size_t i = 0; i < n; ++i) {
      w.put(labels[i]);
      for (auto &x : vector) {
        if (type == 1) {
          x = r.get<float>();
          w.put(x);
        } else {
          auto v = r.get<std::uint8_t>();
          x = v;
          w.put(v);
        }
      }
      auto code = quantizer.quantize_vector(vector.data(), centers.data() + labels[i] * d);
      for (auto x : code.signs)
        w.put(x);
      w.put(code.norm);
      w.put(code.x0);
    }
    if (argc == 4) {
      auto key_bytes = qcovert::load_file(argv[3], 16);
      if (key_bytes.size() != 16)
        throw std::invalid_argument("key must be exactly 16 bytes");
      qcovert::key128 key{};
      std::copy(key_bytes.begin(), key_bytes.end(), key.begin());
      auto encrypted = qcovert::encrypt_dataset(w.bytes, key);
      qcovert::save_file(argv[2], encrypted);
    } else
      qcovert::save_file(argv[2], w.bytes);
    std::cout << "Prepared reusable dataset: " << n << " vectors, " << pnc << " cluster slots\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
