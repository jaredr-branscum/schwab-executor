#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "schwab/order_tracker.hpp"

#include <functional>
#include <vector>

using namespace schwab;
using Catch::Matchers::WithinAbs;

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
        default_resp.status_code = 200;
        default_resp.body = "{}";
        return default_resp;
    }
};

std::string make_schwab_order_json(
    std::string_view order_id,
    std::string_view symbol,
    std::string_view instruction,
    std::string_view status,
    double total_qty,
    double filled_qty,
    double fill_price
) {
    std::string json = R"({
        "orderId": ")" + std::string(order_id) + R"(",
        "status": ")" + std::string(status) + R"(",
        "orderType": "MARKET",
        "quantity": )" + std::to_string(total_qty) + R"(,
        "filledQuantity": )" + std::to_string(filled_qty) + R"(,
        "remainingQuantity": )" + std::to_string(total_qty - filled_qty) + R"(,
        "orderLegCollection": [
            {
                "instrument": { "symbol": ")" + std::string(symbol) + R"(" },
                "instruction": ")" + std::string(instruction) + R"("
            }
        ],
        "orderActivityCollection": [
            {
                "executionLegs": [
                    { "quantity": )" + std::to_string(filled_qty) + R"(, "price": )" + std::to_string(fill_price) + R"( }
                ]
            }
        ]
    })";
    return json;
}

} // namespace

TEST_CASE("OrderTracker single query_order verification", "[order_tracker]") {
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw_mock = mock.get();

    raw_mock->handler = [](const HttpRequest& req) {
        REQUIRE(req.path == "/trader/v1/accounts/TEST_ACC_HASH/orders/998877");
        REQUIRE(req.auth_header == "Bearer test_oauth_token");
        REQUIRE(req.method == HttpMethod::Get);

        HttpResponse resp;
        resp.status_code = 200;
        resp.body = make_schwab_order_json("998877", "AAPL", "BUY", "FILLED", 50.0, 50.0, 150.25);
        return resp;
    };

    HttpClient client(std::move(mock));
    OrderTracker tracker(client, OrderTrackerConfig{
        .account_hash = "TEST_ACC_HASH"
    });

    SECTION("Successful query returns parsed OrderDetail") {
        auto detail = tracker.query_order("998877", "test_oauth_token");
        REQUIRE(detail.has_value());
        CHECK(detail->order_id == "998877");
        CHECK(detail->symbol.view() == "AAPL");
        CHECK(detail->side == OrderSide::Buy);
        CHECK(detail->state == OrderExecutionState::Filled);
        CHECK_THAT(detail->quantity, WithinAbs(50.0, 1e-9));
        CHECK_THAT(detail->filled_quantity, WithinAbs(50.0, 1e-9));
        CHECK_THAT(detail->average_fill_price, WithinAbs(150.25, 1e-9));
        CHECK(detail->is_terminal());
        CHECK(detail->is_filled());
    }

    SECTION("Empty parameter validation returns nullopt without dispatch") {
        CHECK_FALSE(tracker.query_order("", "test_oauth_token").has_value());
        CHECK_FALSE(tracker.query_order("998877", "").has_value());

        OrderTracker tracker_no_hash(client, OrderTrackerConfig{.account_hash = ""});
        CHECK_FALSE(tracker_no_hash.query_order("998877", "test_oauth_token").has_value());
    }

    SECTION("Non-200 HTTP response returns nullopt") {
        raw_mock->handler = [](const HttpRequest&) {
            HttpResponse resp;
            resp.status_code = 404;
            resp.body = "{\"message\":\"Order not found\"}";
            return resp;
        };

        auto detail = tracker.query_order("998877", "test_oauth_token");
        CHECK_FALSE(detail.has_value());
    }
}

