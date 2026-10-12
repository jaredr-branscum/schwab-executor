#include "schwab/http_types.hpp"

#include <algorithm>
#include <charconv>

namespace schwab {

namespace {

constexpr char to_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.data() == b.data() && a.size() == b.size()) {
        return true;
    }
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) {
            return false;
        }
    }
    return true;
}

constexpr bool is_http_whitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r';
}

std::string_view trim(std::string_view sv) noexcept {
    while (!sv.empty() && is_http_whitespace(sv.front())) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && is_http_whitespace(sv.back())) {
        sv.remove_suffix(1);
    }
    return sv;
}

bool decode_chunked_body(std::string_view raw, std::string& out_body) {
    out_body.clear();
    // Decoded payload cannot exceed raw chunked wire size; pre-reserve to eliminate reallocations
    out_body.reserve(raw.size());
    size_t pos = 0;

    while (pos < raw.size()) {
        size_t line_end = raw.find("\r\n", pos);
        size_t step = 2;
        if (line_end == std::string_view::npos) {
            line_end = raw.find('\n', pos);
            if (line_end == std::string_view::npos) [[unlikely]] {
                return false;
            }
            step = 1;
        }

        std::string_view size_part = trim(raw.substr(pos, line_end - pos));
        size_t semi = size_part.find(';');
        if (semi != std::string_view::npos) {
            size_part = trim(size_part.substr(0, semi));
        }

        if (size_part.empty()) [[unlikely]] {
            return false;
        }

        size_t chunk_size = 0;
        auto [ptr, ec] = std::from_chars(size_part.data(), size_part.data() + size_part.size(), chunk_size, 16);
        if (ec != std::errc() || ptr != size_part.data() + size_part.size()) [[unlikely]] {
            return false;
        }

        pos = line_end + step;

        if (chunk_size == 0) {
            // Reached terminating chunk (0\r\n\r\n)
            return true;
        }

        // Defend against integer overflow and truncated chunk payloads
        if (chunk_size > raw.size() - pos) [[unlikely]] {
            return false;
        }

        out_body.append(raw.data() + pos, chunk_size);
        pos += chunk_size;

        // Skip chunk trailing CRLF
        if (pos < raw.size() && raw[pos] == '\r') {
            ++pos;
        }
        if (pos < raw.size() && raw[pos] == '\n') {
            ++pos;
        }
    }

    return true;
}

} // namespace

std::optional<HttpMethod> parse_http_method(std::string_view str) noexcept {
    if (iequals(str, "GET"))     return HttpMethod::Get;
    if (iequals(str, "POST"))    return HttpMethod::Post;
    if (iequals(str, "PUT"))     return HttpMethod::Put;
    if (iequals(str, "DELETE"))  return HttpMethod::Delete;
    if (iequals(str, "PATCH"))   return HttpMethod::Patch;
    if (iequals(str, "HEAD"))    return HttpMethod::Head;
    if (iequals(str, "OPTIONS")) return HttpMethod::Options;
    return std::nullopt;
}

bool HttpRequest::is_valid() const noexcept {
    return !path.empty() && path.front() == '/';
}

size_t HttpRequest::estimated_wire_size(std::string_view host) const noexcept {
    size_t sz = 0;
    sz += to_string_view(method).size() + 1 + path.size() + 11; // "METHOD /path HTTP/1.1\r\n"
    sz += 6 + host.size() + 2;                                   // "Host: host\r\n"
    sz += 32;                                                   // "User-Agent: schwab-executor/1.0\r\n"
    sz += 24;                                                   // "Connection: keep-alive\r\n"

    if (!auth_header.empty()) {
        sz += 15 + auth_header.size() + 2;                      // "Authorization: ...\r\n"
    }
    const bool has_body = !body.empty() ||
                          method == HttpMethod::Post ||
                          method == HttpMethod::Put ||
                          method == HttpMethod::Patch;

    if (has_body && !content_type.empty()) {
        sz += 14 + content_type.size() + 2;                     // "Content-Type: ...\r\n"
    }
    if (!accept.empty()) {
        sz += 8 + accept.size() + 2;                            // "Accept: ...\r\n"
    }
    if (has_body) {
        sz += 16 + 10 + 2;                                      // "Content-Length: 1234567890\r\n"
    }
    for (const auto& h : extra_headers) {
        sz += h.name.size() + 2 + h.value.size() + 2;
    }
    sz += 2;                                                    // "\r\n"
    sz += body.size();
    return sz;
}

void HttpRequest::format_wire(std::string_view host, std::string& out_buffer) const {
    out_buffer.clear();
    const size_t est = estimated_wire_size(host);
    if (out_buffer.capacity() < est) {
        out_buffer.reserve(est);
    }

    // Status Line
    out_buffer.append(to_string_view(method));
    out_buffer.push_back(' ');
    out_buffer.append(path);
    out_buffer.append(" HTTP/1.1\r\n");

    // Host
    out_buffer.append("Host: ");
    out_buffer.append(host);
    out_buffer.append("\r\n");

    // Standard headers
    out_buffer.append("User-Agent: schwab-executor/1.0\r\n");
    out_buffer.append("Connection: keep-alive\r\n");

    if (!auth_header.empty()) {
        out_buffer.append("Authorization: ");
        out_buffer.append(auth_header);
        out_buffer.append("\r\n");
    }

    const bool has_body = !body.empty() ||
                          method == HttpMethod::Post ||
                          method == HttpMethod::Put ||
                          method == HttpMethod::Patch;

    if (has_body && !content_type.empty()) {
        out_buffer.append("Content-Type: ");
        out_buffer.append(content_type);
        out_buffer.append("\r\n");
    }

    if (!accept.empty()) {
        out_buffer.append("Accept: ");
        out_buffer.append(accept);
        out_buffer.append("\r\n");
    }

    if (!body.empty() || method == HttpMethod::Post || method == HttpMethod::Put) {
        out_buffer.append("Content-Length: ");
        char len_buf[32];
        auto [ptr, ec] = std::to_chars(len_buf, len_buf + sizeof(len_buf), body.size());
        out_buffer.append(len_buf, static_cast<size_t>(ptr - len_buf));
        out_buffer.append("\r\n");
    }

    for (const auto& h : extra_headers) {
        if (!h.name.empty()) {
            out_buffer.append(h.name);
            out_buffer.append(": ");
            out_buffer.append(h.value);
            out_buffer.append("\r\n");
        }
    }

    // End of headers
    out_buffer.append("\r\n");

    // Body payload
    if (!body.empty()) {
        out_buffer.append(body);
    }
}

