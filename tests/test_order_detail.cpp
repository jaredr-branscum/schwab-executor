#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "schwab/order_detail.hpp"

using namespace schwab;
using Catch::Matchers::WithinAbs;

TEST_CASE("OrderExecutionState enum conversions and helper predicates", "[order_detail]") {
    SECTION("String conversions") {
        CHECK(to_string(OrderExecutionState::Unknown) == "UNKNOWN");
        CHECK(to_string(OrderExecutionState::Queued) == "QUEUED");
        CHECK(to_string(OrderExecutionState::Working) == "WORKING");
        CHECK(to_string(OrderExecutionState::Filled) == "FILLED");
        CHECK(to_string(OrderExecutionState::PartiallyFilled) == "PARTIALLY_FILLED");
        CHECK(to_string(OrderExecutionState::Cancelled) == "CANCELLED");
        CHECK(to_string(OrderExecutionState::Rejected) == "REJECTED");
        CHECK(to_string(OrderExecutionState::Expired) == "EXPIRED");
    }

    SECTION("Mapping to core OrderStatus") {
        CHECK(to_order_status(OrderExecutionState::Filled) == OrderStatus::Filled);
        CHECK(to_order_status(OrderExecutionState::Cancelled) == OrderStatus::Cancelled);
        CHECK(to_order_status(OrderExecutionState::Expired) == OrderStatus::Cancelled);
        CHECK(to_order_status(OrderExecutionState::Rejected) == OrderStatus::Rejected);
        CHECK(to_order_status(OrderExecutionState::Working) == OrderStatus::PendingSubmit);
        CHECK(to_order_status(OrderExecutionState::PartiallyFilled) == OrderStatus::PendingSubmit);
        CHECK(to_order_status(OrderExecutionState::Queued) == OrderStatus::PendingSubmit);
        CHECK(to_order_status(OrderExecutionState::Unknown) == OrderStatus::New);
    }

    SECTION("Terminal and lifecycle predicates") {
        OrderDetail filled{.state = OrderExecutionState::Filled, .quantity = 50.0, .filled_quantity = 50.0};
        CHECK(filled.is_terminal());
        CHECK(filled.is_filled());
        CHECK_FALSE(filled.is_working());

        OrderDetail working{.state = OrderExecutionState::Working, .quantity = 50.0, .filled_quantity = 0.0};
        CHECK_FALSE(working.is_terminal());
        CHECK_FALSE(working.is_filled());
        CHECK(working.is_working());

        OrderDetail partial{.state = OrderExecutionState::PartiallyFilled, .quantity = 50.0, .filled_quantity = 25.0};
        CHECK_FALSE(partial.is_terminal());
        CHECK_FALSE(partial.is_filled());
        CHECK(partial.is_working());

        OrderDetail cancelled{.state = OrderExecutionState::Cancelled, .quantity = 50.0, .filled_quantity = 0.0};
        CHECK(cancelled.is_terminal());
        CHECK_FALSE(cancelled.is_filled());
        CHECK_FALSE(cancelled.is_working());

        OrderDetail rejected{.state = OrderExecutionState::Rejected, .quantity = 50.0, .filled_quantity = 0.0};
        CHECK(rejected.is_terminal());
        CHECK_FALSE(rejected.is_filled());
        CHECK_FALSE(rejected.is_working());

        OrderDetail expired{.state = OrderExecutionState::Expired, .quantity = 50.0, .filled_quantity = 0.0};
        CHECK(expired.is_terminal());
        CHECK_FALSE(expired.is_filled());
        CHECK_FALSE(expired.is_working());
    }
}

