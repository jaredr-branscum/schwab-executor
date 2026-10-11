#pragma once

#include "schwab/account_manager.hpp"
#include "schwab/clock.hpp"
#include "schwab/config.hpp"
#include "schwab/execution_driver.hpp"
#include "schwab/http_client.hpp"
#include "schwab/order.hpp"
#include "schwab/risk_guard.hpp"
#include "schwab/strategy.hpp"
#include "schwab/token_manager.hpp"

#include <atomic>
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
 * @brief Configuration parameters for execution runner and persistent trading daemon.
 */
struct DaemonConfig {
    bool dry_run{false};                             ///< Safety simulation mode (defaults to .env, overridden by --dry-run)
    std::vector<std::string> scheduled_times{};      ///< Optional discrete bell times (HH:MM:SS format, UTC)
    std::optional<std::chrono::milliseconds> intraday_interval{std::nullopt}; ///< Optional continuous intraday polling interval
    std::filesystem::path config_path{".env"};
    std::filesystem::path token_path{".schwab_token.json"};
    std::string account_hash_override{};
    std::chrono::seconds pre_warm_lead_time{30};     ///< Connect TLS socket 30s before target bell
    std::chrono::milliseconds spin_threshold{20};   ///< Switch to CPU spin-wait 20ms before bell
    std::chrono::seconds token_refresh_interval{1200}; ///< Proactively refresh OAuth token every 20 min
    std::string session_name{"EXECUTE"};             ///< Mode label for telemetry logging
    OrderType default_order_type{OrderType::Market};
    std::optional<std::filesystem::path> telemetry_log_path{std::nullopt}; ///< Optional path to append structured execution reports
};

/**
 * @brief Execution runner supporting immediate execution and persistent daemon loops.
 *
 * Execution Cadences Supported:
 * - Immediate Execution (`run_immediate`): Evaluates strategy against live portfolio state and executes orders immediately.
 * - Timed Orders (`run_daemon`): Execute orders at discrete market times with TLS pre-warming.
 * - Intraday Trading (`run_daemon` with `intraday_interval`): Continuous interval evaluation loop during market hours.
 * - Hybrid Execution: Continuous intraday signal monitoring combined with discrete market timed orders.
 * - Proactive Token Lifecycle: Automatic background token refresh so long-running sessions never expire.
 */
class DaemonRunner {
public:
    explicit DaemonRunner(DaemonConfig config, std::shared_ptr<IStrategy> strategy = nullptr) noexcept;

    /**
     * @brief Evaluates portfolio state and executes strategy orders immediately.
     *
     * @param simulated_positions Optional mock positions for offline dry-run testing.
     * @param simulated_cash Optional mock cash for offline dry-run testing.
     * @return Process exit code (0 on success, non-zero on error).
     */
    int run_immediate(
        std::span<const Position> simulated_positions = {},
        std::optional<double> simulated_cash = std::nullopt
    );

    /**
     * @brief Runs the persistent trading daemon across the configured market schedule.
     *
     * Monitors scheduled bell times, proactively auto-refreshes OAuth tokens in background,
     * executes continuous intraday polling when configured, and handles graceful shutdown.
     *
     * @param stop_signal Optional external atomic flag to request shutdown.
     * @param max_cycles Optional maximum cycles for deterministic unit testing.
     * @return Process exit code (0 on clean shutdown).
     */
    int run_daemon(
        std::atomic<bool>* stop_signal = nullptr,
        std::optional<size_t> max_cycles = std::nullopt
    );

    /**
     * @brief Formats an execution report as an ASCII telemetry table.
     */
    [[nodiscard]] static std::string format_telemetry_table(
        const ExecutionReport& report,
        bool is_dry_run
    );

    [[nodiscard]] const DaemonConfig& config() const noexcept {
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

    [[nodiscard]] HttpClient& http_client() noexcept {
        return http_client_;
    }

private:
    DaemonConfig config_;
    std::shared_ptr<IStrategy> strategy_;
    HttpClient http_client_{};
    TelemetryCallback telemetry_callback_{nullptr};
    std::vector<ExecutionReport> execution_history_{};
};

} // namespace schwab
