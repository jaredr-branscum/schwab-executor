#include <catch2/catch_test_macros.hpp>
#include "schwab/execution_driver.hpp"

#include <functional>
#include <vector>

using namespace schwab;

namespace {

class MockTransport : public ITransport {
public:
    bool prewarm_result = true;
    bool connected = true;
    int prewarm_count = 0;
    int send_count = 0;
    std::vector<HttpRequest> recorded_requests;
    std::function<HttpResponse(const HttpRequest&)> handler;

    bool prewarm() override {
        ++prewarm_count;
        connected = prewarm_result;
        return prewarm_result;
    }

    [[nodiscard]] bool is_connected() const noexcept override {
        return connected;
    }

    [[nodiscard]] bool is_idle_expired() const noexcept override {
        return false;
    }

    void disconnect() noexcept override {
        connected = false;
    }

    [[nodiscard]] HttpResponse send(const HttpRequest& request) override {
        ++send_count;
        recorded_requests.push_back(request);
        if (handler) {
            return handler(request);
        }

        HttpResponse default_resp;
        default_resp.status_code = 201;
        default_resp.headers.push_back({"Location", "/trader/v1/accounts/TESTHASH/orders/999888"});
        default_resp.latency = std::chrono::microseconds{250};
        return default_resp;
    }
};

} // namespace

TEST_CASE("ExecutionDriver batch plan submission", "[execution_driver]") {
    SECTION("Successful multi-order execution with order ID extraction") {
        auto mock = std::make_unique<MockTransport>();
        MockTransport* raw_mock = mock.get();

        int order_counter = 1000;
        raw_mock->handler = [&](const HttpRequest& req) {
            REQUIRE(req.path == "/trader/v1/accounts/ABC123DEF/orders");
            REQUIRE(req.auth_header == "Bearer test_access_token_123");
            REQUIRE(req.method == HttpMethod::Post);

            HttpResponse resp;
            resp.status_code = 201;
            resp.headers.push_back({
                "Location",
                "https://api.schwabapi.com/trader/v1/accounts/ABC123DEF/orders/" + std::to_string(++order_counter)
            });
            resp.latency = std::chrono::microseconds{350};
            return resp;
        };

        HttpClient client(std::move(mock));
        ExecutionDriver driver(client, ExecutionDriverConfig{
            .account_hash = "ABC123DEF",
            .dry_run = false
        });

        RebalancePlan plan;
        Order sell_order;
        sell_order.symbol = Symbol{"AAPL"};
        sell_order.side = OrderSide::Sell;
        sell_order.quantity = 40.0;
        sell_order.type = OrderType::Market;

        Order buy_order;
        buy_order.symbol = Symbol{"MSFT"};
        buy_order.side = OrderSide::Buy;
        buy_order.quantity = 25.0;
        buy_order.type = OrderType::Market;

        plan.orders = {sell_order, buy_order};

        ExecutionReport report = driver.execute_plan(plan, "test_access_token_123");

        REQUIRE(report.all_succeeded);
        REQUIRE(report.total_submitted == 2);
        REQUIRE(report.total_succeeded == 2);
        REQUIRE(report.total_failed == 0);
        REQUIRE(report.results.size() == 2);

        // First order
        REQUIRE(report.results[0].success);
        REQUIRE(report.results[0].http_status == 201);
        REQUIRE(report.results[0].order_id.has_value());
        REQUIRE(*report.results[0].order_id == "1001");
        REQUIRE(report.results[0].order.symbol == "AAPL");

        // Second order
        REQUIRE(report.results[1].success);
        REQUIRE(report.results[1].http_status == 201);
        REQUIRE(report.results[1].order_id.has_value());
        REQUIRE(*report.results[1].order_id == "1002");
        REQUIRE(report.results[1].order.symbol == "MSFT");

        REQUIRE(raw_mock->send_count == 2);
    }

    SECTION("Partial failure scenario with error diagnostic extraction") {
        auto mock = std::make_unique<MockTransport>();
        MockTransport* raw_mock = mock.get();

        int call_idx = 0;
        raw_mock->handler = [&](const HttpRequest&) {
            ++call_idx;
            HttpResponse resp;
            if (call_idx == 1) {
                resp.status_code = 201;
                resp.headers.push_back({"Location", "/orders/8877"});
            } else {
                resp.status_code = 400;
                resp.body = "{\"message\":\"Account has insufficient buying power\"}";
            }
            return resp;
        };

        HttpClient client(std::move(mock));
        ExecutionDriver driver(client, ExecutionDriverConfig{
            .account_hash = "HASH999",
            .dry_run = false
        });

        RebalancePlan plan;
        Order ord1;
        ord1.symbol = Symbol{"AAPL"};
        ord1.quantity = 10.0;

        Order ord2;
        ord2.symbol = Symbol{"GOOG"};
        ord2.quantity = 100.0;

        plan.orders = {ord1, ord2};

        ExecutionReport report = driver.execute_plan(plan, "token_xyz");

        REQUIRE_FALSE(report.all_succeeded);
        REQUIRE(report.total_submitted == 2);
        REQUIRE(report.total_succeeded == 1);
        REQUIRE(report.total_failed == 1);

        REQUIRE(report.results[0].success);
        REQUIRE(report.results[0].order_id == "8877");

        REQUIRE_FALSE(report.results[1].success);
        REQUIRE(report.results[1].http_status == 400);
        REQUIRE(report.results[1].error_message == "Account has insufficient buying power");
    }
}

