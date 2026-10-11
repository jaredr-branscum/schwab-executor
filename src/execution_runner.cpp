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

namespace {

/// @brief Find existing position by symbol.
[[nodiscard]] inline const Position* find_position(
    std::span<const Position> positions,
    Symbol symbol
) noexcept {
    for (const auto& pos : positions) {
        if (pos.symbol == symbol) {
            return &pos;
        }
    }
    return nullptr;
}

/// @brief Find real-time quote by symbol.
[[nodiscard]] inline const Quote* find_quote(
    std::span<const Quote> quotes,
    Symbol symbol
) noexcept {
    for (const auto& q : quotes) {
        if (q.symbol == symbol && q.is_valid()) {
            return &q;
        }
    }
    return nullptr;
}

/// @brief Resolves reference valuation price via priority:
/// Live Quote -> Position Current -> Position Average -> Limit Price -> Fallback/Reject
[[nodiscard]] inline double resolve_reference_price(
    const Order& ord,
    std::span<const Quote> quotes,
    const Position* pos,
    bool is_dry_run,
    std::vector<std::string>& diagnostics
) {
    // Live real-time market quote
    for (const auto& q : quotes) {
        if (q.symbol == ord.symbol && q.is_valid()) {
            if (q.is_crossed()) [[unlikely]] {
                std::string msg = "  * [MARKET WARNING] Crossed order book for ";
                msg.append(ord.symbol.view());
                msg.append(" (bid: $");
                msg.append(std::to_string(q.bid_price));
                msg.append(", ask: $");
                msg.append(std::to_string(q.ask_price));
                msg.append(")");
                diagnostics.push_back(std::move(msg));
            }
            return q.reference_price();
        }
    }

    // Portfolio position snapshot fallback
    if (pos != nullptr) {
        if (pos->current_price > 0.0) {
            return pos->current_price;
        }
        if (pos->average_price > 0.0) {
            return pos->average_price;
        }
    }

    // Limit order explicit price bound
    if (ord.type == OrderType::Limit && ord.limit_price.has_value() && *ord.limit_price > 0.0) {
        return *ord.limit_price;
    }

    // Fallback safeguard
    if (is_dry_run) [[unlikely]] {
        return 1.0; // Controlled dry-run baseline
    }

    // Live unquoted & unpositioned symbol: cannot be safely valued
    std::string msg = "  * [CRITICAL RISK] Cannot approve order for ";
    msg.append(ord.symbol.view());
    msg.append(": unquoted symbol with zero portfolio valuation.");
    diagnostics.push_back(std::move(msg));
    return 0.0;
}

} // namespace

