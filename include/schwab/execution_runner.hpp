#pragma once

#include "schwab/account_manager.hpp"
#include "schwab/config.hpp"
#include "schwab/execution_driver.hpp"
#include "schwab/http_client.hpp"
#include "schwab/order.hpp"
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
    std::filesystem::path config_path{".env"};
    std::filesystem::path token_path{".schwab_token.json"};
    std::string account_hash_override{};
    std::string session_name{"LIVE"};             ///< Mode label for telemetry logging
    OrderType default_order_type{OrderType::Market};
    std::optional<std::filesystem::path> telemetry_log_path{std::nullopt}; ///< Optional path to append structured execution reports (live execution only)
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
        bool is_dry_run
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

    [[nodiscard]] HttpClient& http_client() noexcept {
        return http_client_;
    }

private:
    ExecutionConfig config_;
    std::shared_ptr<IStrategy> strategy_;
    HttpClient http_client_{};
    TelemetryCallback telemetry_callback_{nullptr};
    std::vector<ExecutionReport> execution_history_{};
};

} // namespace schwab
