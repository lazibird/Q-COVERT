#ifndef DOUBLE_ORAM_NETWORK_SIMULATOR_H
#define DOUBLE_ORAM_NETWORK_SIMULATOR_H

#include <chrono>
#include <cstdint>
#include <thread>

namespace double_oram {

// 网络模拟参数；enabled=false 时完全不产生额外延迟和统计时间。
struct NetworkProfile {
    // 是否启用网络模拟。
    bool enabled = false;
    // 启用后是否实际 sleep；false 时只统计估算时间。
    bool sleep = false;
    // 单向传输带宽，单位 Mbps。
    double bandwidth_mbps = 0.0;
    // 每次逻辑请求-响应的往返时延，单位 ms。
    double rtt_ms = 0.0;
};

// 一次逻辑网络通信产生的模拟统计。
struct NetworkSimulationResult {
    // 逻辑请求-响应轮数。
    uint64_t round_trips = 0;
    // 本次传输的总字节数。
    uint64_t bytes = 0;
    // 由带宽产生的传输时间，单位秒。
    double transfer_time_sec = 0.0;
    // 由 RTT 产生的等待时间，单位秒。
    double latency_time_sec = 0.0;
    // 总模拟网络时间，单位秒。
    double total_time_sec = 0.0;
};

// 按“RTT + bytes / bandwidth”的简单模型估算并可选 sleep。
inline NetworkSimulationResult simulate_network_transfer(
        const NetworkProfile& profile,
        uint64_t bytes,
        uint64_t round_trips = 1) {
    NetworkSimulationResult result;
    if (!profile.enabled) {
        return result;
    }

    result.round_trips = round_trips;
    result.bytes = bytes;
    if (profile.bandwidth_mbps > 0.0) {
        result.transfer_time_sec =
            static_cast<double>(bytes) * 8.0 / (profile.bandwidth_mbps * 1000.0 * 1000.0);
    }
    if (profile.rtt_ms > 0.0) {
        result.latency_time_sec =
            static_cast<double>(round_trips) * profile.rtt_ms / 1000.0;
    }
    result.total_time_sec = result.transfer_time_sec + result.latency_time_sec;

    if (profile.sleep && result.total_time_sec > 0.0) {
        std::this_thread::sleep_for(std::chrono::duration<double>(result.total_time_sec));
    }
    return result;
}

} // namespace double_oram

#endif
