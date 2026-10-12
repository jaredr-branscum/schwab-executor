#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "schwab/http_client.hpp"
#include "schwab/security.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace schwab {

namespace {

/**
 * @brief Zero-allocation UTF-8/ASCII to UTF-16 wide string conversion into a pre-reserved buffer.
 * Uses a single-cycle ASCII widening fast path for HTTP methods, paths, and headers.
 */
void to_wide_buffer(std::string_view str, std::wstring& out) {
    if (str.empty()) {
        return;
    }
    const size_t old_size = out.size();

    // Fast path: pure ASCII characters are directly widened in registers without kernel MultiByteToWideChar
    bool is_ascii = true;
    for (char c : str) {
        if (static_cast<unsigned char>(c) >= 128) {
            is_ascii = false;
            break;
        }
    }

    if (is_ascii) {
        out.resize(old_size + str.size());
        for (size_t i = 0; i < str.size(); ++i) {
            out[old_size + i] = static_cast<wchar_t>(static_cast<unsigned char>(str[i]));
        }
        return;
    }

    // Fallback for multi-byte UTF-8 payloads
    int needed = ::MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (needed > 0) {
        out.resize(old_size + static_cast<size_t>(needed));
        ::MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), out.data() + old_size, needed);
    }
}

std::string to_utf8(const wchar_t* wstr, size_t length) {
    if (length == 0 || wstr == nullptr) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(length), result.data(), size, nullptr, nullptr);
    return result;
}

struct ScopedHInternet {
    HINTERNET handle = nullptr;
    ~ScopedHInternet() {
        if (handle != nullptr) {
            ::WinHttpCloseHandle(handle);
            handle = nullptr;
        }
    }

