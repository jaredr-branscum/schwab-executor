#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "schwab/rebalancer.hpp"

using namespace schwab;
using Catch::Matchers::WithinAbs;

TEST_CASE("PortfolioRebalancer delta computation and execution sequencing", "[rebalancer]") {
    SECTION("Sells sequenced strictly before Buys") {
        std::vector<Position> current = {
            Position{.quantity = 100.0, .current_price = 150.0, .symbol = Symbol{"AAPL"}},
            Position{.quantity = 50.0, .current_price = 300.0, .symbol = Symbol{"MSFT"}}
        };

        std::vector<TargetAllocation> targets = {
            TargetAllocation{.symbol = Symbol{"AAPL"}, .target_shares = 60.0, .current_price = 150.0}, // Sell 40
            TargetAllocation{.symbol = Symbol{"MSFT"}, .target_shares = 80.0, .current_price = 300.0}  // Buy 30
        };

        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current);

        REQUIRE(plan.all_approved);
        REQUIRE(plan.orders.size() == 2);
        REQUIRE(plan.rejected_orders.empty());

        // First order MUST be the SELL order (freeing cash)
        const auto& order0 = plan.orders[0];
        REQUIRE(order0.symbol == "AAPL");
        REQUIRE(order0.side == OrderSide::Sell);
        REQUIRE_THAT(order0.quantity, WithinAbs(40.0, 1e-9));

        // Second order MUST be the BUY order
        const auto& order1 = plan.orders[1];
        REQUIRE(order1.symbol == "MSFT");
        REQUIRE(order1.side == OrderSide::Buy);
        REQUIRE_THAT(order1.quantity, WithinAbs(30.0, 1e-9));

        // Notional tracking
        REQUIRE_THAT(plan.total_sell_notional, WithinAbs(6000.0, 1e-9)); // 40 * 150
        REQUIRE_THAT(plan.total_buy_notional, WithinAbs(9000.0, 1e-9));  // 30 * 300
    }

    SECTION("Liquidates untracked positions by default") {
        std::vector<Position> current = {
            Position{.quantity = 100.0, .current_price = 150.0, .symbol = Symbol{"AAPL"}},
            Position{.quantity = 25.0, .current_price = 200.0, .symbol = Symbol{"GOOG"}}
        };

        // GOOG is absent from targets -> must be fully liquidated
        std::vector<TargetAllocation> targets = {
            TargetAllocation{.symbol = Symbol{"AAPL"}, .target_shares = 120.0, .current_price = 150.0} // Buy 20
        };

        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current);

        REQUIRE(plan.all_approved);
        REQUIRE(plan.orders.size() == 2);

        // Untracked liquidation must be sequenced as a Sell first!
        const auto& order0 = plan.orders[0];
        REQUIRE(order0.symbol == "GOOG");
        REQUIRE(order0.side == OrderSide::Sell);
        REQUIRE_THAT(order0.quantity, WithinAbs(25.0, 1e-9));

        // Then Buy order
        const auto& order1 = plan.orders[1];
        REQUIRE(order1.symbol == "AAPL");
        REQUIRE(order1.side == OrderSide::Buy);
        REQUIRE_THAT(order1.quantity, WithinAbs(20.0, 1e-9));
    }

    SECTION("Respects liquidate_untracked = false") {
        std::vector<Position> current = {
            Position{.quantity = 50.0, .current_price = 100.0, .symbol = Symbol{"INTC"}}
        };

        std::vector<TargetAllocation> targets = {
            TargetAllocation{.symbol = Symbol{"NVDA"}, .target_shares = 10.0, .current_price = 120.0}
        };

        RebalancerOptions opts;
        opts.liquidate_untracked = false;

        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current, nullptr, opts);

        REQUIRE(plan.orders.size() == 1);
        REQUIRE(plan.orders[0].symbol == "NVDA");
        REQUIRE(plan.orders[0].side == OrderSide::Buy);
    }

    SECTION("No-op rebalance when targets match current holdings") {
        std::vector<Position> current = {
            Position{.quantity = 100.0, .current_price = 150.0, .symbol = Symbol{"AAPL"}},
            Position{.quantity = 50.0, .current_price = 300.0, .symbol = Symbol{"MSFT"}}
        };

        std::vector<TargetAllocation> targets = {
            TargetAllocation{.symbol = Symbol{"AAPL"}, .target_shares = 100.0, .current_price = 150.0},
            TargetAllocation{.symbol = Symbol{"MSFT"}, .target_shares = 50.0, .current_price = 300.0}
        };

        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current);

        REQUIRE(plan.all_approved);
        REQUIRE(plan.orders.empty());
        REQUIRE(plan.total_sell_notional == 0.0);
        REQUIRE(plan.total_buy_notional == 0.0);
    }

    SECTION("Deltas smaller than min_share_threshold are ignored") {
        std::vector<Position> current = {
            Position{.quantity = 100.000001, .current_price = 150.0, .symbol = Symbol{"AAPL"}}
        };

        std::vector<TargetAllocation> targets = {
            TargetAllocation{.symbol = Symbol{"AAPL"}, .target_shares = 100.0, .current_price = 150.0}
        };

        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current);

        REQUIRE(plan.orders.empty());
    }
}

