#include <catch2/catch_test_macros.hpp>
#include "schwab/http_types.hpp"

using namespace schwab;

TEST_CASE("HttpMethod enum and conversions", "[http_types]") {
    SECTION("Single-byte memory footprint") {
        STATIC_REQUIRE(sizeof(HttpMethod) == 1);
    }

    SECTION("to_string_view string representations") {
        REQUIRE(to_string_view(HttpMethod::Get) == "GET");
        REQUIRE(to_string_view(HttpMethod::Post) == "POST");
        REQUIRE(to_string_view(HttpMethod::Put) == "PUT");
        REQUIRE(to_string_view(HttpMethod::Delete) == "DELETE");
        REQUIRE(to_string_view(HttpMethod::Patch) == "PATCH");
        REQUIRE(to_string_view(HttpMethod::Head) == "HEAD");
        REQUIRE(to_string_view(HttpMethod::Options) == "OPTIONS");
    }

    SECTION("parse_http_method case-insensitivity") {
        REQUIRE(parse_http_method("GET") == HttpMethod::Get);
        REQUIRE(parse_http_method("get") == HttpMethod::Get);
        REQUIRE(parse_http_method("Get") == HttpMethod::Get);

        REQUIRE(parse_http_method("POST") == HttpMethod::Post);
        REQUIRE(parse_http_method("post") == HttpMethod::Post);

        REQUIRE(parse_http_method("PUT") == HttpMethod::Put);
        REQUIRE(parse_http_method("DELETE") == HttpMethod::Delete);
        REQUIRE(parse_http_method("delete") == HttpMethod::Delete);
        REQUIRE(parse_http_method("PATCH") == HttpMethod::Patch);
        REQUIRE(parse_http_method("HEAD") == HttpMethod::Head);
        REQUIRE(parse_http_method("OPTIONS") == HttpMethod::Options);

        REQUIRE(parse_http_method("UNKNOWN") == std::nullopt);
        REQUIRE(parse_http_method("") == std::nullopt);
        REQUIRE(parse_http_method("CONNECT") == std::nullopt);
    }
}

TEST_CASE("HttpStatus predicates and standard reasons", "[http_types]") {
    SECTION("Informational range 100-199") {
        REQUIRE(HttpStatus::is_informational(100));
        REQUIRE_FALSE(HttpStatus::is_success(100));
        REQUIRE_FALSE(HttpStatus::is_error(100));
        REQUIRE(HttpStatus::reason_phrase(100) == "Continue");
    }

    SECTION("Success range 200-299") {
        REQUIRE(HttpStatus::is_success(200));
        REQUIRE(HttpStatus::is_success(201));
        REQUIRE(HttpStatus::is_success(204));
        REQUIRE_FALSE(HttpStatus::is_error(200));
        REQUIRE_FALSE(HttpStatus::is_client_error(200));
        REQUIRE_FALSE(HttpStatus::is_server_error(200));

        REQUIRE(HttpStatus::reason_phrase(200) == "OK");
        REQUIRE(HttpStatus::reason_phrase(201) == "Created");
        REQUIRE(HttpStatus::reason_phrase(204) == "No Content");
    }

    SECTION("Redirect range 300-399") {
        REQUIRE(HttpStatus::is_redirect(301));
        REQUIRE(HttpStatus::is_redirect(302));
        REQUIRE_FALSE(HttpStatus::is_success(301));
        REQUIRE_FALSE(HttpStatus::is_error(301));
    }

    SECTION("Client error range 400-499") {
        REQUIRE(HttpStatus::is_client_error(400));
        REQUIRE(HttpStatus::is_client_error(401));
        REQUIRE(HttpStatus::is_client_error(403));
        REQUIRE(HttpStatus::is_client_error(404));
        REQUIRE(HttpStatus::is_client_error(429));
        REQUIRE(HttpStatus::is_error(400));
        REQUIRE_FALSE(HttpStatus::is_server_error(400));

        REQUIRE(HttpStatus::reason_phrase(400) == "Bad Request");
        REQUIRE(HttpStatus::reason_phrase(401) == "Unauthorized");
        REQUIRE(HttpStatus::reason_phrase(403) == "Forbidden");
        REQUIRE(HttpStatus::reason_phrase(404) == "Not Found");
        REQUIRE(HttpStatus::reason_phrase(429) == "Too Many Requests");
    }

    SECTION("Server error range 500-599") {
        REQUIRE(HttpStatus::is_server_error(500));
        REQUIRE(HttpStatus::is_server_error(502));
        REQUIRE(HttpStatus::is_server_error(503));
        REQUIRE(HttpStatus::is_server_error(504));
        REQUIRE(HttpStatus::is_error(500));
        REQUIRE_FALSE(HttpStatus::is_client_error(500));

        REQUIRE(HttpStatus::reason_phrase(500) == "Internal Server Error");
        REQUIRE(HttpStatus::reason_phrase(502) == "Bad Gateway");
        REQUIRE(HttpStatus::reason_phrase(503) == "Service Unavailable");
        REQUIRE(HttpStatus::reason_phrase(504) == "Gateway Timeout");
    }

    SECTION("Unrecognized code returns Unknown") {
        REQUIRE(HttpStatus::reason_phrase(999) == "Unknown");
    }
}

