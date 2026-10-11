#include "schwab/order_tracker.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace schwab {

OrderTracker::OrderTracker(HttpClient& http_client, OrderTrackerConfig config)
    : http_client_(http_client), config_(std::move(config)) {
    update_base_path();
    path_buf_.reserve(256);
    auth_header_buf_.reserve(2048);
}

void OrderTracker::set_config(const OrderTrackerConfig& config) {
    config_ = config;
    update_base_path();
}

void OrderTracker::update_base_path() {
    base_orders_path_ = "/trader/v1/accounts/";
    base_orders_path_.append(config_.account_hash);
    base_orders_path_.append("/orders/");
}

std::optional<OrderDetail> OrderTracker::query_order(
    std::string_view order_id,
    std::string_view access_token
) {
    if (order_id.empty() || access_token.empty() || config_.account_hash.empty()) {
        return std::nullopt;
    }

    path_buf_.assign(base_orders_path_);
    path_buf_.append(order_id);

    auth_header_buf_.assign("Bearer ");
    auth_header_buf_.append(access_token);

    HttpResponse resp = http_client_.get(path_buf_, auth_header_buf_);
    if (resp.status_code != 200) {
        return std::nullopt;
    }

    return OrderDetailParser::parse_order_detail(resp.body);
}

void OrderTracker::poll_batch_internal(
    std::span<OrderReconciliationResult> orders,
    std::string_view access_token,
    uint32_t timeout_ms
) {
    if (orders.empty() || access_token.empty()) {
        return;
    }

    // Flat contiguous array of indices for in-flight active orders
    std::vector<uint32_t> pending;
    pending.reserve(orders.size());

    for (uint32_t i = 0; i < static_cast<uint32_t>(orders.size()); ++i) {
        if (!orders[i].is_terminal && !orders[i].order_id.empty()) {
            pending.push_back(i);
        }
    }

    if (pending.empty()) {
        return;
    }

    const auto start_time = std::chrono::steady_clock::now();
    const uint32_t effective_timeout = (timeout_ms > 0) ? timeout_ms : config_.timeout_ms;

    while (!pending.empty()) {
        for (size_t p = 0; p < pending.size(); ) {
            const uint32_t idx = pending[p];
            auto& ord_res = orders[idx];

            auto detail_opt = query_order(ord_res.order_id, access_token);
            if (detail_opt) {
                ord_res.symbol = detail_opt->symbol;
                ord_res.side = detail_opt->side;
                ord_res.final_state = detail_opt->state;
                ord_res.ordered_quantity = detail_opt->quantity;
                ord_res.filled_quantity = detail_opt->filled_quantity;
                ord_res.remaining_quantity = detail_opt->remaining_quantity;
                ord_res.average_fill_price = detail_opt->average_fill_price;
                ord_res.is_terminal = detail_opt->is_terminal();
                ord_res.is_filled = detail_opt->is_filled();

                if (ord_res.is_terminal) {
                    ord_res.total_poll_time = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start_time
                    );
                    // O(1) swap-and-pop removal from flat pending index list
                    pending[p] = pending.back();
                    pending.pop_back();
                    continue; // Re-evaluate slot p with the newly swapped element
                }
            }
            ++p;
        }

        if (pending.empty()) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count()
        );

        if (elapsed_ms >= effective_timeout) {
            const auto total_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_time);
            for (uint32_t idx : pending) {
                orders[idx].is_terminal = false;
                orders[idx].error_message = "Polling timed out before order reached terminal state";
                orders[idx].total_poll_time = total_elapsed;
            }
            break;
        }

        if (config_.polling_interval_ms > 0) {
            const uint32_t remaining_ms = effective_timeout - elapsed_ms;
            const uint32_t sleep_ms = std::min(config_.polling_interval_ms, remaining_ms);
            if (sleep_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            }
        }
    }
}

OrderReconciliationResult OrderTracker::poll_order(
    std::string_view order_id,
    std::string_view access_token,
    uint32_t custom_timeout_ms
) {
    OrderReconciliationResult result;
    result.order_id = std::string(order_id);

    if (order_id.empty()) {
        result.error_message = "Order ID is empty";
        return result;
    }
    if (access_token.empty()) {
        result.error_message = "Access token is empty";
        return result;
    }

    std::array<OrderReconciliationResult, 1> single_order{result};
    poll_batch_internal(std::span(single_order), access_token, custom_timeout_ms);
    return single_order[0];
}