TEST_CASE("OrderTracker poll_order lifecycle transitions", "[order_tracker]") {
    SECTION("Order immediate fill on first poll") {
        auto mock = std::make_unique<MockTransport>();
        MockTransport* raw_mock = mock.get();

        raw_mock->handler = [](const HttpRequest&) {
            HttpResponse resp;
            resp.status_code = 200;
            resp.body = make_schwab_order_json("101", "MSFT", "BUY", "FILLED", 20.0, 20.0, 420.50);
            return resp;
        };

        HttpClient client(std::move(mock));
        OrderTracker tracker(client, OrderTrackerConfig{
            .account_hash = "ACC1",
            .polling_interval_ms = 10,
            .timeout_ms = 1000
        });

        auto res = tracker.poll_order("101", "token123");
        REQUIRE(res.is_terminal);
        REQUIRE(res.is_filled);
        CHECK(res.order_id == "101");
        CHECK(res.symbol.view() == "MSFT");
        CHECK(res.side == OrderSide::Buy);
        CHECK(res.final_state == OrderExecutionState::Filled);
        CHECK_THAT(res.average_fill_price, WithinAbs(420.50, 1e-9));
        CHECK(raw_mock->send_count == 1);
    }

    SECTION("Order transitions from WORKING to FILLED on second poll") {
        auto mock = std::make_unique<MockTransport>();
        MockTransport* raw_mock = mock.get();

        int poll_count = 0;
        raw_mock->handler = [&](const HttpRequest&) {
            ++poll_count;
            HttpResponse resp;
            resp.status_code = 200;
            if (poll_count == 1) {
                // First poll: still working
                resp.body = make_schwab_order_json("102", "NVDA", "SELL", "WORKING", 100.0, 0.0, 0.0);
            } else {
                // Second poll: filled
                resp.body = make_schwab_order_json("102", "NVDA", "SELL", "FILLED", 100.0, 100.0, 125.75);
            }
            return resp;
        };

        HttpClient client(std::move(mock));
        OrderTracker tracker(client, OrderTrackerConfig{
            .account_hash = "ACC1",
            .polling_interval_ms = 5,
            .timeout_ms = 1000
        });

        auto res = tracker.poll_order("102", "token123");
        REQUIRE(res.is_terminal);
        REQUIRE(res.is_filled);
        CHECK(res.order_id == "102");
        CHECK(res.symbol.view() == "NVDA");
        CHECK(res.side == OrderSide::Sell);
        CHECK(res.final_state == OrderExecutionState::Filled);
        CHECK_THAT(res.average_fill_price, WithinAbs(125.75, 1e-9));
        CHECK(raw_mock->send_count == 2);
    }

    SECTION("Order rejected by market center") {
        auto mock = std::make_unique<MockTransport>();
        MockTransport* raw_mock = mock.get();

        raw_mock->handler = [](const HttpRequest&) {
            HttpResponse resp;
            resp.status_code = 200;
            resp.body = make_schwab_order_json("103", "GOOG", "BUY", "REJECTED", 15.0, 0.0, 0.0);
            return resp;
        };

        HttpClient client(std::move(mock));
        OrderTracker tracker(client, OrderTrackerConfig{
            .account_hash = "ACC1",
            .polling_interval_ms = 5,
            .timeout_ms = 1000
        });

        auto res = tracker.poll_order("103", "token123");
        REQUIRE(res.is_terminal);
        REQUIRE_FALSE(res.is_filled);
        CHECK(res.final_state == OrderExecutionState::Rejected);
        CHECK(raw_mock->send_count == 1);
    }

    SECTION("Order canceled by broker") {
        auto mock = std::make_unique<MockTransport>();
        MockTransport* raw_mock = mock.get();

        raw_mock->handler = [](const HttpRequest&) {
            HttpResponse resp;
            resp.status_code = 200;
            resp.body = make_schwab_order_json("104", "SPY", "SELL", "CANCELED", 30.0, 0.0, 0.0);
            return resp;
        };

        HttpClient client(std::move(mock));
        OrderTracker tracker(client, OrderTrackerConfig{
            .account_hash = "ACC1",
            .polling_interval_ms = 5,
            .timeout_ms = 1000
        });

        auto res = tracker.poll_order("104", "token123");
        REQUIRE(res.is_terminal);
        REQUIRE_FALSE(res.is_filled);
        CHECK(res.final_state == OrderExecutionState::Cancelled);
        CHECK(raw_mock->send_count == 1);
    }

    SECTION("Order polling timeout when order remains WORKING") {
        auto mock = std::make_unique<MockTransport>();
        MockTransport* raw_mock = mock.get();

        raw_mock->handler = [](const HttpRequest&) {
            HttpResponse resp;
            resp.status_code = 200;
            resp.body = make_schwab_order_json("105", "TSLA", "BUY", "WORKING", 10.0, 0.0, 0.0);
            return resp;
        };

        HttpClient client(std::move(mock));
        OrderTracker tracker(client, OrderTrackerConfig{
            .account_hash = "ACC1",
            .polling_interval_ms = 5,
            .timeout_ms = 30 // Small timeout for test
        });

        auto res = tracker.poll_order("105", "token123");
        REQUIRE_FALSE(res.is_terminal);
        REQUIRE_FALSE(res.is_filled);
        CHECK(res.final_state == OrderExecutionState::Working);
        CHECK(res.error_message.find("timed out") != std::string::npos);
        CHECK(raw_mock->send_count >= 1);
    }
}

