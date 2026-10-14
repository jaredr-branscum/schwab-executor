#pragma once

#include "schwab/account_manager.hpp"
#include "schwab/config.hpp"
#include "schwab/execution_driver.hpp"
#include "schwab/http_client.hpp"
#include "schwab/market_data.hpp"
#include "schwab/order.hpp"
#include "schwab/order_tracker.hpp"
#include "schwab/risk_guard.hpp"
#include "schwab/strategy.hpp"
#include "schwab/token_manager.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace schwab {

/**
 * @brief Configuration parameters for execution runner.
 */
struct ExecutionConfig {
    bool dry_run{true};                             ///< Safety simulation mode (defaults to .env, overridden by --dry-run)
    bool preview_only{false};                       ///< Broker-side validation mode via /previewOrder (no execution)
    std::filesystem::path config_path{".env"};
    std::filesystem::path token_path{".schwab_token.json"};
    std::string account_hash_override{};
    std::string accounts_filter{};                  ///< Filter for accounts: "all", comma-separated numbers/hashes (defaults to SCHWAB_ACCOUNTS in .env)
    std::string session_name{"LIVE"};             ///< Mode label for telemetry logging
    OrderType default_order_type{OrderType::Market};
    std::optional<std::filesystem::path> telemetry_log_path{std::nullopt}; ///< Optional path to append structured execution reports (live execution only)
    bool track_fills{false};                        ///< If true, polls orders to terminal state post-dispatch (live only)
    uint32_t fill_poll_interval_ms{100};            ///< Polling interval in ms for fill reconciliation
    uint32_t fill_poll_timeout_ms{5000};            ///< Maximum timeout in ms for fill reconciliation
    bool enforce_cash_limits{true};                 ///< Enforce aggregate buy commitments <= available cash + sell proceeds
    bool allow_margin_borrowing{false};             ///< If false, blocks buy orders that exceed cash to prevent unintended margin debt
    bool enable_concurrent_dispatch{true};          ///< Enable pre-warmed concurrent worker dispatch for multi-account portfolios
    size_t max_concurrent_workers{16};              ///< Maximum concurrent worker threads (bounds simultaneous threads)
    bool auto_fetch_quotes{true};                   ///< If false, skips automatic synchronous quote pre-fetching
};

/**
 * @brief Telemetry metrics for single account execution in a batch session.
 */
struct AccountSessionResult {
    std::string account_number{};
    std::string masked_account_number{};
    std::string account_hash{};
    std::string account_type{};
    double cash_balance{0.0};
    double liquidation_value{0.0};
    size_t positions_count{0};
    size_t orders_generated{0};
    size_t orders_succeeded{0};
    size_t orders_failed{0};
    bool success{true};
    std::string status_notes{};
    std::chrono::microseconds execution_time{0};
};

/**
 * @brief Multi-account batch execution session aggregate summary.
 */
struct BatchSessionSummary {
    std::vector<AccountSessionResult> account_results{};
    size_t total_accounts{0};
    size_t succeeded_accounts{0};
    size_t failed_accounts{0};
    size_t total_orders_submitted{0};
    size_t total_orders_succeeded{0};
    size_t total_orders_failed{0};
    std::chrono::microseconds total_session_time{0};
};

/**
 * @brief Single-pass execution runner.
 *
 * Evaluates strategy against portfolio state, passes orders through RiskGuard,
 * sequences orders (sells first, then buys), dispatches over WinHTTP, and outputs telemetry.
 */
class ExecutionRunner {
public:
    explicit ExecutionRunner(ExecutionConfig config, std::shared_ptr<IStrategy> strategy = nullptr) noexcept;

    /**
     * @brief Evaluates portfolio state and executes strategy orders immediately.
     *
     * @param simulated_positions Optional mock positions for offline dry-run testing.
     * @param simulated_cash Optional mock cash for offline dry-run testing.
     * @param simulated_accounts Optional mock multi-account portfolios for offline batch testing.
     * @return Process exit code (0 on success, non-zero on error).
     */
    int execute(
        std::span<const Position> simulated_positions = {},
        std::optional<double> simulated_cash = std::nullopt,
        std::span<const AccountPortfolio> simulated_accounts = {}
    );

    /**
     * @brief Formats an execution report as an ASCII telemetry table.
     */
    [[nodiscard]] static std::string format_telemetry_table(
        const ExecutionReport& report,
        bool is_dry_run,
        bool is_preview = false
    );

    /**
     * @brief Formats post-trade fill verification and execution slippage as an ASCII table.
     */
    [[nodiscard]] static std::string format_reconciliation_table(
        const BatchReconciliationReport& report,
        std::span<const Quote> quotes = {}
    );

    /**
     * @brief Formats an aggregate multi-account batch session summary as an ASCII table.
     */
    [[nodiscard]] static std::string format_batch_summary_table(
        const BatchSessionSummary& summary
    );

    [[nodiscard]] const ExecutionConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] const IStrategy& strategy() const noexcept {
        return *strategy_;
    }

    using TelemetryCallback = std::function<void(const ExecutionReport&)>;
    using TransportFactory = std::function<std::unique_ptr<ITransport>()>;

    void set_telemetry_callback(TelemetryCallback cb) noexcept {
        telemetry_callback_ = std::move(cb);
    }

    void set_transport_factory(TransportFactory factory) noexcept {
        transport_factory_ = std::move(factory);
    }

    [[nodiscard]] const TransportFactory& transport_factory() const noexcept {
        return transport_factory_;
    }

    [[nodiscard]] std::span<const ExecutionReport> execution_history() const noexcept {
        return execution_history_;
    }

    [[nodiscard]] std::span<const Quote> last_market_quotes() const noexcept {
        return last_market_quotes_;
    }

    /**
     * @brief Pre-seeds market data quotes to eliminate synchronous quote fetching on the hot path.
     */
    void set_market_quotes(std::vector<Quote> quotes) noexcept {
        last_market_quotes_ = std::move(quotes);
        quotes_pre_seeded_ = true;
    }

    [[nodiscard]] const std::optional<BatchReconciliationReport>& last_reconciliation_report() const noexcept {
        return last_reconciliation_report_;
    }

    [[nodiscard]] const std::optional<BatchSessionSummary>& last_batch_summary() const noexcept {
        return last_batch_summary_;
    }

    [[nodiscard]] HttpClient& http_client() noexcept {
        return http_client_;
    }

private:
    ExecutionConfig config_;
    std::shared_ptr<IStrategy> strategy_;
    HttpClient http_client_{};
    TransportFactory transport_factory_{nullptr};
    TelemetryCallback telemetry_callback_{nullptr};
    std::vector<ExecutionReport> execution_history_{};
    std::vector<Quote> last_market_quotes_{};
    bool quotes_pre_seeded_{false};
    std::optional<BatchReconciliationReport> last_reconciliation_report_{std::nullopt};
    std::optional<BatchSessionSummary> last_batch_summary_{std::nullopt};
};

} // namespace schwab
