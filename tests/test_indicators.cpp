#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "schwab/indicators.hpp"

#include <array>
#include <vector>

using namespace schwab;
using Catch::Matchers::WithinAbs;

TEST_CASE("Simple Moving Average (SMA) calculation", "[indicators]") {
    SECTION("Nominal calculation with trailing window") {
        std::vector<double> prices = {10.0, 20.0, 30.0, 40.0, 50.0};

        // Last 3 elements: 30 + 40 + 50 = 120 / 3 = 40.0
        auto res = sma(prices, 3);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(40.0, 1e-9));

        // Last 2 elements: 40 + 50 = 90 / 2 = 45.0
        auto res2 = sma(prices, 2);
        REQUIRE(res2.has_value());
        CHECK_THAT(*res2, WithinAbs(45.0, 1e-9));
    }

    SECTION("Full-series lookback window") {
        std::vector<double> prices = {10.0, 20.0, 30.0, 40.0, 50.0};

        // All 5 elements: 150 / 5 = 30.0
        auto res = sma(prices, 5);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(30.0, 1e-9));
    }

    SECTION("Lookback period of 1 returns the latest price") {
        std::vector<double> prices = {10.0, 25.5, 42.75};

        auto res = sma(prices, 1);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(42.75, 1e-9));
    }

    SECTION("Zero-allocation with fixed-size stack arrays (std::array)") {
        // Operates directly on stack memory with zero dynamic heap allocation
        std::array<double, 6> stack_prices = {100.50, 101.25, 100.75, 102.00, 103.50, 104.00};

        // Trailing 3: (102.00 + 103.50 + 104.00) / 3 = 309.50 / 3 = 103.166666667
        auto res = sma(stack_prices, 3);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(103.16666666666667, 1e-9));
    }

    SECTION("Floating-point precision with decimal prices") {
        std::vector<double> prices = {580.12, 582.45, 581.90, 585.30};

        // Trailing 3: (582.45 + 581.90 + 585.30) / 3 = 1749.65 / 3 = 583.216666667
        auto res = sma(prices, 3);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(583.2166666666667, 1e-9));
    }
}

TEST_CASE("Simple Moving Average edge cases and boundary defense", "[indicators]") {
    SECTION("Insufficient data points returns std::nullopt") {
        std::vector<double> prices = {10.0, 20.0};

        // Lookback window larger than input sequence
        CHECK(!sma(prices, 3).has_value());
        CHECK(!sma(prices, 50).has_value());
    }

    SECTION("Empty price sequence returns std::nullopt") {
        std::vector<double> empty_prices;

        CHECK(!sma(empty_prices, 1).has_value());
        CHECK(!sma(empty_prices, 10).has_value());
    }

    SECTION("Period of 0 returns std::nullopt") {
        std::vector<double> prices = {10.0, 20.0, 30.0};

        CHECK(!sma(prices, 0).has_value());
    }
}
