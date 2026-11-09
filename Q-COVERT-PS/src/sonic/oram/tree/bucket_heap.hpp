#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/util/formatter.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/humanize.hpp"

namespace sn::oram::tree {

namespace detail {

inline sn::util::log::logger& bucket_heap_logger() {
  static sn::util::log::logger logger = sn::util::log::create("oram.tree.bucket_heap");
  return logger;
}

} // namespace detail

template <typename Bucket> class bucket_heap {
public:
  template <typename BucketFactory>
  void initialize(
      std::uint64_t node_count, BucketFactory&& make_bucket, sn::oram::uid_generator& uid_gen,
      std::string_view label = "bucket_heap", bool fill_slots = true
  ) {
    size_t n_alloc_nodes = node_count + 1;
    const std::size_t approx_bytes = n_alloc_nodes * sizeof(Bucket);
    detail::bucket_heap_logger().trcf(
        "%s::initialize: initializing %d nodes (%s)", std::string(label).c_str(), n_alloc_nodes,
        sn::util::humanize::bytes(static_cast<std::uint64_t>(approx_bytes))
    );

    buckets_.clear();
    buckets_.reserve(n_alloc_nodes);

    buckets_.emplace_back(make_bucket(0));

    for (std::uint64_t node_id = 1; node_id <= node_count; ++node_id) {
      buckets_.emplace_back(make_bucket(node_id));
      if (fill_slots) buckets_.back().fill_dummy(uid_gen);
    }
  }

  void initialize(std::uint64_t node_count, sn::oram::uid_generator& uid_gen, std::string_view label = "bucket_heap") {
    static_assert(
        std::is_default_constructible_v<Bucket>,
        "bucket_heap: Bucket must be default-constructible or provide a factory"
    );
    auto make_bucket = [](std::uint64_t) { return Bucket{}; };
    initialize(node_count, make_bucket, uid_gen, label);
  }

  Bucket& operator[](std::uint64_t node_id) { return buckets_[node_id]; }
  const Bucket& operator[](std::uint64_t node_id) const { return buckets_[node_id]; }

  std::vector<Bucket>& data() noexcept { return buckets_; }
  const std::vector<Bucket>& data() const noexcept { return buckets_; }

private:
  std::vector<Bucket> buckets_;
};

} // namespace sn::oram::tree
