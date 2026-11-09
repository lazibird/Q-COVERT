#pragma once
#include "qcovert/codec.hpp"
#include "qcovert/index.hpp"
#include "qcovert_fixture.hpp"
template <class T>
bool qcovert_smoke_case(std::span<const std::uint8_t> encrypted, sn::threads::thread_team &workers) {
  const auto plain = qcovert::decrypt_dataset(encrypted, qcovert::key128{}); // public TEST key
  auto data = qcovert::decode_dataset<3, T>(plain, 4, 3);
  qcovert::index<3, T, 2> index(data, {3, 12, 5}, workers, {8, 16, 2, 2, 0});
  for (std::size_t round = 0; round < 6; ++round) {
    typename qcovert::layout<3, T>::vector query{0, 0, 0};
    if (round % 2)
      query = {255, 254, 250};
    const auto results = index.search(query, true);
    if (results.size() != 5 || results[0].id != (round % 2 ? 3u : 0u) || results[0].distance != 0 ||
        results[4].id != qcovert::invalid_id)
      return false;
  }
  index.finish();
  return true;
}
inline bool qcovert_sgx_smoke(sn::threads::thread_team &workers) {
  return qcovert_smoke_case<float>(qcovert_float_fixture, workers) &&
         qcovert_smoke_case<std::uint8_t>(qcovert_u8_fixture, workers);
}
