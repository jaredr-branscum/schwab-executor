#include "schwab/execution_runner.hpp"
#include "schwab/order_serializer.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace schwab {

ExecutionRunner::ExecutionRunner(ExecutionConfig config, std::shared_ptr<IStrategy> strategy) noexcept
    : config_(std::move(config)), strategy_(std::move(strategy)) {
    if (!strategy_) {
        // Default to a public reference sample strategy if none provided
        strategy_ = std::make_shared<SampleRebalanceStrategy>(
            std::vector<StrategyTargetWeight>{},
            config_.default_order_type
        );
    }
}

std::string ExecutionRunner::format_telemetry_table(
    const ExecutionReport& report,
    bool is_dry_run
) {
    std::ostringstream ss;

    ss << "\n================================================================================================================\n"
       << "  [SCHWAB-EXECUTOR] " << (is_dry_run ? "SIMULATION TELEMETRY (DRY RUN)" : "LIVE BROKER EXECUTION REPORT") << "\n"
       << "================================================================================================================\n"
       << "  " << std::left 
       << std::setw(6)  << "SIDE"
       << std::setw(10) << "SYMBOL"
       << std::setw(14) << "TAG"
       << std::setw(12) << "QUANTITY"
       << std::setw(16) << "TYPE"
       << std::setw(12) << "STATUS"
       << std::setw(22) << "ORDER ID"
       << std::setw(16) << "LATENCY (us)"
       << "NOTES\n"
       << "  ----------------------------------------------------------------------------------------------------------------\n";

    for (const auto& res : report.results) {
        std::string side_str(to_string(res.order.side));
        std::string sym_str(res.order.symbol.view());
        std::string tag_str = res.order.tag.empty() ? "-" : std::string(res.order.tag.view());
        std::string type_str(to_string(res.order.type));
        std::string status_str = res.success ? (is_dry_run ? "SIMULATED" : "FILLED/STAGED") : "FAILED";
        std::string id_str = res.order_id.value_or(is_dry_run ? "DRY_RUN_ID" : "-");

        std::ostringstream qty_stream;
        qty_stream << std::fixed << std::setprecision(2) << res.order.quantity;

        ss << "  " << std::left
           << std::setw(6)  << side_str
           << std::setw(10) << sym_str
           << std::setw(14) << tag_str
           << std::setw(12) << qty_stream.str()
           << std::setw(16) << type_str
           << std::setw(12) << status_str
           << std::setw(22) << id_str
           << std::setw(16) << res.latency.count()
           << (res.success ? "" : res.error_message) << "\n";
    }

    ss << "  ----------------------------------------------------------------------------------------------------------------\n"
       << "  Total Orders Submitted: " << report.total_submitted
       << " | Succeeded: " << report.total_succeeded
       << " | Failed: " << report.total_failed
       << " | Total Latency: " << (report.total_execution_time.count() / 1000.0) << " ms\n"
       << "================================================================================================================\n\n";

    return ss.str();
}

