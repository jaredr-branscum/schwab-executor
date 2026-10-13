#include "schwab/execution_driver.hpp"
#include "schwab/order_serializer.hpp"

#include <chrono>
#include <string_view>

namespace schwab {

using namespace std::string_view_literals;

ExecutionDriver::ExecutionDriver(HttpClient& http_client, ExecutionDriverConfig config)
    : http_client_(http_client), config_(std::move(config)) {
    update_endpoint_path();
    payload_buf_.reserve(512);
    auth_header_buf_.reserve(512);
}

void ExecutionDriver::set_account_hash(std::string_view hash) {
    while (!hash.empty() && (hash.front() == ' ' || hash.front() == '\t')) {
        hash.remove_prefix(1);
    }
    while (!hash.empty() && (hash.back() == ' ' || hash.back() == '\t')) {
        hash.remove_suffix(1);
    }
    config_.account_hash = std::string(hash);
    update_endpoint_path();
}

void ExecutionDriver::update_endpoint_path() {
    endpoint_path_.clear();
    endpoint_path_.reserve(128);
    endpoint_path_.append("/trader/v1/accounts/"sv);
    endpoint_path_.append(config_.account_hash);
    endpoint_path_.append(config_.preview_only ? "/previewOrder"sv : "/orders"sv);
}

OrderExecutionResult ExecutionDriver::execute_single_order(
    const Order& order,
    std::string_view access_token
) {
    const auto t_start = std::chrono::steady_clock::now();
    OrderExecutionResult result;
    result.order = order;

    if (!order.is_valid()) [[unlikely]] {
        result.success = false;
        result.order.status = OrderStatus::Rejected;
        result.http_status = 400;
        result.error_message = "Order fails structural validation";
        result.engine_latency = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_start);
        return result;
    }

    if (config_.account_hash.empty()) [[unlikely]] {
        result.success = false;
        result.order.status = OrderStatus::Rejected;
        result.http_status = 400;
        result.error_message = "Account hash is empty";
        result.engine_latency = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_start);
        return result;
    }

    if (access_token.empty()) [[unlikely]] {
        result.success = false;
        result.order.status = OrderStatus::Rejected;
        result.http_status = 401;
        result.error_message = "Access token is empty";
        result.engine_latency = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_start);
        return result;
    }

    // Serialize order into pre-allocated scratch buffer
    if (!OrderSerializer::serialize_order(order, payload_buf_)) [[unlikely]] {
        result.success = false;
        result.order.status = OrderStatus::Rejected;
        result.http_status = 400;
        result.error_message = "Failed to serialize order into wire JSON";
        result.engine_latency = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_start);
        return result;
    }

    // Dry-Run Simulation: bypass network dispatch
    if (config_.dry_run) {
        result.success = true;
        result.order.status = OrderStatus::PendingSubmit;
        result.http_status = 200;
        result.order_id = "DRY_RUN_SIMULATED";
        result.latency = std::chrono::microseconds{0};
        result.engine_latency = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_start);
        return result;
    }

    // Format pre-baked Authorization header
    auth_header_buf_.clear();
    auth_header_buf_.append("Bearer "sv);
    auth_header_buf_.append(access_token);

    const auto t_net_start = std::chrono::steady_clock::now();

    // Network Dispatch
    HttpResponse resp = http_client_.post(endpoint_path_, payload_buf_, auth_header_buf_);
    result.http_status = resp.status_code;
    result.latency = resp.latency;

    const auto t_net_end = std::chrono::steady_clock::now();

    // Schwab returns HTTP 201 Created on successful order placement, or HTTP 200 OK on previewOrder
    const bool is_accepted = (config_.preview_only && resp.status_code == 200) || (resp.status_code == 201);
    if (is_accepted) {
        result.success = true;
        result.order.status = config_.preview_only ? OrderStatus::New : OrderStatus::PendingSubmit;
        result.order_id = config_.preview_only ? "PREVIEW_VALIDATED" : OrderSerializer::extract_order_id(resp);
    } else {
        result.success = false;
        result.order.status = OrderStatus::Rejected;
        result.error_message = OrderSerializer::parse_error_message(resp);
    }

    const auto t_done = std::chrono::steady_clock::now();
    result.engine_latency = std::chrono::duration_cast<std::chrono::microseconds>((t_net_start - t_start) + (t_done - t_net_end));

    return result;
}

ExecutionReport ExecutionDriver::execute_plan(
    const RebalancePlan& plan,
    std::string_view access_token
) {
    ExecutionReport report;
    auto start_time = std::chrono::steady_clock::now();

    // Guard: return if plan has no orders
    if (plan.orders.empty()) {
        return report;
    }

    // Pre-flight check: account hash
    if (config_.account_hash.empty()) [[unlikely]] {
        report.all_succeeded = false;
        report.total_failed = plan.orders.size();
        report.total_submitted = plan.orders.size();
        report.results.reserve(plan.orders.size());
        for (const auto& ord : plan.orders) {
            OrderExecutionResult res;
            res.order = ord;
            res.order.status = OrderStatus::Rejected;
            res.http_status = 400;
            res.error_message = "Account hash is empty";
            report.results.push_back(std::move(res));
        }
        return report;
    }

    // Pre-flight check: access token
    if (access_token.empty()) [[unlikely]] {
        report.all_succeeded = false;
        report.total_failed = plan.orders.size();
        report.total_submitted = plan.orders.size();
        report.results.reserve(plan.orders.size());
        for (const auto& ord : plan.orders) {
            OrderExecutionResult res;
            res.order = ord;
            res.order.status = OrderStatus::Rejected;
            res.http_status = 401;
            res.error_message = "Access token is empty";
            report.results.push_back(std::move(res));
        }
        return report;
    }

    // Pre-warm connection if not hot and not in dry-run mode
    if (!config_.dry_run && !http_client_.is_connected()) {
        http_client_.prewarm();
    }

    // Pre-format authorization header once for entire batch
    auth_header_buf_.clear();
    auth_header_buf_.append("Bearer "sv);
    auth_header_buf_.append(access_token);

    report.results.reserve(plan.orders.size());

    bool has_sell_failed = false;

    for (const auto& order : plan.orders) {
        if (order.side == OrderSide::Buy && config_.abort_buys_on_sell_failure && has_sell_failed) [[unlikely]] {
            OrderExecutionResult res;
            res.order = order;
            res.order.status = OrderStatus::Rejected;
            res.success = false;
            res.http_status = 0;
            res.error_message = "Buy order aborted: preceding sell order submission failed at broker (atomic abort)";
            ++report.total_failed;
            ++report.total_submitted;
            report.all_succeeded = false;
            report.results.push_back(std::move(res));
            continue;
        }

        OrderExecutionResult res = execute_single_order(order, access_token);
        if (res.success) {
            ++report.total_succeeded;
        } else {
            ++report.total_failed;
            report.all_succeeded = false;
            if (order.side == OrderSide::Sell) {
                has_sell_failed = true;
            }
        }
        ++report.total_submitted;
        const int status = res.http_status;
        report.results.push_back(std::move(res));

        // Circuit breaker: halt remaining submissions if credentials are unauthorized/forbidden
        if (config_.stop_on_auth_failure && (status == 401 || status == 403)) {
            break;
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    report.total_execution_time = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

    return report;
}

} // namespace schwab
