#include "doctest/doctest.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "coke/http/http_client.h"
#include "coke/wait.h"
#include "kale/core/http_error.h"
#include "kale/core/http_server.h"
#include "kale/core/http_util.h"
#include "kale/middleware/cors.h"
#include "kale/middleware/error_handler.h"
#include "kale/middleware/rate_limiter.h"
#include "kale/security/csrf.h"
#include "kale/security/jwt.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

namespace {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct MiddlewareResponse {
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

class MiddlewareErrorServer {
public:
    explicit MiddlewareErrorServer(kale::HttpServerParams params = {})
        : server_(params) {
        server_.set_logger(kale::Logger::off());
        server_.set_error_formatter(kale::ErrorFormatters::standard_json());
    }
    ~MiddlewareErrorServer() { server_.stop(); }

    kale::HttpServer *operator->() noexcept { return &server_; }
    kale::HttpServer &server() noexcept { return server_; }

    void start() {
        REQUIRE_EQ(server_.start("127.0.0.1", 0), 0);
        REQUIRE_GT(server_.listen_port(), 0);
    }

    MiddlewareResponse request(std::string_view path, std::string method = "GET", Headers req_headers = {}, std::string req_body = "") {
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
        MiddlewareResponse response;
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

TEST_CASE("Middleware error adoption: rate_limiter emits 429 with Retry-After") {
    kale::RateLimitPolicy policy;
    policy.limit = 1;
    policy.window = std::chrono::seconds(10);
    kale::MemoryRateLimitStore store(policy);

    MiddlewareErrorServer server;
    server->use(kale::rate_limiter(store));
    server->GET("/resource", [](kale::Context &ctx) {
        ctx.string("ok");
    });
    server.start();

    // First request must succeed
    auto res1 = server.request("/resource");
    CHECK(res1.status == 200);

    // Second request exceeds limit and must receive 429
    auto res2 = server.request("/resource");
    CHECK(res2.status == 429);
    CHECK(res2.has("Retry-After"));
    CHECK(res2.body.find("RATE_LIMITED") != std::string::npos);
}

TEST_CASE("Middleware error adoption: cors preflight rejection emits 403") {
    kale::CorsConfig cfg;
    cfg.allow_origins = {"https://trusted.example.com"};

    MiddlewareErrorServer server;
    server->use(kale::cors(cfg));
    server->POST("/api/submit", [](kale::Context &ctx) {
        ctx.string("submitted");
    });
    server.start();

    // Disallowed origin preflight
    Headers headers = {
        {"Origin", "https://untrusted.example.com"},
        {"Access-Control-Request-Method", "POST"}
    };
    auto res = server.request("/api/submit", "OPTIONS", headers);
    CHECK(res.status == 403);
    CHECK(res.body.find("CORS_FORBIDDEN") != std::string::npos);
}

TEST_CASE("Middleware error adoption: bearer_auth rejection emits 401 with realm") {
    kale::BearerAuthConfig cfg;
    cfg.secret_or_public_key = "test_signing_secret_at_least_16_bytes";
    cfg.realm = "api_gateway_realm";

    MiddlewareErrorServer server;
    server->use(kale::bearer_auth(cfg));
    server->GET("/secure/data", [](kale::Context &ctx) {
        ctx.string("secret data");
    });
    server.start();

    // Missing Authorization header
    auto res = server.request("/secure/data");
    CHECK(res.status == 401);
    CHECK(res.has("WWW-Authenticate"));
    CHECK(res.header("WWW-Authenticate").find("realm=\"api_gateway_realm\"") != std::string::npos);
    CHECK(res.body.find("INVALID_TOKEN") != std::string::npos);
}

TEST_CASE("Middleware error adoption: csrf rejection emits 403 on state-changing method") {
    kale::CsrfConfig cfg;
    cfg.secret = "a_very_long_secret_key_exceeding_16_bytes";

    MiddlewareErrorServer server;
    server->use(kale::csrf(cfg));
    server->POST("/account/update", [](kale::Context &ctx) {
        ctx.string("updated");
    });
    server.start();

    // State-changing POST without CSRF token
    auto res = server.request("/account/update", "POST");
    CHECK(res.status == 403);
    CHECK(res.body.find("CSRF_FAILED") != std::string::npos);
}

TEST_CASE("Middleware error_handler centralized recovery formats JSON") {
    MiddlewareErrorServer server;
    server->use(kale::error_handler(kale::ErrorFormatters::standard_json()));

    server->GET("/cause-conflict", [](kale::Context &) {
        throw kale::HttpError::conflict("Resource conflict detected", "CONFLICT_KEY");
    });
    server.start();

    auto res = server.request("/cause-conflict");
    CHECK(res.status == 409);
    CHECK(res.has("Content-Type"));
    CHECK(res.header("Content-Type").find("application/json") != std::string::npos);
    CHECK(res.body.find("\"code\":409") != std::string::npos);
    CHECK(res.body.find("\"error_code\":\"CONFLICT_KEY\"") != std::string::npos);
    CHECK(res.body.find("\"message\":\"Resource conflict detected\"") != std::string::npos);
}
