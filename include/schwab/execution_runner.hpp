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
    std::string session_name{"LIVE"};             ///< Mode label for telemetry logging
    OrderType default_order_type{OrderType::Market};
    std::optional<std::filesystem::path> telemetry_log_path{std::nullopt}; ///< Optional path to append structured execution reports (live execution only)
    bool track_fills{false};                        ///< If true, polls orders to terminal state post-dispatch (live only)
    uint32_t fill_poll_interval_ms{100};            ///< Polling interval in ms for fill reconciliation
    uint32_t fill_poll_timeout_ms{5000};            ///< Maximum timeout in ms for fill reconciliation
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
     * @return Process exit code (0 on success, non-zero on error).
     */
    int execute(
        std::span<const Position> simulated_positions = {},
        std::optional<double> simulated_cash = std::nullopt
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

    [[nodiscard]] const ExecutionConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] const IStrategy& strategy() const noexcept {
        return *strategy_;
    }

    using TelemetryCallback = std::function<void(const ExecutionReport&)>;

    void set_telemetry_callback(TelemetryCallback cb) noexcept {
        telemetry_callback_ = std::move(cb);
    }

    [[nodiscard]] std::span<const ExecutionReport> execution_history() const noexcept {
        return execution_history_;
    }

    [[nodiscard]] std::span<const Quote> last_market_quotes() const noexcept {
        return last_market_quotes_;
    }

    [[nodiscard]] const std::optional<BatchReconciliationReport>& last_reconciliation_report() const noexcept {
        return last_reconciliation_report_;
    }

    [[nodiscard]] HttpClient& http_client() noexcept {
        return http_client_;
    }

private:
    ExecutionConfig config_;
    std::shared_ptr<IStrategy> strategy_;
    HttpClient http_client_{};
    TelemetryCallback telemetry_callback_{nullptr};
    std::vector<ExecutionReport> execution_history_{};
    std::vector<Quote> last_market_quotes_{};
    std::optional<BatchReconciliationReport> last_reconciliation_report_{std::nullopt};
};

} // namespace schwab
