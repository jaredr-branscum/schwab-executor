#pragma once

#include "schwab/order.hpp"
#include "schwab/http_types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace schwab {

/**
 * @brief Serializer for Charles Schwab REST API order requests and responses.
 *
 * Implements zero-allocation serialization for order execution hot paths
 * and parsing for Schwab order IDs and error diagnostics.
 */
class OrderSerializer {
public:
    /**
     * @brief Serializes Order into Charles Schwab REST API JSON wire payload.
     * Formats into pre-reserved string buffer.
     *
     * @param order Internal validated Order struct.
     * @param out_buffer Pre-allocated string to receive formatted JSON.
     * @return true if serialization succeeded, false if order was invalid.
     */
    static bool serialize_order(const Order& order, std::string& out_buffer);

    /**
     * @brief Convenience overload allocating and returning new JSON string.
     *
     * @param order Internal validated Order struct.
     * @return Formatted JSON string, or empty string if invalid.
     */
    [[nodiscard]] static std::string serialize_order(const Order& order);

    /**
     * @brief Extracts Schwab numeric order ID from HTTP 201 response.
     * Schwab returns new order URL in "Location" header:
     * e.g., ".../accounts/{hash}/orders/12345678" -> "12345678".
     *
     * @param response HTTP response received from order submission.
     * @return Order ID string if found, std::nullopt otherwise.
     */
    [[nodiscard]] static std::optional<std::string> extract_order_id(const HttpResponse& response);

    /**
     * @brief Extracts diagnostic error reason from failed (4xx/5xx) order response.
     * Parses standard Schwab error fields ("error", "message", "errors").
     *
     * @param response Failed HTTP response.
     * @return Human-readable error description, or status text if unparseable.
     */
    [[nodiscard]] static std::string parse_error_message(const HttpResponse& response);
};

} // namespace schwab
