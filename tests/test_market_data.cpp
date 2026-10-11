#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "schwab/market_data.hpp"

using namespace schwab;
using Catch::Matchers::WithinAbs;

TEST_CASE("Quote struct memory layout and validation", "[market_data]") {
    SECTION("Exact 64-byte L1 CPU cache line alignment") {
        STATIC_REQUIRE(sizeof(Quote) == 64);
        STATIC_REQUIRE(alignof(Quote) == 8);
    }

    SECTION("Default construction is invalid") {
        Quote q;
        CHECK_FALSE(q.is_valid());
        CHECK(q.reference_price() == 0.0);
        CHECK(q.spread() == 0.0);
        CHECK_FALSE(q.is_crossed());
        CHECK_FALSE(q.is_locked());
    }

    SECTION("Valid quote with mark price") {
        Quote q{
            .symbol = Symbol("AAPL"),
            .bid_price = 224.20,
            .ask_price = 224.25,
            .last_price = 224.22,
            .mark_price = 224.23,
            .total_volume = 12'000'000,
            .quote_time_ms = 1728676800000
        };

        REQUIRE(q.is_valid());
        CHECK_THAT(q.reference_price(), WithinAbs(224.23, 1e-6));
        CHECK_THAT(q.spread(), WithinAbs(0.05, 1e-6));
        CHECK_FALSE(q.is_crossed());
        CHECK_FALSE(q.is_locked());
    }

    SECTION("Reference price priority fallback: mark -> last -> mid-market") {
        Quote q;
        q.symbol = Symbol("SPY");
        q.bid_price = 500.0;
        q.ask_price = 500.10;

        // No mark, no last -> midpoint
        CHECK_THAT(q.reference_price(), WithinAbs(500.05, 1e-6));

        // Add last price -> last takes priority over mid
        q.last_price = 500.08;
        CHECK_THAT(q.reference_price(), WithinAbs(500.08, 1e-6));

        // Add mark price -> mark takes top priority
        q.mark_price = 500.04;
        CHECK_THAT(q.reference_price(), WithinAbs(500.04, 1e-6));
    }

    SECTION("Locked and crossed order book defenses") {
        Quote q_locked{
            .symbol = Symbol("MSFT"),
            .bid_price = 400.0,
            .ask_price = 400.0,
            .last_price = 400.0,
            .mark_price = 400.0
        };
        CHECK(q_locked.is_locked());
        CHECK_FALSE(q_locked.is_crossed());
        CHECK(q_locked.spread() == 0.0);
        CHECK(q_locked.spread_bps() == 0.0);

        // Crossed / inverted market: ask drops below bid during order book surge
        Quote q_crossed{
            .symbol = Symbol("NVDA"),
            .bid_price = 125.50,
            .ask_price = 125.10, // Inverted!
            .last_price = 125.30,
            .mark_price = 125.30
        };
        CHECK(q_crossed.is_crossed());
        CHECK_FALSE(q_crossed.is_locked());
        // spread() clamps to 0.0 to prevent negative spread mishaps
        CHECK(q_crossed.spread() == 0.0);
        CHECK(q_crossed.spread_bps() == 0.0);
        // raw_spread() preserves signed difference for diagnostics
        CHECK_THAT(q_crossed.raw_spread(), WithinAbs(-0.40, 1e-6));
        // reference_price uses mark price rather than inverted midpoint
        CHECK_THAT(q_crossed.reference_price(), WithinAbs(125.30, 1e-6));
    }

    SECTION("Normal spread bps calculation") {
        Quote q_normal{
            .symbol = Symbol("AAPL"),
            .bid_price = 100.00,
            .ask_price = 100.05,
            .last_price = 100.02,
            .mark_price = 100.00
        };
        CHECK_FALSE(q_normal.is_crossed());
        CHECK_FALSE(q_normal.is_locked());
        CHECK_THAT(q_normal.spread(), WithinAbs(0.05, 1e-6));
        // 0.05 / 100.00 * 10,000 = 5.0 bps
        CHECK_THAT(q_normal.spread_bps(), WithinAbs(5.0, 1e-6));
    }
}

TEST_CASE("QuoteParser parse_single Schwab JSON", "[market_data]") {
    SECTION("Standard nested Schwab quote payload") {
        constexpr std::string_view json = R"({
            "AAPL": {
                "assetMainType": "EQUITY",
                "symbol": "AAPL",
                "quote": {
                    "bidPrice": 224.20,
                    "askPrice": 224.25,
                    "lastPrice": 224.23,
                    "mark": 224.24,
                    "totalVolume": 45123450,
                    "quoteTime": 1728676800000
                }
            }
        })";

        auto opt = QuoteParser::parse_single(json, "AAPL");
        REQUIRE(opt.has_value());
        CHECK(opt->symbol.view() == "AAPL");
        CHECK_THAT(opt->bid_price, WithinAbs(224.20, 1e-6));
        CHECK_THAT(opt->ask_price, WithinAbs(224.25, 1e-6));
        CHECK_THAT(opt->last_price, WithinAbs(224.23, 1e-6));
        CHECK_THAT(opt->mark_price, WithinAbs(224.24, 1e-6));
        CHECK(opt->total_volume == 45123450);
        CHECK(opt->quote_time_ms == 1728676800000);
    }

    SECTION("Missing target symbol returns nullopt") {
        constexpr std::string_view json = R"({
            "MSFT": {
                "quote": { "lastPrice": 400.0, "mark": 400.0 }
            }
        })";

        auto opt = QuoteParser::parse_single(json, "AAPL");
        CHECK_FALSE(opt.has_value());
    }

    SECTION("Empty or malformed JSON returns nullopt") {
        CHECK_FALSE(QuoteParser::parse_single("", "AAPL").has_value());
        CHECK_FALSE(QuoteParser::parse_single("not json at all", "AAPL").has_value());
        CHECK_FALSE(QuoteParser::parse_single("{}", "AAPL").has_value());
    }
}

TEST_CASE("QuoteParser parse_batch Schwab JSON", "[market_data]") {
    SECTION("Batch multi-symbol dictionary response") {
        constexpr std::string_view json = R"({
            "SPY": {
                "symbol": "SPY",
                "quote": {
                    "bidPrice": 570.10,
                    "askPrice": 570.15,
                    "lastPrice": 570.12,
                    "mark": 570.12,
                    "totalVolume": 35000000,
                    "quoteTime": 1728676801000
                }
            },
            "QQQ": {
                "symbol": "QQQ",
                "quote": {
                    "bidPrice": 485.50,
                    "askPrice": 485.58,
                    "lastPrice": 485.55,
                    "mark": 485.54,
                    "totalVolume": 25000000,
                    "quoteTime": 1728676802000
                }
            }
        })";

        auto quotes = QuoteParser::parse_batch(json);
        REQUIRE(quotes.size() == 2);

        bool found_spy = false;
        bool found_qqq = false;

        for (const auto& q : quotes) {
            if (q.symbol == Symbol("SPY")) {
                found_spy = true;
                CHECK_THAT(q.mark_price, WithinAbs(570.12, 1e-6));
            } else if (q.symbol == Symbol("QQQ")) {
                found_qqq = true;
                CHECK_THAT(q.mark_price, WithinAbs(485.54, 1e-6));
            }
        }

        CHECK(found_spy);
        CHECK(found_qqq);
    }

    SECTION("Empty or malformed batch returns empty vector") {
        CHECK(QuoteParser::parse_batch("").empty());
        CHECK(QuoteParser::parse_batch("invalid json string").empty());
        CHECK(QuoteParser::parse_batch("{}").empty());
    }
}