TEST_CASE("OrderDetailParser::map_status mapping logic", "[order_detail]") {
    CHECK(OrderDetailParser::map_status("FILLED", 10.0, 10.0) == OrderExecutionState::Filled);
    CHECK(OrderDetailParser::map_status("CANCELED", 0.0, 10.0) == OrderExecutionState::Cancelled);
    CHECK(OrderDetailParser::map_status("CANCELLED", 0.0, 10.0) == OrderExecutionState::Cancelled);
    CHECK(OrderDetailParser::map_status("PENDING_CANCEL", 0.0, 10.0) == OrderExecutionState::Cancelled);
    CHECK(OrderDetailParser::map_status("REJECTED", 0.0, 10.0) == OrderExecutionState::Rejected);
    CHECK(OrderDetailParser::map_status("EXPIRED", 0.0, 10.0) == OrderExecutionState::Expired);

    CHECK(OrderDetailParser::map_status("WORKING", 0.0, 10.0) == OrderExecutionState::Working);
    CHECK(OrderDetailParser::map_status("WORKING", 4.0, 10.0) == OrderExecutionState::PartiallyFilled);
    CHECK(OrderDetailParser::map_status("WORKING", 10.0, 10.0) == OrderExecutionState::Filled);

    CHECK(OrderDetailParser::map_status("QUEUED", 0.0, 10.0) == OrderExecutionState::Queued);
    CHECK(OrderDetailParser::map_status("QUEUED", 5.0, 10.0) == OrderExecutionState::PartiallyFilled);
    CHECK(OrderDetailParser::map_status("PENDING_ACTIVATION", 0.0, 10.0) == OrderExecutionState::Queued);
    CHECK(OrderDetailParser::map_status("AWAITING_MANUAL_REVIEW", 0.0, 10.0) == OrderExecutionState::Queued);

    CHECK(OrderDetailParser::map_status("UNKNOWN_XYZ", 0.0, 10.0) == OrderExecutionState::Unknown);
    CHECK(OrderDetailParser::map_status("", 0.0, 10.0) == OrderExecutionState::Unknown);
}