TEST_CASE("PortfolioRebalancer order options propagation", "[rebalancer]") {
    SECTION("Propagates order type, session, and duration") {
        std::vector<Position> current = {};
        std::vector<TargetAllocation> targets = {
            TargetAllocation{
                .symbol = Symbol{"AAPL"},
                .target_shares = 10.0,
                .current_price = 150.0,
                .limit_price = 149.50
            }
        };

        RebalancerOptions opts;
        opts.order_type = OrderType::Limit;
        opts.session = OrderSession::Seamless;
        opts.duration = TimeInForce::Gtc;

        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current, nullptr, opts);

        REQUIRE(plan.orders.size() == 1);
        const auto& ord = plan.orders[0];
        REQUIRE(ord.type == OrderType::Limit);
        REQUIRE(ord.session == OrderSession::Seamless);
        REQUIRE(ord.duration == TimeInForce::Gtc);
        REQUIRE(ord.limit_price.has_value());
        REQUIRE(*ord.limit_price == 149.50);
    }
}

TEST_CASE("PortfolioRebalancer pre-trade RiskGuard integration", "[rebalancer]") {
    SECTION("RiskGuard blocks oversized orders while permitting compliant orders") {
        RiskLimits limits;
        limits.max_order_notional = 5000.0; // $5k limit per order
        RiskGuard guard(limits);

        std::vector<Position> current = {
            Position{.quantity = 20.0, .current_price = 100.0, .symbol = Symbol{"DIS"}}
        };

        std::vector<TargetAllocation> targets = {
            // Sell 20 DIS = 20 * 100 = $2,000 <= $5,000 (Passes)
            TargetAllocation{.symbol = Symbol{"DIS"}, .target_shares = 0.0, .current_price = 100.0},
            // Buy 100 AMZN @ $180 = $18,000 > $5,000 (Rejected)
            TargetAllocation{.symbol = Symbol{"AMZN"}, .target_shares = 100.0, .current_price = 180.0}
        };

        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current, &guard);

        REQUIRE_FALSE(plan.all_approved);
        // Only DIS sell is approved
        REQUIRE(plan.orders.size() == 1);
        REQUIRE(plan.orders[0].symbol == "DIS");
        REQUIRE(plan.orders[0].side == OrderSide::Sell);

        // AMZN buy is quarantined in rejected_orders
        REQUIRE(plan.rejected_orders.size() == 1);
        REQUIRE(plan.rejected_orders[0].symbol == "AMZN");
        REQUIRE(plan.rejected_orders[0].side == OrderSide::Buy);

        REQUIRE(plan.rejection_reasons.size() == 1);
        REQUIRE_FALSE(plan.rejection_reasons[0].empty());
    }

    SECTION("Fallback to average_price for untracked positions when current_price is zero") {
        RiskLimits limits;
        limits.max_order_notional = 10000.0;
        RiskGuard guard(limits);

        std::vector<Position> current = {
            Position{.quantity = 10.0, .average_price = 50.0, .current_price = 0.0, .symbol = Symbol{"ORCL"}}
        };

        std::vector<TargetAllocation> targets = {};

        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current, &guard);

        REQUIRE(plan.all_approved);
        REQUIRE(plan.orders.size() == 1);
        REQUIRE(plan.orders[0].symbol == "ORCL");
        REQUIRE(plan.orders[0].side == OrderSide::Sell);
        REQUIRE_THAT(plan.total_sell_notional, WithinAbs(500.0, 1e-9)); // 10 * 50
    }

    SECTION("Skips invalid target allocations gracefully") {
        std::vector<Position> current = {};
        std::vector<TargetAllocation> targets = {
            // Negative target shares
            TargetAllocation{.symbol = Symbol{"AAPL"}, .target_shares = -10.0, .current_price = 150.0},
            // Empty symbol
            TargetAllocation{.symbol = Symbol{""}, .target_shares = 10.0, .current_price = 150.0},
            // Invalid limit price (negative)
            TargetAllocation{.symbol = Symbol{"MSFT"}, .target_shares = 10.0, .current_price = 300.0, .limit_price = -5.0},
            // Valid target
            TargetAllocation{.symbol = Symbol{"GOOG"}, .target_shares = 15.0, .current_price = 180.0}
        };

        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current);

        REQUIRE(plan.all_approved);
        REQUIRE(plan.orders.size() == 1);
        REQUIRE(plan.orders[0].symbol == "GOOG");
        REQUIRE(plan.orders[0].side == OrderSide::Buy);
    }
}

