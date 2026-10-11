#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace schwab {

/**
 * @brief High-precision clock and low-jitter synchronization utility.
 *
 * Implements two-stage hybrid wait:
 * 1. Coarse OS sleep (`std::this_thread::sleep_for`) while far from target timestamp.
 * 2. Hardware-assisted countdown spin-wait (`_mm_pause`) within final threshold
 *    to eliminate OS thread scheduler jitter (< 1 microsecond accuracy).
 */
class PrecisionClock {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    /**
     * @brief Monotonic high-resolution timestamp.
     */
    [[nodiscard]] static TimePoint now() noexcept {
        return Clock::now();
    }

    /**
     * @brief Wall-clock system timestamp.
     */
    [[nodiscard]] static std::chrono::system_clock::time_point system_now() noexcept {
        return std::chrono::system_clock::now();
    }

    /**
     * @brief Sleeps until target time point using two-stage hybrid wait.
     *
     * @param target Monotonic target time point to wake at.
     * @param spin_threshold Duration before target to switch from OS sleep to spin-wait (default 20ms).
     * @return Nanosecond signed jitter: positive if woke after target, negative if before or exact.
     */
    static std::chrono::nanoseconds sleep_until(
        TimePoint target,
        std::chrono::nanoseconds spin_threshold = std::chrono::milliseconds(20)
    ) noexcept;

    /**
     * @brief Sleeps for specified duration using two-stage hybrid wait.
     *
     * @param duration Time to sleep.
     * @param spin_threshold Duration before target to switch from OS sleep to spin-wait (default 20ms).
     * @return Nanosecond signed jitter.
     */
    static std::chrono::nanoseconds sleep_for(
        std::chrono::nanoseconds duration,
        std::chrono::nanoseconds spin_threshold = std::chrono::milliseconds(20)
    ) noexcept {
        return sleep_until(now() + duration, spin_threshold);
    }

    /**
     * @brief Parses time-of-day string ("HH:MM:SS" or "HH:MM") into seconds since midnight.
     *
     * @param str Time string (e.g. "09:30:00", "15:59:45", "16:00").
     * @return Seconds since midnight [0..86399], or std::nullopt if invalid format.
     */
    [[nodiscard]] static std::optional<std::chrono::seconds> parse_time_of_day(
        std::string_view str
    ) noexcept;

    /**
     * @brief Computes next system_clock time point for a given UTC seconds-from-midnight target.
     *
     * If time for today has already passed, computes time point for tomorrow.
     *
     * @param target_seconds_utc Seconds since midnight in UTC [0..86399].
     * @param ref_time Reference system time (defaults to system_now()).
     * @return Next UTC system time point matching target time of day.
     */
    [[nodiscard]] static std::chrono::system_clock::time_point next_occurrence_utc(
        std::chrono::seconds target_seconds_utc,
        std::chrono::system_clock::time_point ref_time = std::chrono::system_clock::now()
    ) noexcept;
};

} // namespace schwab
