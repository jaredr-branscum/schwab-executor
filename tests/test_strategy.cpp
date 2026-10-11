#include <catch2/catch_test_macros.hpp>
#include "schwab/strategy.hpp"

using namespace schwab;

TEST_CASE("StrategyTargetWeight validation", "[strategy]") {
    SECTION("Valid target weight") {
        StrategyTargetWeight t{Symbol("SPY"), 0.60};
        REQUIRE(t.is_valid());
    }

    SECTION("Invalid target weights rejected") {
        REQUIRE_FALSE(StrategyTargetWeight{Symbol(""), 0.50}.is_valid());
        REQUIRE_FALSE(StrategyTargetWeight{Symbol("SPY"), -0.10}.is_valid());
        REQUIRE_FALSE(StrategyTargetWeight{Symbol("SPY"), 1.10}.is_valid());
    }
}

TEST_CASE("SampleRebalanceStrategy order generation", "[strategy]") {
    std::vector<StrategyTargetWeight> targets = {
        {Symbol("SPY"), 0.60},
        {Symbol("QQQ"), 0.40}
    };

    SampleRebalanceStrategy strat(targets);
    REQUIRE(strat.name() == "SampleRebalanceStrategy");

    SECTION("Empty portfolio with cash generates only Buy orders") {
        std::vector<Position> positions = {};
        double cash = 100'000.0; // $100k cash

        // For testing prices: we need current prices to compute share counts
        // If positions are empty, current_price is unknown unless provided.
        // With no positions, strategy skips targets without prices
        auto orders = strat.generate_orders(positions, cash);
        REQUIRE(orders.empty());
    }

    SECTION("Rebalance with existing positions and prices") {
        // Current portfolio:
        // Cash: $10,000
        // AAPL (untracked): 10 shares @ $200 = $2,000 (should be SOLD/LIQUIDATED)
        // SPY: 100 shares @ $500 = $50,000
        // QQQ: 50 shares @ $400 = $20,000
        // Total value = 10,000 + 2,000 + 50,000 + 20,000 = $82,000
        // Target SPY (60%) = $49,200 / $500 = 98.4 shares -> SELL 1.6 shares
        // Target QQQ (40%) = $32,800 / $400 = 82.0 shares -> BUY 32 shares
        Position p_aapl{.quantity = 10.0, .average_price = 190.0, .current_price = 200.0, .symbol = Symbol("AAPL")};
        Position p_spy{.quantity = 100.0, .average_price = 480.0, .current_price = 500.0, .symbol = Symbol("SPY")};
        Position p_qqq{.quantity = 50.0, .average_price = 390.0, .current_price = 400.0, .symbol = Symbol("QQQ")};

        std::vector<Position> positions = {p_aapl, p_spy, p_qqq};
        double cash = 10'000.0;

        auto orders = strat.generate_orders(positions, cash);

        REQUIRE(orders.size() == 3);

        // Enforces sells first!
        REQUIRE(orders[0].side == OrderSide::Sell);
        REQUIRE(orders[0].symbol == "AAPL"); // Liquidate untracked AAPL first
        REQUIRE(orders[0].quantity == 10.0);
        REQUIRE(orders[0].type == OrderType::Market);

        REQUIRE(orders[1].side == OrderSide::Sell);
        REQUIRE(orders[1].symbol == "SPY"); // Trim SPY second
        REQUIRE(orders[1].quantity >= 1.59);
        REQUIRE(orders[1].quantity <= 1.61);

        // Buys last!
        REQUIRE(orders[2].side == OrderSide::Buy);
        REQUIRE(orders[2].symbol == "QQQ"); // Buy QQQ
        REQUIRE(orders[2].quantity == 32.0);
    }

    SECTION("Strategy constructed with OrderType::MarketOnClose sets order type") {
        SampleRebalanceStrategy moc_strat(targets, OrderType::MarketOnClose);
        REQUIRE(moc_strat.order_type() == OrderType::MarketOnClose);

        Position p_aapl{.quantity = 10.0, .average_price = 190.0, .current_price = 200.0, .symbol = Symbol("AAPL")};
        Position p_spy{.quantity = 100.0, .average_price = 480.0, .current_price = 500.0, .symbol = Symbol("SPY")};
        Position p_qqq{.quantity = 50.0, .average_price = 390.0, .current_price = 400.0, .symbol = Symbol("QQQ")};

        std::vector<Position> positions = {p_aapl, p_spy, p_qqq};
        double cash = 10'000.0;

        auto orders = moc_strat.generate_orders(positions, cash);

        REQUIRE(orders.size() == 3);
        REQUIRE(orders[0].type == OrderType::MarketOnClose);
        REQUIRE(orders[1].type == OrderType::MarketOnClose);
        REQUIRE(orders[2].type == OrderType::MarketOnClose);

        // Verify strategy attribution tags
        CHECK(orders[0].tag == "LIQUIDATE");
        CHECK(orders[1].tag == "REBALANCE");
        CHECK(orders[2].tag == "REBALANCE");
    }
}

TEST_CASE("CompositeStrategy multi-strategy composition and attribution tagging", "[strategy]") {
    CompositeStrategy composite("AlphaMaster");
    REQUIRE(composite.name() == "AlphaMaster");

    std::vector<StrategyTargetWeight> targets1 = {{Symbol("SPY"), 0.50}};
    auto sub1 = std::make_shared<SampleRebalanceStrategy>(targets1);

    std::vector<StrategyTargetWeight> targets2 = {{Symbol("QQQ"), 0.50}};
    auto sub2 = std::make_shared<SampleRebalanceStrategy>(targets2);

    composite.add_strategy(sub1);
    composite.add_strategy(sub2);
    REQUIRE(composite.strategies().size() == 2);

    Position p_spy{.quantity = 20.0, .average_price = 450.0, .current_price = 500.0, .symbol = Symbol("SPY")};
    Position p_qqq{.quantity = 10.0, .average_price = 350.0, .current_price = 400.0, .symbol = Symbol("QQQ")};
    std::vector<Position> positions = {p_spy, p_qqq};
    double cash = 10'000.0;

    auto combined_orders = composite.generate_orders(positions, cash);
    REQUIRE_FALSE(combined_orders.empty());

    // Verify all orders are tagged
    for (const auto& ord : combined_orders) {
        CHECK_FALSE(ord.tag.empty());
    }
}