std::string HttpRequest::format_wire(std::string_view host) const {
    std::string result;
    format_wire(host, result);
    return result;
}

std::optional<std::string_view> HttpResponse::find_header(std::string_view name) const noexcept {
    for (const auto& header : headers) {
        if (iequals(header.name, name)) {
            return std::string_view(header.value);
        }
    }
    return std::nullopt;
}

std::optional<HttpResponse> HttpResponse::parse_wire(std::string_view wire_data) {
    if (wire_data.empty()) [[unlikely]] {
        return std::nullopt;
    }

    HttpResponse resp;

    // Locate end of status line
    size_t line_end = wire_data.find("\r\n");
    size_t step = 2;
    if (line_end == std::string_view::npos) {
        line_end = wire_data.find('\n');
        if (line_end == std::string_view::npos) [[unlikely]] {
            return std::nullopt;
        }
        step = 1;
    }

    std::string_view status_line = wire_data.substr(0, line_end);

    // Verify HTTP protocol prefix
    if (!status_line.starts_with("HTTP/")) [[unlikely]] {
        return std::nullopt;
    }

    // Find space after "HTTP/1.1" or "HTTP/1.0"
    size_t code_start = status_line.find(' ');
    if (code_start == std::string_view::npos) [[unlikely]] {
        return std::nullopt;
    }
    while (code_start < status_line.size() && status_line[code_start] == ' ') {
        ++code_start;
    }
    if (code_start >= status_line.size()) [[unlikely]] {
        return std::nullopt;
    }

    size_t code_end = status_line.find(' ', code_start);
    std::string_view code_str = (code_end == std::string_view::npos)
        ? status_line.substr(code_start)
        : status_line.substr(code_start, code_end - code_start);

    int parsed_code = 0;
    auto [ptr, ec] = std::from_chars(code_str.data(), code_str.data() + code_str.size(), parsed_code);
    if (ec != std::errc() || ptr != code_str.data() + code_str.size()) [[unlikely]] {
        return std::nullopt;
    }
    if (parsed_code < 100 || parsed_code > 999) [[unlikely]] {
        return std::nullopt;
    }
    resp.status_code = parsed_code;

    // Parse headers (pre-reserve to avoid vector heap reallocations)
    resp.headers.reserve(16);
    size_t pos = line_end + step;
    size_t header_end_pos = std::string_view::npos;

    while (pos < wire_data.size()) {
        // Check for empty line marking end of headers
        if (wire_data.substr(pos, 2) == "\r\n") {
            header_end_pos = pos + 2;
            break;
        }
        if (wire_data[pos] == '\n') {
            header_end_pos = pos + 1;
            break;
        }

        size_t next_line = wire_data.find("\r\n", pos);
        size_t next_step = 2;
        if (next_line == std::string_view::npos) {
            next_line = wire_data.find('\n', pos);
            if (next_line == std::string_view::npos) [[unlikely]] {
                // Incomplete header line
                return std::nullopt;
            }
            next_step = 1;
        }

        std::string_view header_line = wire_data.substr(pos, next_line - pos);
        size_t colon = header_line.find(':');
        if (colon != std::string_view::npos) {
            std::string_view h_name = trim(header_line.substr(0, colon));
            std::string_view h_val = trim(header_line.substr(colon + 1));
            if (!h_name.empty()) {
                resp.headers.push_back(HttpHeader{std::string(h_name), std::string(h_val)});
            }
        }

        pos = next_line + next_step;
    }

    if (header_end_pos == std::string_view::npos) [[unlikely]] {
        // Headers never terminated
        return std::nullopt;
    }

    // Parse Body
    std::string_view raw_body = wire_data.substr(header_end_pos);

    auto transfer_encoding = resp.find_header("Transfer-Encoding");
    if (transfer_encoding.has_value() && iequals(*transfer_encoding, "chunked")) {
        if (!decode_chunked_body(raw_body, resp.body)) [[unlikely]] {
            return std::nullopt;
        }
    } else {
        auto content_length_hdr = resp.find_header("Content-Length");
        if (content_length_hdr.has_value()) {
            size_t content_length = 0;
            auto [cl_ptr, cl_ec] = std::from_chars(content_length_hdr->data(),
                                                   content_length_hdr->data() + content_length_hdr->size(),
                                                   content_length);
            if (cl_ec == std::errc() && cl_ptr == content_length_hdr->data() + content_length_hdr->size()) {
                // Fail-safe error handling: reject truncated responses where received bytes < Content-Length
                if (raw_body.size() < content_length) [[unlikely]] {
                    return std::nullopt;
                }
                resp.body.assign(raw_body.data(), content_length);
            } else {
                resp.body.assign(raw_body.data(), raw_body.size());
            }
        } else {
            resp.body.assign(raw_body.data(), raw_body.size());
        }
    }

    return resp;
}

} // namespace schwab