TEST_CASE("ExecutionDriver dry-run interception", "[execution_driver]") {
    SECTION("Dry-run serializes payloads but bypasses network calls") {
        auto mock = std::make_unique<MockTransport>();
        MockTransport* raw_mock = mock.get();

        HttpClient client(std::move(mock));
        ExecutionDriver driver(client, ExecutionDriverConfig{
            .account_hash = "DRY_RUN_HASH",
            .dry_run = true
        });

        RebalancePlan plan;
        Order ord;
        ord.symbol = Symbol{"NVDA"};
        ord.quantity = 15.0;
        plan.orders = {ord};

        ExecutionReport report = driver.execute_plan(plan, "simulated_token");

        REQUIRE(report.all_succeeded);
        REQUIRE(report.total_submitted == 1);
        REQUIRE(report.total_succeeded == 1);
        REQUIRE(report.results[0].http_status == 200);
        REQUIRE(report.results[0].order_id == "DRY_RUN_SIMULATED");

        // Zero network dispatch
        REQUIRE(raw_mock->send_count == 0);
    }
}

TEST_CASE("ExecutionDriver pre-flight input validation", "[execution_driver]") {
    auto mock = std::make_unique<MockTransport>();
    HttpClient client(std::move(mock));
    ExecutionDriver driver(client, ExecutionDriverConfig{
        .account_hash = "TEST_HASH",
        .dry_run = false
    });

    SECTION("Empty access token is rejected") {
        Order ord;
        ord.symbol = Symbol{"AAPL"};
        ord.quantity = 10.0;

        auto res = driver.execute_single_order(ord, "");
        REQUIRE_FALSE(res.success);
        REQUIRE(res.http_status == 401);
        REQUIRE(res.error_message == "Access token is empty");
    }

    SECTION("Empty account hash is rejected") {
        driver.set_account_hash("");

        Order ord;
        ord.symbol = Symbol{"AAPL"};
        ord.quantity = 10.0;

        auto res = driver.execute_single_order(ord, "valid_token");
        REQUIRE_FALSE(res.success);
        REQUIRE(res.http_status == 400);
        REQUIRE(res.error_message == "Account hash is empty");
    }

    SECTION("Invalid order structure is rejected") {
        Order invalid_ord;
        invalid_ord.symbol = Symbol{""};
        invalid_ord.quantity = 0.0;

        auto res = driver.execute_single_order(invalid_ord, "valid_token");
        REQUIRE_FALSE(res.success);
        REQUIRE(res.http_status == 400);
        REQUIRE(res.error_message == "Order fails structural validation");
    }

    SECTION("Order status transition tracking") {
        Order ord;
        ord.symbol = Symbol{"AAPL"};
        ord.quantity = 10.0;
        ord.status = OrderStatus::New;

        auto res = driver.execute_single_order(ord, "valid_token");
        REQUIRE(res.success);
        REQUIRE(res.order.status == OrderStatus::PendingSubmit);
    }
}