TEST_CASE("OrderDetailParser::parse_order_detail JSON parsing", "[order_detail]") {
    SECTION("Nominal: fully filled single-leg equity market order") {
        std::string json = R"({
            "orderId": 12345678,
            "status": "FILLED",
            "orderType": "MARKET",
            "quantity": 50.0,
            "filledQuantity": 50.0,
            "remainingQuantity": 0.0,
            "enteredTime": "2026-10-11T19:00:00+0000",
            "closeTime": "2026-10-11T19:00:02+0000",
            "orderLegCollection": [
                {
                    "orderLegType": "EQUITY",
                    "legId": 1,
                    "instrument": {
                        "assetType": "EQUITY",
                        "symbol": "SPY"
                    },
                    "instruction": "BUY",
                    "quantity": 50.0
                }
            ],
            "orderActivityCollection": [
                {
                    "activityType": "EXECUTION",
                    "executionType": "FILL",
                    "quantity": 50.0,
                    "executionLegs": [
                        {
                            "legId": 1,
                            "quantity": 50.0,
                            "price": 575.25,
                            "time": "2026-10-11T19:00:01+0000"
                        }
                    ]
                }
            ]
        })";

        auto detail_opt = OrderDetailParser::parse_order_detail(json);
        REQUIRE(detail_opt.has_value());

        const auto& d = *detail_opt;
        CHECK(d.order_id == "12345678");
        CHECK(d.symbol.view() == "SPY");
        CHECK(d.side == OrderSide::Buy);
        CHECK(d.type == OrderType::Market);
        CHECK(d.state == OrderExecutionState::Filled);
        CHECK(d.raw_status == "FILLED");
        CHECK_THAT(d.quantity, WithinAbs(50.0, 1e-9));
        CHECK_THAT(d.filled_quantity, WithinAbs(50.0, 1e-9));
        CHECK_THAT(d.remaining_quantity, WithinAbs(0.0, 1e-9));
        CHECK_THAT(d.average_fill_price, WithinAbs(575.25, 1e-9));
        CHECK(d.entered_time == "2026-10-11T19:00:00+0000");
        CHECK(d.close_time == "2026-10-11T19:00:02+0000");
        CHECK(d.is_terminal());
        CHECK(d.is_filled());
        CHECK_FALSE(d.is_working());
    }

    SECTION("Multi-leg execution VWAP calculation") {
        std::string json = R"({
            "orderId": "987654321",
            "status": "FILLED",
            "orderType": "LIMIT",
            "quantity": 100.0,
            "filledQuantity": 100.0,
            "remainingQuantity": 0.0,
            "orderLegCollection": [
                {
                    "instrument": { "symbol": "AAPL" },
                    "instruction": "SELL",
                    "quantity": 100.0
                }
            ],
            "orderActivityCollection": [
                {
                    "executionLegs": [
                        { "quantity": 40.0, "price": 150.00 },
                        { "quantity": 60.0, "price": 155.00 }
                    ]
                }
            ]
        })";

        // Expected VWAP: (40 * 150 + 60 * 155) / 100 = (6000 + 9300) / 100 = 153.00
        auto detail_opt = OrderDetailParser::parse_order_detail(json);
        REQUIRE(detail_opt.has_value());

        const auto& d = *detail_opt;
        CHECK(d.order_id == "987654321");
        CHECK(d.symbol.view() == "AAPL");
        CHECK(d.side == OrderSide::Sell);
        CHECK(d.type == OrderType::Limit);
        CHECK(d.state == OrderExecutionState::Filled);
        CHECK_THAT(d.average_fill_price, WithinAbs(153.00, 1e-9));
        CHECK(d.is_terminal());
        CHECK(d.is_filled());
    }

    SECTION("Working order with partial fill") {
        std::string json = R"({
            "orderId": 55555,
            "status": "WORKING",
            "orderType": "LIMIT",
            "quantity": 100.0,
            "filledQuantity": 25.0,
            "remainingQuantity": 75.0,
            "orderLegCollection": [
                {
                    "instrument": { "symbol": "QQQ" },
                    "instruction": "BUY",
                    "quantity": 100.0
                }
            ],
            "orderActivityCollection": [
                {
                    "executionLegs": [
                        { "quantity": 25.0, "price": 450.00 }
                    ]
                }
            ]
        })";

        auto detail_opt = OrderDetailParser::parse_order_detail(json);
        REQUIRE(detail_opt.has_value());

        const auto& d = *detail_opt;
        CHECK(d.order_id == "55555");
        CHECK(d.symbol.view() == "QQQ");
        CHECK(d.state == OrderExecutionState::PartiallyFilled);
        CHECK_THAT(d.filled_quantity, WithinAbs(25.0, 1e-9));
        CHECK_THAT(d.remaining_quantity, WithinAbs(75.0, 1e-9));
        CHECK_THAT(d.average_fill_price, WithinAbs(450.00, 1e-9));
        CHECK_FALSE(d.is_terminal());
        CHECK_FALSE(d.is_filled());
        CHECK(d.is_working());
    }

    SECTION("Rejected order without execution legs") {
        std::string json = R"({
            "orderId": 88888,
            "status": "REJECTED",
            "orderType": "MARKET",
            "quantity": 20.0,
            "filledQuantity": 0.0,
            "remainingQuantity": 20.0,
            "orderLegCollection": [
                {
                    "instrument": { "symbol": "NVDA" },
                    "instruction": "BUY",
                    "quantity": 20.0
                }
            ]
        })";

        auto detail_opt = OrderDetailParser::parse_order_detail(json);
        REQUIRE(detail_opt.has_value());

        const auto& d = *detail_opt;
        CHECK(d.order_id == "88888");
        CHECK(d.state == OrderExecutionState::Rejected);
        CHECK_THAT(d.average_fill_price, WithinAbs(0.0, 1e-9));
        CHECK(d.is_terminal());
        CHECK_FALSE(d.is_filled());
        CHECK_FALSE(d.is_working());
    }

    SECTION("Invalid and empty payloads") {
        CHECK_FALSE(OrderDetailParser::parse_order_detail("").has_value());
        CHECK_FALSE(OrderDetailParser::parse_order_detail("   ").has_value());
        CHECK_FALSE(OrderDetailParser::parse_order_detail("{invalid_json").has_value());
        CHECK_FALSE(OrderDetailParser::parse_order_detail("[]").has_value());
        CHECK_FALSE(OrderDetailParser::parse_order_detail("42").has_value());
    }
}

