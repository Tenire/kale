#include "doctest/doctest.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "coke/http/http_client.h"
#include "coke/wait.h"
#include "kale/core/context.h"
#include "kale/core/error_response.h"
#include "kale/core/http_error.h"
#include "kale/core/http_server.h"
#include "kale/core/http_util.h"
#include "kale/http/json_reader.h"
#include "kale/http/validator.h"
#include "kale/middleware/error_handler.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

namespace {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct ClientResponse {
    int state{-1};
    int status{0};
    std::string body;
    Headers headers;

    bool has(std::string_view name) const {
        return std::any_of(headers.begin(), headers.end(), [name](const auto &h) {
            return kale::case_insensitive_equal(h.first, name);
        });
    }
    std::string header(std::string_view name) const {
        for (const auto &h : headers) {
            if (kale::case_insensitive_equal(h.first, name)) {
                return h.second;
            }
        }
        return "";
    }
};

class FeatureErrorServer {
public:
    explicit FeatureErrorServer(kale::HttpServerParams params = {})
        : server_(params) {
        server_.set_logger(kale::Logger::off());
        if (!params.error_formatter) {
            server_.set_error_formatter(kale::ErrorFormatters::plain_text());
        }
    }
    ~FeatureErrorServer() { server_.stop(); }

    kale::HttpServer *operator->() noexcept { return &server_; }
    kale::HttpServer &server() noexcept { return server_; }

    void start() {
        REQUIRE_EQ(server_.start("127.0.0.1", 0), 0);
        REQUIRE_GT(server_.listen_port(), 0);
    }

    ClientResponse request(std::string_view path, std::string method = "GET", Headers req_headers = {}, std::string req_body = "") {
        auto *task = WFTaskFactory::create_http_task(
            "http://127.0.0.1:" + std::to_string(server_.listen_port()) + std::string(path),
            0, 0, nullptr);
        auto *request = task->get_req();
        request->set_method(method.c_str());
        request->add_header_pair("Connection", "close");
        for (const auto &[name, value] : req_headers) {
            request->add_header_pair(name, value);
        }
        if (!req_body.empty()) {
            request->append_output_body(req_body);
        }
        auto result = coke::sync_wait(coke::HttpAwaiter(task));
        ClientResponse response;
        response.state = result.state;
        REQUIRE_EQ(result.state, 0);
        if (const char *status_str = result.resp.get_status_code()) {
            const std::string_view code(status_str);
            std::from_chars(code.data(), code.data() + code.size(), response.status);
        }
        if (result.resp.is_chunked()) {
            response.body = protocol::HttpUtil::decode_chunked_body(&result.resp);
        } else {
            const void *data = nullptr;
            size_t size = 0;
            result.resp.get_parsed_body(&data, &size);
            if (data && size) response.body.assign(static_cast<const char *>(data), size);
        }
        protocol::HttpHeaderCursor cursor(&result.resp);
        std::string name, value;
        while (cursor.next(name, value)) response.headers.emplace_back(name, value);
        return response;
    }

private:
    kale::HttpServer server_;
};

} // namespace

TEST_CASE("HttpError header validation and RFC framing boundaries") {
    SUBCASE("Case-insensitive rejection of framing and hop-by-hop headers") {
        kale::HttpError err(500, "internal error");
        CHECK_THROWS_AS(err.with_header("Content-Length", "42"), std::invalid_argument);
        CHECK_THROWS_AS(err.with_header("transfer-encoding", "chunked"), std::invalid_argument);
        CHECK_THROWS_AS(err.with_header("Connection", "close"), std::invalid_argument);
        CHECK_THROWS_AS(err.with_header("Upgrade", "websocket"), std::invalid_argument);
        CHECK_THROWS_AS(err.with_header("Host", "localhost"), std::invalid_argument);
        CHECK_THROWS_AS(err.with_header("Content-Range", "bytes 0-1/2"), std::invalid_argument);
    }

    SUBCASE("Rejection of CRLF injection and invalid field names") {
        kale::HttpError err(400, "bad request");
        CHECK_THROWS_AS(err.with_header("Invalid Name", "val"), std::invalid_argument);
        CHECK_THROWS_AS(err.with_header("Header:Colon", "val"), std::invalid_argument);
        CHECK_THROWS_AS(err.with_header("X-Injected\r\n", "val"), std::invalid_argument);
        CHECK_THROWS_AS(err.with_header("X-Valid", "val\r\nEvil: true"), std::invalid_argument);
    }

    SUBCASE("Bearer realm RFC 6750 escaping") {
        auto unauth = kale::HttpError::unauthorized("Unauthorized", "UNAUTH", "api \"secure\" \\ test");
        bool has_auth = false;
        for (const auto &[k, v] : unauth.headers()) {
            if (k == "WWW-Authenticate") {
                has_auth = true;
                CHECK(v.find("realm=\"api \\\"secure\\\" \\\\ test\"") != std::string::npos);
            }
        }
        CHECK(has_auth);
    }
}

