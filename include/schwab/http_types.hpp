#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace schwab {

/**
 * @brief HTTP methods stored in a single byte (uint8_t).
 */
enum class HttpMethod : uint8_t {
    Get,
    Post,
    Put,
    Delete,
    Patch,
    Head,
    Options
};

/**
 * @brief Converts HttpMethod to its uppercase string_view representation ("GET", "POST", etc.).
 */
[[nodiscard]] constexpr std::string_view to_string_view(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::Get:     return "GET";
        case HttpMethod::Post:    return "POST";
        case HttpMethod::Put:     return "PUT";
        case HttpMethod::Delete:  return "DELETE";
        case HttpMethod::Patch:   return "PATCH";
        case HttpMethod::Head:    return "HEAD";
        case HttpMethod::Options: return "OPTIONS";
        default:                  return "GET";
    }
}

/**
 * @brief Parses HTTP method from string_view (case-insensitive).
 */
[[nodiscard]] std::optional<HttpMethod> parse_http_method(std::string_view str) noexcept;

/**
 * @brief Non-owning HTTP header view.
 */
struct HttpHeaderView {
    std::string_view name;
    std::string_view value;
};

/**
 * @brief Owned HTTP header pair for parsed responses.
 */
struct HttpHeader {
    std::string name;
    std::string value;
};

/**
 * @brief Standard HTTP status code predicates and standard RFC reason phrases.
 */
namespace HttpStatus {
    [[nodiscard]] constexpr bool is_informational(int code) noexcept { return code >= 100 && code < 200; }
    [[nodiscard]] constexpr bool is_success(int code) noexcept       { return code >= 200 && code < 300; }
    [[nodiscard]] constexpr bool is_redirect(int code) noexcept      { return code >= 300 && code < 400; }
    [[nodiscard]] constexpr bool is_client_error(int code) noexcept  { return code >= 400 && code < 500; }
    [[nodiscard]] constexpr bool is_server_error(int code) noexcept  { return code >= 500 && code < 600; }
    [[nodiscard]] constexpr bool is_error(int code) noexcept         { return code >= 400; }

    [[nodiscard]] constexpr std::string_view reason_phrase(int code) noexcept {
        switch (code) {
            case 100: return "Continue";
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 408: return "Request Timeout";
            case 409: return "Conflict";
            case 422: return "Unprocessable Entity";
            case 429: return "Too Many Requests";
            case 500: return "Internal Server Error";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            case 504: return "Gateway Timeout";
            default:  return "Unknown";
        }
    }
} // namespace HttpStatus

/**
 * @brief Low-latency HTTP request representation.
 *
 * Designed for zero allocation on the execution hot path. Uses non-owning string_view
 * slices into pre-baked authorization buffers, literals, and JSON payloads.
 */
struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string_view path;                              // Must start with '/' (e.g. "/trader/v1/accounts")
    std::string_view auth_header;                       // Pre-baked "Bearer <token>"
    std::string_view content_type = "application/json"; // Default JSON
    std::string_view accept = "application/json";       // Default JSON
    std::string_view body;                              // Order JSON payload
    std::vector<HttpHeaderView> extra_headers;          // Optional custom header views

    /**
     * @brief Validates request structure (non-empty path beginning with '/').
     */
    [[nodiscard]] bool is_valid() const noexcept;

    /**
     * @brief Estimates byte size of formatted wire request.
     */
    [[nodiscard]] size_t estimated_wire_size(std::string_view host) const noexcept;

    /**
     * @brief Formats request into RFC 9112 HTTP/1.1 wire format in a reusable buffer.
     * When out_buffer has sufficient pre-reserved capacity, this performs zero heap allocations.
     *
     * @param host Target host (e.g. "api.schwabapi.com")
     * @param out_buffer Pre-allocated string buffer to receive formatted bytes
     */
    void format_wire(std::string_view host, std::string& out_buffer) const;

    /**
     * @brief Convenience overload that allocates and returns a new wire string.
     */
    [[nodiscard]] std::string format_wire(std::string_view host) const;
};

/**
 * @brief HTTP response representation with latency metric.
 */
struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::vector<HttpHeader> headers;
    std::chrono::microseconds latency{0};

    [[nodiscard]] constexpr bool is_success() const noexcept {
        return HttpStatus::is_success(status_code);
    }

    [[nodiscard]] constexpr bool is_error() const noexcept {
        return HttpStatus::is_error(status_code);
    }

    [[nodiscard]] constexpr bool is_rate_limited() const noexcept {
        return status_code == 429;
    }

    [[nodiscard]] constexpr bool is_unauthorized() const noexcept {
        return status_code == 401 || status_code == 403;
    }

    [[nodiscard]] constexpr bool is_server_error() const noexcept {
        return HttpStatus::is_server_error(status_code);
    }

    [[nodiscard]] constexpr std::string_view status_text() const noexcept {
        return HttpStatus::reason_phrase(status_code);
    }

    /**
     * @brief Case-insensitive header lookup.
     *
     * @param name Header name to search for (e.g. "Content-Type", "Retry-After")
     * @return Value view if header exists, std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<std::string_view> find_header(std::string_view name) const noexcept;

    /**
     * @brief Parses an RFC 9112 HTTP/1.1 wire response into an HttpResponse.
     * Handles status lines, headers, Content-Length, and Transfer-Encoding: chunked.
     *
     * @param wire_data Raw bytes received from the TLS socket.
     * @return Populated HttpResponse, or std::nullopt if the response is incomplete or malformed.
     */
    [[nodiscard]] static std::optional<HttpResponse> parse_wire(std::string_view wire_data);
};

} // namespace schwab
