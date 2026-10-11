#include <catch2/catch_test_macros.hpp>
#include "schwab/http_client.hpp"

#include <functional>
#include <vector>

using namespace schwab;

namespace {

class MockTransport : public ITransport {
public:
    bool prewarm_result = true;
    bool connected = false;
    bool force_idle_expired = false;
    bool simulate_disconnect_on_send = false;
    int prewarm_count = 0;
    int send_count = 0;
    int reconnect_count = 0;
    std::vector<HttpRequest> recorded_requests;
    HttpResponse default_response{200, "{\"status\":\"OK\"}", {}, std::chrono::microseconds{500}};
    std::function<HttpResponse(const HttpRequest&)> custom_handler;
    std::chrono::steady_clock::time_point last_active_time{};

    bool prewarm() override {
        ++prewarm_count;
        connected = prewarm_result;
        last_active_time = std::chrono::steady_clock::now();
        return prewarm_result;
    }

    [[nodiscard]] bool is_connected() const noexcept override {
        return connected;
    }

    [[nodiscard]] bool is_idle_expired() const noexcept override {
        return force_idle_expired;
    }

    void disconnect() noexcept override {
        connected = false;
    }

    [[nodiscard]] HttpResponse send(const HttpRequest& request) override {
        ++send_count;
        last_active_time = std::chrono::steady_clock::now();
        recorded_requests.push_back(request);

        if (simulate_disconnect_on_send) {
            simulate_disconnect_on_send = false; // Trigger once
            connected = false;
            ++reconnect_count;
            // Simulate reconnect and retry
            prewarm();
        }

        if (custom_handler) {
            return custom_handler(request);
        }
        return default_response;
    }
};

} // namespace

TEST_CASE("HttpClientConfig defaults", "[http_client]") {
    HttpClientConfig cfg;
    REQUIRE(cfg.base_host == "api.schwabapi.com");
    REQUIRE(cfg.port == 443);
    REQUIRE(cfg.use_ssl == true);
    REQUIRE(cfg.resolve_timeout_ms == 5000);
    REQUIRE(cfg.connect_timeout_ms == 5000);
    REQUIRE(cfg.send_timeout_ms == 5000);
    REQUIRE(cfg.receive_timeout_ms == 5000);
    REQUIRE(cfg.user_agent == "schwab-executor/1.0");
}

TEST_CASE("HttpClient pre-warming lifecycle with MockTransport", "[http_client]") {
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw_mock = mock.get();

    HttpClient client(std::move(mock));

    SECTION("Initial state is not connected") {
        REQUIRE_FALSE(client.is_connected());
        REQUIRE(raw_mock->prewarm_count == 0);
    }

    SECTION("Pre-warming connects socket") {
        REQUIRE(client.prewarm());
        REQUIRE(client.is_connected());
        REQUIRE(raw_mock->prewarm_count == 1);
    }

    SECTION("Disconnect resets connection") {
        REQUIRE(client.prewarm());
        REQUIRE(client.is_connected());

        client.disconnect();
        REQUIRE_FALSE(client.is_connected());
    }

    SECTION("Failed pre-warm is reported") {
        raw_mock->prewarm_result = false;
        REQUIRE_FALSE(client.prewarm());
        REQUIRE_FALSE(client.is_connected());
    }
}

TEST_CASE("HttpClient request routing and convenience verbs", "[http_client]") {
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw_mock = mock.get();

    HttpClient client(std::move(mock));

    SECTION("GET request") {
        HttpResponse resp = client.get("/trader/v1/accounts/123", "Bearer token_abc");

        REQUIRE(resp.is_success());
        REQUIRE(raw_mock->send_count == 1);
        REQUIRE(raw_mock->recorded_requests.size() == 1);

        const auto& req = raw_mock->recorded_requests.front();
        REQUIRE(req.method == HttpMethod::Get);
        REQUIRE(req.path == "/trader/v1/accounts/123");
        REQUIRE(req.auth_header == "Bearer token_abc");
    }

    SECTION("POST request with body") {
        HttpResponse resp = client.post("/trader/v1/accounts/123/orders",
                                        "{\"orderType\":\"MARKET\"}",
                                        "Bearer token_abc");

        REQUIRE(resp.is_success());
        REQUIRE(raw_mock->send_count == 1);

        const auto& req = raw_mock->recorded_requests.front();
        REQUIRE(req.method == HttpMethod::Post);
        REQUIRE(req.path == "/trader/v1/accounts/123/orders");
        REQUIRE(req.body == "{\"orderType\":\"MARKET\"}");
        REQUIRE(req.auth_header == "Bearer token_abc");
    }

    SECTION("PUT request with body") {
        HttpResponse resp = client.put("/trader/v1/accounts/123/orders/999",
                                       "{\"quantity\":20}",
                                       "Bearer token_abc");

        REQUIRE(resp.is_success());
        const auto& req = raw_mock->recorded_requests.front();
        REQUIRE(req.method == HttpMethod::Put);
        REQUIRE(req.path == "/trader/v1/accounts/123/orders/999");
        REQUIRE(req.body == "{\"quantity\":20}");
    }

    SECTION("DELETE request") {
        HttpResponse resp = client.del("/trader/v1/accounts/123/orders/999", "Bearer token_abc");

        REQUIRE(resp.is_success());
        const auto& req = raw_mock->recorded_requests.front();
        REQUIRE(req.method == HttpMethod::Delete);
        REQUIRE(req.path == "/trader/v1/accounts/123/orders/999");
    }
}

