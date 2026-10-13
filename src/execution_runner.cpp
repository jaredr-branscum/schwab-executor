#include "schwab/execution_runner.hpp"
#include "schwab/order_serializer.hpp"
#include "schwab/security.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

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
    bool is_dry_run,
    bool is_preview
) {
    std::ostringstream ss;

    ss << "\n=====================================================================================================================\n"
       << "  [SCHWAB-EXECUTOR] "
       << (is_preview ? "BROKER ORDER PREVIEW VALIDATION (/previewOrder)" : (is_dry_run ? "SIMULATION TELEMETRY (DRY RUN)" : "LIVE BROKER EXECUTION REPORT")) << "\n"
       << "=====================================================================================================================\n"
       << "  " << std::left 
       << std::setw(5)  << "SIDE"
       << std::setw(7)  << "SYMBOL"
       << std::setw(12) << "TAG"
       << std::setw(9)  << "QUANTITY"
       << std::setw(8)  << "PRICE"
       << std::setw(7)  << "TYPE"
       << std::setw(14) << "STATUS"
       << std::setw(18) << "ORDER ID"
       << std::setw(12) << "ENGINE (us)"
       << std::setw(18) << "NETWORK RTT (ms)"
       << "NOTES\n"
       << "  -------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& res : report.results) {
        std::string side_str(to_string(res.order.side));
        std::string sym_str(res.order.symbol.view());
        std::string tag_str = res.order.tag.empty() ? "-" : std::string(res.order.tag.view());
        std::string type_str(to_string(res.order.type));
        std::string status_str = res.success 
            ? (is_preview ? "PREVIEW_OK" : (is_dry_run ? "SIMULATED" : "FILLED/STAGED")) 
            : "FAILED";
        std::string id_str = res.order_id.value_or(is_preview ? "PREVIEW_VALIDATED" : (is_dry_run ? "DRY_RUN_ID" : "-"));

        std::ostringstream qty_stream;
        qty_stream << std::fixed << std::setprecision(2) << res.order.quantity;

        std::string price_str = "-";
        if (res.order.type == OrderType::Limit && res.order.limit_price.has_value() && *res.order.limit_price > 0.0) {
            std::ostringstream ps;
            const double p = *res.order.limit_price;
            if (std::abs(p * 100.0 - std::round(p * 100.0)) > 1e-4) {
                ps << "$" << std::fixed << std::setprecision(4) << p;
            } else {
                ps << "$" << std::fixed << std::setprecision(2) << p;
            }
            price_str = ps.str();
        } else if (res.order.type == OrderType::Market) {
            price_str = "MKT";
        } else if (res.order.type == OrderType::MarketOnClose) {
            price_str = "MOC";
        }

        std::ostringstream wire_stream;
        if (res.latency.count() > 0) {
            wire_stream << std::fixed << std::setprecision(1) << (res.latency.count() / 1000.0) << " ms";
        } else {
            wire_stream << "0.0 ms";
        }

        ss << "  " << std::left
           << std::setw(5)  << side_str
           << std::setw(7)  << sym_str
           << std::setw(12) << tag_str
           << std::setw(9)  << qty_stream.str()
           << std::setw(8)  << price_str
           << std::setw(7)  << type_str
           << std::setw(14) << status_str
           << std::setw(18) << id_str
           << std::setw(12) << res.engine_latency.count();

        if (res.success && res.error_message.empty()) {
            ss << wire_stream.str() << "\n";
        } else {
            ss << std::setw(18) << wire_stream.str()
               << res.error_message << "\n";
        }
    }

    std::chrono::microseconds total_engine_us{0};
    std::chrono::microseconds total_wire_us{0};
    for (const auto& res : report.results) {
        total_engine_us += res.engine_latency;
        total_wire_us += res.latency;
    }

    ss << "  -------------------------------------------------------------------------------------------------------------------\n"
       << "  Total Orders Submitted: " << report.total_submitted
       << " | Succeeded: " << report.total_succeeded
       << " | Failed: " << report.total_failed << "\n"
       << "  Engine Dispatch CPU Time: " << total_engine_us.count() << " us ("
       << std::fixed << std::setprecision(3) << (total_engine_us.count() / 1000.0) << " ms)"
       << " | Network RTT: " << std::fixed << std::setprecision(1) << (total_wire_us.count() / 1000.0) << " ms"
       << " | Total Latency: " << std::fixed << std::setprecision(3) << (report.total_execution_time.count() / 1000.0) << " ms\n"
       << "=====================================================================================================================\n\n";

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