TEST_CASE("ExecutionDriver circuit breaker on authorization failure", "[execution_driver]") {
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw_mock = mock.get();

    raw_mock->handler = [](const HttpRequest&) {
        HttpResponse r;
        r.status_code = 401;
        r.body = "{\"error\":\"invalid_token\",\"error_description\":\"Access token expired\"}";
        return r;
    };

    HttpClient client(std::move(mock));
    ExecutionDriver driver(client, ExecutionDriverConfig{
        .account_hash = "TEST_HASH",
        .dry_run = false,
        .stop_on_auth_failure = true
    });

    RebalancePlan plan;
    Order o1; o1.symbol = Symbol{"AAPL"}; o1.quantity = 10.0;
    Order o2; o2.symbol = Symbol{"MSFT"}; o2.quantity = 20.0;
    Order o3; o3.symbol = Symbol{"GOOG"}; o3.quantity = 30.0;
    plan.orders = {o1, o2, o3};

    ExecutionReport report = driver.execute_plan(plan, "expired_token");

    // Only 1 order was dispatched before circuit breaker halted execution
    REQUIRE(raw_mock->send_count == 1);
    REQUIRE(report.total_submitted == 1);
    REQUIRE(report.total_failed == 1);
    REQUIRE_FALSE(report.all_succeeded);
    REQUIRE(report.results.size() == 1);
    REQUIRE(report.results[0].http_status == 401);
    REQUIRE(report.results[0].error_message == "invalid_token: Access token expired");
}

TEST_CASE("ExecutionDriver batch pre-flight validation", "[execution_driver]") {
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw_mock = mock.get();
    HttpClient client(std::move(mock));
    ExecutionDriver driver(client, ExecutionDriverConfig{
        .account_hash = "TEST_HASH",
        .dry_run = false
    });

    RebalancePlan plan;
    Order o1; o1.symbol = Symbol{"AAPL"}; o1.quantity = 10.0;
    plan.orders = {o1};

    SECTION("Empty token fails pre-flight without network dispatch") {
        ExecutionReport report = driver.execute_plan(plan, "");
        REQUIRE_FALSE(report.all_succeeded);
        REQUIRE(report.total_failed == 1);
        REQUIRE(report.results[0].http_status == 401);
        REQUIRE(raw_mock->send_count == 0);
    }

    SECTION("Empty account hash fails pre-flight without network dispatch") {
        driver.set_account_hash("");
        ExecutionReport report = driver.execute_plan(plan, "valid_token");
        REQUIRE_FALSE(report.all_succeeded);
        REQUIRE(report.total_failed == 1);
        REQUIRE(report.results[0].http_status == 400);
        REQUIRE(raw_mock->send_count == 0);
    }
}

TEST_CASE("ExecutionDriver runtime atomic abort on sell failure", "[execution_driver]") {
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw_mock = mock.get();

    // Sell order fails with 500 internal server error from broker
    raw_mock->handler = [](const HttpRequest& req) {
        HttpResponse r;
        if (req.body.find("\"instruction\":\"SELL\"") != std::string_view::npos) {
            r.status_code = 500;
            r.body = "{\"error\":\"broker_error\",\"message\":\"Exchange connection dropped\"}";
        } else {
            r.status_code = 201;
            r.headers.push_back({"Location", "/trader/v1/accounts/MOCK/orders/123"});
        }
        return r;
    };

    HttpClient client(std::move(mock));
    ExecutionDriver driver(client, ExecutionDriverConfig{
        .account_hash = "TEST_HASH",
        .dry_run = false,
        .abort_buys_on_sell_failure = true
    });

    RebalancePlan plan;
    Order s1; s1.symbol = Symbol{"AAPL"}; s1.side = OrderSide::Sell; s1.quantity = 10.0;
    Order b1; b1.symbol = Symbol{"MSFT"}; b1.side = OrderSide::Buy; b1.quantity = 20.0;
    plan.orders = {s1, b1};

    ExecutionReport report = driver.execute_plan(plan, "valid_token");

    // S1 was sent over network and failed
    REQUIRE(raw_mock->send_count == 1);
    REQUIRE(report.total_submitted == 2);
    REQUIRE(report.total_failed == 2);
    REQUIRE_FALSE(report.all_succeeded);

    // B1 was aborted without network dispatch under atomic abort policy!
    REQUIRE(report.results.size() == 2);
    CHECK(report.results[0].http_status == 500);
    CHECK(report.results[1].http_status == 0);
    CHECK(report.results[1].error_message.find("atomic abort") != std::string::npos);
}


