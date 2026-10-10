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

TEST_CASE("Exponential Moving Average (EMA) calculation", "[indicators]") {
    SECTION("Nominal calculation with multi-period recursion") {
        std::vector<double> prices = {10.0, 20.0, 30.0, 40.0, 50.0};

        // Seed SMA (first 3): (10 + 20 + 30) / 3 = 20.0
        // alpha = 2 / (3 + 1) = 0.5
        // Step 4 (40.0): 0.5 * 40.0 + 0.5 * 20.0 = 30.0
        // Step 5 (50.0): 0.5 * 50.0 + 0.5 * 30.0 = 40.0
        auto res = ema(prices, 3);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(40.0, 1e-9));

        // Different sequence with period 4
        std::vector<double> prices2 = {2.0, 4.0, 6.0, 8.0, 12.0, 14.0};
        // Seed SMA (first 4): (2 + 4 + 6 + 8) / 4 = 5.0
        // alpha = 2 / (4 + 1) = 0.4
        // Step 5 (12.0): 0.4 * 12.0 + 0.6 * 5.0 = 7.8
        // Step 6 (14.0): 0.4 * 14.0 + 0.6 * 7.8 = 5.6 + 4.68 = 10.28
        auto res2 = ema(prices2, 4);
        REQUIRE(res2.has_value());
        CHECK_THAT(*res2, WithinAbs(10.28, 1e-9));
    }

    SECTION("Exact lookback match (prices.size() == period)") {
        std::vector<double> prices = {10.0, 20.0, 30.0};

        // Seed SMA equals full series arithmetic mean
        auto res = ema(prices, 3);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(20.0, 1e-9));
    }

    SECTION("Period of 1 evaluates to the latest price") {
        std::vector<double> prices = {10.0, 25.5, 42.75};

        auto res = ema(prices, 1);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(42.75, 1e-9));
    }

    SECTION("Zero-allocation with fixed-size stack arrays (std::array)") {
        std::array<double, 5> stack_prices = {10.0, 20.0, 30.0, 40.0, 50.0};

        auto res = ema(stack_prices, 3);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(40.0, 1e-9));
    }
}

TEST_CASE("Exponential Moving Average edge cases and boundary defense", "[indicators]") {
    SECTION("Insufficient data points returns std::nullopt") {
        std::vector<double> prices = {10.0, 20.0};

        CHECK(!ema(prices, 3).has_value());
        CHECK(!ema(prices, 50).has_value());
    }

    SECTION("Empty price sequence returns std::nullopt") {
        std::vector<double> empty_prices;

        CHECK(!ema(empty_prices, 1).has_value());
        CHECK(!ema(empty_prices, 10).has_value());
    }

    SECTION("Period of 0 returns std::nullopt") {
        std::vector<double> prices = {10.0, 20.0, 30.0};

        CHECK(!ema(prices, 0).has_value());
    }
}

TEST_CASE("Realized Volatility calculation", "[indicators]") {
    SECTION("Nominal calculation matches manual statistical derivation") {
        std::vector<double> prices = {100.0, 102.0, 101.0, 104.0};

        // 3 returns from 4 prices:
        // r1 = ln(102/100) = 0.019802627
        // r2 = ln(101/102) = -0.009852297
        // r3 = ln(104/101) = 0.029270438
        // Telescoping mean = ln(104/100)/3 = 0.013073590
        // Sample standard deviation (ddof=1) = ~0.020410954
        // Annualized vol (factor 252) = ~0.324013868
        auto res = realized_volatility(prices, 3, 252.0);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(0.324013868, 1e-6));

        // Daily unannualized volatility (factor 1.0)
        auto res_daily = realized_volatility(prices, 3, 1.0);
        REQUIRE(res_daily.has_value());
        CHECK_THAT(*res_daily, WithinAbs(0.020410954, 1e-6));
    }

    SECTION("Constant prices yield zero volatility") {
        std::vector<double> prices = {50.0, 50.0, 50.0, 50.0, 50.0};

        auto res = realized_volatility(prices, 4);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(0.0, 1e-9));
    }

    SECTION("Trailing window ignores preceding history") {
        // Preceding extreme prices should be excluded
        std::vector<double> prices = {999.0, 5.0, 100.0, 102.0, 101.0, 104.0};

        auto res = realized_volatility(prices, 3, 252.0);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(0.324013868, 1e-6));
    }

    SECTION("Zero-allocation with fixed-size stack arrays (std::array)") {
        std::array<double, 4> stack_prices = {100.0, 102.0, 101.0, 104.0};

        auto res = realized_volatility(stack_prices, 3, 252.0);
        REQUIRE(res.has_value());
        CHECK_THAT(*res, WithinAbs(0.324013868, 1e-6));
    }
}

TEST_CASE("Realized Volatility edge cases and boundary defense", "[indicators]") {
    SECTION("Period less than 2 returns std::nullopt") {
        std::vector<double> prices = {100.0, 102.0, 101.0};

        CHECK(!realized_volatility(prices, 0).has_value());
        CHECK(!realized_volatility(prices, 1).has_value());
    }

    SECTION("Insufficient prices (less than period + 1) returns std::nullopt") {
        std::vector<double> prices = {100.0, 102.0, 101.0};

        // Period 3 requires 4 prices
        CHECK(!realized_volatility(prices, 3).has_value());
    }

    SECTION("Empty price sequence returns std::nullopt") {
        std::vector<double> empty_prices;

        CHECK(!realized_volatility(empty_prices, 2).has_value());
    }

    SECTION("Non-positive prices return std::nullopt") {
        std::vector<double> zero_price = {100.0, 0.0, 101.0, 104.0};
        std::vector<double> neg_price = {100.0, -10.0, 101.0, 104.0};

        CHECK(!realized_volatility(zero_price, 3).has_value());
        CHECK(!realized_volatility(neg_price, 3).has_value());
    }

    SECTION("Non-positive annualization factor returns std::nullopt") {
        std::vector<double> prices = {100.0, 102.0, 101.0, 104.0};

        CHECK(!realized_volatility(prices, 3, 0.0).has_value());
        CHECK(!realized_volatility(prices, 3, -252.0).has_value());
    }
}