/// @brief Pre-trade order validation, sequence partitioning (sells first, then buys),
/// and cash limit gating across account state.
[[nodiscard]] inline std::vector<Order> stage_and_validate_orders(
    const AccountPortfolio& acc,
    std::span<const Order> raw_orders,
    RiskGuard& risk_guard,
    std::span<const Quote> quotes,
    bool dry_run,
    std::vector<std::string>& diagnostics
) {
    // Sequencing: Partition Sells first, Buys second
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

    std::vector<Order> approved_orders;
    approved_orders.reserve(raw_orders.size());
    bool has_rejected_sells = false;

    // PASS 1: Validate and Stage Sells
    for (const auto& ord : staged_sells) {
        const Position* pos = find_position(acc.positions, ord.symbol);
        const double current_qty = pos ? pos->quantity : 0.0;
        const Quote* quote = find_quote(quotes, ord.symbol);

        RiskCheckResult check;
        if (quote != nullptr) {
            check = risk_guard.validate_order(ord, *quote, current_qty);
        } else {
            const double market_price = resolve_reference_price(
                ord, quotes, pos, dry_run, diagnostics
            );
            if (market_price <= 0.0 && !dry_run) [[unlikely]] {
                has_rejected_sells = true;
                continue;
            }
            check = risk_guard.validate_order(ord, current_qty, market_price);
        }

        if (check.approved) {
            approved_orders.push_back(ord);
        } else {
            has_rejected_sells = true;
            std::string wmsg = "  * [RISK WARNING] Order rejected for ";
            wmsg.append(ord.symbol.view());
            wmsg.append(": ");
            wmsg.append(check.message);
            diagnostics.push_back(std::move(wmsg));
        }
    }

    // PASS 2: Validate and Stage Buys (Enforcing Atomic Abort if preceding Sells were blocked)
    for (const auto& ord : staged_buys) {
        if (has_rejected_sells) [[unlikely]] {
            std::string amsg = "  * [ATOMIC ABORT] Buy order aborted for ";
            amsg.append(ord.symbol.view());
            amsg.append(": preceding sell order was blocked by risk controls");
            diagnostics.push_back(std::move(amsg));
            continue;
        }

        const Position* pos = find_position(acc.positions, ord.symbol);
        const double current_qty = pos ? pos->quantity : 0.0;
        const Quote* quote = find_quote(quotes, ord.symbol);

        RiskCheckResult check;
        if (quote != nullptr) {
            check = risk_guard.validate_order(ord, *quote, current_qty);
        } else {
            const double market_price = resolve_reference_price(
                ord, quotes, pos, dry_run, diagnostics
            );
            if (market_price <= 0.0 && !dry_run) [[unlikely]] {
                continue;
            }
            check = risk_guard.validate_order(ord, current_qty, market_price);
        }

        if (check.approved) {
            approved_orders.push_back(ord);
        } else {
            std::string wmsg = "  * [RISK WARNING] Order rejected for ";
            wmsg.append(ord.symbol.view());
            wmsg.append(": ");
            wmsg.append(check.message);
            diagnostics.push_back(std::move(wmsg));
        }
    }

    // PASS 3: Aggregate Cash & Buying Power Gate
    if (risk_guard.limits().enforce_cash_limits && !risk_guard.limits().allow_margin_borrowing) {
        double total_available_cash = (std::max)(0.0, acc.available_cash());
        for (const auto& ord : approved_orders) {
            if (ord.side == OrderSide::Sell) {
                double p = 0.0;
                if (ord.type == OrderType::Limit && ord.limit_price.has_value() && *ord.limit_price > 0.0) {
                    p = *ord.limit_price;
                } else {
                    const Quote* q = find_quote(quotes, ord.symbol);
                    if (q != nullptr && q->is_valid()) {
                        p = (q->bid_price > 0.0) ? q->bid_price : q->reference_price();
                    } else {
                        const Position* pos = find_position(acc.positions, ord.symbol);
                        p = (pos != nullptr && pos->current_price > 0.0) ? pos->current_price : 0.0;
                    }
                }
                if (p > 0.0) {
                    total_available_cash += ord.quantity * p;
                }
            }
        }

        double total_buy_commitment = 0.0;
        for (const auto& ord : approved_orders) {
            if (ord.side == OrderSide::Buy) {
                double p = 0.0;
                if (ord.type == OrderType::Limit && ord.limit_price.has_value() && *ord.limit_price > 0.0) {
                    p = *ord.limit_price;
                } else {
                    const Quote* q = find_quote(quotes, ord.symbol);
                    if (q != nullptr && q->is_valid()) {
                        p = (q->ask_price > 0.0) ? q->ask_price : q->reference_price();
                    } else {
                        const Position* pos = find_position(acc.positions, ord.symbol);
                        p = (pos != nullptr && pos->current_price > 0.0) ? pos->current_price : 0.0;
                    }
                }
                total_buy_commitment += ord.quantity * p;
            }
        }

        auto cash_check = risk_guard.validate_cash_commitment(total_buy_commitment, total_available_cash);
        if (!cash_check.approved) [[unlikely]] {
            char diag_buf[256];
            std::snprintf(diag_buf, sizeof(diag_buf),
                "  * [ATOMIC ABORT] Entire rebalance batch aborted for account %.*s: Aggregate buy commitments ($%.2f) exceed available purchasing power ($%.2f) by $%.2f (unintended margin borrowing blocked)",
                static_cast<int>(acc.masked_account_number().size()), acc.masked_account_number().data(),
                total_buy_commitment, total_available_cash, total_buy_commitment - total_available_cash);
            diagnostics.push_back(std::string(diag_buf));

            approved_orders.clear();
        }
    }

    return approved_orders;
}

/// @brief Generates simulated fill reconciliation for dry-run validation.
[[nodiscard]] inline BatchReconciliationReport generate_simulated_reconciliation(
    const ExecutionReport& report,
    std::span<const Quote> quotes
) {
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
        const Quote* q = find_quote(quotes, r.symbol);
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
    return sim_recon;
}

