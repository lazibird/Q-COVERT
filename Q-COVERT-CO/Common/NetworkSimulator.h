#ifndef DOUBLE_ORAM_NETWORK_SIMULATOR_H
#define DOUBLE_ORAM_NETWORK_SIMULATOR_H

#include <chrono>
#include <cstdint>
#include <thread>

namespace double_oram {

// Network simulation settings; disabled simulation adds no delay or simulated time.
struct NetworkProfile {
    // Enable network simulation.
    bool enabled = false;
    // Sleep to simulate delay; false only records the estimated time.
    bool sleep = false;
    // One-way transfer bandwidth in Mbps.
    double bandwidth_mbps = 0.0;
    // Round-trip latency per logical request and response, in milliseconds.
    double rtt_ms = 0.0;
};

// Simulated statistics for one logical network transfer.
struct NetworkSimulationResult {
    // Number of logical request and response rounds.
    uint64_t round_trips = 0;
    // Total bytes transferred.
    uint64_t bytes = 0;
    // Transfer time determined by bandwidth, in seconds.
    double transfer_time_sec = 0.0;
    // Delay determined by RTT, in seconds.
    double latency_time_sec = 0.0;
    // Total simulated network time in seconds.
    double total_time_sec = 0.0;
};

// Estimate RTT + bytes / bandwidth and optionally sleep for the resulting delay.
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