void OrderTracker::compute_batch_summary(
    BatchReconciliationReport& report,
    std::chrono::microseconds elapsed_time,
    std::span<const Quote> quotes
) noexcept {
    report.total_tracked = report.orders.size();
    report.total_filled = 0;
    report.total_cancelled = 0;
    report.total_rejected = 0;
    report.total_timed_out = 0;
    report.average_slippage_bps = 0.0;
    report.all_terminal = true;
    report.all_filled = true;
    report.elapsed_time = elapsed_time;

    double total_slippage_bps = 0.0;
    size_t slippage_count = 0;

    for (auto& ord_res : report.orders) {
        if (ord_res.is_filled) {
            report.total_filled++;

            // If pre-trade quotes provided, compute slippage against arrival/reference price
            if (!quotes.empty() && ord_res.average_fill_price > 0.0 && !ord_res.slippage.has_slippage) {
                for (const auto& q : quotes) {
                    if (q.symbol == ord_res.symbol && q.is_valid()) {
                        double ref_price = (ord_res.side == OrderSide::Buy) ? q.ask_price : q.bid_price;
                        if (ref_price <= 0.0) ref_price = q.reference_price();
                        ord_res.slippage = calculate_slippage(ord_res.side, ref_price, ord_res.average_fill_price);
                        break;
                    }
                }
            }

            if (ord_res.slippage.has_slippage) {
                total_slippage_bps += ord_res.slippage.slippage_bps;
                slippage_count++;
            }
        } else {
            report.all_filled = false;
        }

        if (ord_res.final_state == OrderExecutionState::Cancelled) {
            report.total_cancelled++;
        } else if (ord_res.final_state == OrderExecutionState::Rejected) {
            report.total_rejected++;
        }

        if (!ord_res.is_terminal) {
            report.total_timed_out++;
            report.all_terminal = false;
        }
    }

    if (slippage_count > 0) {
        report.average_slippage_bps = total_slippage_bps / static_cast<double>(slippage_count);
    }
}

BatchReconciliationReport OrderTracker::reconcile_orders(
    std::span<const std::string> order_ids,
    std::string_view access_token,
    std::span<const Quote> quotes
) {
    BatchReconciliationReport report;
    if (order_ids.empty()) {
        return report;
    }

    report.orders.resize(order_ids.size());
    for (size_t i = 0; i < order_ids.size(); ++i) {
        report.orders[i].order_id = order_ids[i];
    }

    const auto start_time = std::chrono::steady_clock::now();
    poll_batch_internal(std::span(report.orders), access_token, config_.timeout_ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start_time
    );

    compute_batch_summary(report, elapsed, quotes);
    return report;
}

BatchReconciliationReport OrderTracker::reconcile_report(
    const ExecutionReport& report,
    std::string_view access_token,
    std::span<const Quote> quotes
) {
    BatchReconciliationReport final_report;
    if (report.results.empty()) {
        return final_report;
    }

    final_report.orders.resize(report.results.size());
    for (size_t i = 0; i < report.results.size(); ++i) {
        const auto& res = report.results[i];
        auto& ord_res = final_report.orders[i];

        ord_res.symbol = res.order.symbol;
        ord_res.side = res.order.side;
        ord_res.ordered_quantity = res.order.quantity;

        if (res.order_id.has_value() && !res.order_id->empty()) {
            ord_res.order_id = *res.order_id;
            ord_res.is_terminal = false;
        } else {
            // Unsubmitted order (rejected/failed submission)
            ord_res.order_id.clear();
            ord_res.final_state = OrderExecutionState::Rejected;
            ord_res.is_terminal = true;
            ord_res.is_filled = false;
            ord_res.error_message = res.error_message.empty() ? "Order submission failed" : res.error_message;
            ord_res.total_poll_time = std::chrono::microseconds{0};
        }
    }

    const auto start_time = std::chrono::steady_clock::now();
    poll_batch_internal(std::span(final_report.orders), access_token, config_.timeout_ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start_time
    );

    compute_batch_summary(final_report, elapsed, quotes);
    return final_report;
}

} // namespace schwab
