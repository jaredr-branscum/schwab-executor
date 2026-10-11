#pragma once

#include "schwab/http_client.hpp"
#include "schwab/order_detail.hpp"
#include "schwab/execution_driver.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace schwab {

/**
 * @brief Configuration parameters for OrderTracker polling.
 */
struct OrderTrackerConfig {
    std::string account_hash;          ///< Schwab encrypted account hash
    std::string base_url{"https://api.schwabapi.com"}; ///< Base REST API URL
    uint32_t polling_interval_ms{100}; ///< Sleep duration between status polling iterations (default 100ms)
    uint32_t timeout_ms{5000};         ///< Maximum timeout to wait for orders to reach terminal state (default 5000ms = 5s)
};

/**
 * @brief Outcome of tracking an individual order to its terminal state.
 *
 * Fields packed with scalar primitives first to fit primary execution metrics
 * within single 64-byte L1 cache line.
 */
struct OrderReconciliationResult {
    double ordered_quantity{0.0};
    double filled_quantity{0.0};
    double remaining_quantity{0.0};
    double average_fill_price{0.0};
    std::chrono::microseconds total_poll_time{0};
    Symbol symbol{};
    OrderSide side{OrderSide::Buy};
    OrderExecutionState final_state{OrderExecutionState::Unknown};
    bool is_terminal{false};
    bool is_filled{false};

    std::string order_id{};
    std::string error_message{};
};

/**
 * @brief Batch reconciliation report summarizing all in-flight orders after post-dispatch tracking.
 */
struct BatchReconciliationReport {
    std::vector<OrderReconciliationResult> orders;
    size_t total_tracked{0};
    size_t total_filled{0};
    size_t total_cancelled{0};
    size_t total_rejected{0};
    size_t total_timed_out{0};
    bool all_terminal{true};
    bool all_filled{true};
    std::chrono::microseconds elapsed_time{0};
};

/**
 * @brief Post-dispatch order tracking and fill reconciliation engine.
 *
 * Decouples post-trade fill verification from hot-path order submission loop,
 * ensuring high-speed wire dispatch is never stalled by exchange matching latency.
 *
 * Low-latency optimizations:
 * - Pre-baked base REST path and pre-allocated query/auth buffers (zero heap allocations during polling).
 * - Flat contiguous index-mapped batch tracking (zero node allocations, no unordered_map pointer chasing).
 * - O(1) swap-and-pop removal of fulfilled orders during batch ticks.
 */
class OrderTracker {
public:
    OrderTracker(HttpClient& http_client, OrderTrackerConfig config);

    /**
     * @brief Queries single order status by order ID (one-shot query).
     * GET /trader/v1/accounts/{accountHash}/orders/{orderId}
     *
     * @param order_id Schwab numeric order ID.
     * @param access_token Active OAuth access token.
     * @return Parsed OrderDetail if HTTP 200, std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<OrderDetail> query_order(
        std::string_view order_id,
        std::string_view access_token
    );

    /**
     * @brief Monotonically polls single order until it reaches a terminal state or timeout expires.
     *
     * @param order_id Schwab numeric order ID.
     * @param access_token Active OAuth access token.
     * @param custom_timeout_ms Optional custom timeout in milliseconds (0 uses config_.timeout_ms).
     * @return Final OrderReconciliationResult.
     */
    [[nodiscard]] OrderReconciliationResult poll_order(
        std::string_view order_id,
        std::string_view access_token,
        uint32_t custom_timeout_ms = 0
    );

    /**
     * @brief Reconciles all in-flight orders from an ExecutionReport post-dispatch.
     *
     * Preserves strict 1:1 order sequence with ExecutionReport without hash maps,
     * polling active orders concurrently and recording unsubmitted orders as rejected.
     *
     * @param report ExecutionReport produced by ExecutionDriver.
     * @param access_token Active OAuth access token.
     * @return BatchReconciliationReport detailing verified fills and terminal states.
     */
    [[nodiscard]] BatchReconciliationReport reconcile_report(
        const ExecutionReport& report,
        std::string_view access_token
    );

    /**
     * @brief Reconciles list of order IDs post-dispatch using flat round-robin polling.
     *
     * @param order_ids Collection of Schwab order IDs to reconcile.
     * @param access_token Active OAuth access token.
     * @return BatchReconciliationReport detailing verified fills and terminal states.
     */
    [[nodiscard]] BatchReconciliationReport reconcile_orders(
        std::span<const std::string> order_ids,
        std::string_view access_token
    );

    [[nodiscard]] const OrderTrackerConfig& config() const noexcept { return config_; }
    void set_config(const OrderTrackerConfig& config);

private:
    void update_base_path();

    void poll_batch_internal(
        std::span<OrderReconciliationResult> orders,
        std::string_view access_token,
        uint32_t timeout_ms
    );

    static void compute_batch_summary(
        BatchReconciliationReport& report,
        std::chrono::microseconds elapsed_time
    ) noexcept;

    HttpClient& http_client_;
    OrderTrackerConfig config_;

    // Pre-baked base path prefix and reusable scratch buffers
    std::string base_orders_path_;
    std::string path_buf_;
    std::string auth_header_buf_;
};

} // namespace schwab