TEST_CASE("calculate_slippage execution slippage metrics", "[order_detail]") {
    SECTION("Buy order slippage") {
        // Adverse: Quote 100.0, Fill 100.20 -> +20.0 bps
        auto s1 = calculate_slippage(OrderSide::Buy, 100.0, 100.20);
        REQUIRE(s1.has_slippage);
        CHECK_THAT(s1.quote_price, WithinAbs(100.0, 1e-9));
        CHECK_THAT(s1.fill_price, WithinAbs(100.20, 1e-9));
        CHECK_THAT(s1.price_diff, WithinAbs(0.20, 1e-9));
        CHECK_THAT(s1.slippage_bps, WithinAbs(20.0, 1e-9));
        CHECK_THAT(s1.abs_slippage_bps, WithinAbs(20.0, 1e-9));

        // Favorable: Quote 100.0, Fill 99.85 -> -15.0 bps
        auto s2 = calculate_slippage(OrderSide::Buy, 100.0, 99.85);
        REQUIRE(s2.has_slippage);
        CHECK_THAT(s2.price_diff, WithinAbs(-0.15, 1e-9));
        CHECK_THAT(s2.slippage_bps, WithinAbs(-15.0, 1e-9));
        CHECK_THAT(s2.abs_slippage_bps, WithinAbs(15.0, 1e-9));
    }

    SECTION("Sell order slippage") {
        // Adverse: Quote 200.0, Fill 199.80 -> sold for less -> +10.0 bps
        auto s1 = calculate_slippage(OrderSide::Sell, 200.0, 199.80);
        REQUIRE(s1.has_slippage);
        CHECK_THAT(s1.quote_price, WithinAbs(200.0, 1e-9));
        CHECK_THAT(s1.fill_price, WithinAbs(199.80, 1e-9));
        CHECK_THAT(s1.price_diff, WithinAbs(-0.20, 1e-9));
        CHECK_THAT(s1.slippage_bps, WithinAbs(10.0, 1e-9));
        CHECK_THAT(s1.abs_slippage_bps, WithinAbs(10.0, 1e-9));

        // Favorable: Quote 200.0, Fill 200.40 -> sold for more -> -20.0 bps
        auto s2 = calculate_slippage(OrderSide::Sell, 200.0, 200.40);
        REQUIRE(s2.has_slippage);
        CHECK_THAT(s2.price_diff, WithinAbs(0.40, 1e-9));
        CHECK_THAT(s2.slippage_bps, WithinAbs(-20.0, 1e-9));
        CHECK_THAT(s2.abs_slippage_bps, WithinAbs(20.0, 1e-9));
    }

    SECTION("Exact match has zero slippage") {
        auto s = calculate_slippage(OrderSide::Buy, 50.0, 50.0);
        REQUIRE(s.has_slippage);
        CHECK_THAT(s.slippage_bps, WithinAbs(0.0, 1e-9));
        CHECK_THAT(s.abs_slippage_bps, WithinAbs(0.0, 1e-9));
    }

    SECTION("Zero and invalid price bounds return has_slippage == false") {
        CHECK_FALSE(calculate_slippage(OrderSide::Buy, 0.0, 100.0).has_slippage);
        CHECK_FALSE(calculate_slippage(OrderSide::Buy, 100.0, 0.0).has_slippage);
        CHECK_FALSE(calculate_slippage(OrderSide::Sell, -10.0, 100.0).has_slippage);
        CHECK_FALSE(calculate_slippage(OrderSide::Sell, 100.0, -5.0).has_slippage);
    }
}

