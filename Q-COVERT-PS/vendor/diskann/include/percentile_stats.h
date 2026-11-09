// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#ifdef _WINDOWS
#include <numeric>
#endif
#include <string>
#include <vector>

#include "distance.h"
#include "parameters.h"

namespace diskann
{
struct QueryStats
{
    float total_us = 0; // total time to process query in micros
    float io_us = 0;    // total time spent in IO
    float cpu_us = 0;   // total time spent in CPU

    unsigned n_4k = 0;         // # of 4kB reads
    unsigned n_8k = 0;         // # of 8kB reads
    unsigned n_12k = 0;        // # of 12kB reads
    unsigned n_ios = 0;        // total # of IOs issued
    unsigned read_size = 0;    // total # of bytes read
    unsigned n_cmps_saved = 0; // # cmps saved
    unsigned n_cmps = 0;       // # cmps
    unsigned n_cache_hits = 0; // # cache_hits
    unsigned n_hops = 0;       // # search hops

    unsigned n_fullvectorreads = 0; // # of full vector reads
    unsigned n_actualhops = 0;
    unsigned num_fixed_rts = 0;
    unsigned num_search_iterations = 0; // while loop counter

    float diskann_compute_time = 0;
    float oram_client_time_us = 0;
    float oram_wait_time_us = 0;
    float oram_total_time_us = 0;
    float user_time_us = 0;
    float total_time_us = 0;

    std::chrono::duration<double> local_compute_time;
    std::chrono::duration<double> oram_total_time;
    std::chrono::duration<double> oram_wait_time;
    std::chrono::duration<double> oram_local_time;

    std::chrono::duration<double> communication_time;
    std::chrono::duration<double> user_perceived_time;
    std::chrono::duration<double> e2e_time;
};

template <typename T>
inline T get_percentile_stats(QueryStats *stats, uint64_t len, float percentile,
                              const std::function<T(const QueryStats &)> &member_fn)
{
    std::vector<T> vals(len);
    for (uint64_t i = 0; i < len; i++)
    {
        vals[i] = member_fn(stats[i]);
    }

    std::sort(vals.begin(), vals.end(), [](const T &left, const T &right) { return left < right; });

    auto retval = vals[(uint64_t)(percentile * len)];
    vals.clear();
    return retval;
}

template <typename T>
inline double get_mean_stats(QueryStats *stats, uint64_t len, const std::function<T(const QueryStats &)> &member_fn)
{
    double avg = 0;
    for (uint64_t i = 0; i < len; i++)
    {
        avg += (double)member_fn(stats[i]);
    }
    return avg / len;
}

template <typename T>
inline double get_median_stats(QueryStats *stats, uint64_t len, const std::function<T(const QueryStats &)> &member_fn)
{
    std::vector<double> values;
    values.reserve(len);

    for (uint64_t i = 0; i < len; i++)
    {
        values.push_back(static_cast<double>(member_fn(stats[i])));
    }
    // Print all values before median calculation
    // std::cout << "Values from median: ";
    // for (double v : values)
    // {
    //     std::cout << v/(1000) << " ";
    // }
    // std::cout << std::endl;

    std::sort(values.begin(), values.end());

    if (len % 2 == 0)
    {
        // even length → average of two middle values
        return (values[len / 2 - 1] + values[len / 2]) / 2.0;
    }
    else
    {
        // odd length → middle value
        return values[len / 2];
    }
}


template <typename T>
inline double get_total_stats(QueryStats *stats, uint64_t len, const std::function<T(const QueryStats &)> &member_fn)
{
    double sum = 0;
    for (uint64_t i = 0; i < len; i++)
    {
        sum += (double)member_fn(stats[i]);
    }
    return sum;
}
}