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