TEST_CASE("HttpRequest validation and wire serialization", "[http_types]") {
    SECTION("Validation logic") {
        HttpRequest req;
        req.path = "/trader/v1/accounts";
        REQUIRE(req.is_valid());

        HttpRequest invalid_empty;
        REQUIRE_FALSE(invalid_empty.is_valid());

        HttpRequest invalid_relative;
        invalid_relative.path = "trader/v1/accounts";
        REQUIRE_FALSE(invalid_relative.is_valid());
    }

    SECTION("GET wire format with pre-baked auth header") {
        HttpRequest req;
        req.method = HttpMethod::Get;
        req.path = "/trader/v1/accounts/12345/orders";
        req.auth_header = "Bearer test_token_xyz";

        std::string wire = req.format_wire("api.schwabapi.com");

        REQUIRE(wire.find("GET /trader/v1/accounts/12345/orders HTTP/1.1\r\n") != std::string::npos);
        REQUIRE(wire.find("Host: api.schwabapi.com\r\n") != std::string::npos);
        REQUIRE(wire.find("Authorization: Bearer test_token_xyz\r\n") != std::string::npos);
        REQUIRE(wire.find("Accept: application/json\r\n") != std::string::npos);
        REQUIRE(wire.find("Connection: keep-alive\r\n") != std::string::npos);
        REQUIRE(wire.ends_with("\r\n\r\n"));
    }

    SECTION("POST wire format with JSON payload and Content-Length") {
        HttpRequest req;
        req.method = HttpMethod::Post;
        req.path = "/trader/v1/accounts/12345/orders";
        req.auth_header = "Bearer test_token_xyz";
        req.body = "{\"orderType\":\"MARKET\",\"session\":\"NORMAL\"}";

        std::string wire = req.format_wire("api.schwabapi.com");

        REQUIRE(wire.find("POST /trader/v1/accounts/12345/orders HTTP/1.1\r\n") != std::string::npos);
        REQUIRE(wire.find("Content-Type: application/json\r\n") != std::string::npos);
        std::string expected_len = "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
        REQUIRE(wire.find(expected_len) != std::string::npos);
        REQUIRE(wire.ends_with("\r\n\r\n" + std::string(req.body)));
    }

    SECTION("Zero-allocation buffer reuse") {
        HttpRequest req;
        req.method = HttpMethod::Get;
        req.path = "/trader/v1/userPreference";
        req.auth_header = "Bearer abc";

        std::string preallocated_buffer;
        preallocated_buffer.reserve(2048);
        const char* initial_ptr = preallocated_buffer.data();

        req.format_wire("api.schwabapi.com", preallocated_buffer);

        // Verification: capacity preserved, pointer was not reallocated
        REQUIRE(preallocated_buffer.capacity() >= 2048);
        REQUIRE(preallocated_buffer.data() == initial_ptr);
        REQUIRE_FALSE(preallocated_buffer.empty());
    }

    SECTION("Extra custom headers") {
        HttpRequest req;
        req.path = "/trader/v1/accounts";
        req.extra_headers.push_back({"Schwab-Client-CorrelId", "12345-uuid"});

        std::string wire = req.format_wire("api.schwabapi.com");
        REQUIRE(wire.find("Schwab-Client-CorrelId: 12345-uuid\r\n") != std::string::npos);
    }
}

