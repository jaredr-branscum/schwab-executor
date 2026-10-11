#include <catch2/catch_test_macros.hpp>
#include "schwab/risk_guard.hpp"

using namespace schwab;

TEST_CASE("RiskGuard memory layout and cache line guarantees", "[risk_guard]") {
    SECTION("Packed into 64-byte L1 CPU cache lines") {
        CHECK(sizeof(RiskLimits) <= 64);
        CHECK(sizeof(RiskCheckResult) <= 32);
        CHECK(alignof(RiskLimits) == 8);
    }
}

TEST_CASE("RiskRejectReason string representation", "[risk_guard]") {
    CHECK(to_string(RiskRejectReason::None) == "NONE");
    CHECK(to_string(RiskRejectReason::InvalidOrderStructure) == "INVALID_ORDER_STRUCTURE");
    CHECK(to_string(RiskRejectReason::ExceedsMaxOrderNotional) == "EXCEEDS_MAX_ORDER_NOTIONAL");
    CHECK(to_string(RiskRejectReason::ExceedsMaxOrderQuantity) == "EXCEEDS_MAX_ORDER_QUANTITY");
    CHECK(to_string(RiskRejectReason::ExceedsMaxPositionNotional) == "EXCEEDS_MAX_POSITION_NOTIONAL");
    CHECK(to_string(RiskRejectReason::PriceCollarViolation) == "PRICE_COLLAR_VIOLATION");
    CHECK(to_string(RiskRejectReason::MocCutoffExceeded) == "MOC_CUTOFF_EXCEEDED");
    CHECK(to_string(RiskRejectReason::DryRunIntercepted) == "DRY_RUN_INTERCEPTED");
    CHECK(to_string(RiskRejectReason::CrossedBookAnomaly) == "CROSSED_BOOK_ANOMALY");
    CHECK(to_string(RiskRejectReason::MaxSpreadExceeded) == "MAX_SPREAD_EXCEEDED");
}

