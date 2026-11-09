// Build-time public smoke fixture. No production secrets are embedded here.
#include "qcovert/provider.hpp"
#include <iostream>
template <class T> void generate(std::ostream &out, const char *name) {
  constexpr std::size_t D = 3;
  std::vector<std::array<T, D>> vectors{{0, 0, 0}, {1, 2, 3}, {7, 8, 9}, {255, 254, 250}};
  std::vector<std::uint64_t> labels{0, 0, 1, 1};
  std::vector<std::array<float, D>> centroids{{.5f, 1, 1.5f}, {131, 131, 129.5f}};
  auto data = qcovert::prepare_dataset<D, T>(vectors, labels, centroids, 3, 3);
  auto encrypted = qcovert::encrypt_dataset(qcovert::encode_dataset<D, T>(data), qcovert::key128{});
  out << "inline constexpr unsigned char " << name << "[]={";
  for (auto b : encrypted)
    out << unsigned(b) << ',';
  out << "};\n";
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  std::ofstream out(argv[1]);
  if (!out)
    return 2;
  out << "#pragma once\n";
  generate<float>(out, "qcovert_float_fixture");
  generate<std::uint8_t>(out, "qcovert_u8_fixture");
  return out ? 0 : 3;
}
