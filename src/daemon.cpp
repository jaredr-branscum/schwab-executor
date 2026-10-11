#include "schwab/daemon.hpp"
#include "schwab/order_serializer.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace schwab {

namespace {

std::atomic<bool> g_daemon_stop_requested{false};

void default_sigint_handler(int) {
    g_daemon_stop_requested.store(true);
}

} // namespace

DaemonRunner::DaemonRunner(DaemonConfig config, std::shared_ptr<IStrategy> strategy) noexcept
    : config_(std::move(config)), strategy_(std::move(strategy)) {
    if (!strategy_) {
        // Default to a public reference sample strategy if none provided
        strategy_ = std::make_shared<SampleRebalanceStrategy>(
            std::vector<StrategyTargetWeight>{},
            config_.default_order_type
        );
    }
}

std::string DaemonRunner::format_telemetry_table(
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

int DaemonRunner::run_immediate(
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
        std::cerr << "  * Error: SCHWAB_TARGET_ACCOUNT_NUMBER is required for live execution.\n"
                  << "    Set SCHWAB_TARGET_ACCOUNT_NUMBER in .env or provide --account-hash <hash>.\n\n";
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

    if (config_.telemetry_log_path.has_value()) {
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

int DaemonRunner::run_daemon(
    std::atomic<bool>* stop_signal,
    std::optional<size_t> max_cycles
) {
    std::atomic<bool>* active_stop = stop_signal;
    if (!active_stop) {
        g_daemon_stop_requested.store(false);
        std::signal(SIGINT, default_sigint_handler);
        active_stop = &g_daemon_stop_requested;
    }

    if (active_stop->load(std::memory_order_relaxed)) {
        return 0;
    }

    if (config_.scheduled_times.empty() && !config_.intraday_interval.has_value()) {
        Config cfg = Config::load_from_file(config_.config_path);
        if (cfg.has_param("SCHWAB_INTRADAY_INTERVAL_MS")) {
            auto interval_ms = cfg.get_int("SCHWAB_INTRADAY_INTERVAL_MS", 0);
            if (interval_ms > 0) {
                config_.intraday_interval = std::chrono::milliseconds(interval_ms);
            }
        }
    }

    if (config_.scheduled_times.empty() && !config_.intraday_interval.has_value()) {
        std::cerr << "  * [ERROR] Neither scheduled bell times nor intraday_interval configured. Stopping daemon.\n";
        return 1;
    }

    std::cout << "\n================================================================================\n"
              << "  [SCHWAB-EXECUTOR] PERSISTENT TRADING DAEMON\n"
              << "================================================================================\n"
              << "  * Strategy Model     : [" << strategy_->name() << "]\n"
              << "  * Execution Target   : " << (config_.dry_run ? "[SIMULATION - DRY RUN]" : "[LIVE BROKER ORDERS]") << "\n"
              << "  * Scheduled Bells    : " << config_.scheduled_times.size() << " target times registered\n";

    for (const auto& t : config_.scheduled_times) {
        std::cout << "                         - " << t << " UTC\n";
    }

    if (config_.intraday_interval.has_value()) {
        std::cout << "  * Intraday Interval  : " << config_.intraday_interval->count() << " ms (Continuous Loop Active)\n";
    } else {
        std::cout << "  * Intraday Interval  : [DISABLED] Pure Timed Bell Execution Mode\n";
    }

    std::cout << "  * Daemon Status      : [RUNNING] Press Ctrl+C to gracefully stop.\n"
              << "================================================================================\n\n";

    auto last_token_refresh = PrecisionClock::now();
    size_t cycles_completed = 0;

    while (!active_stop->load(std::memory_order_relaxed)) {
        if (max_cycles.has_value() && cycles_completed >= *max_cycles) {
            break;
        }

        // OAuth token auto-refresh maintenance
        auto now_mono = PrecisionClock::now();
        if (now_mono - last_token_refresh >= config_.token_refresh_interval) {
            if (!config_.dry_run) {
                std::cout << "  * [DAEMON HEARTBEAT] Auto-refreshing OAuth access token...\n";
                TokenManager::ensure_authenticated(config_.token_path);
            }
            last_token_refresh = now_mono;
        }

        // Identify next scheduled market bell target
        auto now_sys = PrecisionClock::system_now();
        std::optional<std::chrono::system_clock::time_point> next_bell_tp;
        std::string next_bell_str;

        for (const auto& t_str : config_.scheduled_times) {
            auto secs_opt = PrecisionClock::parse_time_of_day(t_str);
            if (!secs_opt.has_value()) {
                continue;
            }
            auto tp = PrecisionClock::next_occurrence_utc(*secs_opt, now_sys);
            if (!next_bell_tp.has_value() || tp < *next_bell_tp) {
                next_bell_tp = tp;
                next_bell_str = t_str;
            }
        }

        if (!config_.scheduled_times.empty() && !next_bell_tp.has_value() && !config_.intraday_interval.has_value()) {
            std::cerr << "  * [ERROR] No valid scheduled times configured. Stopping daemon.\n";
            return 1;
        }

        // Pure Intraday Loop (no scheduled bells)
        if (!next_bell_tp.has_value()) {
            config_.session_name = "INTRADAY";
            run_immediate();
            cycles_completed++;

            // Interruptible sleep for configured interval
            auto sleep_duration = *config_.intraday_interval;
            auto sleep_chunk = std::chrono::milliseconds(100);
            auto elapsed = std::chrono::milliseconds(0);

            while (elapsed < sleep_duration && !active_stop->load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::min(sleep_chunk, sleep_duration - elapsed));
                elapsed += sleep_chunk;
            }
            continue;
        }

        auto time_to_bell = *next_bell_tp - now_sys;

        // Check if approaching scheduled bell (within pre-warm window)
        if (time_to_bell <= config_.pre_warm_lead_time && time_to_bell > std::chrono::seconds(0)) {
            std::cout << "  * [BELL SCHEDULED] Approaching target " << next_bell_str << " UTC ("
                      << std::chrono::duration_cast<std::chrono::seconds>(time_to_bell).count() << "s remaining)\n";

            // Pre-warm WinHTTP TLS socket
            if (!config_.dry_run) {
                std::cout << "  * [TIMER] Pre-warming Charles Schwab gateway TLS socket...\n";
                http_client_.prewarm();
                std::cout << "  * [TIMER] Connection hot and ready.\n";
            }

            // Hybrid countdown to the target microsecond
            auto monotonic_target = PrecisionClock::now() + std::chrono::duration_cast<PrecisionClock::Duration>(time_to_bell);
            auto jitter = PrecisionClock::sleep_until(monotonic_target, config_.spin_threshold);

            std::cout << "  * [BELL ALIGNED] Target reached! Measured clock jitter: "
                      << (jitter.count() / 1000.0) << " us. Firing scheduled execution...\n";

            config_.session_name = "BELL_" + next_bell_str;
            run_immediate();
            cycles_completed++;
            continue;
        }

        // Continuous Intraday Evaluation (if enabled)
        if (config_.intraday_interval.has_value()) {
            // Check if sufficient lead time remains before next scheduled bell
            if (time_to_bell > (config_.pre_warm_lead_time + *config_.intraday_interval)) {
                config_.session_name = "INTRADAY";
                run_immediate();
                cycles_completed++;

                // Interruptible sleep for configured interval
                auto sleep_duration = *config_.intraday_interval;
                auto sleep_chunk = std::chrono::milliseconds(100);
                auto elapsed = std::chrono::milliseconds(0);

                while (elapsed < sleep_duration && !active_stop->load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::min(sleep_chunk, sleep_duration - elapsed));
                    elapsed += sleep_chunk;
                }
                continue;
            }
        }

        // Coarse Sleep until pre-warm window
        auto coarse_sleep = time_to_bell - config_.pre_warm_lead_time;
        if (coarse_sleep > std::chrono::seconds(1)) {
            // Sleep in 500ms interruptible intervals to ensure prompt Ctrl+C responsiveness
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    std::cout << "\n================================================================================\n"
              << "  [SCHWAB-EXECUTOR] DAEMON STOPPED CLEANLY (SIGINT / SHUTDOWN)\n"
              << "================================================================================\n\n";

    return 0;
}

} // namespace schwab
