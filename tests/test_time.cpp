#include <catch2/catch_test_macros.hpp>
#include "schwab/time.hpp"

TEST_CASE("Time utility returns valid real-world wall-clock epoch", "[time]") {
    double now = schwab::epoch_seconds_now();

    // Verify system clock returned valid modern epoch timestamp (> Nov 2023)
    REQUIRE(now > 1700000000.0);
}

TEST_CASE("Time utility monotonic_nanos_now is monotonically increasing", "[time]") {
    int64_t t1 = schwab::monotonic_nanos_now();
    int64_t t2 = schwab::monotonic_nanos_now();

    // Monotonic clock should never run backwards
    REQUIRE(t2 >= t1);
    REQUIRE(t1 > 0);
}
