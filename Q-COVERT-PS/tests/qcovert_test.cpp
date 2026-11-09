#include "qcovert/index.hpp"
#include "qcovert/provider.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include <algorithm>
#include <iostream>
#include <random>
void check(bool ok, const char *what) {
  if (!ok)
    throw std::runtime_error(what);
}
void test_topk() {
  std::mt19937 rng(17);
  for (std::size_t n = 1; n < 85; ++n)
    for (std::size_t k = 0; k <= n; ++k) {
      std::vector<qcovert::result> data(n);
      for (std::size_t i = 0; i < n; ++i)
        data[i] = {i, double(rng() % 9)};
      auto expected = data;
      auto less = [](const auto &a, const auto &b) {
        return (a.distance < b.distance) | ((a.distance == b.distance) & (a.id < b.id));
      };
      std::sort(expected.begin(), expected.end(), less);
      qcovert::exact_topk(data, k, qcovert::result{}, less);
      check(data.size() == k, "topk size");
      for (std::size_t i = 0; i < k; ++i)
        check(data[i].id == expected[i].id, "topk wrong");
    }
}
void test_partition_topk() {
  std::vector<qcovert::result> values;
  for (std::size_t i = 0; i < 20; ++i)
    values.push_back({i, double((i * 7) % 19)});
  const auto less = [](const auto &a, const auto &b) {
    return (a.distance < b.distance) | ((a.distance == b.distance) & (a.id < b.id));
  };
  std::vector<qcovert::result> reference;
  const std::array<std::size_t, 2> quotas{3, 4};
  for (std::size_t rank = 0; rank < quotas.size(); ++rank)
    for (std::size_t part = 0; part < quotas[rank]; ++part) {
      auto best = qcovert::result{};
      for (std::size_t j = part * 10 / quotas[rank]; j < (part + 1) * 10 / quotas[rank]; ++j)
        if (less(values[rank * 10 + j], best)) best = values[rank * 10 + j];
      reference.push_back(best);
    }
  std::sort(reference.begin(), reference.end(), less);
  qcovert::partition_topk(values, 5, 10, quotas, qcovert::result{}, less);
  check(values.size() == 5, "partition result count");
  for (std::size_t i = 0; i < 5; ++i)
    check(values[i].id == reference[i].id, "partition topk wrong");
  bool rejected = false;
  try {
    qcovert::partition_topk(values, 5, 10, quotas, qcovert::result{}, less);
  } catch (const std::invalid_argument &) { rejected = true; }
  check(rejected, "partition shape accepted");
}
void test_compact_topk() {
  // Include a large payload, non-power-of-two inputs, invalid centroids,
  // repeated distances, and every possible k (including zero/all-input).
  struct item {
    std::array<std::uint64_t, 129> payload{};
    std::uint64_t score = UINT64_MAX, id = UINT64_MAX, counter = 0;
  };
  const auto key = [](const item &x) { return std::array<std::uint64_t, 2>{x.score, x.id}; };
  const auto less = [](const auto &a, const auto &b) {
    return sn::obliv::ct_lt(a[0], b[0]) |
           (sn::obliv::ct_eq(a[0], b[0]) & sn::obliv::ct_lt(a[1], b[1]));
  };
  std::mt19937 rng(57);
  qcovert::compact_topk_workspace<item> workspace;
  for (auto n : {1u, 2u, 3u, 7u, 17u, 33u, 65u}) {
    std::vector<item> source(n);
    for (std::size_t i = 0; i < n; ++i) {
      source[i].id = i;
      source[i].score = i % 5 == 0 ? UINT64_MAX : rng() % 9;
      source[i].counter = rng();
      for (auto &x : source[i].payload) x = rng();
    }
    for (std::size_t k = 0; k <= n; ++k) {
      auto expected = source, actual = source;
      qcovert::exact_topk(expected, k, item{}, [&](const auto &a, const auto &b) { return less(key(a), key(b)); });
      qcovert::exact_topk_compact(actual, k, item{}, key, less, workspace);
      check(actual.size() == k, "compact topk size");
      for (std::size_t i = 0; i < k; ++i)
        check(actual[i].id == expected[i].id && actual[i].score == expected[i].score &&
              actual[i].counter == expected[i].counter && actual[i].payload == expected[i].payload,
              "compact topk changed key/payload association");
    }
  }
  bool rejected = false;
  std::vector<item> empty;
  try { qcovert::exact_topk_compact(empty, 1, item{}, key, less); }
  catch (const std::invalid_argument &) { rejected = true; }
  check(rejected, "compact topk accepted excessive k");
}
template <class Scalar> void test_pipeline(std::uint32_t R = 0, std::uint32_t E = 2) {
  constexpr std::size_t D = 3;
  using L = qcovert::layout<D, Scalar>;
  std::vector<typename L::vector> vectors{{0, 0, 0},    {0, 0, 0},       {2, 3, 5},      {7, 8, 9},
                                          {10, 20, 30}, {255, 254, 250}, {200, 201, 202}};
  std::vector<std::uint64_t> labels{0, 0, 0, 1, 1, 2, 2};
  std::vector<std::array<float, D>> centroids{{0, 0, 0}, {8.5f, 14, 19.5f}, {227.5f, 227.5f, 226}};
  auto data = qcovert::prepare_dataset<D, Scalar>(vectors, labels, centroids, 3, 4);
  auto serialized = qcovert::encode_dataset<D, Scalar>(data);
  qcovert::key128 key{};
  sn::crypto::random_device{}.fill(key.data(), key.size());
  auto encrypted = qcovert::encrypt_dataset(serialized, key);
  auto decoded = qcovert::decrypt_dataset(encrypted, key);
  check(decoded == serialized, "encrypted file roundtrip");
  auto loaded = qcovert::decode_dataset<D, Scalar>(decoded, 100, 10);
  encrypted.back() ^= 1;
  bool rejected = false;
  try {
    (void)qcovert::decrypt_dataset(encrypted, key);
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, "tampering not rejected");
  sn::threads::pthread_thread_pool pool(3);
  sn::threads::thread_team team(pool.pool(), 4);
  qcovert::index<D, Scalar, 2> index(loaded, {4, 16, 9}, team, {8, 16, 2, E, 0, R});
  check(index.quantized_oram().shape().subtree_count == (1U << R), "forest routing depth ignored");
  for (std::size_t round = 0; round < 16; ++round) {
    typename L::vector query{};
    for (std::size_t d = 0; d < D; ++d)
      query[d] = Scalar((round * 17 + d * 11) % 256);
    if (round == 0)
      query = {0, 0, 0};
    auto actual = index.search(query, round % 2 == 0);
    std::vector<qcovert::result> expected;
    for (std::size_t i = 0; i < vectors.size(); ++i) {
      double sum = 0;
      for (std::size_t d = 0; d < D; ++d) {
        double delta = double(query[d]) - double(vectors[i][d]);
        sum += delta * delta;
      }
      expected.push_back({i, sum});
    }
    std::sort(expected.begin(), expected.end(), [](auto a, auto b) {
      return a.distance < b.distance || (a.distance == b.distance && a.id < b.id);
    });
    for (std::size_t i = 0; i < vectors.size(); ++i) {
      check(actual[i].id == expected[i].id, "pipeline ID");
      check(actual[i].distance == expected[i].distance, "pipeline squared L2");
    }
    for (std::size_t i = vectors.size(); i < actual.size(); ++i)
      check(actual[i].id == qcovert::invalid_id, "dummy result");
    auto stats = index.stats();
    check(stats.quantized_reads == 8 && stats.full_reads == 16 && stats.quantized_candidates == 16,
          "public counts");
  }
  index.finish();
  qcovert::index<D, Scalar, 2> partitioned(loaded, {4, 16, 9, {4, 4, 4, 4}}, team,
                                            {8, 16, 2, E, 0, R});
  for (std::size_t round = 0; round < 3; ++round) {
    typename L::vector query{};
    for (std::size_t d = 0; d < D; ++d)
      query[d] = Scalar((round * 17 + d * 11) % 256);
    const auto actual = partitioned.search(query);
    std::vector<qcovert::result> expected;
    for (std::size_t i = 0; i < vectors.size(); ++i) {
      double sum = 0;
      for (std::size_t d = 0; d < D; ++d) {
        const double delta = double(query[d]) - double(vectors[i][d]);
        sum += delta * delta;
      }
      expected.push_back({i, sum});
    }
    std::sort(expected.begin(), expected.end(), [](auto a, auto b) {
      return a.distance < b.distance || (a.distance == b.distance && a.id < b.id);
    });
    for (std::size_t i = 0; i < expected.size(); ++i)
      check(actual[i].id == expected[i].id, "full-partition topk differs from exact");
  }
  partitioned.finish();
  // Restrict both centroid probes and quantized candidates. Independently build
  // the non-oblivious IVF/RaBitQ reference to exercise stages 1 and 2.
  qcovert::index<D, Scalar, 2> approximate(loaded, {2, 3, 2}, team);
  for (std::size_t round = 0; round < 12; ++round) {
    typename L::vector query{};
    for (std::size_t d = 0; d < D; ++d)
      query[d] = Scalar((round * 19 + d * 7) % 256);
    std::vector<std::pair<double, std::size_t>> cs;
    for (std::size_t c = 0; c < centroids.size(); ++c) {
      double sum = 0;
      for (std::size_t d = 0; d < D; ++d) {
        double x = double(query[d]) - centroids[c][d];
        sum += x * x;
      }
      cs.push_back({sum, c});
    }
    std::sort(cs.begin(), cs.end());
    ivf_rabitq::RabitQ quantizer(D);
    std::array<float, D> fp{};
    for (std::size_t d = 0; d < D; ++d)
      fp[d] = float(query[d]);
    std::vector<std::pair<double, std::size_t>> qs;
    for (std::size_t c = 0; c < 2; ++c) {
      auto qq = quantizer.quantize_query(quantizer.prepare(fp.data()),
                                         quantizer.prepare(centroids[cs[c].second].data()));
      for (std::size_t i = 0; i < vectors.size(); ++i)
        if (labels[i] == cs[c].second) {
          const auto &code = loaded.entries[i].quantized;
          ivf_rabitq::BinaryCode bc{{code.signs.begin(), code.signs.end()}, code.norm, code.x0};
          qs.push_back({quantizer.approximate_distance(qq, bc), i});
        }
    }
    std::sort(qs.begin(), qs.end());
    qs.resize(3);
    for (auto &[score, id] : qs) {
      score = 0;
      for (std::size_t d = 0; d < D; ++d) {
        double x = double(query[d]) - double(vectors[id][d]);
        score += x * x;
      }
    }
    std::sort(qs.begin(), qs.end());
    auto actual = approximate.search(query);
    for (std::size_t i = 0; i < 2; ++i)
      check(actual[i].id == qs[i].second && actual[i].distance == qs[i].first, "ANN reference");
  }
}
template <std::size_t D> void test_quantization() {
  using L = qcovert::layout<D, float>;
  ivf_rabitq::RabitQ quantizer(D);
  std::array<float, D> center{}, vector{}, query{};
  for (std::size_t i = 0; i < D; ++i) {
    center[i] = float(i % 7) - 3;
    vector[i] = float(i % 11) - 5;
    query[i] = float(i % 13) - 6;
  }
  auto pc = quantizer.prepare(center.data()), pq = quantizer.prepare(query.data());
  typename L::centroid c{};
  c.values = center;
  std::copy(pc.values.begin(), pc.values.end(), c.rotated.begin());
  std::array<float, L::padded> rotated{}, offsets{};
  std::copy(pq.values.begin(), pq.values.end(), rotated.begin());
  std::copy(quantizer.query_offsets().begin(), quantizer.query_offsets().end(), offsets.begin());
  auto native_query = quantizer.quantize_query(pq, pc);
  auto native_code = quantizer.quantize_vector(vector.data(), center.data());
  auto online_query = qcovert::prepare_query<L>(rotated, c, offsets);
  typename L::code code{};
  code.norm = native_code.norm;
  code.x0 = native_code.x0;
  std::copy(native_code.signs.begin(), native_code.signs.end(), code.signs.begin());
  for (std::size_t i = 0; i < L::padded; ++i)
    check(online_query.codes[i] == native_query.codes[i], "query quantization codes");
  check(qcovert::estimate<L>(online_query, code) == quantizer.approximate_distance(native_query, native_code),
        "quantized estimate");
  online_query = qcovert::prepare_query<L>(c.rotated, c, offsets);
  for (auto x : online_query.codes)
    check(x == 0, "zero residual query codes");
  check(qcovert::estimate<L>(online_query, typename L::code{}) == 0, "zero norm estimate");
}
void test_rejections() {
  bool rejected = false;
  try {
    (void)qcovert::decode_dataset<3, float>(std::vector<std::uint8_t>(48), 100, 10);
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, "bad file accepted");
  qcovert::key128 key{};
  qcovert::nonce96 nonce{};
  std::array<std::uint8_t, 16> plain{}, cipher{}, tag{}, decoded{};
  qcovert::encrypt(key, nonce, plain.data(), plain.size(), cipher.data(), tag.data());
  nonce[4] = 1;
  rejected = false;
  try {
    qcovert::decrypt(key, nonce, cipher.data(), cipher.size(), tag.data(), decoded.data());
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, "wrong slot/version nonce accepted");
}
int main() {
  try {
    test_topk();
    test_compact_topk();
    test_partition_topk();
    test_quantization<3>();
    test_quantization<65>();
    test_rejections();
    test_pipeline<float>();
    test_pipeline<std::uint8_t>();
    test_pipeline<float>(1, 2);
    test_pipeline<std::uint8_t>(1, 2);
    test_pipeline<std::uint8_t>(2, 1);
    std::cout << "Q-COVERT correctness passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
