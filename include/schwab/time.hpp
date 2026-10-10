#pragma once

#include <chrono>
#include <cstdint>

namespace schwab {

/**
 * @brief Returns current wall-clock time in epoch seconds.
 */
inline double epoch_seconds_now() noexcept {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

/**
 * @brief Returns high-resolution monotonic time in nanoseconds.
 * 
 * Immune to system clock adjustments/NTP leaps. Used for spin-wait loops
 * and execution latency benchmarks.
 */
inline int64_t monotonic_nanos_now() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace schwab
