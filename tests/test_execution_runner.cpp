#include <catch2/catch_test_macros.hpp>
#include "schwab/execution_runner.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace schwab;

TEST_CASE("ExecutionConfig initialization defaults and overrides", "[execution_runner]") {
    SECTION("Default configuration has institutional safety defaults") {
        ExecutionConfig cfg;
        REQUIRE(cfg.dry_run);
        REQUIRE(cfg.config_path == ".env");
        REQUIRE(cfg.token_path == ".schwab_token.json");
        REQUIRE(cfg.account_hash_override.empty());
    }

    SECTION("Custom configuration fields properly stored") {
        ExecutionConfig cfg;
        cfg.session_name = "AUCTION_CLOSE";
        cfg.dry_run = true;
        cfg.account_hash_override = "ACC_HASH_TEST_123";

        ExecutionRunner runner(cfg);
        REQUIRE(runner.config().session_name == "AUCTION_CLOSE");
        REQUIRE(runner.config().dry_run);
        REQUIRE(runner.config().account_hash_override == "ACC_HASH_TEST_123");
    }
}

TEST_CASE("ExecutionRunner Telemetry Table formatting", "[execution_runner]") {
    ExecutionReport report;
    report.total_submitted = 2;
    report.total_succeeded = 2;
    report.total_failed = 0;
    report.total_execution_time = std::chrono::microseconds(1250);
    report.all_succeeded = true;

    OrderExecutionResult r1;
    r1.order.side = OrderSide::Sell;
    r1.order.symbol = Symbol("AAPL");
    r1.order.quantity = 15.0;
    r1.order.type = OrderType::Market;
    r1.success = true;
    r1.order_id = "ORD_SIM_001";
    r1.latency = std::chrono::microseconds(620);

    OrderExecutionResult r2;
    r2.order.side = OrderSide::Buy;
    r2.order.symbol = Symbol("MSFT");
    r2.order.quantity = 25.0;
    r2.order.type = OrderType::MarketOnClose;
    r2.success = true;
    r2.order_id = "ORD_SIM_002";
    r2.latency = std::chrono::microseconds(630);

    report.results.push_back(r1);
    report.results.push_back(r2);

    SECTION("Dry run telemetry contains expected columns and labels") {
        std::string table = ExecutionRunner::format_telemetry_table(report, true);

        REQUIRE_FALSE(table.empty());
        REQUIRE(table.find("SIMULATION TELEMETRY (DRY RUN)") != std::string::npos);
        REQUIRE(table.find("SELL") != std::string::npos);
        REQUIRE(table.find("BUY") != std::string::npos);
        REQUIRE(table.find("AAPL") != std::string::npos);
        REQUIRE(table.find("MSFT") != std::string::npos);
        REQUIRE(table.find("SIMULATED") != std::string::npos);
        REQUIRE(table.find("TAG") != std::string::npos);
        REQUIRE(table.find("Total Orders Submitted: 2") != std::string::npos);
    }

    SECTION("Live broker telemetry header displayed when not dry run") {
        std::string table = ExecutionRunner::format_telemetry_table(report, false);

        REQUIRE(table.find("LIVE BROKER EXECUTION REPORT") != std::string::npos);
        REQUIRE(table.find("FILLED/STAGED") != std::string::npos);
    }
}

TEST_CASE("ExecutionRunner strategy management", "[execution_runner]") {
    SECTION("Defaults to SampleRebalanceStrategy when none provided") {
        ExecutionConfig cfg;
        ExecutionRunner runner(cfg);
        REQUIRE(runner.strategy().name() == "SampleRebalanceStrategy");
    }

    SECTION("Accepts custom IStrategy implementation") {
        class CustomTestStrategy : public IStrategy {
        public:
            [[nodiscard]] std::string_view name() const noexcept override {
                return "CustomTestStrategy";
            }
            [[nodiscard]] std::vector<Order> generate_orders(
                std::span<const Position>,
                double
            ) override {
                return {};
            }
        };

        ExecutionConfig cfg;
        auto custom_strat = std::make_shared<CustomTestStrategy>();
        ExecutionRunner runner(cfg, custom_strat);
        REQUIRE(runner.strategy().name() == "CustomTestStrategy");
    }
}

