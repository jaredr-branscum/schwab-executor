#pragma once

#include "schwab/http_client.hpp"
#include "schwab/order.hpp"
#include "schwab/rebalancer.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace schwab {

/**
 * @brief Outcome of single order submission attempt.
 */
struct OrderExecutionResult {
    Order order{};
    int http_status{0};
    bool success{false};
    std::optional<std::string> order_id{std::nullopt};
    std::string error_message{};
    std::chrono::microseconds latency{0};        // Network HTTP round-trip latency
    std::chrono::microseconds engine_latency{0}; // Internal engine preparation & processing latency
};

/**
 * @brief Execution batch summary report.
 */
struct ExecutionReport {
    std::vector<OrderExecutionResult> results;
    size_t total_submitted{0};
    size_t total_succeeded{0};
    size_t total_failed{0};
    std::chrono::microseconds total_execution_time{0};
    bool all_succeeded{true};
};

/**
 * @brief Configuration parameters for execution driver.
 */
struct ExecutionDriverConfig {
    std::string account_hash;              // Schwab encrypted account hash
    bool dry_run{false};                   // If true, validates and serializes without network I/O
    bool preview_only{false};             // If true, validates orders via /previewOrder without live placement
    bool stop_on_auth_failure{true};       // If true, halts batch execution immediately if 401/403 is encountered
    bool abort_buys_on_sell_failure{true}; // If true, halts execution before dispatching buys if any sell submission failed
};

/**
 * @brief High-speed execution driver for Charles Schwab REST API order submission.
 *
 * Coordinates HttpClient, OrderSerializer, and RebalancePlan to execute sequenced
 * orders against Schwab /trader/v1/accounts/{accountHash}/orders endpoint with
 * pre-allocated scratch buffers and result logging.
 */
class ExecutionDriver {
public:
    ExecutionDriver(HttpClient& http_client, ExecutionDriverConfig config);

    /**
     * @brief Executes all approved orders in RebalancePlan.
     *
     * Orders are submitted sequentially (Sells first to unlock capital, then Buys).
     *
     * @param plan RebalancePlan containing sequenced, risk-approved orders.
     * @param access_token Valid OAuth Bearer access token.
     * @return ExecutionReport summarizing submission outcomes.
     */
    [[nodiscard]] ExecutionReport execute_plan(
        const RebalancePlan& plan,
        std::string_view access_token
    );

    /**
     * @brief Submits single order to Schwab account endpoint.
     *
     * @param order Validated Order struct.
     * @param access_token Valid OAuth Bearer access token.
     * @return OrderExecutionResult.
     */
    [[nodiscard]] OrderExecutionResult execute_single_order(
        const Order& order,
        std::string_view access_token
    );

    [[nodiscard]] const ExecutionDriverConfig& config() const noexcept { return config_; }
    void set_dry_run(bool dry_run) noexcept { config_.dry_run = dry_run; }
    void set_account_hash(std::string_view hash);

    /**
     * @brief Pre-warms scratch buffers, floating-point lookup tables, and memory pages.
     */
    void prewarm() noexcept;

private:
    void update_endpoint_path();

    HttpClient& http_client_;
    ExecutionDriverConfig config_;

    // Pre-allocated scratch buffers
    std::string endpoint_path_;
    std::string auth_header_buf_;
    std::string payload_buf_;
};

} // namespace schwab
