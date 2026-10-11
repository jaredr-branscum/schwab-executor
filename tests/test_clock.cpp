#include <catch2/catch_test_macros.hpp>
#include "schwab/clock.hpp"

#include <chrono>

using namespace schwab;

TEST_CASE("PrecisionClock basic time properties", "[clock]") {
    SECTION("Monotonic clock advances forward") {
        auto t1 = PrecisionClock::now();
        auto t2 = PrecisionClock::now();
        REQUIRE(t2 >= t1);
    }

    SECTION("System clock returns valid time point") {
        auto sys_time = PrecisionClock::system_now();
        auto epoch_seconds = std::chrono::duration_cast<std::chrono::seconds>(sys_time.time_since_epoch()).count();
        // Timestamp must be well past year 2024 (approx 1.7e9 seconds)
        REQUIRE(epoch_seconds > 1'700'000'000);
    }
}

TEST_CASE("PrecisionClock hybrid sleep accuracy", "[clock]") {
    SECTION("Short spin-wait sleep (under spin threshold)") {
        auto t_start = PrecisionClock::now();
        auto jitter = PrecisionClock::sleep_for(std::chrono::milliseconds(5), std::chrono::milliseconds(10));
        auto t_end = PrecisionClock::now();

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();
        // Must sleep at least 5000 microseconds, but not excessively overshoot
        REQUIRE(elapsed >= 5000);
        REQUIRE(elapsed <= 15000);

        // Jitter must be non-premature (>= 0) and strictly bounded (< 5ms)
        REQUIRE(jitter.count() >= 0);
        REQUIRE(jitter < std::chrono::milliseconds(5));
    }

    SECTION("Pure spin-wait precision (sub-microsecond resolution)") {
        // Run 50 iterations of 50-microsecond pure spin-waits.
        // Since duration (50us) < spin_threshold (100us), Stage 1 OS sleep is bypassed,
        // testing pure Stage 2 hardware _mm_pause() precision.
        int64_t min_jitter_ns = (std::numeric_limits<int64_t>::max)();
        int sub_microsecond_count = 0;
        constexpr int kIterations = 50;

        for (int i = 0; i < kIterations; ++i) {
            auto jitter = PrecisionClock::sleep_for(
                std::chrono::microseconds(50),
                std::chrono::microseconds(100)
            );
            if (jitter.count() < min_jitter_ns) {
                min_jitter_ns = jitter.count();
            }
            if (jitter.count() < 1000) {
                ++sub_microsecond_count;
            }
        }
        REQUIRE(min_jitter_ns >= 0);
        REQUIRE(min_jitter_ns < 1000); // Proves sub-microsecond precision is achieved
        // Vast majority of uninterrupted spin-waits should be sub-microsecond
        REQUIRE(sub_microsecond_count >= 40);
    }

    SECTION("Two-stage sleep (above spin threshold)") {
        auto t_start = PrecisionClock::now();
        // Sleep for 25ms with 10ms spin threshold (tests coarse sleep + spin-wait transition)
        auto jitter = PrecisionClock::sleep_for(std::chrono::milliseconds(25), std::chrono::milliseconds(10));
        auto t_end = PrecisionClock::now();

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();
        REQUIRE(elapsed >= 25000);
        REQUIRE(elapsed <= 45000);

        // Jitter must be non-premature (>= 0) and strictly bounded (< 10ms)
        REQUIRE(jitter.count() >= 0);
        REQUIRE(jitter < std::chrono::milliseconds(10));
    }

    SECTION("Target in the past returns immediately") {
        auto t_past = PrecisionClock::now() - std::chrono::milliseconds(100);
        auto t_start = PrecisionClock::now();
        auto jitter = PrecisionClock::sleep_until(t_past);
        auto t_end = PrecisionClock::now();

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();
        // Should return in sub-millisecond time without sleeping
        REQUIRE(elapsed < 2000);
        // Jitter reflects the elapsed past duration (~100ms)
        REQUIRE(jitter >= std::chrono::milliseconds(100));
        REQUIRE(jitter < std::chrono::milliseconds(110));
    }
}

TEST_CASE("PrecisionClock time-of-day parsing", "[clock]") {
    SECTION("Valid HH:MM:SS format") {
        auto res = PrecisionClock::parse_time_of_day("09:30:00");
        REQUIRE(res.has_value());
        REQUIRE(res->count() == (9 * 3600 + 30 * 60));

        auto res2 = PrecisionClock::parse_time_of_day("15:59:45");
        REQUIRE(res2.has_value());
        REQUIRE(res2->count() == (15 * 3600 + 59 * 60 + 45));

        auto res_midnight = PrecisionClock::parse_time_of_day("00:00:00");
        REQUIRE(res_midnight.has_value());
        REQUIRE(res_midnight->count() == 0);

        auto res_end = PrecisionClock::parse_time_of_day("23:59:59");
        REQUIRE(res_end.has_value());
        REQUIRE(res_end->count() == 86399);
    }

    SECTION("Valid HH:MM format defaults seconds to zero") {
        auto res = PrecisionClock::parse_time_of_day("16:00");
        REQUIRE(res.has_value());
        REQUIRE(res->count() == 16 * 3600);
    }

    SECTION("Whitespace trimming") {
        auto res = PrecisionClock::parse_time_of_day("  09:30:00 \r\n");
        REQUIRE(res.has_value());
        REQUIRE(res->count() == (9 * 3600 + 30 * 60));
    }

    SECTION("Invalid formats rejected") {
        REQUIRE_FALSE(PrecisionClock::parse_time_of_day("").has_value());
        REQUIRE_FALSE(PrecisionClock::parse_time_of_day("   ").has_value());
        REQUIRE_FALSE(PrecisionClock::parse_time_of_day("invalid").has_value());
        REQUIRE_FALSE(PrecisionClock::parse_time_of_day("24:00:00").has_value());
        REQUIRE_FALSE(PrecisionClock::parse_time_of_day("12:60:00").has_value());
        REQUIRE_FALSE(PrecisionClock::parse_time_of_day("12:30:60").has_value());
        REQUIRE_FALSE(PrecisionClock::parse_time_of_day("-1:00:00").has_value());
        REQUIRE_FALSE(PrecisionClock::parse_time_of_day("12").has_value());
        REQUIRE_FALSE(PrecisionClock::parse_time_of_day("::").has_value());
    }
}

TEST_CASE("PrecisionClock next occurrence calculation", "[clock]") {
    SECTION("Target later today schedules for today") {
        // Reference time: 10:00:00 UTC on a fixed day
        auto ref_day = std::chrono::sys_days{std::chrono::year{2026}/10/10};
        auto ref_time = ref_day + std::chrono::hours(10); // 10:00:00 UTC

        // Target: 15:30:00 UTC
        auto target_secs = std::chrono::seconds(15 * 3600 + 30 * 60);
        auto next = PrecisionClock::next_occurrence_utc(target_secs, ref_time);

        REQUIRE(next == ref_day + target_secs);
    }

    SECTION("Target earlier today schedules for tomorrow") {
        auto ref_day = std::chrono::sys_days{std::chrono::year{2026}/10/10};
        auto ref_time = ref_day + std::chrono::hours(16); // 16:00:00 UTC

        // Target: 09:30:00 UTC (has already passed today)
        auto target_secs = std::chrono::seconds(9 * 3600 + 30 * 60);
        auto next = PrecisionClock::next_occurrence_utc(target_secs, ref_time);

        auto tomorrow = ref_day + std::chrono::days(1);
        REQUIRE(next == tomorrow + target_secs);
    }
}