TEST_CASE("ExecutionRunner execution flows", "[execution_runner]") {
    SECTION("Empty order set exits cleanly with code 0") {
        ExecutionConfig cfg;
        cfg.dry_run = true;

        ExecutionRunner runner(cfg);
        int code = runner.execute({}, 50'000.0);
        REQUIRE(code == 0);
    }

    SECTION("Simulated positions with strategy rebalance in dry-run mode") {
        std::vector<StrategyTargetWeight> targets = {
            {Symbol("SPY"), 0.50},
            {Symbol("QQQ"), 0.50}
        };
        auto strat = std::make_shared<SampleRebalanceStrategy>(targets);

        ExecutionConfig cfg;
        cfg.session_name = "REBALANCE";
        cfg.dry_run = true;

        ExecutionRunner runner(cfg, strat);

        Position pos_untracked{.quantity = 20.0, .average_price = 100.0, .current_price = 100.0, .symbol = Symbol("XYZ")};
        Position pos_spy{.quantity = 100.0, .average_price = 500.0, .current_price = 500.0, .symbol = Symbol("SPY")};
        std::vector<Position> positions = {pos_untracked, pos_spy};
        double cash = 10'000.0;

        int code = runner.execute(positions, cash);
        REQUIRE(code == 0);
    }

    SECTION("Dry run suppresses file logging; live execution appends to telemetry log") {
        auto temp_log = std::filesystem::temp_directory_path() / "test_exec_telemetry_out.log";
        auto temp_token = std::filesystem::temp_directory_path() / "test_exec_telemetry_token.json";
        std::error_code ec;
        std::filesystem::remove(temp_log, ec);
        std::filesystem::remove(temp_token, ec);

        std::vector<StrategyTargetWeight> targets = {
            {Symbol("SPY"), 0.50}
        };
        auto strat = std::make_shared<SampleRebalanceStrategy>(targets);

        Position pos_spy{.quantity = 100.0, .average_price = 450.0, .current_price = 500.0, .symbol = Symbol("SPY")};
        std::vector<Position> positions = {pos_spy};
        double cash = 0.0;

        // 1. Dry run: callback fires, execution history records report, but NO log file written
        {
            ExecutionConfig dry_cfg;
            dry_cfg.dry_run = true;
            dry_cfg.telemetry_log_path = temp_log;

            ExecutionRunner dry_runner(dry_cfg, strat);

            bool callback_fired = false;
            dry_runner.set_telemetry_callback([&](const ExecutionReport& rep) {
                callback_fired = true;
                CHECK(rep.total_submitted == 1);
            });

            int code = dry_runner.execute(positions, cash);
            REQUIRE(code == 0);
            REQUIRE(callback_fired);
            REQUIRE(dry_runner.execution_history().size() == 1);
            CHECK_FALSE(std::filesystem::exists(temp_log));
        }

        // 2. Live execution: writes telemetry table to file
        {
            // Write temporary env file with SCHWAB_DRY_RUN=false
            auto temp_env = std::filesystem::temp_directory_path() / "test_exec_telemetry.env";
            {
                std::ofstream f(temp_env);
                f << "SCHWAB_DRY_RUN=false\n";
            }

            // Write a dummy unexpired token
            {
                std::ofstream f(temp_token);
                auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                f << "{\n"
                  << "  \"access_token\": \"mock_access_token\",\n"
                  << "  \"refresh_token\": \"mock_refresh_token\",\n"
                  << "  \"token_type\": \"Bearer\",\n"
                  << "  \"expires_at\": " << (now_epoch + 3600) << ",\n"
                  << "  \"refresh_token_expires_at\": " << (now_epoch + 86400 * 7) << "\n"
                  << "}\n";
            }

            ExecutionConfig live_cfg;
            live_cfg.dry_run = false;
            live_cfg.config_path = temp_env;
            live_cfg.account_hash_override = "MOCK_ACCOUNT_HASH";
            live_cfg.token_path = temp_token;
            live_cfg.telemetry_log_path = temp_log;

            ExecutionRunner live_runner(live_cfg, strat);

            class LiveMockTransport : public ITransport {
            public:
                bool prewarm() override { return true; }
                [[nodiscard]] bool is_connected() const noexcept override { return true; }
                [[nodiscard]] bool is_idle_expired() const noexcept override { return false; }
                void disconnect() noexcept override {}
                [[nodiscard]] HttpResponse send(const HttpRequest&) override {
                    HttpResponse resp;
                    resp.status_code = 201;
                    resp.headers.push_back({"Location", "/trader/v1/accounts/MOCK/orders/12345"});
                    resp.latency = std::chrono::microseconds{100};
                    return resp;
                }
            };

            live_runner.http_client() = HttpClient(std::make_unique<LiveMockTransport>());

            int code = live_runner.execute(positions, cash);
            REQUIRE(code == 0);
            REQUIRE(std::filesystem::exists(temp_log));
            CHECK(std::filesystem::file_size(temp_log) > 0);

            std::filesystem::remove(temp_log, ec);
            std::filesystem::remove(temp_token, ec);
            std::filesystem::remove(temp_env, ec);
        }
    }
}

TEST_CASE("ExecutionRunner real-time market data valuation and fallback", "[execution_runner]") {
    auto temp_token = std::filesystem::temp_directory_path() / "test_exec_runner_md_token.json";
    auto temp_env = std::filesystem::temp_directory_path() / "test_exec_runner_md.env";
    std::error_code ec;
    std::filesystem::remove(temp_token, ec);
    std::filesystem::remove(temp_env, ec);

    {
        std::ofstream f(temp_env);
        f << "SCHWAB_DRY_RUN=false\n";
    }

    {
        std::ofstream f(temp_token);
        auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        f << "{\n"
          << "  \"access_token\": \"mock_market_token\",\n"
          << "  \"refresh_token\": \"mock_refresh_token\",\n"
          << "  \"token_type\": \"Bearer\",\n"
          << "  \"expires_at\": " << (now_epoch + 3600) << ",\n"
          << "  \"refresh_token_expires_at\": " << (now_epoch + 86400 * 7) << "\n"
          << "}\n";
    }

    class MarketDataMockTransport : public ITransport {
    public:
        std::string quote_json;
        int quote_status{200};
        int orders_submitted{0};

        bool prewarm() override { return true; }
        [[nodiscard]] bool is_connected() const noexcept override { return true; }
        [[nodiscard]] bool is_idle_expired() const noexcept override { return false; }
        void disconnect() noexcept override {}

        [[nodiscard]] HttpResponse send(const HttpRequest& req) override {
            HttpResponse resp;
            if (req.path.find("/marketdata/v1/quotes") != std::string_view::npos) {
                resp.status_code = quote_status;
                resp.body = quote_json;
                resp.latency = std::chrono::microseconds{150};
                return resp;
            }
            if (req.method == HttpMethod::Post) {
                ++orders_submitted;
                resp.status_code = 201;
                resp.headers.push_back({"Location", "/trader/v1/accounts/MOCK/orders/999"});
                resp.latency = std::chrono::microseconds{200};
                return resp;
            }
            resp.status_code = 404;
            return resp;
        }
    };

    SECTION("Real-time quote successfully fetched and used for order valuation") {
        std::vector<StrategyTargetWeight> targets = {
            {Symbol("SPY"), 0.50}
        };
        auto strat = std::make_shared<SampleRebalanceStrategy>(targets);

        ExecutionConfig cfg;
        cfg.dry_run = false;
        cfg.config_path = temp_env;
        cfg.token_path = temp_token;
        cfg.account_hash_override = "MOCK_ACCOUNT_HASH";

        ExecutionRunner runner(cfg, strat);

        auto transport = std::make_unique<MarketDataMockTransport>();
        transport->quote_status = 200;
        transport->quote_json = R"({
            "SPY": {
                "symbol": "SPY",
                "quote": {
                    "bidPrice": 574.90,
                    "askPrice": 575.10,
                    "lastPrice": 575.00,
                    "mark": 575.05,
                    "totalVolume": 50000000,
                    "quoteTime": 1728676800000
                }
            }
        })";

        auto* transport_ptr = transport.get();
        runner.http_client() = HttpClient(std::move(transport));

        Position pos_spy{.quantity = 100.0, .average_price = 450.0, .current_price = 500.0, .symbol = Symbol("SPY")};
        std::vector<Position> positions = {pos_spy};
        double cash = 0.0;

        int code = runner.execute(positions, cash);
        REQUIRE(code == 0);
        REQUIRE(transport_ptr->orders_submitted == 1);
        REQUIRE(runner.last_market_quotes().size() == 1);
        CHECK(runner.last_market_quotes()[0].symbol.view() == "SPY");
        CHECK(runner.last_market_quotes()[0].mark_price == 575.05);
    }

    SECTION("Unquoted and unpositioned symbol in live execution is rejected by risk gate") {
        class UnpositionedStrategy : public IStrategy {
        public:
            [[nodiscard]] std::string_view name() const noexcept override { return "UnpositionedStrategy"; }
            [[nodiscard]] std::vector<Order> generate_orders(std::span<const Position>, double) override {
                Order ord;
                ord.symbol = Symbol("UNKNOWN");
                ord.side = OrderSide::Buy;
                ord.quantity = 10.0;
                ord.type = OrderType::Market;
                return {ord};
            }
        };

        ExecutionConfig cfg;
        cfg.dry_run = false;
        cfg.config_path = temp_env;
        cfg.token_path = temp_token;
        cfg.account_hash_override = "MOCK_ACCOUNT_HASH";

        ExecutionRunner runner(cfg, std::make_shared<UnpositionedStrategy>());

        auto transport = std::make_unique<MarketDataMockTransport>();
        transport->quote_status = 404; // Quote lookup fails
        transport->quote_json = "{}";

        runner.http_client() = HttpClient(std::move(transport));

        // In live execution, with no quote and no position price, order cannot be valued safely
        int code = runner.execute({}, 10'000.0);
        REQUIRE(code != 0); // Must reject order and return failure
    }

    SECTION("Crossed order book in live execution triggers circuit breaker rejection") {
        class CrossedBookStrategy : public IStrategy {
        public:
            [[nodiscard]] std::string_view name() const noexcept override { return "CrossedBookStrategy"; }
            [[nodiscard]] std::vector<Order> generate_orders(std::span<const Position>, double) override {
                Order ord;
                ord.symbol = Symbol("SPY");
                ord.side = OrderSide::Buy;
                ord.quantity = 10.0;
                ord.type = OrderType::Market;
                return {ord};
            }
        };

        ExecutionConfig cfg;
        cfg.dry_run = false;
        cfg.config_path = temp_env;
        cfg.token_path = temp_token;
        cfg.account_hash_override = "MOCK_ACCOUNT_HASH";

        ExecutionRunner runner(cfg, std::make_shared<CrossedBookStrategy>());

        auto transport = std::make_unique<MarketDataMockTransport>();
        transport->quote_status = 200;
        transport->quote_json = R"({
            "SPY": {
                "symbol": "SPY",
                "quote": {
                    "bidPrice": 580.00,
                    "askPrice": 570.00,
                    "lastPrice": 575.00,
                    "mark": 575.00,
                    "totalVolume": 1000000,
                    "quoteTime": 1728676800000
                }
            }
        })";

        auto* transport_ptr = transport.get();
        runner.http_client() = HttpClient(std::move(transport));

        int code = runner.execute({}, 10'000.0);
        REQUIRE(code != 0); // Must be rejected by crossed-book circuit breaker
        REQUIRE(transport_ptr->orders_submitted == 0);
    }

    SECTION("Atomic abort in ExecutionRunner: sell failure aborts subsequent buy orders") {
        class MultiOrderStrategy : public IStrategy {
        public:
            [[nodiscard]] std::string_view name() const noexcept override { return "MultiOrderStrategy"; }
            [[nodiscard]] std::vector<Order> generate_orders(std::span<const Position>, double) override {
                Order s;
                s.symbol = Symbol("BAD");
                s.side = OrderSide::Sell;
                s.quantity = 10.0;
                s.type = OrderType::Market;

                Order b;
                b.symbol = Symbol("SPY");
                b.side = OrderSide::Buy;
                b.quantity = 10.0;
                b.type = OrderType::Market;

                return {s, b};
            }
        };

        ExecutionConfig cfg;
        cfg.dry_run = false;
        cfg.config_path = temp_env;
        cfg.token_path = temp_token;
        cfg.account_hash_override = "MOCK_ACCOUNT_HASH";

        ExecutionRunner runner(cfg, std::make_shared<MultiOrderStrategy>());

        auto transport = std::make_unique<MarketDataMockTransport>();
        transport->quote_status = 200;
        transport->quote_json = R"({
            "BAD": {
                "symbol": "BAD",
                "quote": {
                    "bidPrice": 105.00,
                    "askPrice": 100.00,
                    "lastPrice": 102.00,
                    "mark": 102.00,
                    "totalVolume": 1000,
                    "quoteTime": 1728676800000
                }
            },
            "SPY": {
                "symbol": "SPY",
                "quote": {
                    "bidPrice": 574.90,
                    "askPrice": 575.10,
                    "lastPrice": 575.00,
                    "mark": 575.05,
                    "totalVolume": 50000000,
                    "quoteTime": 1728676800000
                }
            }
        })";

        auto* transport_ptr = transport.get();
        runner.http_client() = HttpClient(std::move(transport));

        Position pos_bad{.quantity = 10.0, .average_price = 100.0, .current_price = 100.0, .symbol = Symbol("BAD")};
        int code = runner.execute({&pos_bad, 1}, 10'000.0);

        // BAD was rejected due to crossed book -> SPY buy was aborted -> zero orders approved
        REQUIRE(code != 0);
        REQUIRE(transport_ptr->orders_submitted == 0);
    }

    std::filesystem::remove(temp_token, ec);
    std::filesystem::remove(temp_env, ec);
}