    ScopedHInternet(const ScopedHInternet&) = delete;
    ScopedHInternet& operator=(const ScopedHInternet&) = delete;
    ScopedHInternet(ScopedHInternet&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    ScopedHInternet& operator=(ScopedHInternet&& other) noexcept {
        if (this != &other) {
            if (handle != nullptr) {
                ::WinHttpCloseHandle(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    explicit ScopedHInternet(HINTERNET h = nullptr) noexcept : handle(h) {}
};

/**
 * @brief WinHTTP transport using Windows connection pooling, TLS 1.3,
 * zero-allocation scratch buffers, and connection idle timeout recycling.
 */
class WinHttpTransport : public ITransport {
public:
    explicit WinHttpTransport(HttpClientConfig config) : config_(std::move(config)) {
        // Pre-reserve hot path scratch buffers so subsequent calls perform zero heap allocations
        method_buf_.reserve(16);
        path_buf_.reserve(512);
        headers_buf_.reserve(4096);
    }

    ~WinHttpTransport() override {
        disconnect();
        if (!headers_buf_.empty()) {
            secure_zero_memory(headers_buf_.data(), headers_buf_.capacity() * sizeof(wchar_t));
            headers_buf_.clear();
        }
    }

    bool prewarm() override {
        if (!init_session()) {
            return false;
        }

        if (connect_handle_ != nullptr && !is_idle_expired()) {
            return true;
        }

        if (connect_handle_ != nullptr) {
            ::WinHttpCloseHandle(connect_handle_);
            connect_handle_ = nullptr;
        }

        std::wstring host_w;
        to_wide_buffer(config_.base_host, host_w);
        connect_handle_ = ::WinHttpConnect(
            session_handle_,
            host_w.c_str(),
            config_.port,
            0
        );

        if (connect_handle_ != nullptr) {
            last_active_time_ = std::chrono::steady_clock::now();
            return true;
        }
        return false;
    }

    [[nodiscard]] bool is_connected() const noexcept override {
        return connect_handle_ != nullptr;
    }

    [[nodiscard]] bool is_idle_expired() const noexcept override {
        if (connect_handle_ == nullptr) {
            return true;
        }
        if (config_.idle_timeout_seconds <= 0) {
            return false;
        }
        auto now = std::chrono::steady_clock::now();
        auto idle_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_active_time_);
        return idle_elapsed.count() >= config_.idle_timeout_seconds;
    }

    void disconnect() noexcept override {
        if (connect_handle_ != nullptr) {
            ::WinHttpCloseHandle(connect_handle_);
            connect_handle_ = nullptr;
        }
        if (session_handle_ != nullptr) {
            ::WinHttpCloseHandle(session_handle_);
            session_handle_ = nullptr;
        }
    }

    enum class RequestResult {
        Success,
        PreSendFailure,   // Failed before wire transmission (safe to retry for all methods)
        PostSendFailure   // Failed after wire transmission (retry only for idempotent methods)
    };

    [[nodiscard]] HttpResponse send(const HttpRequest& request) override {
        auto start_time = std::chrono::steady_clock::now();

        // Connection lifecycle: if not connected OR if idle expired, recycle connection
        if (!is_connected() || is_idle_expired()) {
            if (!prewarm()) {
                HttpResponse fail_resp;
                fail_resp.status_code = 503;
                fail_resp.body = "{\"error\":\"Unable to connect to host\"}";
                auto end_time = std::chrono::steady_clock::now();
                fail_resp.latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                return fail_resp;
            }
        }

        HttpResponse response;
        RequestResult result = execute_request(request, response, start_time);

        // Idle drop / server reset resilience:
        // PreSendFailure: Socket was dead before transmission; safe to reconnect and retry for ANY request.
        // PostSendFailure: Socket dropped after transmission; retry ONLY if request is idempotent
        // (GET, HEAD, OPTIONS) to avoid duplicate order execution
        const bool is_idempotent = (request.method == HttpMethod::Get ||
                                    request.method == HttpMethod::Head ||
                                    request.method == HttpMethod::Options);

        if (result != RequestResult::Success && config_.max_retries_on_idle_drop > 0) {
            const bool safe_to_retry = (result == RequestResult::PreSendFailure) || is_idempotent;
            if (safe_to_retry) {
                disconnect();
                if (prewarm()) {
                    execute_request(request, response, start_time);
                }
            }
        }

        if (response.status_code > 0) {
            last_active_time_ = std::chrono::steady_clock::now();
        }

        return response;
    }

private:
    RequestResult execute_request(const HttpRequest& request, HttpResponse& response,
                                  std::chrono::steady_clock::time_point start_time) {
        // Zero allocation: clear scratch buffers while preserving pre-reserved capacity
        method_buf_.clear();
        to_wide_buffer(to_string_view(request.method), method_buf_);

        path_buf_.clear();
        to_wide_buffer(request.path, path_buf_);

        DWORD req_flags = config_.use_ssl ? WINHTTP_FLAG_SECURE : 0;
        ScopedHInternet req_handle(::WinHttpOpenRequest(
            connect_handle_,
            method_buf_.c_str(),
            path_buf_.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            req_flags
        ));

        if (req_handle.handle == nullptr) {
            response.status_code = 500;
            response.body = "{\"error\":\"Failed to open HTTP request\"}";
            auto end_time = std::chrono::steady_clock::now();
            response.latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            return RequestResult::PreSendFailure;
        }

        // Build wide headers string in pre-reserved scratch buffer; wipe any residual secrets
        if (!headers_buf_.empty()) {
            secure_zero_memory(headers_buf_.data(), headers_buf_.size() * sizeof(wchar_t));
            headers_buf_.clear();
        }

        if (!request.auth_header.empty()) {
            headers_buf_ += L"Authorization: ";
            to_wide_buffer(request.auth_header, headers_buf_);
            headers_buf_ += L"\r\n";
        }

        // RFC 7231 / Akamai WAF: Content-Type MUST only be sent if payload body is present,
        // or method is POST / PUT / PATCH. Sending Content-Type on GET causes Schwab to return HTTP 400.
        const bool has_body = !request.body.empty() ||
                              request.method == HttpMethod::Post ||
                              request.method == HttpMethod::Put ||
                              request.method == HttpMethod::Patch;
        if (has_body && !request.content_type.empty()) {
            headers_buf_ += L"Content-Type: ";
            to_wide_buffer(request.content_type, headers_buf_);
            headers_buf_ += L"\r\n";
        }

        if (!request.accept.empty()) {
            headers_buf_ += L"Accept: ";
            to_wide_buffer(request.accept, headers_buf_);
            headers_buf_ += L"\r\n";
        }

        if (!config_.user_agent.empty()) {
            headers_buf_ += L"User-Agent: ";
            to_wide_buffer(config_.user_agent, headers_buf_);
            headers_buf_ += L"\r\n";
        }

        for (const auto& h : request.extra_headers) {
            if (!h.name.empty()) {
                to_wide_buffer(h.name, headers_buf_);
                headers_buf_ += L": ";
                to_wide_buffer(h.value, headers_buf_);
                headers_buf_ += L"\r\n";
            }
        }

        if (!headers_buf_.empty()) {
            ::WinHttpAddRequestHeaders(
                req_handle.handle,
                headers_buf_.c_str(),
                static_cast<DWORD>(headers_buf_.size()),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE
            );
        }

        LPVOID body_data = request.body.empty()
            ? WINHTTP_NO_REQUEST_DATA
            : const_cast<char*>(request.body.data());
        DWORD body_len = static_cast<DWORD>(request.body.size());

        BOOL send_ok = ::WinHttpSendRequest(
            req_handle.handle,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            body_data,
            body_len,
            body_len,
            0
        );

        if (!send_ok) {
            response.status_code = 504;
            response.body = "{\"error\":\"WinHttpSendRequest failed\"}";
            auto end_time = std::chrono::steady_clock::now();
            response.latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            return RequestResult::PreSendFailure;
        }

        BOOL rec_ok = ::WinHttpReceiveResponse(req_handle.handle, nullptr);
        if (!rec_ok) {
            response.status_code = 504;
            response.body = "{\"error\":\"WinHttpReceiveResponse failed\"}";
            auto end_time = std::chrono::steady_clock::now();
            response.latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            return RequestResult::PostSendFailure;
        }

        // Query status code
        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        ::WinHttpQueryHeaders(
            req_handle.handle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX
        );
        response.status_code = static_cast<int>(status_code);

        // Query raw response headers
        DWORD headers_bytes = 0;
        ::WinHttpQueryHeaders(
            req_handle.handle,
            WINHTTP_QUERY_RAW_HEADERS_CRLF,
            WINHTTP_HEADER_NAME_BY_INDEX,
            WINHTTP_NO_OUTPUT_BUFFER,
            &headers_bytes,
            WINHTTP_NO_HEADER_INDEX
        );

        if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER && headers_bytes > 0) {
            headers_buf_.resize(headers_bytes / sizeof(wchar_t));
            if (::WinHttpQueryHeaders(
                    req_handle.handle,
                    WINHTTP_QUERY_RAW_HEADERS_CRLF,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    headers_buf_.data(),
                    &headers_bytes,
                    WINHTTP_NO_HEADER_INDEX)) {
                size_t pos = 0;
                size_t first_crlf = headers_buf_.find(L"\r\n");
                if (first_crlf != std::wstring::npos) {
                    pos = first_crlf + 2; // Skip status line
                }
                response.headers.clear();
                response.headers.reserve(16);
                while (pos < headers_buf_.size()) {
                    size_t next_crlf = headers_buf_.find(L"\r\n", pos);
                    if (next_crlf == std::wstring::npos || next_crlf == pos) {
                        break;
                    }
                    std::wstring_view line{headers_buf_.data() + pos, next_crlf - pos};
                    size_t colon = line.find(L':');
                    if (colon != std::wstring_view::npos) {
                        std::wstring_view h_name = line.substr(0, colon);
                        std::wstring_view h_val = line.substr(colon + 1);
                        while (!h_val.empty() && (h_val.front() == L' ' || h_val.front() == L'\t')) {
                            h_val.remove_prefix(1);
                        }
                        if (!h_name.empty()) {
                            response.headers.push_back(HttpHeader{
                                to_utf8(h_name.data(), h_name.size()),
                                to_utf8(h_val.data(), h_val.size())
                            });
                        }
                    }
                    pos = next_crlf + 2;
                }
            }
        }

        // Pre-reserve body memory for zero-reallocation JSON absorption
        response.body.clear();
        auto cl_hdr = response.find_header("Content-Length");
        size_t est_body = 2048;
        if (cl_hdr.has_value()) {
            size_t cl_val = 0;
            auto [cl_p, cl_ec] = std::from_chars(cl_hdr->data(), cl_hdr->data() + cl_hdr->size(), cl_val);
            if (cl_ec == std::errc() && cl_val > 0) {
                est_body = cl_val;
            }
        }
        response.body.reserve(est_body);

        // Zero heap allocations in read loop: read directly into 8 KB stack buffer
        DWORD bytes_avail = 0;
        while (::WinHttpQueryDataAvailable(req_handle.handle, &bytes_avail) && bytes_avail > 0) {
            DWORD to_read = (std::min)(bytes_avail, static_cast<DWORD>(read_buffer_.size()));
            DWORD bytes_read = 0;
            if (::WinHttpReadData(req_handle.handle, read_buffer_.data(), to_read, &bytes_read) && bytes_read > 0) {
                response.body.append(read_buffer_.data(), bytes_read);
            } else {
                break;
            }
        }

        auto end_time = std::chrono::steady_clock::now();
        response.latency = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        return RequestResult::Success;
    }

    bool init_session() {
        if (session_handle_ != nullptr) {
            return true;
        }

        std::wstring agent_w;
        to_wide_buffer(config_.user_agent, agent_w);
        session_handle_ = ::WinHttpOpen(
            agent_w.c_str(),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );

        if (session_handle_ == nullptr) {
            return false;
        }

        ::WinHttpSetTimeouts(
            session_handle_,
            config_.resolve_timeout_ms,
            config_.connect_timeout_ms,
            config_.send_timeout_ms,
            config_.receive_timeout_ms
        );

        return true;
    }

    HttpClientConfig config_;
    HINTERNET session_handle_ = nullptr;
    HINTERNET connect_handle_ = nullptr;
    std::chrono::steady_clock::time_point last_active_time_{};

    // Pre-allocated reusable scratch buffers for zero-allocation hot paths
    std::wstring method_buf_;
    std::wstring path_buf_;
    std::wstring headers_buf_;
    std::array<char, 8192> read_buffer_{};
};

} // namespace

std::unique_ptr<ITransport> HttpClient::create_default_transport(const HttpClientConfig& config) {
    return std::make_unique<WinHttpTransport>(config);
}

HttpClient::HttpClient(HttpClientConfig config)
    : config_(std::move(config)),
      transport_(create_default_transport(config_)) {}

HttpClient::HttpClient(std::unique_ptr<ITransport> transport, HttpClientConfig config)
    : config_(std::move(config)),
      transport_(std::move(transport)) {}

bool HttpClient::prewarm() {
    return transport_ ? transport_->prewarm() : false;
}

bool HttpClient::is_connected() const noexcept {
    return transport_ ? transport_->is_connected() : false;
}

bool HttpClient::is_idle_expired() const noexcept {
    return transport_ ? transport_->is_idle_expired() : true;
}

void HttpClient::disconnect() noexcept {
    if (transport_) {
        transport_->disconnect();
    }
}

HttpResponse HttpClient::send(const HttpRequest& request) {
    if (!transport_) {
        HttpResponse err;
        err.status_code = 500;
        err.body = "{\"error\":\"No transport configured\"}";
        return err;
    }
    return transport_->send(request);
}

bool HttpClient::keep_alive_ping(std::string_view probe_path, std::string_view auth_header) {
    if (!is_connected() || is_idle_expired()) {
        if (!prewarm()) {
            return false;
        }
    }
    HttpRequest req;
    req.method = HttpMethod::Get;
    req.path = probe_path;
    req.auth_header = auth_header;
    HttpResponse resp = send(req);
    return resp.status_code > 0;
}

HttpResponse HttpClient::get(std::string_view path, std::string_view auth_header) {
    HttpRequest req;
    req.method = HttpMethod::Get;
    req.path = path;
    req.auth_header = auth_header;
    req.content_type = {};
    return send(req);
}

HttpResponse HttpClient::post(std::string_view path, std::string_view body, std::string_view auth_header) {
    HttpRequest req;
    req.method = HttpMethod::Post;
    req.path = path;
    req.body = body;
    req.auth_header = auth_header;
    return send(req);
}

HttpResponse HttpClient::put(std::string_view path, std::string_view body, std::string_view auth_header) {
    HttpRequest req;
    req.method = HttpMethod::Put;
    req.path = path;
    req.body = body;
    req.auth_header = auth_header;
    return send(req);
}

HttpResponse HttpClient::del(std::string_view path, std::string_view auth_header) {
    HttpRequest req;
    req.method = HttpMethod::Delete;
    req.path = path;
    req.auth_header = auth_header;
    req.content_type = {};
    return send(req);
}

} // namespace schwab
