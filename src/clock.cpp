#include "schwab/clock.hpp"

#include <charconv>
#include <thread>

#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define SCHWAB_CPU_PAUSE() _mm_pause()
#else
#define SCHWAB_CPU_PAUSE() std::this_thread::yield()
#endif

namespace schwab {

std::chrono::nanoseconds PrecisionClock::sleep_until(
    TimePoint target,
    std::chrono::nanoseconds spin_threshold
) noexcept {
    auto current = now();
    if (current >= target) [[unlikely]] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(current - target);
    }

    if (spin_threshold < std::chrono::nanoseconds{0}) {
        spin_threshold = std::chrono::milliseconds(20);
    }

    // STAGE 1: Coarse OS sleep while remaining time > spin_threshold
    while (true) {
        current = now();
        auto remaining = target - current;
        if (remaining <= spin_threshold) {
            break;
        }

        // Sleep for half safe remaining delta to prevent OS scheduler oversleeping
        auto coarse_sleep = (remaining - spin_threshold) / 2;
        if (coarse_sleep > std::chrono::milliseconds(1)) {
            std::this_thread::sleep_for(coarse_sleep);
        } else {
            break;
        }
    }

    // STAGE 2: Hardware-assisted spin-wait with _mm_pause()
    while (now() < target) {
        SCHWAB_CPU_PAUSE();
    }

    auto wake_time = now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(wake_time - target);
}

std::optional<std::chrono::seconds> PrecisionClock::parse_time_of_day(
    std::string_view str
) noexcept {
    while (!str.empty() && (str.front() == ' ' || str.front() == '\t')) {
        str.remove_prefix(1);
    }
    while (!str.empty() && (str.back() == ' ' || str.back() == '\t' || str.back() == '\r' || str.back() == '\n')) {
        str.remove_suffix(1);
    }

    if (str.empty()) {
        return std::nullopt;
    }

    // Expected formats: HH:MM or HH:MM:SS
    size_t first_colon = str.find(':');
    if (first_colon == std::string_view::npos || first_colon == 0) {
        return std::nullopt;
    }

    std::string_view hh_sv = str.substr(0, first_colon);
    int hh = -1;
    auto [p1, ec1] = std::from_chars(hh_sv.data(), hh_sv.data() + hh_sv.size(), hh);
    if (ec1 != std::errc() || p1 != hh_sv.data() + hh_sv.size() || hh < 0 || hh > 23) {
        return std::nullopt;
    }

    std::string_view rest = str.substr(first_colon + 1);
    size_t second_colon = rest.find(':');

    std::string_view mm_sv = (second_colon == std::string_view::npos) ? rest : rest.substr(0, second_colon);
    int mm = -1;
    auto [p2, ec2] = std::from_chars(mm_sv.data(), mm_sv.data() + mm_sv.size(), mm);
    if (ec2 != std::errc() || p2 != mm_sv.data() + mm_sv.size() || mm < 0 || mm > 59) {
        return std::nullopt;
    }

    int ss = 0;
    if (second_colon != std::string_view::npos) {
        std::string_view ss_sv = rest.substr(second_colon + 1);
        auto [p3, ec3] = std::from_chars(ss_sv.data(), ss_sv.data() + ss_sv.size(), ss);
        if (ec3 != std::errc() || p3 != ss_sv.data() + ss_sv.size() || ss < 0 || ss > 59) {
            return std::nullopt;
        }
    }

    return std::chrono::seconds(hh * 3600 + mm * 60 + ss);
}

std::chrono::system_clock::time_point PrecisionClock::next_occurrence_utc(
    std::chrono::seconds target_seconds_utc,
    std::chrono::system_clock::time_point ref_time
) noexcept {
    // Floor to beginning of UTC day
    auto dp = std::chrono::floor<std::chrono::days>(ref_time);
    auto time_since_midnight = std::chrono::duration_cast<std::chrono::seconds>(ref_time - dp);

    if (time_since_midnight < target_seconds_utc) {
        // Target is later today
        return dp + target_seconds_utc;
    }

    // Target has already passed today; schedule for tomorrow
    return dp + std::chrono::days(1) + target_seconds_utc;
}

} // namespace schwab
