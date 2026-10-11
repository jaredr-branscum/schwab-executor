#include <catch2/catch_test_macros.hpp>
#include "schwab/order_serializer.hpp"

#include <nlohmann/json.hpp>

using namespace schwab;

TEST_CASE("OrderSerializer order JSON serialization", "[order_serializer]") {
    SECTION("Standard Market Buy order") {
        Order order;
        order.symbol = Symbol{"AAPL"};
        order.side = OrderSide::Buy;
        order.type = OrderType::Market;
        order.quantity = 100.0;
        order.duration = TimeInForce::Day;

        std::string json_str = OrderSerializer::serialize_order(order);
        REQUIRE_FALSE(json_str.empty());

        auto j = nlohmann::json::parse(json_str);
        REQUIRE(j["orderType"] == "MARKET");
        REQUIRE(j["session"] == "NORMAL");
        REQUIRE(j["duration"] == "DAY");
        REQUIRE(j["orderStrategyType"] == "SINGLE");
        REQUIRE_FALSE(j.contains("price"));

        REQUIRE(j["orderLegCollection"].is_array());
        REQUIRE(j["orderLegCollection"].size() == 1);
        const auto& leg = j["orderLegCollection"][0];
        REQUIRE(leg["instruction"] == "BUY");
        REQUIRE(leg["quantity"] == 100);
        REQUIRE(leg["instrument"]["symbol"] == "AAPL");
        REQUIRE(leg["instrument"]["assetType"] == "EQUITY");
    }

    SECTION("Standard Market Sell order") {
        Order order;
        order.symbol = Symbol{"MSFT"};
        order.side = OrderSide::Sell;
        order.type = OrderType::Market;
        order.quantity = 50.0;

        std::string json_str = OrderSerializer::serialize_order(order);
        auto j = nlohmann::json::parse(json_str);
        REQUIRE(j["orderLegCollection"][0]["instruction"] == "SELL");
        REQUIRE(j["orderLegCollection"][0]["quantity"] == 50);
        REQUIRE(j["orderLegCollection"][0]["instrument"]["symbol"] == "MSFT");
    }

    SECTION("Limit Buy order with price") {
        Order order;
        order.symbol = Symbol{"GOOG"};
        order.side = OrderSide::Buy;
        order.type = OrderType::Limit;
        order.quantity = 25.0;
        order.limit_price = 175.50;

        std::string json_str = OrderSerializer::serialize_order(order);
        auto j = nlohmann::json::parse(json_str);
        REQUIRE(j["orderType"] == "LIMIT");
        REQUIRE(j.contains("price"));
        REQUIRE(j["price"] == 175.50);
    }

    SECTION("Market on Close (MOC) order") {
        Order order;
        order.symbol = Symbol{"NVDA"};
        order.side = OrderSide::Buy;
        order.type = OrderType::MarketOnClose;
        order.quantity = 10.0;

        std::string json_str = OrderSerializer::serialize_order(order);
        auto j = nlohmann::json::parse(json_str);
        REQUIRE(j["orderType"] == "MARKET_ON_CLOSE");
    }

    SECTION("Extended hours session serialization") {
        Order am_order;
        am_order.symbol = Symbol{"AAPL"};
        am_order.quantity = 10.0;
        am_order.session = OrderSession::Am;
        auto j_am = nlohmann::json::parse(OrderSerializer::serialize_order(am_order));
        REQUIRE(j_am["session"] == "AM");

        Order pm_order;
        pm_order.symbol = Symbol{"AAPL"};
        pm_order.quantity = 10.0;
        pm_order.session = OrderSession::Pm;
        auto j_pm = nlohmann::json::parse(OrderSerializer::serialize_order(pm_order));
        REQUIRE(j_pm["session"] == "PM");

        Order seamless_order;
        seamless_order.symbol = Symbol{"AAPL"};
        seamless_order.quantity = 10.0;
        seamless_order.session = OrderSession::Seamless;
        auto j_seamless = nlohmann::json::parse(OrderSerializer::serialize_order(seamless_order));
        REQUIRE(j_seamless["session"] == "SEAMLESS");
    }

    SECTION("Fractional share quantity") {
        Order order;
        order.symbol = Symbol{"AMZN"};
        order.side = OrderSide::Buy;
        order.type = OrderType::Market;
        order.quantity = 12.5;

        std::string json_str = OrderSerializer::serialize_order(order);
        auto j = nlohmann::json::parse(json_str);
        REQUIRE(j["orderLegCollection"][0]["quantity"] == 12.5);
    }

    SECTION("Buffer re-use preserves capacity") {
        Order order;
        order.symbol = Symbol{"META"};
        order.quantity = 15.0;

        std::string preallocated;
        preallocated.reserve(1024);
        const char* initial_ptr = preallocated.data();

        REQUIRE(OrderSerializer::serialize_order(order, preallocated));
        REQUIRE(preallocated.capacity() >= 1024);
        REQUIRE(preallocated.data() == initial_ptr);
        REQUIRE_FALSE(preallocated.empty());
    }

    SECTION("Invalid orders rejected") {
        std::string dummy;

        Order empty_sym;
        empty_sym.quantity = 10.0;
        REQUIRE_FALSE(OrderSerializer::serialize_order(empty_sym, dummy));
        REQUIRE(OrderSerializer::serialize_order(empty_sym).empty());

        Order zero_qty;
        zero_qty.symbol = Symbol{"AAPL"};
        zero_qty.quantity = 0.0;
        REQUIRE_FALSE(OrderSerializer::serialize_order(zero_qty, dummy));

        Order limit_no_price;
        limit_no_price.symbol = Symbol{"AAPL"};
        limit_no_price.type = OrderType::Limit;
        limit_no_price.quantity = 10.0;
        limit_no_price.limit_price = std::nullopt;
        Order invalid_sym_chars;
        invalid_sym_chars.symbol = Symbol{"AAPL\"quote"};
        invalid_sym_chars.quantity = 10.0;
        REQUIRE_FALSE(OrderSerializer::serialize_order(invalid_sym_chars, dummy));
    }
}