TEST_CASE("OrderTracker batch reconciliation", "[order_tracker]") {
    SECTION("reconcile_orders tracks multiple in-flight orders round-robin") {
        auto mock = std::make_unique<MockTransport>();
        MockTransport* raw_mock = mock.get();

        int ord2_polls = 0;
        raw_mock->handler = [&](const HttpRequest& req) {
            HttpResponse resp;
            resp.status_code = 200;

            if (req.path.find("/orders/201") != std::string_view::npos) {
                // Ord 201 fills immediately
                resp.body = make_schwab_order_json("201", "AAPL", "BUY", "FILLED", 10.0, 10.0, 150.0);
            } else if (req.path.find("/orders/202") != std::string_view::npos) {
                // Ord 202 is working on 1st poll, filled on 2nd
                ++ord2_polls;
                if (ord2_polls == 1) {
                    resp.body = make_schwab_order_json("202", "MSFT", "BUY", "WORKING", 20.0, 0.0, 0.0);
                } else {
                    resp.body = make_schwab_order_json("202", "MSFT", "BUY", "FILLED", 20.0, 20.0, 420.0);
                }
            } else if (req.path.find("/orders/203") != std::string_view::npos) {
                // Ord 203 cancelled
                resp.body = make_schwab_order_json("203", "GOOG", "SELL", "CANCELED", 30.0, 0.0, 0.0);
            }
            return resp;
        };

        HttpClient client(std::move(mock));
        OrderTracker tracker(client, OrderTrackerConfig{
            .account_hash = "ACC_BATCH",
            .polling_interval_ms = 5,
            .timeout_ms = 1000
        });

        std::vector<std::string> order_ids = {"201", "202", "203"};
        BatchReconciliationReport report = tracker.reconcile_orders(order_ids, "batch_token");

        REQUIRE(report.total_tracked == 3);
        REQUIRE(report.total_filled == 2);
        REQUIRE(report.total_cancelled == 1);
        REQUIRE(report.total_rejected == 0);
        REQUIRE(report.total_timed_out == 0);
        REQUIRE(report.all_terminal);
        REQUIRE_FALSE(report.all_filled);
        REQUIRE(report.orders.size() == 3);

        CHECK(report.orders[0].is_filled);
        CHECK(report.orders[0].order_id == "201");

        CHECK(report.orders[1].is_filled);
        CHECK(report.orders[1].order_id == "202");

        CHECK_FALSE(report.orders[2].is_filled);
        CHECK(report.orders[2].final_state == OrderExecutionState::Cancelled);
        CHECK(report.orders[2].order_id == "203");
    }

    SECTION("Empty order batch returns empty report") {
        auto mock = std::make_unique<MockTransport>();
        HttpClient client(std::move(mock));
        OrderTracker tracker(client, OrderTrackerConfig{
            .account_hash = "ACC_EMPTY"
        });

        std::vector<std::string> empty_ids;
        BatchReconciliationReport report = tracker.reconcile_orders(empty_ids, "token");
        CHECK(report.total_tracked == 0);
        CHECK(report.orders.empty());
    }
}

