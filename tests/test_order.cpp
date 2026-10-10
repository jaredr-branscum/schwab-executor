#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "schwab/order.hpp"

using namespace schwab;
using Catch::Matchers::WithinAbs;

TEST_CASE("Cache-line memory layout and alignment guarantees", "[order]") {
    SECTION("Packed into 64-byte L1 CPU cache lines") {
        CHECK(sizeof(Order) <= 64);
        CHECK(sizeof(Position) <= 64);
        CHECK(alignof(Order) == 8);
        CHECK(alignof(Position) == 8);
    }
}

TEST_CASE("Order enum conversions and string parsing", "[order]") {
    SECTION("OrderSide string conversions") {
        CHECK(to_string(OrderSide::Buy) == "BUY");
        CHECK(to_string(OrderSide::Sell) == "SELL");

        CHECK(order_side_from_string("BUY") == OrderSide::Buy);
        CHECK(order_side_from_string("buy") == OrderSide::Buy);
        CHECK(order_side_from_string("SELL") == OrderSide::Sell);
        CHECK(order_side_from_string("sell") == OrderSide::Sell);
        CHECK(!order_side_from_string("HOLD").has_value());
    }

    SECTION("OrderType string conversions") {
        CHECK(to_string(OrderType::Market) == "MARKET");
        CHECK(to_string(OrderType::Limit) == "LIMIT");
        CHECK(to_string(OrderType::MarketOnClose) == "MARKET_ON_CLOSE");

        CHECK(order_type_from_string("MARKET") == OrderType::Market);
        CHECK(order_type_from_string("market") == OrderType::Market);
        CHECK(order_type_from_string("LIMIT") == OrderType::Limit);
        CHECK(order_type_from_string("limit") == OrderType::Limit);
        CHECK(order_type_from_string("MARKET_ON_CLOSE") == OrderType::MarketOnClose);
        CHECK(order_type_from_string("market_on_close") == OrderType::MarketOnClose);
        CHECK(order_type_from_string("MOC") == OrderType::MarketOnClose);
        CHECK(order_type_from_string("moc") == OrderType::MarketOnClose);
        CHECK(!order_type_from_string("STOP").has_value());
    }

    SECTION("TimeInForce string conversions") {
        CHECK(to_string(TimeInForce::Day) == "DAY");
        CHECK(to_string(TimeInForce::Gtc) == "GTC");

        CHECK(time_in_force_from_string("DAY") == TimeInForce::Day);
        CHECK(time_in_force_from_string("day") == TimeInForce::Day);
        CHECK(time_in_force_from_string("GTC") == TimeInForce::Gtc);
        CHECK(time_in_force_from_string("gtc") == TimeInForce::Gtc);
        CHECK(time_in_force_from_string("GOOD_TILL_CANCEL") == TimeInForce::Gtc);
        CHECK(!time_in_force_from_string("IOC").has_value());
    }

    SECTION("OrderStatus string conversions") {
        CHECK(to_string(OrderStatus::New) == "NEW");
        CHECK(to_string(OrderStatus::PendingSubmit) == "PENDING_SUBMIT");
        CHECK(to_string(OrderStatus::Filled) == "FILLED");
        CHECK(to_string(OrderStatus::Cancelled) == "CANCELLED");
        CHECK(to_string(OrderStatus::Rejected) == "REJECTED");
    }
}

TEST_CASE("Order validation rules", "[order]") {
    SECTION("Valid Market order") {
        Order ord{
            .quantity = 50.0,
            .limit_price = std::nullopt,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .duration = TimeInForce::Day,
            .status = OrderStatus::New,
            .symbol = "AAPL"
        };

        CHECK(ord.is_valid());
    }

    SECTION("Valid MarketOnClose order") {
        Order ord{
            .quantity = 100.0,
            .limit_price = std::nullopt,
            .side = OrderSide::Sell,
            .type = OrderType::MarketOnClose,
            .duration = TimeInForce::Day,
            .status = OrderStatus::New,
            .symbol = "AAPL"
        };

        CHECK(ord.is_valid());
    }

    SECTION("Valid Limit order") {
        Order ord{
            .quantity = 25.0,
            .limit_price = 420.50,
            .side = OrderSide::Sell,
            .type = OrderType::Limit,
            .duration = TimeInForce::Day,
            .status = OrderStatus::New,
            .symbol = "MSFT"
        };

        CHECK(ord.is_valid());
    }

    SECTION("Invalid Limit order without limit price") {
        Order ord{
            .quantity = 25.0,
            .limit_price = std::nullopt,
            .side = OrderSide::Buy,
            .type = OrderType::Limit,
            .duration = TimeInForce::Day,
            .status = OrderStatus::New,
            .symbol = "MSFT"
        };

        CHECK(!ord.is_valid());
    }

    SECTION("Invalid Limit order with non-positive price") {
        Order ord{
            .quantity = 25.0,
            .limit_price = 0.0,
            .side = OrderSide::Buy,
            .type = OrderType::Limit,
            .duration = TimeInForce::Day,
            .status = OrderStatus::New,
            .symbol = "MSFT"
        };

        CHECK(!ord.is_valid());

        ord.limit_price = -10.0;
        CHECK(!ord.is_valid());
    }

    SECTION("Invalid orders with empty symbol or non-positive quantity") {
        Order empty_sym{
            .quantity = 10.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = ""
        };
        CHECK(!empty_sym.is_valid());

        Order zero_qty{
            .quantity = 0.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "AAPL"
        };
        CHECK(!zero_qty.is_valid());

        Order neg_qty{
            .quantity = -5.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "AAPL"
        };
        CHECK(!neg_qty.is_valid());
    }
}