TEST_CASE("OrderSerializer order ID extraction from Location header", "[order_serializer]") {
    SECTION("Absolute URL Location header") {
        HttpResponse resp;
        resp.status_code = 201;
        resp.headers.push_back({"Location", "https://api.schwabapi.com/trader/v1/accounts/ABC123DEF/orders/987654321"});

        auto order_id = OrderSerializer::extract_order_id(resp);
        REQUIRE(order_id.has_value());
        REQUIRE(*order_id == "987654321");
    }

    SECTION("Relative URL Location header") {
        HttpResponse resp;
        resp.status_code = 201;
        resp.headers.push_back({"Location", "/trader/v1/accounts/ABC123DEF/orders/11223344"});

        auto order_id = OrderSerializer::extract_order_id(resp);
        REQUIRE(order_id.has_value());
        REQUIRE(*order_id == "11223344");
    }

    SECTION("Trailing slash in Location header") {
        HttpResponse resp;
        resp.status_code = 201;
        resp.headers.push_back({"Location", "https://api.schwabapi.com/trader/v1/accounts/ABC/orders/55443322/"});

        auto order_id = OrderSerializer::extract_order_id(resp);
        REQUIRE(order_id.has_value());
        REQUIRE(*order_id == "55443322");
    }

    SECTION("Location header with query parameters and fragment") {
        HttpResponse resp;
        resp.status_code = 201;
        resp.headers.push_back({"Location", "https://api.schwabapi.com/trader/v1/accounts/ABC/orders/987654321?accountNumber=123#summary"});

        auto order_id = OrderSerializer::extract_order_id(resp);
        REQUIRE(order_id.has_value());
        REQUIRE(*order_id == "987654321");
    }

    SECTION("Location header with trailing slash and query parameters") {
        HttpResponse resp;
        resp.status_code = 201;
        resp.headers.push_back({"Location", "https://api.schwabapi.com/trader/v1/accounts/ABC/orders/44332211/?accountNumber=123"});

        auto order_id = OrderSerializer::extract_order_id(resp);
        REQUIRE(order_id.has_value());
        REQUIRE(*order_id == "44332211");
    }

    SECTION("Missing Location header returns nullopt") {
        HttpResponse resp;
        resp.status_code = 201;
        REQUIRE(OrderSerializer::extract_order_id(resp) == std::nullopt);
    }

    SECTION("Empty Location header returns nullopt") {
        HttpResponse resp;
        resp.status_code = 201;
        resp.headers.push_back({"Location", "   "});
        REQUIRE(OrderSerializer::extract_order_id(resp) == std::nullopt);
    }
}

TEST_CASE("OrderSerializer error message extraction", "[order_serializer]") {
    SECTION("JSON with 'error' and 'error_description' fields") {
        HttpResponse resp;
        resp.status_code = 401;
        resp.body = "{\"error\":\"invalid_token\",\"error_description\":\"Token has expired\"}";

        REQUIRE(OrderSerializer::parse_error_message(resp) == "invalid_token: Token has expired");
    }

    SECTION("JSON with 'error' and 'description' fields") {
        HttpResponse resp;
        resp.status_code = 400;
        resp.body = "{\"error\":\"validation_error\",\"description\":\"Order size exceeds limits\"}";

        REQUIRE(OrderSerializer::parse_error_message(resp) == "validation_error: Order size exceeds limits");
    }

    SECTION("JSON with 'message' field") {
        HttpResponse resp;
        resp.status_code = 400;
        resp.body = "{\"message\":\"Order size exceeds account maximum\"}";

        REQUIRE(OrderSerializer::parse_error_message(resp) == "Order size exceeds account maximum");
    }

    SECTION("JSON with 'error' field") {
        HttpResponse resp;
        resp.status_code = 401;
        resp.body = "{\"error\":\"Unauthorized API access\"}";

        REQUIRE(OrderSerializer::parse_error_message(resp) == "Unauthorized API access");
    }

    SECTION("JSON with 'errors' array") {
        HttpResponse resp;
        resp.status_code = 400;
        resp.body = "{\"errors\":[\"Invalid symbol\",\"Market is currently closed\"]}";

        REQUIRE(OrderSerializer::parse_error_message(resp) == "Invalid symbol, Market is currently closed");
    }

    SECTION("Plain text error body") {
        HttpResponse resp;
        resp.status_code = 502;
        resp.body = "Bad Gateway from upstream gateway";

        REQUIRE(OrderSerializer::parse_error_message(resp) == "Bad Gateway from upstream gateway");
    }

    SECTION("Empty body returns status reason phrase") {
        HttpResponse resp;
        resp.status_code = 400;
        resp.body = "";

        REQUIRE(OrderSerializer::parse_error_message(resp) == "Bad Request");
    }
}
