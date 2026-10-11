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