TEST_CASE("HttpResponse helper predicates and header search", "[http_types]") {
    HttpResponse resp;
    resp.status_code = 200;
    resp.headers.push_back({"Content-Type", "application/json;charset=utf-8"});
    resp.headers.push_back({"Retry-After", "30"});

    REQUIRE(resp.is_success());
    REQUIRE_FALSE(resp.is_error());
    REQUIRE_FALSE(resp.is_rate_limited());
    REQUIRE_FALSE(resp.is_unauthorized());
    REQUIRE(resp.status_text() == "OK");

    SECTION("Case-insensitive header search") {
        REQUIRE(resp.find_header("Content-Type") == "application/json;charset=utf-8");
        REQUIRE(resp.find_header("content-type") == "application/json;charset=utf-8");
        REQUIRE(resp.find_header("CONTENT-TYPE") == "application/json;charset=utf-8");
        REQUIRE(resp.find_header("Retry-After") == "30");
        REQUIRE(resp.find_header("retry-after") == "30");
        REQUIRE(resp.find_header("X-Missing") == std::nullopt);
    }

    SECTION("Rate limited check") {
        resp.status_code = 429;
        REQUIRE(resp.is_rate_limited());
        REQUIRE(resp.is_error());
        REQUIRE_FALSE(resp.is_success());
    }

    SECTION("Unauthorized and forbidden checks") {
        resp.status_code = 401;
        REQUIRE(resp.is_unauthorized());
        resp.status_code = 403;
        REQUIRE(resp.is_unauthorized());
    }

    SECTION("Server error check") {
        resp.status_code = 503;
        REQUIRE(resp.is_server_error());
        REQUIRE(resp.status_text() == "Service Unavailable");
    }
}

TEST_CASE("HttpResponse wire parsing", "[http_types]") {
    SECTION("Standard 200 OK with Content-Length") {
        std::string_view raw =
            "HTTP/1.1 200 OK\r\n"
            "Date: Sat, 10 Oct 2026 19:40:00 GMT\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 17\r\n"
            "\r\n"
            "{\"status\":\"open\"}";

        auto parsed = HttpResponse::parse_wire(raw);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->status_code == 200);
        REQUIRE(parsed->is_success());
        REQUIRE(parsed->find_header("Content-Type") == "application/json");
        REQUIRE(parsed->body == "{\"status\":\"open\"}");
    }

    SECTION("201 Created with Location header and empty body") {
        std::string_view raw =
            "HTTP/1.1 201 Created\r\n"
            "Location: /trader/v1/accounts/123/orders/999\r\n"
            "Content-Length: 0\r\n"
            "\r\n";

        auto parsed = HttpResponse::parse_wire(raw);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->status_code == 201);
        REQUIRE(parsed->is_success());
        REQUIRE(parsed->find_header("Location") == "/trader/v1/accounts/123/orders/999");
        REQUIRE(parsed->body.empty());
    }

    SECTION("400 Bad Request error response") {
        std::string_view raw =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 36\r\n"
            "\r\n"
            "{\"error\":\"Invalid order parameters\"}";

        auto parsed = HttpResponse::parse_wire(raw);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->status_code == 400);
        REQUIRE(parsed->is_error());
        REQUIRE(parsed->body == "{\"error\":\"Invalid order parameters\"}");
    }

    SECTION("Chunked transfer encoding parsing") {
        std::string_view raw =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Content-Type: application/json\r\n"
            "\r\n"
            "5\r\n"
            "hello\r\n"
            "6\r\n"
            " world\r\n"
            "0\r\n"
            "\r\n";

        auto parsed = HttpResponse::parse_wire(raw);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->status_code == 200);
        REQUIRE(parsed->body == "hello world");
    }

    SECTION("Truncated Content-Length detection") {
        std::string_view raw =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 50\r\n"
            "\r\n"
            "Short payload"; // only 13 bytes instead of 50

        // Must reject rather than returning incomplete/corrupted body
        REQUIRE(HttpResponse::parse_wire(raw) == std::nullopt);
    }

    SECTION("Malformed wire data rejection") {
        REQUIRE(HttpResponse::parse_wire("") == std::nullopt);
        REQUIRE(HttpResponse::parse_wire("INVALID 200 OK\r\n\r\n") == std::nullopt);
        REQUIRE(HttpResponse::parse_wire("HTTP/1.1 ABC OK\r\n\r\n") == std::nullopt);
        REQUIRE(HttpResponse::parse_wire("HTTP/1.1 050 OK\r\n\r\n") == std::nullopt);  // Out of bounds status
        REQUIRE(HttpResponse::parse_wire("HTTP/1.1 1000 OK\r\n\r\n") == std::nullopt); // Out of bounds status
        REQUIRE(HttpResponse::parse_wire("HTTP/1.1 200 OK") == std::nullopt);          // Missing delimiter
    }

    SECTION("Chunked overflow rejection") {
        std::string_view raw =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "FFFFFFFFFFFFFFFF\r\n" // Gigantic hex chunk size
            "abc\r\n";

        REQUIRE(HttpResponse::parse_wire(raw) == std::nullopt);
    }
}