TEST_CASE("Order estimated notional calculation", "[order]") {
    SECTION("Limit order uses limit price directly") {
        Order ord{
            .quantity = 10.0,
            .limit_price = 125.00,
            .side = OrderSide::Buy,
            .type = OrderType::Limit,
            .symbol = "NVDA"
        };

        // 10 * 125.00 = 1250.00
        CHECK_THAT(ord.estimated_notional(), WithinAbs(1250.0, 1e-9));
        // Fallback price is ignored for Limit orders with valid limit price
        CHECK_THAT(ord.estimated_notional(999.0), WithinAbs(1250.0, 1e-9));
    }

    SECTION("Market order uses fallback price") {
        Order ord{
            .quantity = 10.0,
            .limit_price = std::nullopt,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = "NVDA"
        };

        // 10 * 130.50 = 1305.00
        CHECK_THAT(ord.estimated_notional(130.50), WithinAbs(1305.0, 1e-9));
        // If no fallback price provided, returns 0.0
        CHECK_THAT(ord.estimated_notional(0.0), WithinAbs(0.0, 1e-9));
    }

    SECTION("MarketOnClose order uses fallback price") {
        Order ord{
            .quantity = 20.0,
            .limit_price = std::nullopt,
            .side = OrderSide::Sell,
            .type = OrderType::MarketOnClose,
            .symbol = "NVDA"
        };

        // 20 * 130.00 = 2600.00
        CHECK_THAT(ord.estimated_notional(130.00), WithinAbs(2600.0, 1e-9));
    }
}

TEST_CASE("Position valuation and P&L calculations", "[order]") {
    SECTION("Profitable long position") {
        Position pos{
            .quantity = 100.0,
            .average_price = 150.0,
            .current_price = 175.0,
            .symbol = "GOOGL"
        };

        // Market value = 100 * 175 = 17,500
        CHECK_THAT(pos.market_value(), WithinAbs(17500.0, 1e-9));
        // Cost basis = 100 * 150 = 15,000
        CHECK_THAT(pos.cost_basis(), WithinAbs(15000.0, 1e-9));
        // Unrealized PnL = 17,500 - 15,000 = +2,500
        CHECK_THAT(pos.unrealized_pnl(), WithinAbs(2500.0, 1e-9));
        // Unrealized PnL % = 2,500 / 15,000 = +16.6666667%
        CHECK_THAT(pos.unrealized_pnl_percent(), WithinAbs(0.1666666667, 1e-9));
    }

    SECTION("Losing long position") {
        Position pos{
            .quantity = 100.0,
            .average_price = 200.0,
            .current_price = 180.0,
            .symbol = "GOOGL"
        };

        // Market value = 18,000
        CHECK_THAT(pos.market_value(), WithinAbs(18000.0, 1e-9));
        // Cost basis = 20,000
        CHECK_THAT(pos.cost_basis(), WithinAbs(20000.0, 1e-9));
        // Unrealized PnL = -2,000
        CHECK_THAT(pos.unrealized_pnl(), WithinAbs(-2000.0, 1e-9));
        // Unrealized PnL % = -2,000 / 20,000 = -10.0%
        CHECK_THAT(pos.unrealized_pnl_percent(), WithinAbs(-0.10, 1e-9));
    }

    SECTION("Empty position (zero quantity)") {
        Position pos{
            .quantity = 0.0,
            .average_price = 0.0,
            .current_price = 100.0,
            .symbol = "CASH"
        };

        CHECK_THAT(pos.market_value(), WithinAbs(0.0, 1e-9));
        CHECK_THAT(pos.cost_basis(), WithinAbs(0.0, 1e-9));
        CHECK_THAT(pos.unrealized_pnl(), WithinAbs(0.0, 1e-9));
        CHECK_THAT(pos.unrealized_pnl_percent(), WithinAbs(0.0, 1e-9));
    }
}

TEST_CASE("Symbol FixedString bounds checking and error handling", "[order]") {
    SECTION("Valid ticker symbols up to 15 characters") {
        Symbol s1{"AAPL"};
        CHECK(s1 == "AAPL");
        CHECK(s1.size() == 4);
        CHECK(!s1.empty());

        Symbol s2{"BRK.B"};
        CHECK(s2 == "BRK.B");
        CHECK(s2.size() == 5);

        // Maximum 15 characters
        Symbol s3{"123456789012345"};
        CHECK(s3 == "123456789012345");
        CHECK(s3.size() == 15);
    }

    SECTION("Oversized runtime symbol fails gracefully without silent truncation") {
        std::string oversized = "OVERSIZED_SYMBOL_NAME_12345";
        Symbol invalid_sym{std::string_view(oversized)};

        // Must refuse to copy and remain empty/invalid
        CHECK(invalid_sym.empty());
        CHECK(invalid_sym.size() == 0);
        CHECK(invalid_sym.view().empty());

        // An order constructed with an oversized runtime symbol is rejected by validation
        Order ord{
            .quantity = 10.0,
            .side = OrderSide::Buy,
            .type = OrderType::Market,
            .symbol = std::string_view(oversized)
        };
        CHECK(!ord.is_valid());
    }
}
