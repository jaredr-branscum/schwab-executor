#include <catch2/catch_test_macros.hpp>
#include "schwab/daemon.hpp"
#include "schwab/strategy.hpp"

using namespace schwab;

TEST_CASE("DaemonConfig initialization defaults and overrides", "[daemon]") {
    SECTION("Default configuration has institutional safety defaults") {
        DaemonConfig cfg;
        REQUIRE(cfg.session_name == "EXECUTE");
        REQUIRE(cfg.scheduled_times.empty());
        REQUIRE_FALSE(cfg.intraday_interval.has_value());
        REQUIRE_FALSE(cfg.dry_run);
        REQUIRE(cfg.config_path == ".env");
        REQUIRE(cfg.token_path == ".schwab_token.json");
        REQUIRE(cfg.account_hash_override.empty());
        REQUIRE(cfg.pre_warm_lead_time == std::chrono::seconds(30));
        REQUIRE(cfg.spin_threshold == std::chrono::milliseconds(20));
        REQUIRE(cfg.token_refresh_interval == std::chrono::seconds(1200));
    }

    SECTION("Custom configuration fields properly stored") {
        DaemonConfig cfg;
        cfg.session_name = "AUCTION_CLOSE";
        cfg.scheduled_times = {"15:49:00"};
        cfg.intraday_interval = std::chrono::milliseconds(5000);
        cfg.dry_run = true;
        cfg.account_hash_override = "ACC_HASH_TEST_123";

        DaemonRunner runner(cfg);
        REQUIRE(runner.config().session_name == "AUCTION_CLOSE");
        REQUIRE(runner.config().scheduled_times.size() == 1);
        REQUIRE(runner.config().scheduled_times[0] == "15:49:00");
        REQUIRE(runner.config().intraday_interval == std::chrono::milliseconds(5000));
        REQUIRE(runner.config().dry_run);
        REQUIRE(runner.config().account_hash_override == "ACC_HASH_TEST_123");
    }
}

TEST_CASE("DaemonRunner Telemetry Table formatting", "[daemon]") {
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
        std::string table = DaemonRunner::format_telemetry_table(report, true);

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
        std::string table = DaemonRunner::format_telemetry_table(report, false);

        REQUIRE(table.find("LIVE BROKER EXECUTION REPORT") != std::string::npos);
        REQUIRE(table.find("FILLED/STAGED") != std::string::npos);
    }
}

TEST_CASE("DaemonRunner strategy management", "[daemon]") {
    SECTION("Defaults to SampleRebalanceStrategy when none provided") {
        DaemonConfig cfg;
        DaemonRunner runner(cfg);
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

        DaemonConfig cfg;
        auto custom_strat = std::make_shared<CustomTestStrategy>();
        DaemonRunner runner(cfg, custom_strat);
        REQUIRE(runner.strategy().name() == "CustomTestStrategy");
    }
}

TEST_CASE("DaemonRunner immediate execution flows", "[daemon]") {
    SECTION("Empty order set exits cleanly with code 0") {
        DaemonConfig cfg;
        cfg.dry_run = true;

        DaemonRunner runner(cfg);
        int code = runner.run_immediate({}, 50'000.0);
        REQUIRE(code == 0);
    }

    SECTION("Simulated positions with strategy rebalance in dry-run mode") {
        std::vector<StrategyTargetWeight> targets = {
            {Symbol("SPY"), 0.50},
            {Symbol("QQQ"), 0.50}
        };
        auto strat = std::make_shared<SampleRebalanceStrategy>(targets);

        DaemonConfig cfg;
        cfg.session_name = "INTRADAY_REBALANCE";
        cfg.dry_run = true;

        DaemonRunner runner(cfg, strat);

        Position pos_untracked{.quantity = 20.0, .average_price = 100.0, .current_price = 100.0, .symbol = Symbol("XYZ")};
        Position pos_spy{.quantity = 100.0, .average_price = 500.0, .current_price = 500.0, .symbol = Symbol("SPY")};
        std::vector<Position> positions = {pos_untracked, pos_spy};
        double cash = 10'000.0;

        int code = runner.run_immediate(positions, cash);
        REQUIRE(code == 0);
    }
}

TEST_CASE("DaemonRunner persistent daemon execution loops", "[daemon]") {
    SECTION("Pre-stopped daemon exits immediately with code 0") {
        DaemonConfig cfg;
        cfg.dry_run = true;

        DaemonRunner runner(cfg);
        std::atomic<bool> stop_flag{true};
        int code = runner.run_daemon(&stop_flag);
        REQUIRE(code == 0);
    }

    SECTION("Pure intraday interval loop executes cycle and terminates with max_cycles") {
        std::vector<StrategyTargetWeight> targets = {
            {Symbol("SPY"), 0.50}
        };
        auto strat = std::make_shared<SampleRebalanceStrategy>(targets);

        DaemonConfig cfg;
        cfg.dry_run = true;
        cfg.intraday_interval = std::chrono::milliseconds(10);
        // scheduled_times is empty by default -> pure intraday mode

        DaemonRunner runner(cfg, strat);
        int code = runner.run_daemon(nullptr, 1);
        REQUIRE(code == 0);
    }

    SECTION("Daemon with invalid scheduled times returns error code 1") {
        DaemonConfig cfg;
        cfg.dry_run = true;
        cfg.scheduled_times = {"invalid_time"};

        DaemonRunner runner(cfg);
        int code = runner.run_daemon(nullptr, 1);
        REQUIRE(code == 1);
    }

    SECTION("Daemon with neither scheduled times nor intraday interval returns error code 1") {
        DaemonConfig cfg;
        cfg.dry_run = true;
        cfg.config_path = "non_existent_file.env";

        DaemonRunner runner(cfg);
        int code = runner.run_daemon(nullptr, 1);
        REQUIRE(code == 1);
    }

    SECTION("Telemetry logging to file and callback invocation") {
        auto temp_log = std::filesystem::temp_directory_path() / "test_telemetry_out.log";
        std::filesystem::remove(temp_log);

        std::vector<StrategyTargetWeight> targets = {
            {Symbol("SPY"), 0.50}
        };
        auto strat = std::make_shared<SampleRebalanceStrategy>(targets);

        DaemonConfig cfg;
        cfg.dry_run = true;
        cfg.telemetry_log_path = temp_log;

        DaemonRunner runner(cfg, strat);

        bool callback_fired = false;
        runner.set_telemetry_callback([&](const ExecutionReport& rep) {
            callback_fired = true;
            CHECK(rep.total_submitted == 1);
        });

        Position pos_spy{.quantity = 100.0, .average_price = 450.0, .current_price = 500.0, .symbol = Symbol("SPY")};
        std::vector<Position> positions = {pos_spy};
        double cash = 0.0;

        int code = runner.run_immediate(positions, cash);
        REQUIRE(code == 0);
        REQUIRE(callback_fired);
        REQUIRE(runner.execution_history().size() == 1);

        // Verify file was written and contains telemetry
        REQUIRE(std::filesystem::exists(temp_log));
        auto file_size = std::filesystem::file_size(temp_log);
        CHECK(file_size > 0);
        std::filesystem::remove(temp_log);
    }
}

