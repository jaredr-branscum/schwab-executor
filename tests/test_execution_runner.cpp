#include <catch2/catch_test_macros.hpp>
#include "schwab/execution_runner.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace schwab;

TEST_CASE("ExecutionConfig initialization defaults and overrides", "[execution_runner]") {
    SECTION("Default configuration has safety defaults") {
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
        REQUIRE(table.find("ENGINE (us)") != std::string::npos);
        REQUIRE(table.find("NETWORK RTT (ms)") != std::string::npos);
        REQUIRE(table.find("Engine Dispatch CPU Time:") != std::string::npos);
        REQUIRE(table.find("Network RTT:") != std::string::npos);
        REQUIRE(table.find("Total Orders Submitted: 2") != std::string::npos);
    }

    SECTION("Live broker telemetry header displayed when not dry run") {
        std::string table = ExecutionRunner::format_telemetry_table(report, false);

        REQUIRE(table.find("LIVE BROKER EXECUTION REPORT") != std::string::npos);
        REQUIRE(table.find("FILLED/STAGED") != std::string::npos);
        REQUIRE(table.find("PRICE") != std::string::npos);
    }

    SECTION("Preview order validation displays limit price alongside quantity") {
        ExecutionReport preview_report;
        preview_report.total_submitted = 1;
        preview_report.total_succeeded = 1;
        preview_report.total_failed = 0;
        preview_report.total_execution_time = std::chrono::microseconds(750);
        preview_report.all_succeeded = true;

        OrderExecutionResult r_lim;
        r_lim.order.side = OrderSide::Buy;
        r_lim.order.symbol = Symbol("SPY");
        r_lim.order.quantity = 142.60;
        r_lim.order.type = OrderType::Limit;
        r_lim.order.limit_price = 120.18;
        r_lim.order.tag = StrategyTag("REBALANCE");
        r_lim.success = true;
        r_lim.order_id = "PREVIEW_VALIDATED";
        r_lim.latency = std::chrono::microseconds(450);
        preview_report.results.push_back(r_lim);

        std::string preview_table = ExecutionRunner::format_telemetry_table(preview_report, false, true);

        REQUIRE(preview_table.find("BROKER ORDER PREVIEW VALIDATION (/previewOrder)") != std::string::npos);
        REQUIRE(preview_table.find("PRICE") != std::string::npos);
        REQUIRE(preview_table.find("142.60") != std::string::npos);
        REQUIRE(preview_table.find("$120.18") != std::string::npos);
        REQUIRE(preview_table.find("LIMIT") != std::string::npos);
        REQUIRE(preview_table.find("PREVIEW_OK") != std::string::npos);
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

TEST_CASE("ExecutionRunner multi-account batch execution (SCHWAB_ACCOUNTS=all)", "[execution_runner]") {
    std::vector<StrategyTargetWeight> targets = {
        {Symbol("SPY"), 0.50}
    };
    auto strat = std::make_shared<SampleRebalanceStrategy>(targets);

    AccountPortfolio acc1;
    acc1.account_number = "11112222";
    acc1.account_hash = "MOCK_HASH_ALPHA_11112222_000011112222333344445555666677778888999900001111";
    acc1.account_type = "INDIVIDUAL";
    acc1.cash_balance = 0.0;
    acc1.available_funds = 0.0;
    acc1.liquidation_value = 50000.0;
    acc1.positions.push_back(Position{
        .quantity = 100.0,
        .average_price = 450.0,
        .current_price = 500.0,
        .symbol = Symbol("SPY")
    });

    AccountPortfolio acc2;
    acc2.account_number = "33334444";
    acc2.account_hash = "MOCK_HASH_BETA_33334444_AAAA9999888877776666555544443333222211110000FFFF";
    acc2.account_type = "INDIVIDUAL";
    acc2.cash_balance = 0.0;
    acc2.available_funds = 0.0;
    acc2.liquidation_value = 0.0;

    std::vector<AccountPortfolio> accounts = {acc1, acc2};

    SECTION("Batch execution across multiple accounts evaluates each independently") {
        ExecutionConfig cfg;
        cfg.dry_run = true;
        cfg.accounts_filter = "all";

        ExecutionRunner runner(cfg, strat);
        int exit_code = runner.execute({}, std::nullopt, accounts);

        REQUIRE(exit_code == 0);
        const auto& summary = runner.last_batch_summary();
        REQUIRE(summary.has_value());
        CHECK(summary->total_accounts == 2);
        CHECK(summary->succeeded_accounts == 2);
        CHECK(summary->failed_accounts == 0);
        CHECK(summary->total_orders_submitted == 1);
        CHECK(summary->total_orders_succeeded == 1);

        // Account 1 had 1 rebalance sell order
        REQUIRE(summary->account_results.size() == 2);
        CHECK(summary->account_results[0].account_number == "11112222");
        CHECK(summary->account_results[0].masked_account_number == "***2222");
        CHECK(summary->account_results[0].orders_generated == 1);
        CHECK(summary->account_results[0].orders_succeeded == 1);
        CHECK(summary->account_results[0].success);

        // Account 2 had 0 positions and 0 cash -> zero orders needed -> succeeded
        CHECK(summary->account_results[1].account_number == "33334444");
        CHECK(summary->account_results[1].masked_account_number == "***4444");
        CHECK(summary->account_results[1].orders_generated == 0);
        CHECK(summary->account_results[1].success);

        // Verify batch summary table formatting
        std::string table = ExecutionRunner::format_batch_summary_table(*summary);
        REQUIRE_FALSE(table.empty());
        CHECK(table.find("MULTI-ACCOUNT BATCH EXECUTION SUMMARY (2 Accounts)") != std::string::npos);
        CHECK(table.find("***2222") != std::string::npos);
        CHECK(table.find("***4444") != std::string::npos);
        CHECK(table.find("Total Accounts: 2") != std::string::npos);
        CHECK(table.find("Succeeded: 2") != std::string::npos);
    }

    SECTION("Account filter selects specific account from batch") {
        ExecutionConfig cfg;
        cfg.dry_run = true;
        cfg.accounts_filter = "4444"; // only second account

        ExecutionRunner runner(cfg, strat);
        int exit_code = runner.execute({}, std::nullopt, accounts);

        REQUIRE(exit_code == 0);
        const auto& summary = runner.last_batch_summary();
        REQUIRE(summary.has_value());
        CHECK(summary->total_accounts == 1);
        CHECK(summary->account_results[0].account_number == "33334444");
    }

    SECTION("Risk rejection on one account records failure in batch summary") {
        class RejectStrategy : public IStrategy {
        public:
            [[nodiscard]] std::string_view name() const noexcept override { return "RejectStrategy"; }
            [[nodiscard]] std::vector<Order> generate_orders(std::span<const Position> positions, double) override {
                if (positions.empty()) return {};
                // Order exceeding notional limits
                Order o;
                o.symbol = Symbol("SPY");
                o.side = OrderSide::Sell;
                o.quantity = 1000000.0; // massive quantity -> blocked by RiskGuard max_order_notional
                o.type = OrderType::Market;
                return {o};
            }
        };

        ExecutionConfig cfg;
        cfg.dry_run = true;
        cfg.accounts_filter = "all";

        ExecutionRunner runner(cfg, std::make_shared<RejectStrategy>());
        int exit_code = runner.execute({}, std::nullopt, accounts);

        // Acc1 blocked by RiskGuard -> overall exit code non-zero
        REQUIRE(exit_code != 0);
        const auto& summary = runner.last_batch_summary();
        REQUIRE(summary.has_value());
        CHECK(summary->total_accounts == 2);
        CHECK(summary->succeeded_accounts == 1);
        CHECK(summary->failed_accounts == 1);
        CHECK_FALSE(summary->account_results[0].success);
        CHECK(summary->account_results[1].success);
    }

    SECTION("Aggregate buy commitment exceeding available cash is blocked by RiskGuard") {
        class CashBreachStrategy : public IStrategy {
        public:
            [[nodiscard]] std::string_view name() const noexcept override { return "CashBreachStrategy"; }
            [[nodiscard]] std::vector<Order> generate_orders(std::span<const Position>, double) override {
                // Emits a BUY order that commits $20,000 when available cash is only $10,000
                Order o;
                o.symbol = Symbol("SPY");
                o.side = OrderSide::Buy;
                o.quantity = 40.0;
                o.type = OrderType::Limit;
                o.limit_price = 500.0; // 40 * 500 = $20,000 > $10,000 cash
                return {o};
            }
        };

        AccountPortfolio acc;
        acc.account_number = "55556666";
        acc.account_hash = "TEST_HASH_CASH";
        acc.cash_balance = 10000.0; // $10,000 cash

        ExecutionConfig cfg;
        cfg.dry_run = true;
        cfg.enforce_cash_limits = true;
        cfg.allow_margin_borrowing = false;

        ExecutionRunner runner(cfg, std::make_shared<CashBreachStrategy>());
        std::vector<AccountPortfolio> test_accounts = {acc};
        int exit_code = runner.execute({}, std::nullopt, test_accounts);

        // Blocked by RiskGuard cash gate -> exit code non-zero
        REQUIRE(exit_code != 0);
        const auto& summary = runner.last_batch_summary();
        REQUIRE(summary.has_value());
        CHECK(summary->failed_accounts == 1);
        CHECK(summary->account_results[0].status_notes == "Blocked by RiskGuard");
    }

    SECTION("Allowing margin borrowing permits buy commitments exceeding cash") {
        class CashBreachStrategy : public IStrategy {
        public:
            [[nodiscard]] std::string_view name() const noexcept override { return "CashBreachStrategy"; }
            [[nodiscard]] std::vector<Order> generate_orders(std::span<const Position>, double) override {
                Order o;
                o.symbol = Symbol("SPY");
                o.side = OrderSide::Buy;
                o.quantity = 40.0;
                o.type = OrderType::Limit;
                o.limit_price = 500.0; // $20,000
                return {o};
            }
        };

        AccountPortfolio acc;
        acc.account_number = "55556666";
        acc.account_hash = "TEST_HASH_CASH";
        acc.cash_balance = 10000.0; // $10,000 cash

        ExecutionConfig cfg;
        cfg.dry_run = true;
        cfg.enforce_cash_limits = true;
        cfg.allow_margin_borrowing = true; // explicitly permit margin debt

        ExecutionRunner runner(cfg, std::make_shared<CashBreachStrategy>());
        std::vector<AccountPortfolio> test_accounts = {acc};
        int exit_code = runner.execute({}, std::nullopt, test_accounts);

        // Allowed -> executes successfully in simulation
        REQUIRE(exit_code == 0);
        const auto& summary = runner.last_batch_summary();
        REQUIRE(summary.has_value());
        CHECK(summary->succeeded_accounts == 1);
        CHECK(summary->account_results[0].success);
    }
}

TEST_CASE("ExecutionRunner concurrent multi-account dispatch architecture", "[execution_runner][concurrency]") {
    std::vector<StrategyTargetWeight> targets = {
        {Symbol("SPY"), 0.50}
    };
    auto strat = std::make_shared<SampleRebalanceStrategy>(targets);

    AccountPortfolio acc1;
    acc1.account_number = "11112222";
    acc1.account_hash = "MOCK_HASH_ACC_1";
    acc1.account_type = "INDIVIDUAL";
    acc1.cash_balance = 0.0;
    acc1.available_funds = 0.0;
    acc1.liquidation_value = 50000.0;
    acc1.positions.push_back(Position{
        .quantity = 100.0,
        .average_price = 450.0,
        .current_price = 500.0,
        .symbol = Symbol("SPY")
    });

    AccountPortfolio acc2;
    acc2.account_number = "33334444";
    acc2.account_hash = "MOCK_HASH_ACC_2";
    acc2.account_type = "ROTH_IRA";
    acc2.cash_balance = 0.0;
    acc2.available_funds = 0.0;
    acc2.liquidation_value = 50000.0;
    acc2.positions.push_back(Position{
        .quantity = 100.0,
        .average_price = 450.0,
        .current_price = 500.0,
        .symbol = Symbol("SPY")
    });

    AccountPortfolio acc3;
    acc3.account_number = "55556666";
    acc3.account_hash = "MOCK_HASH_ACC_3";
    acc3.account_type = "TRADITIONAL_IRA";
    acc3.cash_balance = 0.0;
    acc3.available_funds = 0.0;
    acc3.liquidation_value = 50000.0;
    acc3.positions.push_back(Position{
        .quantity = 100.0,
        .average_price = 450.0,
        .current_price = 500.0,
        .symbol = Symbol("SPY")
    });

    std::vector<AccountPortfolio> accounts = {acc1, acc2, acc3};

    SECTION("Concurrent dispatch executes all accounts simultaneously in dry run") {
        ExecutionConfig cfg;
        cfg.dry_run = true;
        cfg.accounts_filter = "all";
        cfg.enable_concurrent_dispatch = true;
        cfg.max_concurrent_workers = 4;

        ExecutionRunner runner(cfg, strat);
        int exit_code = runner.execute({}, std::nullopt, accounts);

        REQUIRE(exit_code == 0);
        const auto& summary = runner.last_batch_summary();
        REQUIRE(summary.has_value());
        CHECK(summary->total_accounts == 3);
        CHECK(summary->succeeded_accounts == 3);
        CHECK(summary->failed_accounts == 0);
        CHECK(summary->total_orders_submitted == 3);
        CHECK(summary->total_orders_succeeded == 3);

        REQUIRE(summary->account_results.size() == 3);
        CHECK(summary->account_results[0].account_number == "11112222");
        CHECK(summary->account_results[0].success);
        CHECK(summary->account_results[1].account_number == "33334444");
        CHECK(summary->account_results[1].success);
        CHECK(summary->account_results[2].account_number == "55556666");
        CHECK(summary->account_results[2].success);

        // Verify history contains all reports in order
        REQUIRE(runner.execution_history().size() == 3);
    }

    SECTION("Disabling concurrent dispatch falls back to sequential execution with identical summary") {
        ExecutionConfig cfg;
        cfg.dry_run = true;
        cfg.accounts_filter = "all";
        cfg.enable_concurrent_dispatch = false;

        ExecutionRunner runner(cfg, strat);
        int exit_code = runner.execute({}, std::nullopt, accounts);

        REQUIRE(exit_code == 0);
        const auto& summary = runner.last_batch_summary();
        REQUIRE(summary.has_value());
        CHECK(summary->total_accounts == 3);
        CHECK(summary->succeeded_accounts == 3);
        CHECK(summary->failed_accounts == 0);
        CHECK(summary->total_orders_submitted == 3);
        CHECK(summary->total_orders_succeeded == 3);
    }

    SECTION("Max concurrent workers bounds thread pool when accounts exceed limit") {
        ExecutionConfig cfg;
        cfg.dry_run = true;
        cfg.accounts_filter = "all";
        cfg.enable_concurrent_dispatch = true;
        cfg.max_concurrent_workers = 1; // 1 worker thread executes all accounts

        ExecutionRunner runner(cfg, strat);
        int exit_code = runner.execute({}, std::nullopt, accounts);

        REQUIRE(exit_code == 0);
        const auto& summary = runner.last_batch_summary();
        REQUIRE(summary.has_value());
        CHECK(summary->total_accounts == 3);
        CHECK(summary->succeeded_accounts == 3);
    }

    SECTION("Concurrent live dispatch via TransportFactory injects dedicated transports per worker") {
        class MockConcurrentTransport : public ITransport {
        public:
            std::atomic<int>* prewarm_count{nullptr};
            std::atomic<int>* send_count{nullptr};
            bool connected{false};

            bool prewarm() override {
                connected = true;
                if (prewarm_count) prewarm_count->fetch_add(1);
                return true;
            }
            [[nodiscard]] bool is_connected() const noexcept override { return connected; }
            [[nodiscard]] bool is_idle_expired() const noexcept override { return false; }
            void disconnect() noexcept override { connected = false; }

            [[nodiscard]] HttpResponse send(const HttpRequest& req) override {
                if (send_count) send_count->fetch_add(1);
                HttpResponse resp;
                resp.status_code = 201;
                resp.headers.push_back({"Location", std::string(req.path) + "/ORDER_CONCURRENT_123"});
                resp.latency = std::chrono::microseconds{100};
                return resp;
            }
        };

        std::atomic<int> global_prewarms{0};
        std::atomic<int> global_sends{0};

        auto temp_env = std::filesystem::temp_directory_path() / "test_concurrent_live.env";
        {
            std::ofstream ef(temp_env);
            ef << "SCHWAB_DRY_RUN=false\n";
        }

        auto temp_token = std::filesystem::temp_directory_path() / "test_concurrent_live_token.json";
        {
            auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::ofstream tf(temp_token);
            tf << "{\n"
               << "  \"access_token\": \"VALID_CONCURRENT_TEST_TOKEN\",\n"
               << "  \"refresh_token\": \"VALID_REFRESH\",\n"
               << "  \"token_type\": \"Bearer\",\n"
               << "  \"expires_at\": " << (now_epoch + 3600) << ",\n"
               << "  \"refresh_token_expires_at\": " << (now_epoch + 86400 * 7) << "\n"
               << "}\n";
        }

        ExecutionConfig cfg;
        cfg.dry_run = false;
        cfg.config_path = temp_env;
        cfg.token_path = temp_token;
        cfg.accounts_filter = "all";
        cfg.enable_concurrent_dispatch = true;
        cfg.max_concurrent_workers = 4;

        ExecutionRunner runner(cfg, strat);
        runner.set_transport_factory([&]() {
            auto t = std::make_unique<MockConcurrentTransport>();
            t->prewarm_count = &global_prewarms;
            t->send_count = &global_sends;
            return t;
        });

        int exit_code = runner.execute({}, std::nullopt, accounts);

        REQUIRE(exit_code == 0);
        CHECK(global_sends.load() == 3); // 1 order per account across 3 accounts
        CHECK(global_prewarms.load() >= 1); // prewarm was invoked

        const auto& summary = runner.last_batch_summary();
        REQUIRE(summary.has_value());
        CHECK(summary->total_accounts == 3);
        CHECK(summary->succeeded_accounts == 3);
        CHECK(summary->failed_accounts == 0);

        std::error_code ec;
        std::filesystem::remove(temp_env, ec);
        std::filesystem::remove(temp_token, ec);
    }

    SECTION("Environment variables parse SCHWAB_CONCURRENT_DISPATCH and SCHWAB_MAX_CONCURRENT_WORKERS") {
        auto temp_env = std::filesystem::temp_directory_path() / "test_concurrent.env";
        {
            std::ofstream ef(temp_env);
            ef << "SCHWAB_CONCURRENT_DISPATCH=false\n";
            ef << "SCHWAB_MAX_CONCURRENT_WORKERS=8\n";
        }

        ExecutionConfig cfg;
        cfg.config_path = temp_env;
        cfg.dry_run = true;

        ExecutionRunner runner(cfg, strat);
        int exit_code = runner.execute({}, std::nullopt, accounts);

        REQUIRE(exit_code == 0);
        // Verify runner adopted settings from .env
        CHECK_FALSE(runner.config().enable_concurrent_dispatch);
        CHECK(runner.config().max_concurrent_workers == 8);

        std::error_code ec;
        std::filesystem::remove(temp_env, ec);
    }

    SECTION("Pre-seeded market quotes and auto_fetch_quotes bypass quote network requests") {
        ExecutionConfig cfg;
        cfg.dry_run = true;
        cfg.auto_fetch_quotes = false;

        ExecutionRunner runner(cfg, strat);
        
        Quote q;
        q.symbol = Symbol{"SPY"};
        q.bid_price = 500.00;
        q.ask_price = 500.10;
        q.last_price = 500.05;

        runner.set_market_quotes({q});
        REQUIRE(runner.last_market_quotes().size() == 1);
        REQUIRE(runner.last_market_quotes()[0].symbol == Symbol{"SPY"});

        int exit_code = runner.execute({}, std::nullopt, accounts);
        REQUIRE(exit_code == 0);
    }
}