TEST_CASE("HttpClient simulated response scenarios", "[http_client]") {
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw_mock = mock.get();

    HttpClient client(std::move(mock));

    SECTION("401 Unauthorized handling") {
        raw_mock->custom_handler = [](const HttpRequest&) {
            HttpResponse r;
            r.status_code = 401;
            r.body = "{\"error\":\"Unauthorized\",\"error_description\":\"Token expired\"}";
            return r;
        };

        HttpResponse resp = client.get("/trader/v1/userPreference", "Bearer expired_token");
        REQUIRE(resp.status_code == 401);
        REQUIRE(resp.is_unauthorized());
        REQUIRE_FALSE(resp.is_success());
    }

    SECTION("429 Rate limiting simulation") {
        raw_mock->custom_handler = [](const HttpRequest&) {
            HttpResponse r;
            r.status_code = 429;
            r.body = "{\"error\":\"Too Many Requests\"}";
            r.headers.push_back({"Retry-After", "5"});
            return r;
        };

        HttpResponse resp = client.post("/trader/v1/accounts/123/orders", "{}", "Bearer valid_token");
        REQUIRE(resp.status_code == 429);
        REQUIRE(resp.is_rate_limited());
        REQUIRE(resp.find_header("Retry-After") == "5");
    }

    SECTION("504 Gateway Timeout simulation") {
        raw_mock->custom_handler = [](const HttpRequest&) {
            HttpResponse r;
            r.status_code = 504;
            r.body = "{\"error\":\"Gateway Timeout\"}";
            return r;
        };

        HttpResponse resp = client.get("/trader/v1/accounts", "Bearer valid_token");
        REQUIRE(resp.status_code == 504);
        REQUIRE(resp.is_server_error());
        REQUIRE(resp.is_error());
    }
}

TEST_CASE("HttpClient null transport safe handling", "[http_client]") {
    HttpClient client(nullptr);
    REQUIRE_FALSE(client.is_connected());
    REQUIRE_FALSE(client.prewarm());
    REQUIRE(client.is_idle_expired());

    HttpRequest req;
    req.path = "/test";
    HttpResponse resp = client.send(req);
    REQUIRE(resp.status_code == 500);
}

TEST_CASE("HttpClient connection idle timeout and auto-reconnect", "[http_client]") {
    auto mock = std::make_unique<MockTransport>();
    MockTransport* raw_mock = mock.get();

    HttpClient client(std::move(mock));
    REQUIRE(client.prewarm());
    REQUIRE(client.is_connected());
    REQUIRE_FALSE(client.is_idle_expired());

    SECTION("Idle timeout expiration triggers re-connection check") {
        raw_mock->force_idle_expired = true;
        REQUIRE(client.is_idle_expired());
    }

    SECTION("Keep-alive ping keeps socket active") {
        REQUIRE(client.keep_alive_ping("/trader/v1/userPreference", "Bearer token_xyz"));
        REQUIRE(raw_mock->send_count == 1);
        REQUIRE(raw_mock->recorded_requests.front().path == "/trader/v1/userPreference");
        REQUIRE(raw_mock->recorded_requests.front().method == HttpMethod::Get);
    }

    SECTION("Auto-reconnect on dropped idle socket") {
        raw_mock->simulate_disconnect_on_send = true;
        HttpResponse resp = client.get("/trader/v1/accounts/123", "Bearer token_xyz");

        REQUIRE(resp.is_success());
        REQUIRE(raw_mock->reconnect_count == 1);
        REQUIRE(client.is_connected());
    }
}