TEST_CASE("PortfolioRebalancer order sequencing (Liquidate -> Trim -> Buy)", "[rebalancer]") {
    // Current portfolio:
    // - XYZ: 50 shares (untracked -> 100% full liquidation)
    // - SPY: 100 shares (tracked -> target 60 -> trim 40)
    // - QQQ: 10 shares (tracked -> target 50 -> buy 40)
    std::vector<Position> current = {
        Position{.quantity = 100.0, .current_price = 500.0, .symbol = Symbol{"SPY"}},
        Position{.quantity = 50.0, .current_price = 10.0, .symbol = Symbol{"XYZ"}},
        Position{.quantity = 10.0, .current_price = 450.0, .symbol = Symbol{"QQQ"}}
    };

    std::vector<TargetAllocation> targets = {
        TargetAllocation{.symbol = Symbol{"SPY"}, .target_shares = 60.0, .current_price = 500.0},
        TargetAllocation{.symbol = Symbol{"QQQ"}, .target_shares = 50.0, .current_price = 450.0}
    };

    RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current);

    REQUIRE(plan.all_approved);
    REQUIRE(plan.orders.size() == 3);

    // Untracked liquidation first
    const auto& ord0 = plan.orders[0];
    CHECK(ord0.symbol == "XYZ");
    CHECK(ord0.side == OrderSide::Sell);
    CHECK(ord0.tag == "LIQUIDATE");
    CHECK_THAT(ord0.quantity, WithinAbs(50.0, 1e-9));

    // Tracked target trim second
    const auto& ord1 = plan.orders[1];
    CHECK(ord1.symbol == "SPY");
    CHECK(ord1.side == OrderSide::Sell);
    CHECK(ord1.tag == "REBALANCE");
    CHECK_THAT(ord1.quantity, WithinAbs(40.0, 1e-9));

    // Tracked buy third
    const auto& ord2 = plan.orders[2];
    CHECK(ord2.symbol == "QQQ");
    CHECK(ord2.side == OrderSide::Buy);
    CHECK(ord2.tag == "REBALANCE");
    CHECK_THAT(ord2.quantity, WithinAbs(40.0, 1e-9));
}

TEST_CASE("PortfolioRebalancer quote-aware rebalancing and live risk gating", "[rebalancer]") {
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

    std::vector<Position> current = {
        Position{.quantity = 50.0, .current_price = 500.0, .symbol = Symbol{"SPY"}},
        Position{.quantity = 20.0, .current_price = 100.0, .symbol = Symbol{"BAD"}}
    };

    // SPY buy 10, BAD trim 10
    std::vector<TargetAllocation> targets = {
        TargetAllocation{.symbol = Symbol{"SPY"}, .target_shares = 60.0, .current_price = 500.0},
        TargetAllocation{.symbol = Symbol{"BAD"}, .target_shares = 10.0, .current_price = 100.0}
    };

    std::vector<Quote> quotes = {
        // SPY: Tight 2 bps spread, clean quote
        Quote{
            .symbol = Symbol{"SPY"},
            .bid_price = 500.00,
            .ask_price = 500.10,
            .last_price = 500.05,
            .mark_price = 500.05,
            .total_volume = 10'000'000,
            .quote_time_ms = 1728676800000
        },
        // BAD: Crossed book anomaly (bid 102 > ask 100)
        Quote{
            .symbol = Symbol{"BAD"},
            .bid_price = 102.00,
            .ask_price = 100.00,
            .last_price = 101.00,
            .mark_price = 101.00,
            .total_volume = 1'000,
            .quote_time_ms = 1728676800000
        }
    };

    SECTION("Atomic abort enabled (default): Sell rejected -> All buys are aborted") {
        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current, quotes, &guard);

        REQUIRE_FALSE(plan.all_approved);
        // Sells failed, so all buys must be aborted -> zero approved orders staged!
        REQUIRE(plan.orders.empty());

        // Both BAD (rejected by risk) and SPY (aborted due to atomic policy) are quarantined
        REQUIRE(plan.rejected_orders.size() == 2);
        CHECK(plan.rejected_orders[0].symbol == "BAD");
        CHECK(plan.rejected_orders[0].side == OrderSide::Sell);
        CHECK(plan.rejection_reasons[0].find("crossed") != std::string::npos);

        CHECK(plan.rejected_orders[1].symbol == "SPY");
        CHECK(plan.rejected_orders[1].side == OrderSide::Buy);
        CHECK(plan.rejection_reasons[1].find("atomic abort") != std::string::npos);
    }

    SECTION("Atomic abort disabled: Sells rejected but independent buys proceed") {
        RebalancerOptions opts;
        opts.abort_buys_on_sell_rejection = false;

        RebalancePlan plan = PortfolioRebalancer::generate_plan(targets, current, quotes, &guard, opts);

        REQUIRE_FALSE(plan.all_approved);

        // BAD sell is quarantined
        REQUIRE(plan.rejected_orders.size() == 1);
        CHECK(plan.rejected_orders[0].symbol == "BAD");

        // SPY buy is approved
        REQUIRE(plan.orders.size() == 1);
        CHECK(plan.orders[0].symbol == "SPY");
        CHECK(plan.orders[0].side == OrderSide::Buy);
        CHECK_THAT(plan.orders[0].quantity, WithinAbs(10.0, 1e-9));
    }
}