/// @brief Appends telemetry block to log file.
inline void append_telemetry_file(
    const std::filesystem::path& log_path,
    std::string_view session_name,
    std::string_view strategy_name,
    std::string_view masked_account,
    std::string_view table_str,
    std::string_view recon_table_str
) {
    std::error_code ec;
    if (log_path.has_parent_path()) {
        std::filesystem::create_directories(log_path.parent_path(), ec);
    }
    std::ofstream log_file(log_path, std::ios::app);
    if (log_file.is_open()) {
        auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm gm_tm;
#if defined(_WIN32)
        gmtime_s(&gm_tm, &now_t);
#else
        gmtime_r(&now_t, &gm_tm);
#endif
        log_file << "[" << std::put_time(&gm_tm, "%Y-%m-%d %H:%M:%S UTC") << "] "
                 << "Mode: " << session_name << " | Strategy: " << strategy_name
                 << " | Account: " << masked_account << "\n"
                 << table_str << "\n";
        if (!recon_table_str.empty()) {
            log_file << recon_table_str << "\n";
        }
    }
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

std::string ExecutionRunner::format_batch_summary_table(
    const BatchSessionSummary& summary
) {
    std::ostringstream ss;
    ss << "\n================================================================================================================\n"
       << "  [SCHWAB-EXECUTOR] MULTI-ACCOUNT BATCH EXECUTION SUMMARY (" << summary.total_accounts << " Accounts)\n"
       << "================================================================================================================\n"
       << "  " << std::left
       << std::setw(12) << "ACCOUNT"
       << std::setw(14) << "TYPE"
       << std::setw(16) << "EQUITY ($)"
       << std::setw(16) << "CASH ($)"
       << std::setw(10) << "ORDERS"
       << std::setw(14) << "STATUS"
       << std::setw(14) << "TIME (ms)"
       << "NOTES\n"
       << "  ----------------------------------------------------------------------------------------------------------------\n";

    for (const auto& res : summary.account_results) {
        std::string orders_str = std::to_string(res.orders_succeeded) + "/" + std::to_string(res.orders_generated);
        std::string status_str = res.success ? "SUCCEEDED" : "FAILED";

        std::ostringstream eq_stream, cash_stream, time_stream;
        eq_stream << std::fixed << std::setprecision(2) << res.liquidation_value;
        cash_stream << std::fixed << std::setprecision(2) << res.cash_balance;
        time_stream << std::fixed << std::setprecision(3) << (res.execution_time.count() / 1000.0);

        ss << "  " << std::left
           << std::setw(12) << res.masked_account_number
           << std::setw(14) << res.account_type
           << std::setw(16) << eq_stream.str()
           << std::setw(16) << cash_stream.str()
           << std::setw(10) << orders_str
           << std::setw(14) << status_str
           << std::setw(14) << time_stream.str()
           << res.status_notes << "\n";
    }

    ss << "  ----------------------------------------------------------------------------------------------------------------\n"
       << "  Total Accounts: " << summary.total_accounts
       << " | Succeeded: " << summary.succeeded_accounts
       << " | Failed: " << summary.failed_accounts
       << " | Total Orders: " << summary.total_orders_submitted
       << " (Succeeded: " << summary.total_orders_succeeded
       << ", Failed: " << summary.total_orders_failed << ")\n"
       << "  Total Session Time: " << std::fixed << std::setprecision(3)
       << (summary.total_session_time.count() / 1000.0) << " ms\n"
       << "================================================================================================================\n\n";

    return ss.str();
}

int ExecutionRunner::execute(
    std::span<const Position> simulated_positions,
    std::optional<double> simulated_cash,
    std::span<const AccountPortfolio> simulated_accounts
) {
    std::cout << "\n================================================================================\n"
              << "  [SCHWAB-EXECUTOR] SESSION EXECUTION RUNNER\n"
              << "================================================================================\n"
              << "  * Mode               : [" << config_.session_name << "]\n"
              << "  * Strategy Model     : [" << strategy_->name() << "]\n"
              << "  * Execution Target   : " 
              << (config_.preview_only ? "[BROKER PREVIEW - NO EXECUTION]" : (config_.dry_run ? "[SIMULATION - DRY RUN]" : "[LIVE BROKER ORDERS]")) << "\n";

    Config cfg = Config::load_from_file(config_.config_path);

    // Override dry_run from config if not explicitly set via CLI
    if (!config_.dry_run && !config_.preview_only && cfg.execution.dry_run) {
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

    // Populate accounts_filter from config if not explicitly set
    if (config_.accounts_filter.empty()) {
        if (!cfg.accounts.empty()) {
            config_.accounts_filter = cfg.accounts;
        } else if (cfg.has_param("SCHWAB_ACCOUNTS")) {
            config_.accounts_filter = cfg.get_string("SCHWAB_ACCOUNTS");
        } else {
            config_.accounts_filter = "all";
        }
    }

    // Populate concurrent dispatch settings from config if present
    if (cfg.has_param("SCHWAB_CONCURRENT_DISPATCH")) {
        config_.enable_concurrent_dispatch = cfg.get_bool("SCHWAB_CONCURRENT_DISPATCH", config_.enable_concurrent_dispatch);
    }
    if (cfg.has_param("SCHWAB_MAX_CONCURRENT_WORKERS")) {
        int64_t max_w = cfg.get_int("SCHWAB_MAX_CONCURRENT_WORKERS", static_cast<int64_t>(config_.max_concurrent_workers));
        if (max_w > 0) {
            config_.max_concurrent_workers = static_cast<size_t>(max_w);
        }
    }

    last_market_quotes_.clear();

    // Ensure active OAuth session
    std::string access_token;
    if (config_.dry_run && !config_.preview_only) {
        auto token_opt = TokenManager::load_from_file(config_.token_path);
        if (token_opt.has_value() && !token_opt->is_refresh_token_expired()) {
            if (token_opt->is_access_token_expired()) {
                token_opt = TokenManager::ensure_authenticated(config_.token_path);
            }
        }
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

    struct TokenScrubber {
        std::string& tok;
        ~TokenScrubber() { secure_clear(tok); }
    } scrubber{access_token};

    // Resolve Target Accounts to Rebalance
    std::vector<AccountPortfolio> target_accounts;

    if (!simulated_accounts.empty()) {
        std::string active_filter = config_.accounts_filter;
        if (!config_.account_hash_override.empty()) {
            active_filter = config_.account_hash_override;
        }
        std::vector<AccountPortfolio> input_accs(simulated_accounts.begin(), simulated_accounts.end());
        target_accounts = AccountManager::filter_accounts(input_accs, active_filter);
        if (target_accounts.empty()) {
            std::cerr << "  * [ERROR] Zero " << (config_.dry_run ? "simulated" : "pre-loaded") << " accounts matched filter '" << active_filter << "'.\n\n";
            return 1;
        }
        std::cout << "  * Portfolio Context  : " << (config_.dry_run ? "Simulated batch (" : "Pre-loaded batch (") << target_accounts.size() << " account(s))\n";
    } else if (!simulated_positions.empty() || simulated_cash.has_value()) {
        AccountPortfolio mock_acc;
        mock_acc.account_number = "SIMULATED";
        mock_acc.account_hash = config_.account_hash_override.empty() ? "SIMULATED_ACCOUNT" : config_.account_hash_override;
        mock_acc.cash_balance = simulated_cash.value_or(100'000.0);
        mock_acc.available_funds = mock_acc.cash_balance;
        mock_acc.liquidation_value = mock_acc.cash_balance;
        mock_acc.positions.assign(simulated_positions.begin(), simulated_positions.end());
        for (const auto& p : mock_acc.positions) {
            double price = (p.current_price > 0.0) ? p.current_price : p.average_price;
            mock_acc.liquidation_value += (p.quantity * price);
        }
        target_accounts.push_back(std::move(mock_acc));
        std::cout << "  * Portfolio Context  : Simulated (" << target_accounts[0].positions.size() 
                  << " positions, Cash: $" << std::fixed << std::setprecision(2) << target_accounts[0].available_cash() << ")\n";
    } else if (config_.dry_run && !config_.preview_only && access_token == "SIMULATED_ACCESS_TOKEN") {
        AccountPortfolio sim_acc;
        sim_acc.account_number = "SIMULATED";
        sim_acc.account_hash = config_.account_hash_override.empty() ? "SIMULATED_ACCOUNT" : config_.account_hash_override;
        sim_acc.cash_balance = 100'000.0;
        sim_acc.available_funds = 100'000.0;
        sim_acc.liquidation_value = 100'000.0;
        target_accounts.push_back(std::move(sim_acc));
        std::cout << "  * Portfolio Context  : Offline Simulation (0 positions, Cash: $" 
                  << std::fixed << std::setprecision(2) << target_accounts[0].available_cash() << ")\n";
    } else {
        // Query live Charles Schwab linked accounts
        std::cout << "  * Portfolio Context  : Querying live Charles Schwab linked accounts...\n";
        std::string fetch_err;
        auto all_linked = AccountManager::fetch_linked_accounts(http_client_, cfg.base_url, access_token, &fetch_err);

        // Single account fallback if fetch_linked_accounts returned empty
        if (all_linked.empty()) {
            std::string single_hash = !config_.account_hash_override.empty() ? config_.account_hash_override : config_.accounts_filter;
            if (!single_hash.empty()) {
                auto pf_opt = AccountManager::fetch_portfolio(http_client_, cfg.base_url, single_hash, access_token, &fetch_err);
                if (pf_opt.has_value()) {
                    all_linked.push_back(std::move(*pf_opt));
                }
            }
        }

        if (all_linked.empty()) {
            std::cerr << "  * [CRITICAL ERROR] Failed to fetch live account portfolio from Schwab: " 
                      << fetch_err << "\n";
            if (!config_.dry_run) {
                std::cerr << "  * Aborting execution: cannot submit live orders with unknown position state.\n\n";
                return 1;
            }
            std::cout << "  * Simulation fallback: using default cash context for dry-run validation.\n";
            AccountPortfolio fallback_acc;
            fallback_acc.account_number = "SIMULATED";
            fallback_acc.account_hash = "SIMULATED_ACCOUNT";
            fallback_acc.cash_balance = 100'000.0;
            fallback_acc.available_funds = 100'000.0;
            fallback_acc.liquidation_value = 100'000.0;
            target_accounts.push_back(std::move(fallback_acc));
        } else {
            std::string active_filter = config_.accounts_filter;
            if (!config_.account_hash_override.empty()) {
                active_filter = config_.account_hash_override;
            }
            target_accounts = AccountManager::filter_accounts(all_linked, active_filter);
            if (target_accounts.empty()) {
                std::cerr << "  * [ERROR] Zero accounts matched filter '" << active_filter 
                          << "' (found " << all_linked.size() << " linked account(s)).\n\n";
                return 1;
            }
            std::string display_filter = active_filter;
            if (display_filter.length() > 16) {
                display_filter = display_filter.substr(0, 4) + "..." + display_filter.substr(display_filter.length() - 4);
            }
            std::cout << "  * Accounts Resolved  : " << target_accounts.size() << " account(s) selected for execution (Filter: '" 
                      << (display_filter.empty() ? "all" : display_filter) << "')\n";
        }
    }

    // Pre-Trade RiskGuard Limit Configuration
    RiskLimits limits;
    limits.max_order_notional = cfg.execution.max_order_notional;
    limits.price_collar_percent = cfg.get_double("SCHWAB_PRICE_COLLAR_PERCENT", 0.05);
    limits.max_bid_ask_spread_bps = cfg.get_double("SCHWAB_MAX_SPREAD_BPS", 50.0);
    limits.enforce_moc_cutoff = !config_.dry_run && !config_.preview_only;
    limits.dry_run = false;
    limits.reject_crossed_book = true;
    limits.enforce_cash_limits = config_.enforce_cash_limits && cfg.get_bool("SCHWAB_ENFORCE_CASH_LIMITS", true);
    limits.allow_margin_borrowing = config_.allow_margin_borrowing || cfg.get_bool("SCHWAB_ALLOW_MARGIN_BORROWING", false);
    RiskGuard risk_guard(limits);

    auto session_start = std::chrono::steady_clock::now();
    BatchSessionSummary batch_summary;
    batch_summary.total_accounts = target_accounts.size();

    // Pre-resolve market data quotes for strategy target symbols and portfolio holdings
    std::vector<Symbol> pre_quote_symbols;
    auto strat_targets = strategy_->target_symbols();
    for (const auto& sym : strat_targets) {
        if (sym.is_valid() && std::find(pre_quote_symbols.begin(), pre_quote_symbols.end(), sym) == pre_quote_symbols.end()) {
            if (find_quote(last_market_quotes_, sym) == nullptr) {
                pre_quote_symbols.push_back(sym);
            }
        }
    }
    for (const auto& acc : target_accounts) {
        for (const auto& pos : acc.positions) {
            if (pos.symbol.is_valid() && std::find(pre_quote_symbols.begin(), pre_quote_symbols.end(), pos.symbol) == pre_quote_symbols.end()) {
                if (find_quote(last_market_quotes_, pos.symbol) == nullptr) {
                    pre_quote_symbols.push_back(pos.symbol);
                }
            }
        }
    }

    if (!pre_quote_symbols.empty() && access_token != "SIMULATED_ACCESS_TOKEN" && !access_token.empty()) {
        std::string quote_err;
        auto initial_quotes = MarketDataManager::fetch_quotes_batch(
            http_client_,
            pre_quote_symbols,
            access_token,
            &quote_err
        );
        if (!initial_quotes.empty()) {
            std::cout << "  * Market Data Quotes : Pre-fetched " << initial_quotes.size() << " real-time quote(s) for valuation\n";
            for (auto& q : initial_quotes) {
                last_market_quotes_.push_back(std::move(q));
            }
        } else if (!quote_err.empty()) {
            std::cout << "  * Market Data Notice : Quote pre-fetch notice: " << quote_err << "\n";
        }
    }

    bool use_concurrent = (target_accounts.size() > 1) && config_.enable_concurrent_dispatch;

    if (use_concurrent) {
        std::cout << "  * Dispatch Engine    : [CONCURRENT WORKER POOL] Enabled (" 
                  << target_accounts.size() << " accounts, max " 
                  << config_.max_concurrent_workers << " workers)\n";

        struct AccountWorkerContext {
            size_t account_index{0};
            AccountPortfolio account;
            AccountSessionResult session_result;
            RebalancePlan plan;
            std::vector<std::string> diagnostics;
            bool has_work{false};
            bool pre_check_failed{false};
            std::unique_ptr<HttpClient> dedicated_client{nullptr};
            ExecutionReport report;
            std::string formatted_table;
            std::string formatted_recon_table;
            std::chrono::steady_clock::time_point start_time;
            std::chrono::steady_clock::time_point finish_time;
        };

        std::vector<AccountWorkerContext> contexts(target_accounts.size());
        std::vector<std::vector<Order>> raw_orders_per_account(target_accounts.size());
        std::vector<Symbol> additional_symbols_to_quote;

        // Generate raw orders across all accounts and identify any missing quotes
        for (size_t acc_idx = 0; acc_idx < target_accounts.size(); ++acc_idx) {
            auto& ctx = contexts[acc_idx];
            ctx.account_index = acc_idx;
            ctx.account = target_accounts[acc_idx];

            ctx.session_result.account_number = ctx.account.account_number;
            ctx.session_result.masked_account_number = ctx.account.masked_account_number();
            ctx.session_result.account_hash = ctx.account.account_hash;
            ctx.session_result.account_type = ctx.account.account_type;
            ctx.session_result.cash_balance = ctx.account.cash_balance;
            ctx.session_result.liquidation_value = ctx.account.liquidation_value;
            ctx.session_result.positions_count = ctx.account.positions.size();

            // Validate account hash
            if (ctx.account.account_hash.empty()) {
                if (config_.dry_run && !config_.preview_only) {
                    ctx.account.account_hash = "SIMULATED_ACCOUNT";
                    ctx.session_result.account_hash = "SIMULATED_ACCOUNT";
                } else {
                    ctx.diagnostics.push_back("  * [ERROR] Account " + ctx.account.masked_account_number() + " missing encrypted hashValue. Skipping.");
                    ctx.session_result.success = false;
                    ctx.session_result.status_notes = "Missing hashValue";
                    ctx.pre_check_failed = true;
                    continue;
                }
            }

            // Generate orders from strategy
            raw_orders_per_account[acc_idx] = strategy_->generate_orders(
                ctx.account.positions,
                ctx.account.available_cash(),
                last_market_quotes_
            );

            ctx.session_result.orders_generated = raw_orders_per_account[acc_idx].size();

            if (raw_orders_per_account[acc_idx].empty()) {
                ctx.session_result.success = true;
                ctx.session_result.status_notes = "Zero orders needed";
                ctx.has_work = false;
                continue;
            }

            // Check for missing quotes
            for (const auto& ord : raw_orders_per_account[acc_idx]) {
                if (!ord.symbol.is_valid()) continue;
                if (find_quote(last_market_quotes_, ord.symbol) == nullptr) {
                    if (std::find(additional_symbols_to_quote.begin(), additional_symbols_to_quote.end(), ord.symbol) == additional_symbols_to_quote.end()) {
                        additional_symbols_to_quote.push_back(ord.symbol);
                    }
                }
            }
        }

        // Batch fetch any additional quotes needed across all accounts
        if (!additional_symbols_to_quote.empty() && access_token != "SIMULATED_ACCESS_TOKEN" && !access_token.empty()) {
            std::string quote_err;
            auto new_quotes = MarketDataManager::fetch_quotes_batch(
                http_client_,
                additional_symbols_to_quote,
                access_token,
                &quote_err
            );
            if (!new_quotes.empty()) {
                for (auto& q : new_quotes) {
                    last_market_quotes_.push_back(std::move(q));
                }
            }
        }

        // Sequence and RiskGuard-validate each account
        for (size_t acc_idx = 0; acc_idx < target_accounts.size(); ++acc_idx) {
            auto& ctx = contexts[acc_idx];
            if (ctx.pre_check_failed || ctx.session_result.orders_generated == 0) {
                continue;
            }

            auto approved_orders = stage_and_validate_orders(
                ctx.account,
                raw_orders_per_account[acc_idx],
                risk_guard,
                last_market_quotes_,
                config_.dry_run,
                ctx.diagnostics
            );

            if (approved_orders.empty()) {
                ctx.session_result.success = false;
                ctx.session_result.status_notes = "Blocked by RiskGuard";
                ctx.pre_check_failed = true;
                ctx.has_work = false;
                continue;
            }

            ctx.plan.orders = std::move(approved_orders);
            ctx.has_work = true;
        }

        // Thread Worker Allocation & Dispatch
        std::vector<size_t> active_indices;
        for (size_t i = 0; i < contexts.size(); ++i) {
            if (contexts[i].has_work) {
                active_indices.push_back(i);
            }
        }

        if (!active_indices.empty()) {
            size_t max_workers = config_.max_concurrent_workers > 0 ? config_.max_concurrent_workers : 1;
            size_t num_threads = (std::min)(active_indices.size(), max_workers);

            // Instantiate dedicated transports
            for (size_t idx : active_indices) {
                if (transport_factory_) {
                    contexts[idx].dedicated_client = std::make_unique<HttpClient>(transport_factory_());
                } else {
                    contexts[idx].dedicated_client = std::make_unique<HttpClient>();
                }
            }

            std::atomic<bool> bell_triggered{false};
            std::atomic<size_t> ready_count{0};
            std::vector<std::thread> pool;
            pool.reserve(num_threads);

            for (size_t t_id = 0; t_id < num_threads; ++t_id) {
                pool.emplace_back([&, t_id]() {
                    // Pre-warm client for first account on this thread
                    size_t first_idx = active_indices[t_id];
                    if ((!config_.dry_run || config_.preview_only) && !contexts[first_idx].dedicated_client->is_connected()) {
                        contexts[first_idx].dedicated_client->prewarm();
                    }

                    ready_count.fetch_add(1, std::memory_order_release);

                    // Spin-wait barrier with CPU pause
                    while (!bell_triggered.load(std::memory_order_acquire)) {
#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64)
                        _mm_pause();
#else
                        std::this_thread::yield();
#endif
                    }

                    // Hot path dispatch
                    for (size_t work_i = t_id; work_i < active_indices.size(); work_i += num_threads) {
                        size_t idx = active_indices[work_i];
                        auto& c = contexts[idx];

                        // If not first account on this thread, ensure connection is hot
                        if (work_i != t_id && (!config_.dry_run || config_.preview_only) && !c.dedicated_client->is_connected()) {
                            c.dedicated_client->prewarm();
                        }

                        c.start_time = std::chrono::steady_clock::now();
                        ExecutionDriverConfig driver_cfg;
                        driver_cfg.account_hash = c.account.account_hash;
                        driver_cfg.dry_run = config_.dry_run && !config_.preview_only;
                        driver_cfg.preview_only = config_.preview_only;
                        driver_cfg.stop_on_auth_failure = true;
                        driver_cfg.abort_buys_on_sell_failure = true;

                        try {
                            ExecutionDriver driver(*c.dedicated_client, driver_cfg);
                            c.report = driver.execute_plan(c.plan, access_token);
                        } catch (const std::exception& ex) {
                            c.report.all_succeeded = false;
                            c.report.total_failed = c.plan.orders.size();
                            c.diagnostics.push_back(std::string("  * [CRITICAL EXCEPTION] Worker failure: ") + ex.what());
                        } catch (...) {
                            c.report.all_succeeded = false;
                            c.report.total_failed = c.plan.orders.size();
                            c.diagnostics.push_back("  * [CRITICAL EXCEPTION] Unknown worker exception");
                        }
                        c.finish_time = std::chrono::steady_clock::now();
                    }
                });
            }

            // Main thread waits until all worker threads are spinning on barrier
            while (ready_count.load(std::memory_order_acquire) < num_threads) {
                std::this_thread::yield();
            }

            // SIMULTANEOUS ATOMIC DISPATCH
            bell_triggered.store(true, std::memory_order_release);

            for (auto& t : pool) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }

        // Post-Barrier Output & File Logging (Main Thread)
        for (size_t acc_idx = 0; acc_idx < target_accounts.size(); ++acc_idx) {
            auto& ctx = contexts[acc_idx];

            std::cout << "\n--------------------------------------------------------------------------------\n"
                      << "  [ACCOUNT " << (acc_idx + 1) << "/" << target_accounts.size() << "] "
                      << ctx.account.masked_account_number() << " (" << ctx.account.account_type << ")\n"
                      << "--------------------------------------------------------------------------------\n"
                      << "  * Available Cash     : $" << std::fixed << std::setprecision(2) << ctx.account.available_cash() << "\n"
                      << "  * Liquidation Value  : $" << std::fixed << std::setprecision(2) << ctx.account.liquidation_value << "\n"
                      << "  * Existing Positions : " << ctx.account.positions.size() << "\n"
                      << "  * Strategy Output    : " << ctx.session_result.orders_generated << " orders emitted by strategy\n";

            for (const auto& diag : ctx.diagnostics) {
                std::cout << diag << "\n";
            }

            if (ctx.pre_check_failed) {
                std::cerr << "  * Error: " << ctx.session_result.status_notes << "\n\n";
                ctx.session_result.execution_time = std::chrono::microseconds{0};
                batch_summary.failed_accounts++;
                batch_summary.account_results.push_back(ctx.session_result);
                continue;
            }

            if (!ctx.has_work) {
                std::cout << "  * Status: [COMPLETED] Zero orders generated by strategy. Zero execution needed.\n";
                ctx.session_result.execution_time = std::chrono::microseconds{0};
                batch_summary.succeeded_accounts++;
                batch_summary.account_results.push_back(ctx.session_result);
                continue;
            }

            // Has executed orders
            ctx.formatted_table = format_telemetry_table(ctx.report, config_.dry_run, config_.preview_only);
            std::cout << ctx.formatted_table;

            execution_history_.push_back(ctx.report);

            if (telemetry_callback_) {
                telemetry_callback_(ctx.report);
            }

            // Fill tracking
            if (config_.track_fills && ctx.report.total_succeeded > 0 && !config_.preview_only) {
                if (!config_.dry_run) {
                    std::cout << "  * Order Tracking     : Reconciling post-dispatch fills with Charles Schwab...\n";
                    OrderTrackerConfig tracker_cfg;
                    tracker_cfg.account_hash = ctx.account.account_hash;
                    tracker_cfg.polling_interval_ms = config_.fill_poll_interval_ms;
                    tracker_cfg.timeout_ms = config_.fill_poll_timeout_ms;

                    OrderTracker tracker(*ctx.dedicated_client, tracker_cfg);
                    BatchReconciliationReport recon = tracker.reconcile_report(ctx.report, access_token, last_market_quotes_);

                    ctx.formatted_recon_table = format_reconciliation_table(recon, last_market_quotes_);
                    std::cout << ctx.formatted_recon_table;
                    last_reconciliation_report_ = recon;
                } else {
                    BatchReconciliationReport sim_recon = generate_simulated_reconciliation(ctx.report, last_market_quotes_);
                    ctx.formatted_recon_table = format_reconciliation_table(sim_recon, last_market_quotes_);
                    std::cout << ctx.formatted_recon_table;
                    last_reconciliation_report_ = sim_recon;
                }
            }

            // Atomic file logging
            if (!config_.dry_run && config_.telemetry_log_path.has_value()) {
                append_telemetry_file(
                    *config_.telemetry_log_path,
                    config_.session_name,
                    strategy_->name(),
                    ctx.account.masked_account_number(),
                    ctx.formatted_table,
                    ctx.formatted_recon_table
                );
            }

            ctx.session_result.orders_succeeded = ctx.report.total_succeeded;
            ctx.session_result.orders_failed = ctx.report.total_failed;
            ctx.session_result.success = ctx.report.all_succeeded;
            ctx.session_result.status_notes = ctx.report.all_succeeded ? "All orders completed" : "Dispatch failures";
            ctx.session_result.execution_time = std::chrono::duration_cast<std::chrono::microseconds>(ctx.finish_time - ctx.start_time);

            if (ctx.report.all_succeeded) {
                batch_summary.succeeded_accounts++;
            } else {
                batch_summary.failed_accounts++;
            }
            batch_summary.total_orders_submitted += ctx.report.total_submitted;
            batch_summary.total_orders_succeeded += ctx.report.total_succeeded;
            batch_summary.total_orders_failed += ctx.report.total_failed;
            batch_summary.account_results.push_back(ctx.session_result);
        }

        batch_summary.total_session_time = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - session_start
        );
        last_batch_summary_ = batch_summary;

        std::string summary_table = format_batch_summary_table(batch_summary);
        std::cout << summary_table;

        if (!config_.dry_run && config_.telemetry_log_path.has_value()) {
            std::ofstream log_file(*config_.telemetry_log_path, std::ios::app);
            if (log_file.is_open()) {
                log_file << summary_table << "\n";
            }
        }

        return (batch_summary.failed_accounts == 0) ? 0 : 1;
    }

    // Single-account / Sequential Execution Path
    for (size_t acc_idx = 0; acc_idx < target_accounts.size(); ++acc_idx) {
        auto& acc = target_accounts[acc_idx];
        auto acc_start = std::chrono::steady_clock::now();

        AccountSessionResult acc_res;
        acc_res.account_number = acc.account_number;
        acc_res.masked_account_number = acc.masked_account_number();
        acc_res.account_hash = acc.account_hash;
        acc_res.account_type = acc.account_type;
        acc_res.cash_balance = acc.cash_balance;
        acc_res.liquidation_value = acc.liquidation_value;
        acc_res.positions_count = acc.positions.size();

        if (target_accounts.size() > 1) {
            std::cout << "\n--------------------------------------------------------------------------------\n"
                      << "  [ACCOUNT " << (acc_idx + 1) << "/" << target_accounts.size() << "] "
                      << acc.masked_account_number() << " (" << acc.account_type << ")\n"
                      << "--------------------------------------------------------------------------------\n"
                      << "  * Available Cash     : $" << std::fixed << std::setprecision(2) << acc.available_cash() << "\n"
                      << "  * Liquidation Value  : $" << std::fixed << std::setprecision(2) << acc.liquidation_value << "\n"
                      << "  * Existing Positions : " << acc.positions.size() << "\n";
        }

        // Validate account hash for broker execution
        if (acc.account_hash.empty()) {
            if (config_.dry_run && !config_.preview_only) {
                acc.account_hash = "SIMULATED_ACCOUNT";
            } else {
                std::cerr << "  * [ERROR] Account " << acc.masked_account_number() << " missing encrypted hashValue. Skipping.\n";
                acc_res.success = false;
                acc_res.status_notes = "Missing hashValue";
                acc_res.execution_time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - acc_start);
                batch_summary.failed_accounts++;
                batch_summary.account_results.push_back(std::move(acc_res));
                continue;
            }
        }

        // Generate Strategy Session Orders
        std::vector<Order> raw_orders = strategy_->generate_orders(
            acc.positions,
            acc.available_cash(),
            last_market_quotes_
        );

        acc_res.orders_generated = raw_orders.size();
        std::cout << "  * Strategy Output    : " << raw_orders.size() << " orders emitted by strategy\n";

        if (raw_orders.empty()) {
            std::cout << "  * Status: [COMPLETED] Zero orders generated by strategy. Zero execution needed.\n";
            if (target_accounts.size() == 1) {
                std::cout << "================================================================================\n\n";
            }
            acc_res.success = true;
            acc_res.status_notes = "Zero orders needed";
            acc_res.execution_time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - acc_start);
            batch_summary.succeeded_accounts++;
            batch_summary.account_results.push_back(std::move(acc_res));
            continue;
        }

        // Real-Time Market Data Valuation (Fetch any new symbols needed)
        std::vector<Symbol> symbols_to_quote;
        for (const auto& ord : raw_orders) {
            if (!ord.symbol.is_valid()) continue;
            if (find_quote(last_market_quotes_, ord.symbol) == nullptr) {
                if (std::find(symbols_to_quote.begin(), symbols_to_quote.end(), ord.symbol) == symbols_to_quote.end()) {
                    symbols_to_quote.push_back(ord.symbol);
                }
            }
        }

        std::vector<std::string> deferred_diagnostics;
        deferred_diagnostics.reserve(raw_orders.size() + 4);

        if (!symbols_to_quote.empty() && access_token != "SIMULATED_ACCESS_TOKEN" && !access_token.empty()) {
            std::string quote_err;
            auto new_quotes = MarketDataManager::fetch_quotes_batch(
                http_client_,
                symbols_to_quote,
                access_token,
                &quote_err
            );
            if (!new_quotes.empty()) {
                deferred_diagnostics.push_back("  * Market Data Quotes : Fetched " + 
                    std::to_string(new_quotes.size()) + " real-time quote(s)");
                for (auto& q : new_quotes) {
                    last_market_quotes_.push_back(std::move(q));
                }
            } else if (!quote_err.empty()) {
                deferred_diagnostics.push_back("  * Market Data Notice : Quote fetch notice: " + 
                    quote_err + " (falling back to portfolio position marks)");
            }
        } else if (!raw_orders.empty()) {
            size_t quoted_count = 0;
            for (const auto& ord : raw_orders) {
                if (find_quote(last_market_quotes_, ord.symbol) != nullptr) {
                    quoted_count++;
                }
            }
            if (quoted_count > 0) {
                deferred_diagnostics.push_back("  * Market Data Quotes : Valued with " + 
                    std::to_string(quoted_count) + " real-time quote(s)");
            }
        }

        auto approved_orders = stage_and_validate_orders(
            acc,
            raw_orders,
            risk_guard,
            last_market_quotes_,
            config_.dry_run,
            deferred_diagnostics
        );

        if (approved_orders.empty()) {
            for (const auto& diag : deferred_diagnostics) {
                std::cerr << diag << "\n";
            }
            std::cerr << "  * Error: All strategy orders were blocked by RiskGuard.\n\n";
            acc_res.success = false;
            acc_res.status_notes = "Blocked by RiskGuard";
            acc_res.execution_time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - acc_start);
            batch_summary.failed_accounts++;
            batch_summary.account_results.push_back(std::move(acc_res));
            continue;
        }

        // Pre-warm HTTP transport and Dispatch Rebalance Plan
        if ((!config_.dry_run || config_.preview_only) && !http_client_.is_connected()) {
            http_client_.prewarm();
        }

        RebalancePlan plan;
        plan.orders = std::move(approved_orders);

        ExecutionDriverConfig driver_cfg;
        driver_cfg.account_hash = acc.account_hash;
        driver_cfg.dry_run = config_.dry_run && !config_.preview_only;
        driver_cfg.preview_only = config_.preview_only;
        driver_cfg.stop_on_auth_failure = true;
        driver_cfg.abort_buys_on_sell_failure = true;

        ExecutionDriver driver(http_client_, driver_cfg);
        ExecutionReport report = driver.execute_plan(plan, access_token);

        // Flush Deferred Diagnostics and Output Formatted Telemetry Table Post-Execution
        for (const auto& diag : deferred_diagnostics) {
            std::cout << diag << "\n";
        }

        std::string table_str = format_telemetry_table(report, config_.dry_run, config_.preview_only);
        std::cout << table_str;

        execution_history_.push_back(report);

        if (telemetry_callback_) {
            telemetry_callback_(report);
        }

        std::string recon_table_str;
        if (config_.track_fills && report.total_succeeded > 0 && !config_.preview_only) {
            if (!config_.dry_run) {
                std::cout << "  * Order Tracking     : Reconciling post-dispatch fills with Charles Schwab...\n";
                OrderTrackerConfig tracker_cfg;
                tracker_cfg.account_hash = acc.account_hash;
                tracker_cfg.polling_interval_ms = config_.fill_poll_interval_ms;
                tracker_cfg.timeout_ms = config_.fill_poll_timeout_ms;

                OrderTracker tracker(http_client_, tracker_cfg);
                BatchReconciliationReport recon = tracker.reconcile_report(report, access_token, last_market_quotes_);

                recon_table_str = format_reconciliation_table(recon, last_market_quotes_);
                std::cout << recon_table_str;
                last_reconciliation_report_ = recon;
            } else {
                BatchReconciliationReport sim_recon = generate_simulated_reconciliation(report, last_market_quotes_);
                recon_table_str = format_reconciliation_table(sim_recon, last_market_quotes_);
                std::cout << recon_table_str;
                last_reconciliation_report_ = sim_recon;
            }
        }

        if (!config_.dry_run && config_.telemetry_log_path.has_value()) {
            append_telemetry_file(
                *config_.telemetry_log_path,
                config_.session_name,
                strategy_->name(),
                acc.masked_account_number(),
                table_str,
                recon_table_str
            );
        }

        acc_res.orders_succeeded = report.total_succeeded;
        acc_res.orders_failed = report.total_failed;
        acc_res.success = report.all_succeeded;
        acc_res.status_notes = report.all_succeeded ? "All orders completed" : "Dispatch failures";
        acc_res.execution_time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - acc_start);

        if (report.all_succeeded) {
            batch_summary.succeeded_accounts++;
        } else {
            batch_summary.failed_accounts++;
        }
        batch_summary.total_orders_submitted += report.total_submitted;
        batch_summary.total_orders_succeeded += report.total_succeeded;
        batch_summary.total_orders_failed += report.total_failed;
        batch_summary.account_results.push_back(std::move(acc_res));
    }

    batch_summary.total_session_time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - session_start
    );
    last_batch_summary_ = batch_summary;

    if (target_accounts.size() > 1) {
        std::string summary_table = format_batch_summary_table(batch_summary);
        std::cout << summary_table;

        if (!config_.dry_run && config_.telemetry_log_path.has_value()) {
            std::ofstream log_file(*config_.telemetry_log_path, std::ios::app);
            if (log_file.is_open()) {
                log_file << summary_table << "\n";
            }
        }
    }

    return (batch_summary.failed_accounts == 0) ? 0 : 1;
}

} // namespace schwab