TEST_CASE("ExecutionRunner format_reconciliation_table output", "[execution_runner]") {
    BatchReconciliationReport recon;

    OrderReconciliationResult r1;
    r1.symbol = Symbol("AAPL");
    r1.side = OrderSide::Buy;
    r1.ordered_quantity = 50.0;
    r1.filled_quantity = 50.0;
    r1.average_fill_price = 150.25;
    r1.final_state = OrderExecutionState::Filled;
    r1.is_terminal = true;
    r1.is_filled = true;
    r1.order_id = "ORD_101";
    r1.slippage = calculate_slippage(OrderSide::Buy, 150.00, 150.25); // +16.7 bps

    OrderReconciliationResult r2;
    r2.symbol = Symbol("SPY");
    r2.side = OrderSide::Sell;
    r2.ordered_quantity = 20.0;
    r2.filled_quantity = 20.0;
    r2.average_fill_price = 575.50;
    r2.final_state = OrderExecutionState::Filled;
    r2.is_terminal = true;
    r2.is_filled = true;
    r2.order_id = "ORD_102";
    r2.slippage = calculate_slippage(OrderSide::Sell, 575.00, 575.50); // -8.7 bps (favorable)

    recon.orders = {r1, r2};
    recon.total_tracked = 2;
    recon.total_filled = 2;
    recon.total_cancelled = 0;
    recon.total_rejected = 0;
    recon.total_timed_out = 0;
    recon.all_terminal = true;
    recon.all_filled = true;
    recon.average_slippage_bps = (r1.slippage.slippage_bps + r2.slippage.slippage_bps) / 2.0;
    recon.elapsed_time = std::chrono::microseconds{15400};

    std::string table = ExecutionRunner::format_reconciliation_table(recon);

    REQUIRE_FALSE(table.empty());
    CHECK(table.find("POST-TRADE FILL & SLIPPAGE RECONCILIATION") != std::string::npos);
    CHECK(table.find("AAPL") != std::string::npos);
    CHECK(table.find("SPY") != std::string::npos);
    CHECK(table.find("BUY") != std::string::npos);
    CHECK(table.find("SELL") != std::string::npos);
    CHECK(table.find("150.25") != std::string::npos);
    CHECK(table.find("575.50") != std::string::npos);
    CHECK(table.find("bps") != std::string::npos);
    CHECK(table.find("ORD_101") != std::string::npos);
    CHECK(table.find("ORD_102") != std::string::npos);
    CHECK(table.find("Total Tracked: 2") != std::string::npos);
    CHECK(table.find("Filled: 2") != std::string::npos);
    CHECK(table.find("Avg Slippage:") != std::string::npos);
}

TEST_CASE("ExecutionRunner dry-run post-dispatch fill reconciliation", "[execution_runner]") {
    std::vector<StrategyTargetWeight> targets = {
        {Symbol("SPY"), 0.50}
    };
    auto strat = std::make_shared<SampleRebalanceStrategy>(targets);

    Position pos_spy{.quantity = 100.0, .average_price = 450.0, .current_price = 500.0, .symbol = Symbol("SPY")};
    std::vector<Position> positions = {pos_spy};
    double cash = 0.0;

    ExecutionConfig dry_cfg;
    dry_cfg.dry_run = true;
    dry_cfg.track_fills = true;

    ExecutionRunner runner(dry_cfg, strat);
    int code = runner.execute(positions, cash);

    REQUIRE(code == 0);
    const auto& last_recon = runner.last_reconciliation_report();
    REQUIRE(last_recon.has_value());
    CHECK(last_recon->total_tracked == 1);
    CHECK(last_recon->total_filled == 1);
    CHECK(last_recon->all_terminal);
    CHECK(last_recon->all_filled);
    CHECK(last_recon->orders[0].symbol.view() == "SPY");
    CHECK(last_recon->orders[0].side == OrderSide::Sell);
    CHECK(last_recon->orders[0].is_filled);
}