TEST_CASE("OrderTracker reconcile_report preserves sequence and aggregates failed orders", "[order_tracker]") {
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw_mock = mock.get();

    raw_mock->handler = [](const HttpRequest& req) {
        HttpResponse resp;
        resp.status_code = 200;
        if (req.path.find("/orders/501") != std::string_view::npos) {
            resp.body = make_schwab_order_json("501", "AAPL", "SELL", "FILLED", 10.0, 10.0, 155.0);
        } else if (req.path.find("/orders/502") != std::string_view::npos) {
            resp.body = make_schwab_order_json("502", "MSFT", "BUY", "FILLED", 20.0, 20.0, 410.0);
        }
        return resp;
    };

    HttpClient client(std::move(mock));
    OrderTracker tracker(client, OrderTrackerConfig{
        .account_hash = "REPORT_ACC",
        .polling_interval_ms = 5,
        .timeout_ms = 500
    });

    ExecutionReport exec_report;

    // Order 1: successfully submitted
    OrderExecutionResult res1;
    res1.order.symbol = Symbol{"AAPL"};
    res1.order.side = OrderSide::Sell;
    res1.order.quantity = 10.0;
    res1.success = true;
    res1.order_id = "501";
    exec_report.results.push_back(res1);

    // Order 2: rejected/failed submission (no order ID)
    OrderExecutionResult res2;
    res2.order.symbol = Symbol{"NVDA"};
    res2.order.side = OrderSide::Buy;
    res2.order.quantity = 15.0;
    res2.success = false;
    res2.error_message = "Margin violation";
    exec_report.results.push_back(res2);

    // Order 3: successfully submitted
    OrderExecutionResult res3;
    res3.order.symbol = Symbol{"MSFT"};
    res3.order.side = OrderSide::Buy;
    res3.order.quantity = 20.0;
    res3.success = true;
    res3.order_id = "502";
    exec_report.results.push_back(res3);

    BatchReconciliationReport recon = tracker.reconcile_report(exec_report, "test_token");

    REQUIRE(recon.total_tracked == 3);
    REQUIRE(recon.total_filled == 2);
    REQUIRE(recon.total_rejected == 1);
    REQUIRE(recon.all_terminal);
    REQUIRE_FALSE(recon.all_filled);
    REQUIRE(recon.orders.size() == 3);

    // Order 1 verification
    CHECK(recon.orders[0].order_id == "501");
    CHECK(recon.orders[0].symbol.view() == "AAPL");
    CHECK(recon.orders[0].is_filled);
    CHECK_THAT(recon.orders[0].average_fill_price, WithinAbs(155.0, 1e-9));

    // Order 2 verification (unsubmitted, rejected)
    CHECK(recon.orders[1].order_id.empty());
    CHECK(recon.orders[1].symbol.view() == "NVDA");
    CHECK_FALSE(recon.orders[1].is_filled);
    CHECK(recon.orders[1].final_state == OrderExecutionState::Rejected);
    CHECK(recon.orders[1].error_message == "Margin violation");

    // Order 3 verification
    CHECK(recon.orders[2].order_id == "502");
    CHECK(recon.orders[2].symbol.view() == "MSFT");
    CHECK(recon.orders[2].is_filled);
    CHECK_THAT(recon.orders[2].average_fill_price, WithinAbs(410.0, 1e-9));
}

TEST_CASE("OrderTracker reconcile_report computes slippage with pre-trade quotes", "[order_tracker]") {
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw_mock = mock.get();

    raw_mock->handler = [](const HttpRequest& req) {
        HttpResponse resp;
        resp.status_code = 200;
        if (req.path.find("/orders/601") != std::string_view::npos) {
            // AAPL Buy filled at 150.30 (Quote ask was 150.00 -> +20 bps adverse slippage)
            resp.body = make_schwab_order_json("601", "AAPL", "BUY", "FILLED", 100.0, 100.0, 150.30);
        } else if (req.path.find("/orders/602") != std::string_view::npos) {
            // MSFT Sell filled at 400.40 (Quote bid was 400.00 -> -10 bps favorable slippage)
            resp.body = make_schwab_order_json("602", "MSFT", "SELL", "FILLED", 50.0, 50.0, 400.40);
        }
        return resp;
    };

    HttpClient client(std::move(mock));
    OrderTracker tracker(client, OrderTrackerConfig{
        .account_hash = "SLIP_ACC",
        .polling_interval_ms = 5,
        .timeout_ms = 500
    });

    ExecutionReport exec_report;
    OrderExecutionResult r1;
    r1.order.symbol = Symbol{"AAPL"};
    r1.order.side = OrderSide::Buy;
    r1.order.quantity = 100.0;
    r1.success = true;
    r1.order_id = "601";
    exec_report.results.push_back(r1);

    OrderExecutionResult r2;
    r2.order.symbol = Symbol{"MSFT"};
    r2.order.side = OrderSide::Sell;
    r2.order.quantity = 50.0;
    r2.success = true;
    r2.order_id = "602";
    exec_report.results.push_back(r2);

    Quote q1; q1.symbol = Symbol{"AAPL"}; q1.ask_price = 150.00; q1.bid_price = 149.90; q1.last_price = 149.95;
    Quote q2; q2.symbol = Symbol{"MSFT"}; q2.ask_price = 400.10; q2.bid_price = 400.00; q2.last_price = 400.05;
    std::vector<Quote> quotes = {q1, q2};

    BatchReconciliationReport recon = tracker.reconcile_report(exec_report, "test_token", quotes);

    REQUIRE(recon.total_filled == 2);
    REQUIRE(recon.orders[0].slippage.has_slippage);
    CHECK_THAT(recon.orders[0].slippage.slippage_bps, WithinAbs(20.0, 1e-6));

    REQUIRE(recon.orders[1].slippage.has_slippage);
    CHECK_THAT(recon.orders[1].slippage.slippage_bps, WithinAbs(-10.0, 1e-6));

    // Average slippage: (20.0 + (-10.0)) / 2 = +5.0 bps
    CHECK_THAT(recon.average_slippage_bps, WithinAbs(5.0, 1e-6));
}