std::string ExecutionRunner::format_reconciliation_table(
    const BatchReconciliationReport& report,
    std::span<const Quote> quotes
) {
    std::ostringstream ss;

    ss << "\n================================================================================================================\n"
       << "  [SCHWAB-EXECUTOR] POST-TRADE FILL & SLIPPAGE RECONCILIATION\n"
       << "================================================================================================================\n"
       << "  " << std::left
       << std::setw(6)  << "SIDE"
       << std::setw(10) << "SYMBOL"
       << std::setw(12) << "ORDERED"
       << std::setw(12) << "FILLED"
       << std::setw(14) << "FILL ($)"
       << std::setw(14) << "QUOTE ($)"
       << std::setw(16) << "SLIPPAGE"
       << std::setw(14) << "STATE"
       << "ORDER ID\n"
       << "  ----------------------------------------------------------------------------------------------------------------\n";

    for (const auto& ord_res : report.orders) {
        std::string side_str(to_string(ord_res.side));
        std::string sym_str(ord_res.symbol.view());
        std::string state_str(to_string(ord_res.final_state));
        std::string id_str = ord_res.order_id.empty() ? "-" : ord_res.order_id;

        std::ostringstream ord_qty_stream;
        ord_qty_stream << std::fixed << std::setprecision(2) << ord_res.ordered_quantity;

        std::ostringstream fill_qty_stream;
        fill_qty_stream << std::fixed << std::setprecision(2) << ord_res.filled_quantity;

        std::string fill_price_str = "-";
        if (ord_res.average_fill_price > 0.0) {
            std::ostringstream fps;
            fps << "$" << std::fixed << std::setprecision(2) << ord_res.average_fill_price;
            fill_price_str = fps.str();
        }

        std::string quote_price_str = "-";
        double quote_val = 0.0;
        if (ord_res.slippage.has_slippage) {
            quote_val = ord_res.slippage.quote_price;
        } else {
            const Quote* q = find_quote(quotes, ord_res.symbol);
            if (q != nullptr && q->is_valid()) {
                quote_val = (ord_res.side == OrderSide::Buy) ? q->ask_price : q->bid_price;
                if (quote_val <= 0.0) quote_val = q->reference_price();
            }
        }
        if (quote_val > 0.0) {
            std::ostringstream qps;
            qps << "$" << std::fixed << std::setprecision(2) << quote_val;
            quote_price_str = qps.str();
        }

        std::string slippage_str = "-";
        if (ord_res.slippage.has_slippage) {
            std::ostringstream sls;
            if (ord_res.slippage.slippage_bps > 0.0) {
                sls << "+";
            }
            sls << std::fixed << std::setprecision(1) << ord_res.slippage.slippage_bps << " bps";
            slippage_str = sls.str();
        } else if (ord_res.average_fill_price > 0.0 && quote_val > 0.0) {
            auto s = calculate_slippage(ord_res.side, quote_val, ord_res.average_fill_price);
            std::ostringstream sls;
            if (s.slippage_bps > 0.0) {
                sls << "+";
            }
            sls << std::fixed << std::setprecision(1) << s.slippage_bps << " bps";
            slippage_str = sls.str();
        }

        ss << "  " << std::left
           << std::setw(6)  << side_str
           << std::setw(10) << sym_str
           << std::setw(12) << ord_qty_stream.str()
           << std::setw(12) << fill_qty_stream.str()
           << std::setw(14) << fill_price_str
           << std::setw(14) << quote_price_str
           << std::setw(16) << slippage_str
           << std::setw(14) << state_str
           << id_str << "\n";
    }

    ss << "  ----------------------------------------------------------------------------------------------------------------\n"
       << "  Total Tracked: " << report.total_tracked
       << " | Filled: " << report.total_filled
       << " | Cancelled: " << report.total_cancelled
       << " | Rejected: " << report.total_rejected
       << " | Timed Out: " << report.total_timed_out;

    if (report.total_filled > 0) {
        ss << " | Avg Slippage: ";
        if (report.average_slippage_bps > 0.0) ss << "+";
        ss << std::fixed << std::setprecision(1) << report.average_slippage_bps << " bps";
    }

    ss << " | Reconciliation Time: " << (report.elapsed_time.count() / 1000.0) << " ms\n"
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

    // Populate track_fills from config if not explicitly set
    if (!config_.track_fills && cfg.has_param("SCHWAB_TRACK_FILLS")) {
        config_.track_fills = cfg.get_bool("SCHWAB_TRACK_FILLS", false);
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

    // Pre-Trade RiskGuard Validation (Notional caps, collars, spread breaker, MOC cutoff)
    RiskLimits limits;
    limits.max_order_notional = cfg.execution.max_order_notional;
    limits.price_collar_percent = cfg.get_double("SCHWAB_PRICE_COLLAR_PERCENT", 0.05);
    limits.max_bid_ask_spread_bps = cfg.get_double("SCHWAB_MAX_SPREAD_BPS", 50.0);
    limits.enforce_moc_cutoff = true;
    limits.dry_run = false; // Allow plan generation; ExecutionDriver handles dry-run simulation
    limits.reject_crossed_book = true;
    RiskGuard risk_guard(limits);

    std::vector<Order> approved_orders;
    approved_orders.reserve(raw_orders.size());

    // Real-Time Market Data Valuation
    std::vector<Symbol> symbols_to_quote;
    symbols_to_quote.reserve(raw_orders.size());
    for (const auto& ord : raw_orders) {
        if (!ord.symbol.is_valid()) continue;
        if (std::find(symbols_to_quote.begin(), symbols_to_quote.end(), ord.symbol) == symbols_to_quote.end()) {
            symbols_to_quote.push_back(ord.symbol);
        }
    }

    // Deferred diagnostics buffer to keep order validation and dispatch silent and zero-I/O
    std::vector<std::string> deferred_diagnostics;
    deferred_diagnostics.reserve(raw_orders.size() + 4);

    last_market_quotes_.clear();
    if (!symbols_to_quote.empty() && access_token != "SIMULATED_ACCESS_TOKEN" && !access_token.empty()) {
        std::string quote_err;
        last_market_quotes_ = MarketDataManager::fetch_quotes_batch(
            http_client_,
            symbols_to_quote,
            access_token,
            &quote_err
        );
        if (!last_market_quotes_.empty()) {
            deferred_diagnostics.push_back("  * Market Data Quotes : Fetched " + 
                std::to_string(last_market_quotes_.size()) + " real-time quote(s)");
        } else if (!quote_err.empty()) {
            deferred_diagnostics.push_back("  * Market Data Notice : Quote fetch notice: " + 
                quote_err + " (falling back to portfolio position marks)");
        }
    }

    // Institutional Sequencing: Partition Sells first, Buys second
    std::vector<Order> staged_sells;
    std::vector<Order> staged_buys;
    staged_sells.reserve(raw_orders.size());
    staged_buys.reserve(raw_orders.size());

    for (const auto& ord : raw_orders) {
        if (ord.side == OrderSide::Sell) {
            staged_sells.push_back(ord);
        } else {
            staged_buys.push_back(ord);
        }
    }

    bool has_rejected_sells = false;

    // PASS 1: Validate and Stage Sells
    for (const auto& ord : staged_sells) {
        const Position* pos = find_position(positions, ord.symbol);
        const double current_qty = pos ? pos->quantity : 0.0;
        const Quote* quote = find_quote(last_market_quotes_, ord.symbol);

        RiskCheckResult check;
        if (quote != nullptr) {
            check = risk_guard.validate_order(ord, *quote, current_qty);
        } else {
            const double market_price = resolve_reference_price(
                ord, last_market_quotes_, pos, config_.dry_run, deferred_diagnostics
            );
            if (market_price <= 0.0 && !config_.dry_run) [[unlikely]] {
                has_rejected_sells = true;
                continue; // Rejected at risk gate (unquoted and unpositioned)
            }
            check = risk_guard.validate_order(ord, current_qty, market_price);
        }

        if (check.approved) {
            approved_orders.push_back(ord);
        } else {
            has_rejected_sells = true;
            std::string msg = "  * [RISK WARNING] Order rejected for ";
            msg.append(ord.symbol.view());
            msg.append(": ");
            msg.append(check.message);
            deferred_diagnostics.push_back(std::move(msg));
        }
    }

    // PASS 2: Validate and Stage Buys (Enforcing Atomic Abort if preceding Sells were blocked)
    for (const auto& ord : staged_buys) {
        if (has_rejected_sells) [[unlikely]] {
            std::string msg = "  * [ATOMIC ABORT] Buy order aborted for ";
            msg.append(ord.symbol.view());
            msg.append(": preceding sell order was blocked by risk controls");
            deferred_diagnostics.push_back(std::move(msg));
            continue;
        }

        const Position* pos = find_position(positions, ord.symbol);
        const double current_qty = pos ? pos->quantity : 0.0;
        const Quote* quote = find_quote(last_market_quotes_, ord.symbol);

        RiskCheckResult check;
        if (quote != nullptr) {
            check = risk_guard.validate_order(ord, *quote, current_qty);
        } else {
            const double market_price = resolve_reference_price(
                ord, last_market_quotes_, pos, config_.dry_run, deferred_diagnostics
            );
            if (market_price <= 0.0 && !config_.dry_run) [[unlikely]] {
                continue; // Rejected at risk gate (unquoted and unpositioned)
            }
            check = risk_guard.validate_order(ord, current_qty, market_price);
        }

        if (check.approved) {
            approved_orders.push_back(ord);
        } else {
            std::string msg = "  * [RISK WARNING] Order rejected for ";
            msg.append(ord.symbol.view());
            msg.append(": ");
            msg.append(check.message);
            deferred_diagnostics.push_back(std::move(msg));
        }
    }

    if (approved_orders.empty()) {
        for (const auto& diag : deferred_diagnostics) {
            std::cerr << diag << "\n";
        }
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
    driver_cfg.abort_buys_on_sell_failure = true;

    ExecutionDriver driver(http_client_, driver_cfg);
    ExecutionReport report = driver.execute_plan(plan, access_token);

    // Flush Deferred Diagnostics and Output Formatted Telemetry Table Post-Execution
    for (const auto& diag : deferred_diagnostics) {
        std::cout << diag << "\n";
    }

    // Output Formatted Telemetry Table and Save to History/File
    std::string table_str = format_telemetry_table(report, config_.dry_run);
    std::cout << table_str;

    execution_history_.push_back(report);

    if (telemetry_callback_) {
        telemetry_callback_(report);
    }

    std::string recon_table_str;
    if (config_.track_fills && report.total_succeeded > 0) {
        if (!config_.dry_run) {
            std::cout << "  * Order Tracking     : Reconciling post-dispatch fills with Charles Schwab...\n";
            OrderTrackerConfig tracker_cfg;
            tracker_cfg.account_hash = account_hash;
            tracker_cfg.polling_interval_ms = config_.fill_poll_interval_ms;
            tracker_cfg.timeout_ms = config_.fill_poll_timeout_ms;

            OrderTracker tracker(http_client_, tracker_cfg);
            BatchReconciliationReport recon = tracker.reconcile_report(report, access_token, last_market_quotes_);

            recon_table_str = format_reconciliation_table(recon, last_market_quotes_);
            std::cout << recon_table_str;
            last_reconciliation_report_ = recon;
        } else {
            // Simulated dry-run fill reconciliation
            BatchReconciliationReport sim_recon;
            sim_recon.orders.reserve(report.results.size());
            for (const auto& res : report.results) {
                OrderReconciliationResult r;
                r.symbol = res.order.symbol;
                r.side = res.order.side;
                r.ordered_quantity = res.order.quantity;
                r.filled_quantity = res.order.quantity;
                r.order_id = res.order_id.value_or("DRY_RUN_SIMULATED");
                r.final_state = OrderExecutionState::Filled;
                r.is_terminal = true;
                r.is_filled = true;
                const Quote* q = find_quote(last_market_quotes_, r.symbol);
                if (q != nullptr && q->is_valid()) {
                    double ref = (r.side == OrderSide::Buy) ? q->ask_price : q->bid_price;
                    if (ref <= 0.0) ref = q->reference_price();
                    r.average_fill_price = ref;
                    r.slippage = calculate_slippage(r.side, ref, ref);
                } else {
                    r.average_fill_price = res.order.limit_price.value_or(100.0);
                }
                sim_recon.orders.push_back(std::move(r));
            }
            sim_recon.total_tracked = sim_recon.orders.size();
            sim_recon.total_filled = sim_recon.orders.size();
            sim_recon.all_terminal = true;
            sim_recon.all_filled = true;
            recon_table_str = format_reconciliation_table(sim_recon, last_market_quotes_);
            std::cout << recon_table_str;
            last_reconciliation_report_ = sim_recon;
        }
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
            if (!recon_table_str.empty()) {
                log_file << recon_table_str << "\n";
            }
        }
    }

    return report.all_succeeded ? 0 : 1;
}

} // namespace schwab