TEST_CASE("JsonBindError and ValidationError details propagation") {
    SUBCASE("JsonBindError retains field path and reason in HttpError details") {
        kale::JsonBindError err("user.profile.age", "uint", "expected integer");
        CHECK(err.status_code() == 400);
        CHECK(err.error_code() == "JSON_BIND_ERROR");
        CHECK(err.details().count("user.profile.age") == 1);
    }

    SUBCASE("ValidationError wraps ValidationResult and sets unprocessable entity status") {
        kale::ValidationResult res;
        res.add_error("email", "format invalid");
        CHECK_FALSE(res.is_valid());
        CHECK_THROWS_AS(res.throw_if_invalid(), kale::ValidationError);

        try {
            res.throw_if_invalid();
        } catch (const kale::ValidationError &ve) {
            CHECK(ve.status_code() == 422);
            CHECK(ve.error_code() == "VALIDATION_FAILED");
            CHECK(ve.details().at("email") == "format invalid");
        }
    }
}

TEST_CASE("Server error handling end-to-end via native HTTP connection") {
    FeatureErrorServer server;

    server->GET("/error/bad-request", [](kale::Context &) {
        throw kale::HttpError::bad_request("invalid query parameter", "BAD_PARAM");
    });

    server->GET("/error/unauthorized", [](kale::Context &) {
        throw kale::HttpError::unauthorized("Access token required", "TOKEN_REQUIRED", "members_area");
    });

    server->GET("/error/rate-limited", [](kale::Context &) {
        throw kale::HttpError::too_many_requests("Quota exceeded", 45);
    });

    server->POST("/only-post", [](kale::Context &ctx) {
        ctx.string("posted");
    });

    server.start();

    SUBCASE("HttpError mapped to correct status and error payload") {
        auto res = server.request("/error/bad-request");
        CHECK(res.status == 400);
        CHECK(res.body.find("invalid query parameter") != std::string::npos);
    }

    SUBCASE("Unauthorized error sets WWW-Authenticate header") {
        auto res = server.request("/error/unauthorized");
        CHECK(res.status == 401);
        CHECK(res.has("WWW-Authenticate"));
        CHECK(res.header("WWW-Authenticate").find("realm=\"members_area\"") != std::string::npos);
    }

    SUBCASE("Rate limited error sets Retry-After header") {
        auto res = server.request("/error/rate-limited");
        CHECK(res.status == 429);
        CHECK(res.has("Retry-After"));
        CHECK(res.header("Retry-After") == "45");
    }

    SUBCASE("Server fallback for unmapped path returns 404") {
        auto res = server.request("/non-existent-route");
        CHECK(res.status == 404);
        CHECK(res.body.find("Not Found") != std::string::npos);
    }

    SUBCASE("Server fallback for unsupported method returns 405 with Allow header") {
        auto res = server.request("/only-post", "GET");
        CHECK(res.status == 405);
        CHECK(res.has("Allow"));
        CHECK(res.header("Allow").find("POST") != std::string::npos);
    }
}

TEST_CASE("ErrorPolicy production redaction and RPC always_http_200 mode") {
    SUBCASE("Unexposed internal error is redacted when expose_internal_errors is false") {
        kale::HttpServerParams params;
        params.error_policy.expose_internal_errors = false;
        FeatureErrorServer server(params);

        server->GET("/fail", [](kale::Context &) {
            throw std::runtime_error("secret internal db credentials leaked");
        });
        server.start();

        auto res = server.request("/fail");
        CHECK(res.status == 500);
        CHECK(res.body.find("secret internal db credentials leaked") == std::string::npos);
        CHECK(res.body.find("Internal Server Error") != std::string::npos);
    }

    SUBCASE("always_http_200 with problem_details conforms to RFC 9457 status=200 and error_status") {
        kale::HttpServerParams params;
        params.set_error_formatter(kale::ErrorFormatters::problem_details());
        params.set_error_policy(kale::ErrorPolicy{
            .expose_internal_errors = false,
            .always_http_200 = true,
            .default_status_code = 500
        });
        FeatureErrorServer server(params);

        server->GET("/not-found-item", [](kale::Context &) {
            throw kale::HttpError::not_found("Requested item is missing", "ITEM_NOT_FOUND");
        });
        server.start();

        auto res = server.request("/not-found-item");
        CHECK(res.status == 200); // HTTP wire status is 200 in RPC mode
        CHECK(res.has("Content-Type"));
        CHECK(res.header("Content-Type").find("application/problem+json") != std::string::npos);
        // RFC 9457 §3.1.2: status member in problem details object MUST match HTTP status code (200)
        CHECK(res.body.find("\"status\":200") != std::string::npos);
        // Extension member preserves original business HTTP status code
        CHECK(res.body.find("\"error_status\":404") != std::string::npos);
        CHECK(res.body.find("\"error_code\":\"ITEM_NOT_FOUND\"") != std::string::npos);
    }
}

TEST_CASE("Custom throwing formatter boundary safely produces 500 fallback") {
    kale::HttpServerParams params;
    params.set_error_formatter([](kale::Context &, const kale::ErrorContext &) {
        throw std::runtime_error("faulty custom formatter explosion");
    });
    FeatureErrorServer server(params);

    server->GET("/trigger-error", [](kale::Context &) {
        throw kale::HttpError::bad_request("invalid input");
    });
    server.start();

    auto res = server.request("/trigger-error");
    CHECK(res.status == 500);
    CHECK(res.body.find("Internal Server Error") != std::string::npos);
}
