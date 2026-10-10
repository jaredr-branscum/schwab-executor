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
    CHECK(to_string(RiskRejectReason::DryRunIntercepted) == "DRY_RUN_INTERCEPTED");
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

        // 200 shares * $120 = $24,000 notional
        auto res = guard.validate_order(ord, 500.0, 120.0);
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
