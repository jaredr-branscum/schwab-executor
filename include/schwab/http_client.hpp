#pragma once

#include "schwab/http_types.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace schwab {

/**
 * @brief Configuration parameters for persistent HTTPS transport connection.
 */
struct HttpClientConfig {
    std::string base_host = "api.schwabapi.com";
    uint16_t port = 443;
    bool use_ssl = true;
    int resolve_timeout_ms = 5000;
    int connect_timeout_ms = 5000;
    int send_timeout_ms = 5000;
    int receive_timeout_ms = 5000;
    int idle_timeout_seconds = 20;     // Proactively refresh socket if idle longer than this
    int max_retries_on_idle_drop = 1;  // Resend once if server closed keep-alive socket
    std::string user_agent = "schwab-executor/1.0";
};

/**
 * @brief Abstract transport interface for low-latency HTTP/HTTPS execution.
 * Decouples transport implementation to enable offline unit testing.
 */
class ITransport {
public:
    virtual ~ITransport() = default;

    /**
     * @brief Pre-warms the TLS connection pool in advance of execution.
     * @return true if socket and TLS session were successfully established.
     */
    virtual bool prewarm() = 0;

    /**
     * @brief Checks if the persistent TLS connection is active and hot.
     */
    [[nodiscard]] virtual bool is_connected() const noexcept = 0;

    /**
     * @brief Checks if the active connection has exceeded the idle timeout.
     */
    [[nodiscard]] virtual bool is_idle_expired() const noexcept = 0;

    /**
     * @brief Disconnects and releases active transport socket handles.
     */
    virtual void disconnect() noexcept = 0;

    /**
     * @brief Sends an HTTP request and receives the response.
     */
    [[nodiscard]] virtual HttpResponse send(const HttpRequest& request) = 0;
};

/**
 * @brief Pre-warmed HTTPS client for the Charles Schwab REST API.
 *
 * Supports pre-warming persistent keep-alive TLS connections 10-30s before the market bell,
 * eliminating DNS resolution and TLS handshake jitter on order execution. Features idle timeout
 * recycling and zero-allocation scratch buffers on the hot path.
 */
class HttpClient {
public:
    /**
     * @brief Constructs HttpClient with default production native transport.
     */
    explicit HttpClient(HttpClientConfig config = {});

    /**
     * @brief Constructs HttpClient with custom/mock transport for testing.
     */
    explicit HttpClient(std::unique_ptr<ITransport> transport, HttpClientConfig config = {});

    ~HttpClient() = default;

    // Movable but non-copyable (manages unique transport ownership)
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) noexcept = default;
    HttpClient& operator=(HttpClient&&) noexcept = default;

    /**
     * @brief Pre-warms the TLS connection pool in advance of execution window.
     */
    bool prewarm();

    /**
     * @brief Checks if persistent TLS connection is open and ready.
     */
    [[nodiscard]] bool is_connected() const noexcept;

    /**
     * @brief Checks if connection has exceeded idle threshold.
     */
    [[nodiscard]] bool is_idle_expired() const noexcept;

    /**
     * @brief Closes the connection pool.
     */
    void disconnect() noexcept;

    /**
     * @brief Sends an HttpRequest over the persistent connection.
     */
    [[nodiscard]] HttpResponse send(const HttpRequest& request);

    /**
     * @brief Proactively pings endpoint to keep persistent socket hot and prevent server idle drop.
     */
    [[nodiscard]] bool keep_alive_ping(std::string_view probe_path = "/trader/v1/userPreference",
                                       std::string_view auth_header = "");

    /**
     * @brief Convenience GET request.
     */
    [[nodiscard]] HttpResponse get(std::string_view path, std::string_view auth_header = "");

    /**
     * @brief Convenience POST request with JSON payload.
     */
    [[nodiscard]] HttpResponse post(std::string_view path, std::string_view body, std::string_view auth_header = "");

    /**
     * @brief Convenience PUT request with JSON payload.
     */
    [[nodiscard]] HttpResponse put(std::string_view path, std::string_view body, std::string_view auth_header = "");

    /**
     * @brief Convenience DELETE request.
     */
    [[nodiscard]] HttpResponse del(std::string_view path, std::string_view auth_header = "");

    /**
     * @brief Read-only view of client configuration.
     */
    [[nodiscard]] const HttpClientConfig& config() const noexcept { return config_; }

    /**
     * @brief Factory for default native transport.
     */
    [[nodiscard]] static std::unique_ptr<ITransport> create_default_transport(const HttpClientConfig& config);

private:
    HttpClientConfig config_;
    std::unique_ptr<ITransport> transport_;
};

} // namespace schwab