int ExecutionRunner::execute(
    std::span<const Position> simulated_positions,
    std::optional<double> simulated_cash
) {
    std::cout << "\n================================================================================\n"
              << "  [SCHWAB-EXECUTOR] SESSION EXECUTION RUNNER\n"
              << "================================================================================\n"
              << "  * Mode               : [" << config_.session_name << "]\n"
              << "  * Strategy Model     : [" << strategy_->name() << "]\n"
              << "  * Execution Target   : " << (config_.dry_run ? "[SIMULATION - DRY RUN]" : "[LIVE BROKER ORDERS]") << "\n";

    Config cfg = Config::load_from_file(config_.config_path);

    // Override dry_run from config if not explicitly set via CLI
    if (!config_.dry_run && cfg.execution.dry_run) {
        config_.dry_run = true;
    }

    // Populate telemetry_log_path from config if not explicitly set
    if (!config_.telemetry_log_path.has_value() && cfg.has_param("SCHWAB_TELEMETRY_LOG_PATH")) {
        config_.telemetry_log_path = std::filesystem::path(cfg.get_string("SCHWAB_TELEMETRY_LOG_PATH"));
    }

    // Resolve Account Hash
    std::string account_hash = config_.account_hash_override;
    if (account_hash.empty()) {
        account_hash = cfg.target_account_number;
    }
    if (account_hash.empty() && config_.dry_run) {
        account_hash = "SIMULATED_ACCOUNT";
    }

    if (account_hash.empty() && !config_.dry_run) {
        std::cerr << "  * Error: Target account hash is required for live execution.\n"
                  << "    Set SCHWAB_ACCOUNT_NUMBER in .env or provide --account-hash <hash>.\n\n";
        return 1;
    }

    // Ensure active OAuth session
    std::string access_token;
    if (config_.dry_run) {
        auto token_opt = TokenManager::load_from_file(config_.token_path);
        if (token_opt.has_value() && !token_opt->is_access_token_expired()) {
            access_token = token_opt->access_token;
            std::cout << "  * OAuth Status       : [AUTHENTICATED] Access Token Valid\n";
        } else {
            access_token = "SIMULATED_ACCESS_TOKEN";
            std::cout << "  * OAuth Status       : [SIMULATION] Using Simulated Offline Token\n";
        }
    } else {
        auto token_opt = TokenManager::ensure_authenticated(config_.token_path);
        if (!token_opt.has_value()) {
            std::cerr << "  * Error: Could not authenticate OAuth session. Run 'schwab_executor --mode auth'.\n\n";
            return 1;
        }
        access_token = token_opt->access_token;
        std::cout << "  * OAuth Status       : [AUTHENTICATED] Access Token Valid\n";
    }

    // Resolve Current Portfolio Context (Positions & Liquid Cash)
    std::vector<Position> positions;
    double available_cash = 0.0;

    if (!simulated_positions.empty() || simulated_cash.has_value()) {
        // Use provided mock context (for tests and offline simulations)
        positions.assign(simulated_positions.begin(), simulated_positions.end());
        available_cash = simulated_cash.value_or(100'000.0);
        std::cout << "  * Portfolio Context  : Simulated (" << positions.size() 
                  << " positions, Cash: $" << std::fixed << std::setprecision(2) << available_cash << ")\n";
    } else if (config_.dry_run && access_token == "SIMULATED_ACCESS_TOKEN") {
        available_cash = 100'000.0;
        std::cout << "  * Portfolio Context  : Offline Simulation (0 positions, Cash: $" 
                  << std::fixed << std::setprecision(2) << available_cash << ")\n";
    } else {
        // Query live Charles Schwab portfolio state
        std::cout << "  * Portfolio Context  : Querying live Charles Schwab account positions...\n";
        std::string fetch_err;
        auto pf_opt = AccountManager::fetch_portfolio(http_client_, cfg.base_url, account_hash, access_token, &fetch_err);
        if (pf_opt.has_value()) {
            positions = std::move(pf_opt->positions);
            available_cash = pf_opt->available_cash();
            std::cout << "  * Portfolio Context  : Live Account (" << positions.size() 
                      << " positions, Cash: $" << std::fixed << std::setprecision(2) << available_cash
                      << ", Liquidation Value: $" << pf_opt->liquidation_value << ")\n";
        } else {
            std::cerr << "  * [CRITICAL ERROR] Failed to fetch live account portfolio from Schwab: " 
                      << fetch_err << "\n";
            if (!config_.dry_run) {
                std::cerr << "  * Aborting execution: cannot submit live orders with unknown position state.\n\n";
                return 1;
            }
            std::cout << "  * Simulation fallback: using default cash context for dry-run validation.\n";
            available_cash = 100'000.0;
        }
    }

    // Generate Strategy Session Orders
    std::vector<Order> raw_orders = strategy_->generate_orders(
        positions,
        available_cash
    );

    std::cout << "  * Strategy Output    : " << raw_orders.size() << " orders emitted by strategy\n";

    if (raw_orders.empty()) {
        std::cout << "  * Status: [COMPLETED] Zero orders generated by strategy. Zero execution needed.\n"
                  << "================================================================================\n\n";
        return 0;
    }

    // Pre-Trade RiskGuard Validation (Notional caps, collars, and MOC cutoff gate)
    RiskLimits limits;
    limits.max_order_notional = cfg.execution.max_order_notional;
    limits.enforce_moc_cutoff = true;
    limits.dry_run = false; // Allow plan generation; ExecutionDriver handles dry-run simulation
    RiskGuard risk_guard(limits);

    std::vector<Order> approved_orders;
    approved_orders.reserve(raw_orders.size());

    //TODO: must quote all symbols before approving orders
    for (const auto& ord : raw_orders) {
        double current_qty = 0.0;
        double market_price = 100.0; // fallback if unquoted

        for (const auto& pos : positions) {
            if (pos.symbol == ord.symbol) {
                current_qty = pos.quantity;
                if (pos.current_price > 0.0) {
                    market_price = pos.current_price;
                }
                break;
            }
        }

        auto check = risk_guard.validate_order(ord, current_qty, market_price);
        if (check.approved) {
            approved_orders.push_back(ord);
        } else {
            std::cout << "  * [RISK WARNING] Order rejected for " << ord.symbol.view() 
                      << ": " << check.message << "\n";
        }
    }

    if (approved_orders.empty()) {
        std::cerr << "  * Error: All strategy orders were blocked by RiskGuard.\n\n";
        return 1;
    }

    // Pre-warm HTTP transport and Dispatch Rebalance Plan
    if (!config_.dry_run && !http_client_.is_connected()) {
        http_client_.prewarm();
    }

    RebalancePlan plan;
    plan.orders = std::move(approved_orders);

    ExecutionDriverConfig driver_cfg;
    driver_cfg.account_hash = account_hash;
    driver_cfg.dry_run = config_.dry_run;
    driver_cfg.stop_on_auth_failure = true;

    ExecutionDriver driver(http_client_, driver_cfg);
    ExecutionReport report = driver.execute_plan(plan, access_token);

    // Output Formatted Telemetry Table and Save to History/File
    std::string table_str = format_telemetry_table(report, config_.dry_run);
    std::cout << table_str;

    execution_history_.push_back(report);

    if (telemetry_callback_) {
        telemetry_callback_(report);
    }

    if (!config_.dry_run && config_.telemetry_log_path.has_value()) {
        std::error_code ec;
        if (config_.telemetry_log_path->has_parent_path()) {
            std::filesystem::create_directories(config_.telemetry_log_path->parent_path(), ec);
        }
        std::ofstream log_file(*config_.telemetry_log_path, std::ios::app);
        if (log_file.is_open()) {
            auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm gm_tm;
#if defined(_WIN32)
            gmtime_s(&gm_tm, &now_t);
#else
            gmtime_r(&now_t, &gm_tm);
#endif
            log_file << "[" << std::put_time(&gm_tm, "%Y-%m-%d %H:%M:%S UTC") << "] "
                     << "Mode: " << config_.session_name << " | Strategy: " << strategy_->name() << "\n"
                     << table_str << "\n";
        }
    }

    return report.all_succeeded ? 0 : 1;
}

} // namespace schwab