TEST_CASE("RiskGuard nominal approval", "[risk_guard]") {
    RiskLimits limits{
        .max_order_notional = 100'000.0,
        .max_order_quantity = 5'000.0,
        .max_position_notional = 250'000.0,
        .price_collar_percent = 0.05,
        .dry_run = false
    };

    RiskGuard guard(limits);

    SECTION("Valid Market order within risk thresholds") {
        Order ord{
            .quantity = 100.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "AAPL"
        };

        // 100 shares * $150 = $15,000 notional (well under $100k)
        auto res = guard.validate_order(ord, 0.0, 150.0);
        REQUIRE(res.approved);
        CHECK(res.reason == RiskRejectReason::None);
        CHECK(static_cast<bool>(res));
    }

    SECTION("Valid Limit order within collar") {
        Order ord{
            .quantity = 50.0,
            .limit_price = 202.0,
            .side = OrderSide::Buy,
            .type = OrderType::Limit,
            .symbol = "MSFT"
        };

        // Market price $200, buy limit $202 (+1%, within 5% collar)
        auto res = guard.validate_order(ord, 100.0, 200.0);
        REQUIRE(res.approved);
        CHECK(res.reason == RiskRejectReason::None);
    }

    SECTION("Valid MarketOnClose order within thresholds") {
        Order ord{
            .quantity = 200.0,
            .side = OrderSide::Sell,
            .type = OrderType::MarketOnClose,
            .symbol = "NVDA"
        };

        // 200 shares * $120 = $24,000 notional (pass 70,000s UTC / 14:30 EST to be before cutoff)
        auto res = guard.validate_order(ord, 500.0, 120.0, 70'000);
        REQUIRE(res.approved);
        CHECK(res.reason == RiskRejectReason::None);
    }
}

TEST_CASE("RiskGuard pre-trade rejection gates", "[risk_guard]") {
    RiskLimits limits{
        .max_order_notional = 50'000.0,
        .max_order_quantity = 1'000.0,
        .max_position_notional = 100'000.0,
        .price_collar_percent = 0.05,
        .dry_run = false
    };

    RiskGuard guard(limits);

    SECTION("Rejection: Invalid order structure") {
        Order empty_sym{
            .quantity = 10.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = ""
        };
        auto res1 = guard.validate_order(empty_sym, 0.0, 100.0);
        CHECK(!res1.approved);
        CHECK(res1.reason == RiskRejectReason::InvalidOrderStructure);

        Order missing_market_price{
            .quantity = 10.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "AAPL"
        };
        auto res2 = guard.validate_order(missing_market_price, 0.0, 0.0);
        CHECK(!res2.approved);
        CHECK(res2.reason == RiskRejectReason::InvalidOrderStructure);
    }

    SECTION("Rejection: Single order notional exceeded") {
        Order ord{
            .quantity = 400.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "AAPL"
        };

        // 400 shares * $150 = $60,000 (exceeds $50,000 limit)
        auto res = guard.validate_order(ord, 0.0, 150.0);
        CHECK(!res.approved);
        CHECK(res.reason == RiskRejectReason::ExceedsMaxOrderNotional);
    }

    SECTION("Rejection: Single order quantity exceeded") {
        Order ord{
            .quantity = 1'500.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "AAPL"
        };

        // 1,500 shares exceeds 1,000 share max
        auto res = guard.validate_order(ord, 0.0, 20.0);
        CHECK(!res.approved);
        CHECK(res.reason == RiskRejectReason::ExceedsMaxOrderQuantity);
    }

    SECTION("Rejection: Price collar breach (Buy limit too high)") {
        Order ord{
            .quantity = 10.0,
            .limit_price = 110.0,
            .side = OrderSide::Buy,
            .type = OrderType::Limit,
            .symbol = "AAPL"
        };

        // Market price $100, 5% collar max = $105. Limit is $110 (+10%)
        auto res = guard.validate_order(ord, 0.0, 100.0);
        CHECK(!res.approved);
        CHECK(res.reason == RiskRejectReason::PriceCollarViolation);
    }

    SECTION("Rejection: Price collar breach (Sell limit too low)") {
        Order ord{
            .quantity = 10.0,
            .limit_price = 90.0,
            .side = OrderSide::Sell,
            .type = OrderType::Limit,
            .symbol = "AAPL"
        };

        // Market price $100, 5% collar min = $95. Limit is $90 (-10%)
        auto res = guard.validate_order(ord, 100.0, 100.0);
        CHECK(!res.approved);
        CHECK(res.reason == RiskRejectReason::PriceCollarViolation);
    }

    SECTION("Rejection: Resulting position notional exceeded") {
        Order ord{
            .quantity = 300.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "AAPL"
        };

        // Already hold 500 shares ($75,000).
        // Buying 300 more -> 800 shares * $150 = $120,000 (exceeds $100,000 max position)
        auto res = guard.validate_order(ord, 500.0, 150.0);
        CHECK(!res.approved);
        CHECK(res.reason == RiskRejectReason::ExceedsMaxPositionNotional);

        // However, selling shares from the same position should reduce exposure and pass!
        Order sell_ord{
            .quantity = 200.0,
            .side = OrderSide::Sell,
            .type = OrderType::Market,
            .symbol = "AAPL"
        };
        // 500 - 200 = 300 shares * $150 = $45,000 (well within $100k)
        auto sell_res = guard.validate_order(sell_ord, 500.0, 150.0);
        CHECK(sell_res.approved);
    }

    SECTION("Rejection: Dry-run interceptor") {
        limits.dry_run = true;
        guard.set_limits(limits);

        Order ord{
            .quantity = 10.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "AAPL"
        };

        auto res = guard.validate_order(ord, 0.0, 150.0);
        CHECK(!res.approved);
        CHECK(res.reason == RiskRejectReason::DryRunIntercepted);
    }
}

TEST_CASE("RiskGuard Market-on-Close (MOC) cutoff gate", "[risk_guard]") {
    RiskLimits limits{
        .max_order_notional = 100'000.0,
        .max_order_quantity = 5'000.0,
        .max_position_notional = 250'000.0,
        .price_collar_percent = 0.05,
        .moc_cutoff_seconds_utc = 75'000, // 20:50:00 UTC (15:50 EST)
        .enforce_moc_cutoff = true,
        .dry_run = false
    };

    RiskGuard guard(limits);

    Order moc_ord{
        .quantity = 50.0,
        .side = OrderSide::Buy,
        .type = OrderType::MarketOnClose,
        .symbol = "SPY"
    };

    SECTION("Approval: MOC order submitted before cutoff (e.g. 15:45 EST / 74,700s UTC)") {
        constexpr uint32_t t_before_cutoff = 74'700;
        auto res = guard.validate_order(moc_ord, 0.0, 500.0, t_before_cutoff);
        REQUIRE(res.approved);
        CHECK(res.reason == RiskRejectReason::None);
    }

    SECTION("Rejection: MOC order submitted after cutoff (e.g. 15:52 EST / 75,120s UTC)") {
        constexpr uint32_t t_after_cutoff = 75'120;
        auto res = guard.validate_order(moc_ord, 0.0, 500.0, t_after_cutoff);
        REQUIRE_FALSE(res.approved);
        CHECK(res.reason == RiskRejectReason::MocCutoffExceeded);
        CHECK(res.message.find("cutoff") != std::string_view::npos);
    }

    SECTION("Continuous Market order passes even after MOC cutoff") {
        Order mkt_ord{
            .quantity = 50.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "SPY"
        };
        constexpr uint32_t t_late_bell = 75'585; // 15:59:45 EST
        auto res = guard.validate_order(mkt_ord, 0.0, 500.0, t_late_bell);
        REQUIRE(res.approved);
        CHECK(res.reason == RiskRejectReason::None);
    }

    SECTION("Enforcement disabled allows MOC after cutoff") {
        limits.enforce_moc_cutoff = false;
        guard.set_limits(limits);

        constexpr uint32_t t_after_cutoff = 75'120;
        auto res = guard.validate_order(moc_ord, 0.0, 500.0, t_after_cutoff);
        REQUIRE(res.approved);
        CHECK(res.reason == RiskRejectReason::None);
    }
}

TEST_CASE("RiskGuard real-time quote validation, spread circuit breaker, and spread-aware collars", "[risk_guard]") {
    RiskLimits limits{
        .max_order_notional = 100'000.0,
        .max_order_quantity = 5'000.0,
        .max_position_notional = 250'000.0,
        .price_collar_percent = 0.03, // 3%
        .max_bid_ask_spread_bps = 50.0, // 50 bps (0.50%)
        .dry_run = false,
        .reject_crossed_book = true
    };

    RiskGuard guard(limits);

    Quote valid_quote{
        .symbol = Symbol("SPY"),
        .bid_price = 500.00,
        .ask_price = 500.20,
        .last_price = 500.10,
        .mark_price = 500.10,
        .total_volume = 1'000'000,
        .quote_time_ms = 1728676800000
    };

    SECTION("Nominal approval: market order with tight spread quote") {
        Order ord{
            .quantity = 100.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "SPY"
        };
        auto res = guard.validate_order(ord, valid_quote, 0.0);
        REQUIRE(res.approved);
        CHECK(res.reason == RiskRejectReason::None);
    }

    SECTION("Rejection: Quote symbol mismatch") {
        Order ord{
            .quantity = 100.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "QQQ"
        };
        auto res = guard.validate_order(ord, valid_quote, 0.0);
        REQUIRE_FALSE(res.approved);
        CHECK(res.reason == RiskRejectReason::InvalidOrderStructure);
        CHECK(res.message.find("symbol") != std::string_view::npos);
    }

    SECTION("Rejection: Invalid quote structure") {
        Order ord{
            .quantity = 100.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "SPY"
        };
        Quote bad_quote{
            .symbol = Symbol("SPY"),
            .bid_price = 0.0,
            .ask_price = 0.0,
            .last_price = 0.0,
            .mark_price = 0.0
        };
        auto res = guard.validate_order(ord, bad_quote, 0.0);
        REQUIRE_FALSE(res.approved);
        CHECK(res.reason == RiskRejectReason::InvalidOrderStructure);
    }

    SECTION("Rejection: Crossed book anomaly (bid > ask)") {
        Order ord{
            .quantity = 100.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "SPY"
        };
        Quote crossed_quote{
            .symbol = Symbol("SPY"),
            .bid_price = 502.00,
            .ask_price = 500.00,
            .last_price = 501.00,
            .mark_price = 501.00
        };
        auto res = guard.validate_order(ord, crossed_quote, 0.0);
        REQUIRE_FALSE(res.approved);
        CHECK(res.reason == RiskRejectReason::CrossedBookAnomaly);
        CHECK(res.message.find("crossed") != std::string_view::npos);
    }

    SECTION("Rejection: Bid-ask spread exceeds maximum allowed threshold (50 bps)") {
        Order ord{
            .quantity = 100.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "SPY"
        };
        // Spread $5.00 on $500 mark = 100 bps (exceeds 50 bps limit)
        Quote wide_quote{
            .symbol = Symbol("SPY"),
            .bid_price = 497.50,
            .ask_price = 502.50,
            .last_price = 500.00,
            .mark_price = 500.00
        };
        auto res = guard.validate_order(ord, wide_quote, 0.0);
        REQUIRE_FALSE(res.approved);
        CHECK(res.reason == RiskRejectReason::MaxSpreadExceeded);
        CHECK(res.message.find("spread") != std::string_view::npos);
    }

    SECTION("Spread-aware Buy Limit collar pegged to Ask price") {
        // valid_quote: ask_price = 500.20, collar = 3% -> max allowed = 500.20 * 1.03 = 515.206
        Order buy_pass{
            .quantity = 10.0,
            .limit_price = 510.00, // +1.95% over ask, within 3% collar
            .side = OrderSide::Buy,
            .type = OrderType::Limit,
            .symbol = "SPY"
        };
        auto res_pass = guard.validate_order(buy_pass, valid_quote, 0.0);
        REQUIRE(res_pass.approved);
        CHECK(res_pass.reason == RiskRejectReason::None);

        Order buy_breach{
            .quantity = 10.0,
            .limit_price = 520.00, // > 515.206, breaches collar
            .side = OrderSide::Buy,
            .type = OrderType::Limit,
            .symbol = "SPY"
        };
        auto res_breach = guard.validate_order(buy_breach, valid_quote, 0.0);
        REQUIRE_FALSE(res_breach.approved);
        CHECK(res_breach.reason == RiskRejectReason::PriceCollarViolation);
    }

    SECTION("Spread-aware Sell Limit collar pegged to Bid price") {
        // valid_quote: bid_price = 500.00, collar = 3% -> min allowed = 500.00 * (1 - 0.03) = 485.00
        Order sell_pass{
            .quantity = 10.0,
            .limit_price = 490.00, // -2% from bid, within 3% collar
            .side = OrderSide::Sell,
            .type = OrderType::Limit,
            .symbol = "SPY"
        };
        auto res_pass = guard.validate_order(sell_pass, valid_quote, 100.0);
        REQUIRE(res_pass.approved);
        CHECK(res_pass.reason == RiskRejectReason::None);

        Order sell_breach{
            .quantity = 10.0,
            .limit_price = 480.00, // < 485.00, breaches collar
            .side = OrderSide::Sell,
            .type = OrderType::Limit,
            .symbol = "SPY"
        };
        auto res_breach = guard.validate_order(sell_breach, valid_quote, 100.0);
        REQUIRE_FALSE(res_breach.approved);
        CHECK(res_breach.reason == RiskRejectReason::PriceCollarViolation);
    }
}


