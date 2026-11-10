#include "doctest/doctest.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coke/http/http_client.h"
#include "coke/sleep.h"
#include "coke/wait.h"
#include "kale/middleware/access_log.h"
#include "kale/core/http_server.h"
#include "kale/core/logger.h"
#include "kale/middleware/cors.h"
#include "kale/core/http_error.h"
#include "kale/core/http_util.h"
#include "kale/middleware/request_id.h"
#include "kale/middleware/security_headers.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

namespace {
using namespace std::chrono_literals;
using Headers = std::vector<std::pair<std::string, std::string>>;

struct MiddlewareResponse {
    int state{-1};
    int status{0};
    std::string body;
    Headers headers;

    bool has(std::string_view name) const {
        return std::any_of(headers.begin(), headers.end(), [name](const auto &header) {
            return kale::case_insensitive_equal(header.first, name);
        });
    }
    std::string header(std::string_view name) const {
        std::string value;
        for (const auto &header : headers) {
            if (kale::case_insensitive_equal(header.first, name)) {
                if (!value.empty()) value += ", ";
                value += header.second;
            }
        }
        return value;
    }
};

coke::Task<MiddlewareResponse> middleware_request(std::string url, std::string method, Headers headers) {
    auto *task = WFTaskFactory::create_http_task(url, 0, 0, nullptr);
    auto *request = task->get_req();
    request->set_method(method);
    request->add_header_pair("Connection", "close");
    for (const auto &[name, value] : headers) request->add_header_pair(name, value);
    auto result = co_await coke::HttpAwaiter(task);
    MiddlewareResponse response;
    response.state = result.state;
    if (result.state != 0) co_return response;
    if (const char *status = result.resp.get_status_code()) {
        const std::string_view code(status);
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
    co_return response;
}

class MiddlewareServer {
public:
    ~MiddlewareServer() { stop(); }
    kale::HttpServer *operator->() noexcept { return &server_; }
    void start() {
        server_.set_logger(kale::Logger::off());
        REQUIRE_EQ(server_.start("127.0.0.1", 0), 0);
        REQUIRE_GT(server_.listen_port(), 0);
    }
    void stop() { server_.stop(); }
    MiddlewareResponse request(std::string_view target, std::string method = "GET", Headers headers = {}) {
        auto response = coke::sync_wait(middleware_request(
            "http://127.0.0.1:" + std::to_string(server_.listen_port()) + std::string(target),
            std::move(method), std::move(headers)));
        REQUIRE_EQ(response.state, 0);
        return response;
    }
private:
    kale::HttpServer server_;
};

bool token_present(std::string_view list, std::string_view token) {
    while (!list.empty()) {
        const auto comma = list.find(44);
        auto item = list.substr(0, comma);
        while (!item.empty() && (item.front() == 32 || item.front() == 9)) item.remove_prefix(1);
        while (!item.empty() && (item.back() == 32 || item.back() == 9)) item.remove_suffix(1);
        if (kale::case_insensitive_equal(item, token)) return true;
        if (comma == std::string_view::npos) return false;
        list.remove_prefix(comma + 1);
    }
    return false;
}

struct LogStore {
    void push(kale::AccessLogRecord record) {
        const std::lock_guard lock(mutex);
        records.push_back(std::move(record));
    }
    std::vector<kale::AccessLogRecord> copy() {
        const std::lock_guard lock(mutex);
        return records;
    }
    std::mutex mutex;
    std::vector<kale::AccessLogRecord> records;
};

void install_id_echo(MiddlewareServer &server) {
    server->GET("/id", [](kale::Context &ctx) -> coke::Task<void> {
        const auto selected = kale::get_request_id(ctx);
        co_await coke::sleep(1ms);
        ctx.string(selected);
    });
}

} // namespace

TEST_SUITE("application_middleware") {

TEST_CASE("application middleware is opt-in; no factory changes the server implicitly") {
    MiddlewareServer server;
    auto cors = kale::cors();
    auto security = kale::security_headers();
    auto id = kale::request_id();
    (void)cors;
    (void)security;
    (void)id;
    server->GET("/plain", [](kale::Context &ctx) {
        CHECK(kale::get_request_id(ctx).empty());
        ctx.string("plain");
    });
    server.start();
    auto response = server.request("/plain", "GET", {{"Origin", "https://client.example"}});
    CHECK_EQ(response.status, 200);
    CHECK_EQ(response.body, "plain");
    CHECK_FALSE(response.has("Access-Control-Allow-Origin"));
    CHECK_FALSE(response.has("X-Content-Type-Options"));
    CHECK_FALSE(response.has("X-Request-Id"));
}

TEST_CASE("CORS rejects unsafe configuration at factory creation before registration") {
    kale::CorsConfig config;
    config.allow_credentials = true;
    CHECK_THROWS_AS(kale::cors(config), std::invalid_argument);
    config.allow_credentials = false;
    for (const std::string origin : {"https://ok.example/path", "https://user@ok.example",
             "https://ok.example?x", "https://ok.example#x", "https://ok.example:bad",
             "https://ok.example:65536", "https://[not-ipv6]", "https://ok.example https://other.example",
             "https://ok.example\r\nX-Injected: yes"}) {
        config.allow_origins = {origin};
        CHECK_THROWS_AS(kale::cors(config), std::invalid_argument);
    }
    config.allow_origins = {"https://ok.example", "*"};
    CHECK_THROWS_AS(kale::cors(config), std::invalid_argument);
    config.allow_origins = {"https://ok.example"};
    config.allow_methods = {"GET\r\nX-Bad"};
    CHECK_THROWS_AS(kale::cors(config), std::invalid_argument);
    config.allow_methods = {"GET"};
    config.allow_headers = {"Bad:Header"};
    CHECK_THROWS_AS(kale::cors(config), std::invalid_argument);
    config.allow_headers = {"*", "X-Other"};
    CHECK_THROWS_AS(kale::cors(config), std::invalid_argument);
    config.allow_headers = {"X-Other"};
    config.expose_headers = {"bad header"};
    CHECK_THROWS_AS(kale::cors(config), std::invalid_argument);
    config.expose_headers = {};
    config.max_age = -1;
    CHECK_THROWS_AS(kale::cors(config), std::invalid_argument);
    config.max_age = 0;
    config.allow_origins = {"https://[::1]:8443", "null"};
    CHECK_NOTHROW(kale::cors(config));
}

TEST_CASE("CORS ordinary allowed denied and absent origins retain application behavior and Vary") {
    kale::CorsConfig config;
    config.allow_origins = {"https://client.example", "null"};
    config.allow_credentials = true;
    config.expose_headers = {"X-Result", "X-Trace"};
    MiddlewareServer server;
    server->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        ctx.set_header("Vary", "Accept-Encoding");
        co_await next();
    });
    server->use(kale::cors(config));
    server->GET("/data", [](kale::Context &ctx) {
        ctx.set_header("Vary", "Accept-Language");
        ctx.string("application");
    });
    server.start();
    for (const auto &origin : {std::string("https://client.example"), std::string("null")}) {
        auto response = server.request("/data", "GET", {{"Origin", origin}});
        CHECK_EQ(response.status, 200);
        CHECK_EQ(response.body, "application");
        CHECK_EQ(response.header("Access-Control-Allow-Origin"), origin);
        CHECK_EQ(response.header("Access-Control-Allow-Credentials"), "true");
        CHECK_EQ(response.header("Access-Control-Expose-Headers"), "X-Result, X-Trace");
        CHECK(token_present(response.header("Vary"), "Origin"));
        CHECK(token_present(response.header("Vary"), "Accept-Language"));
    }
    for (const auto &headers : {Headers{}, Headers{{"Origin", "https://denied.example"}},
             Headers{{"Origin", "https://client.example/path"}},
             Headers{{"Origin", "https://client.example https://evil.example"}},
             Headers{{"Origin", "https://client.example"}, {"Origin", "https://client.example"}}}) {
        auto response = server.request("/data", "GET", headers);
        CHECK_EQ(response.status, 200);
        CHECK_EQ(response.body, "application");
        CHECK_FALSE(response.has("Access-Control-Allow-Origin"));
        CHECK_FALSE(response.has("Access-Control-Allow-Credentials"));
        CHECK_FALSE(response.has("Access-Control-Expose-Headers"));
        CHECK(token_present(response.header("Vary"), "Origin"));
    }
    auto not_found = server.request("/missing", "GET", {{"Origin", "https://client.example"}});
    CHECK_EQ(not_found.status, 404);
    CHECK_EQ(not_found.header("Access-Control-Allow-Origin"), "https://client.example");
    auto wrong_method = server.request("/data", "POST", {{"Origin", "https://client.example"}});
    CHECK_EQ(wrong_method.status, 405);
    CHECK_EQ(wrong_method.header("Access-Control-Allow-Origin"), "https://client.example");
}

TEST_CASE("CORS merges multiple Vary fields and wildcard without discarding cache dimensions") {
    MiddlewareServer server;
    server->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        ctx.raw_resp().add_header_pair("Vary", "Accept-Encoding");
        ctx.raw_resp().add_header_pair("Vary", "Accept-Language");
        co_await next();
    });
    server->use(kale::cors());
    server->GET("/data", [](kale::Context &ctx) { ctx.string("ok"); });
    server->GET("/star", [](kale::Context &ctx) { ctx.set_header("Vary", "*"); });
    server.start();
    auto response = server.request("/data", "GET", {{"Origin", "https://any.example"}});
    CHECK_EQ(response.header("Access-Control-Allow-Origin"), "*");
    CHECK_FALSE(response.has("Access-Control-Allow-Credentials"));
    CHECK(token_present(response.header("Vary"), "Accept-Encoding"));
    CHECK(token_present(response.header("Vary"), "Accept-Language"));
    CHECK(token_present(response.header("Vary"), "Origin"));
    CHECK_EQ(server.request("/star").header("Vary"), "*");
}

TEST_CASE("CORS preflight validates origin method and case-insensitive requested header lists") {
    kale::CorsConfig config;
    config.allow_origins = {"https://client.example"};
    config.allow_methods = {"GET", "POST"};
    config.allow_headers = {"X-Custom", "Content-Type", "Authorization"};
    config.allow_credentials = true;
    config.max_age = 7200;
    std::atomic<int> invoked{0};
    MiddlewareServer server;
    server->use(kale::security_headers());
    server->use(kale::cors(config));
    server->OPTIONS("/data", [&invoked](kale::Context &ctx) { ++invoked; ctx.string("ordinary options"); });
    server.start();
    auto allowed = server.request("/missing", "OPTIONS", {
        {"Origin", "https://client.example"}, {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "x-CUSTOM, CONTENT-type"},
        {"Access-Control-Request-Headers", "authorization"}});
    CHECK_EQ(allowed.status, 204);
    CHECK(allowed.body.empty());
    CHECK_EQ(allowed.header("Access-Control-Allow-Origin"), "https://client.example");
    CHECK_EQ(allowed.header("Access-Control-Allow-Credentials"), "true");
    CHECK(token_present(allowed.header("Access-Control-Allow-Headers"), "X-Custom"));
    CHECK(token_present(allowed.header("Access-Control-Allow-Headers"), "Content-Type"));
    CHECK(token_present(allowed.header("Access-Control-Allow-Headers"), "Authorization"));
    CHECK_EQ(allowed.header("Access-Control-Allow-Methods"), "GET, POST");
    CHECK_EQ(allowed.header("Access-Control-Max-Age"), "7200");
    CHECK(token_present(allowed.header("Vary"), "Origin"));
    CHECK(token_present(allowed.header("Vary"), "Access-Control-Request-Method"));
    CHECK(token_present(allowed.header("Vary"), "Access-Control-Request-Headers"));
    CHECK_EQ(allowed.header("X-Content-Type-Options"), "nosniff");
    for (const auto &change : {std::pair<std::string, std::string>{"Origin", "https://denied.example"},
             {"Origin", "https://client.example/"}, {"Access-Control-Request-Method", "DELETE"},
             {"Access-Control-Request-Method", "post"}, {"Access-Control-Request-Method", "POST, GET"},
             {"Access-Control-Request-Headers", "X-Other"}, {"Access-Control-Request-Headers", "X-Custom,"},
             {"Access-Control-Request-Headers", "Bad Header"}, {"Access-Control-Request-Headers", "*"}}) {
        Headers headers{{"Origin", "https://client.example"}, {"Access-Control-Request-Method", "POST"},
                        {"Access-Control-Request-Headers", "X-Custom"}};
        for (auto &[name, value] : headers) if (name == change.first) value = change.second;
        auto denied = server.request("/data", "OPTIONS", std::move(headers));
        CHECK_EQ(denied.status, 403);
        CHECK_FALSE(denied.has("Access-Control-Allow-Origin"));
        CHECK_FALSE(denied.has("Access-Control-Allow-Methods"));
        CHECK_FALSE(denied.has("Access-Control-Allow-Headers"));
        CHECK_EQ(denied.header("X-Frame-Options"), "DENY");
        CHECK(token_present(denied.header("Vary"), "Origin"));
        CHECK(token_present(denied.header("Vary"), "Access-Control-Request-Headers"));
    }
    CHECK_EQ(invoked.load(), 0);
    CHECK_EQ(server.request("/data", "OPTIONS").body, "ordinary options");
    CHECK_EQ(server.request("/data", "OPTIONS", {{"Origin", "https://client.example"}}).body, "ordinary options");
    CHECK_EQ(server.request("/data", "OPTIONS", {{"Access-Control-Request-Method", "POST"}}).body, "ordinary options");
    CHECK_EQ(invoked.load(), 3);
}

TEST_CASE("CORS wildcard reflects validated requested names and requires explicit Authorization") {
    kale::CorsConfig config;
    config.max_age = 0;
    MiddlewareServer server;
    server->use(kale::cors(config));
    server.start();
    auto response = server.request("/any", "OPTIONS", {{"Origin", "https://any.example"},
        {"Access-Control-Request-Method", "PUT"}, {"Access-Control-Request-Headers", "x-one, X-Two"}});
    CHECK_EQ(response.status, 204);
    CHECK_EQ(response.header("Access-Control-Allow-Headers"), "x-one, X-Two");
    CHECK_FALSE(response.has("Access-Control-Max-Age"));
    CHECK_FALSE(response.has("Access-Control-Allow-Credentials"));
    auto auth = server.request("/any", "OPTIONS", {{"Origin", "https://any.example"},
        {"Access-Control-Request-Method", "GET"}, {"Access-Control-Request-Headers", "AUTHORIZATION"}});
    CHECK_EQ(auth.status, 403);
    auto no_headers = server.request("/any", "OPTIONS", {{"Origin", "https://any.example"},
        {"Access-Control-Request-Method", "GET"}});
    CHECK_EQ(no_headers.status, 204);
    CHECK_FALSE(no_headers.has("Access-Control-Allow-Headers"));
}

TEST_CASE("security headers have coherent opt-in defaults and preserve application overrides") {
    MiddlewareServer server;
    server->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        ctx.set_header("X-Frame-Options", "SAMEORIGIN");
        co_await next();
    });
    server->use(kale::security_headers());
    server->GET("/defaults", [](kale::Context &ctx) { ctx.string("defaults"); });
    server->GET("/override", [](kale::Context &ctx) {
        ctx.set_header("Referrer-Policy", "no-referrer");
        ctx.erase_resp_header("Cross-Origin-Resource-Policy");
        ctx.set_status(422).string("rejected");
    });
    server.start();
    for (const auto &path : {"/defaults", "/missing"}) {
        auto response = server.request(path);
        CHECK_EQ(response.status, std::string_view(path) == "/defaults" ? 200 : 404);
        CHECK_EQ(response.header("X-Content-Type-Options"), "nosniff");
        CHECK_EQ(response.header("X-Frame-Options"), "SAMEORIGIN");
        CHECK_EQ(response.header("X-XSS-Protection"), "0");
        CHECK_EQ(response.header("Referrer-Policy"), "strict-origin-when-cross-origin");
        CHECK_EQ(response.header("Cross-Origin-Opener-Policy"), "same-origin");
        CHECK_EQ(response.header("Cross-Origin-Resource-Policy"), "same-origin");
        CHECK_FALSE(response.has("Strict-Transport-Security"));
        CHECK_FALSE(response.has("Content-Security-Policy"));
        CHECK_FALSE(response.has("Permissions-Policy"));
        CHECK_FALSE(response.has("Cross-Origin-Embedder-Policy"));
    }
    auto rejected = server.request("/override");
    CHECK_EQ(rejected.status, 422);
    CHECK_EQ(rejected.header("Referrer-Policy"), "no-referrer");
    CHECK_FALSE(rejected.has("Cross-Origin-Resource-Policy"));
    CHECK_EQ(rejected.header("X-Content-Type-Options"), "nosniff");
    CHECK_EQ(server.request("/defaults", "POST").header("X-Frame-Options"), "SAMEORIGIN");
}

TEST_CASE("security header configuration owns values supports disabling and rejects all control bytes") {
    using Config = kale::SecurityHeadersConfig;
    const std::vector<std::string Config::*> members{
        &Config::frame_options, &Config::xss_protection, &Config::hsts, &Config::referrer_policy,
        &Config::content_security_policy, &Config::permissions_policy, &Config::cross_origin_opener_policy,
        &Config::cross_origin_resource_policy, &Config::cross_origin_embedder_policy};
    for (auto member : members) {
        for (unsigned int byte = 0; byte <= 127; ++byte) {
            if (byte >= 32 && byte != 127) continue;
            Config bad;
            bad.*member = "prefix" + std::string(1, static_cast<char>(byte)) + "suffix";
            CHECK_THROWS_AS(kale::security_headers(bad), std::invalid_argument);
        }
    }
    MiddlewareServer server;
    {
        Config config;
        config.content_type_nosniff = false;
        config.frame_options.clear();
        config.xss_protection.clear();
        config.referrer_policy.clear();
        config.cross_origin_opener_policy.clear();
        config.cross_origin_resource_policy.clear();
        config.hsts = "max-age=60";
        config.content_security_policy = "default-src https:";
        config.permissions_policy = "camera=()";
        config.cross_origin_embedder_policy = "require-corp";
        server->use(kale::security_headers(config));
        config.hsts = "changed";
    }
    server->GET("/configured", [](kale::Context &ctx) { ctx.string("configured"); });
    server.start();
    auto response = server.request("/configured");
    CHECK_EQ(response.header("Strict-Transport-Security"), "max-age=60");
    CHECK_EQ(response.header("Content-Security-Policy"), "default-src https:");
    CHECK_EQ(response.header("Permissions-Policy"), "camera=()");
    CHECK_EQ(response.header("Cross-Origin-Embedder-Policy"), "require-corp");
    CHECK_FALSE(response.has("X-Content-Type-Options"));
    CHECK_FALSE(response.has("X-Frame-Options"));
    CHECK_FALSE(response.has("X-XSS-Protection"));
    CHECK_FALSE(response.has("Referrer-Policy"));
    CHECK_FALSE(response.has("Cross-Origin-Opener-Policy"));
    CHECK_FALSE(response.has("Cross-Origin-Resource-Policy"));
}

TEST_CASE("request ID defaults generate distinct owned values and accept a single bounded incoming value") {
    MiddlewareServer server;
    server->use(kale::request_id());
    install_id_echo(server);
    server.start();
    auto first = server.request("/id");
    auto second = server.request("/id");
    const auto first_id = first.header("X-Request-Id");
    CHECK_EQ(first_id.size(), 32);
    CHECK(std::all_of(first_id.begin(), first_id.end(), [](unsigned char c) {
        return (c >= 48 && c <= 57) || (c >= 97 && c <= 102);
    }));
    CHECK_NE(first_id, second.header("X-Request-Id"));
    CHECK_EQ(first.body, first_id);
    CHECK_EQ(second.body, second.header("X-Request-Id"));
    const std::string boundary(128, 120);
    auto accepted = server.request("/id", "GET", {{"x-ReQuEsT-iD", boundary}});
    CHECK_EQ(accepted.body, boundary);
    CHECK_EQ(accepted.header("X-Request-Id"), boundary);
    auto fallback = server.request("/missing");
    CHECK_EQ(fallback.status, 404);
    CHECK_EQ(fallback.header("X-Request-Id").size(), 32);
}

TEST_CASE("request ID invalid incoming values and duplicate fields use the owned generator") {
    std::atomic<int> generated{0};
    std::atomic<int> middleware_entered{0};
    MiddlewareServer server;
    server->use([&middleware_entered](kale::Context &, kale::Next next) -> coke::Task<void> {
        ++middleware_entered;
        co_await next();
    });
    {
        kale::RequestIdConfig config;
        config.header_name = "X-Correlation";
        config.max_length = 16;
        config.generator = [&generated] { ++generated; return std::string("generated-id"); };
        server->use(kale::request_id(config));
        config.header_name = "changed";
        config.generator = [] { return std::string("changed"); };
    }
    install_id_echo(server);
    server.start();
    for (const auto &headers : {Headers{}, Headers{{"X-Correlation", ""}},
             Headers{{"X-Correlation", std::string(17, 120)}}, Headers{{"X-Correlation", "two words"}},
             Headers{{"X-Correlation", std::string("bad") + char(9) + "id"}},
             Headers{{"X-Correlation", std::string("bad") + char(127)}},
             Headers{{"X-Correlation", "first"}, {"x-correlation", "second"}}}) {
        auto response = server.request("/id", "GET", headers);
        CHECK_EQ(response.status, 200);
        CHECK_EQ(response.header("X-Correlation"), "generated-id");
        CHECK_EQ(response.body, "generated-id");
        CHECK_FALSE(response.has("X-Request-Id"));
    }
    CHECK_EQ(generated.load(), 7);
    CHECK_EQ(middleware_entered.load(), 7);
    // Workflow __parse_header_value rejects byte 0x80 via its signed-char
    // check and sends an empty 400 before application middleware is entered.
    // This wire rejection is separate from selecting an ID on a parsed request.
    auto wire_rejected = server.request("/id", "GET",
        {{"X-Correlation", std::string("bad") + char(128)}});
    CHECK_EQ(wire_rejected.status, 400);
    CHECK_FALSE(wire_rejected.has("X-Correlation"));
    CHECK(wire_rejected.body.empty());
    CHECK_EQ(generated.load(), 7);
    CHECK_EQ(middleware_entered.load(), 7);
    auto accepted = server.request("/id", "GET", {{"X-Correlation", std::string(16, 97)}});
    CHECK_EQ(accepted.body, std::string(16, 97));
    CHECK_EQ(generated.load(), 7);
    CHECK_EQ(middleware_entered.load(), 8);
}

TEST_CASE("request ID rejects unsafe config and invalid generator results before header attachment") {
    kale::RequestIdConfig config;
    for (const std::string name : {"", "bad header", "Bad:Name"}) {
        config.header_name = name;
        CHECK_THROWS_AS(kale::request_id(config), std::invalid_argument);
    }
    config.header_name = "X-Request-Id";
    for (size_t limit : {size_t(0), size_t(31), size_t(4097)}) {
        config.max_length = limit;
        CHECK_THROWS_AS(kale::request_id(config), std::invalid_argument);
    }
    config.generator = [] { return std::string("x"); };
    config.max_length = 1;
    CHECK_NOTHROW(kale::request_id(config));
    for (const auto &bad : {std::string{}, std::string(9, 120), std::string("bad id"),
             std::string("a") + char(0), std::string("a") + char(10), std::string("a") + char(13),
             std::string("a") + char(127), std::string("a") + char(255)}) {
        MiddlewareServer server;
        std::atomic<int> invoked{0};
        kale::RequestIdConfig invalid;
        invalid.max_length = 8;
        invalid.generator = [bad] { return bad; };
        server->use(kale::request_id(invalid));
        server->GET("/id", [&invoked](kale::Context &ctx) { ++invoked; ctx.string("unexpected"); });
        server.start();
        auto response = server.request("/id");
        CHECK_EQ(response.status, 500);
        CHECK_FALSE(response.has("X-Request-Id"));
        CHECK_FALSE(response.has("X-Injected"));
        CHECK_EQ(invoked.load(), 0);
    }
    MiddlewareServer throwing;
    config.max_length = 8;
    config.generator = []() -> std::string { throw kale::HttpError(503, "entropy unavailable"); };
    throwing->use(kale::request_id(config));
    throwing->GET("/id", [](kale::Context &) {});
    throwing.start();
    CHECK_EQ(throwing.request("/id").status, 503);
}

TEST_CASE("access log receives owning route target status duration and selected typed request ID") {
    LogStore logs;
    MiddlewareServer server;
    server->use(kale::access_log([&logs](kale::AccessLogRecord record) { logs.push(std::move(record)); }));
    kale::RequestIdConfig id_config;
    id_config.generator = [] { return std::string("generated-log-id"); };
    server->use(kale::request_id(id_config));
    server->GET("/items/:id", [](kale::Context &ctx) -> coke::Task<void> {
        co_await coke::sleep(2ms);
        ctx.set_status(201).string(ctx.param("id"));
    });
    server.start();
    auto response = server.request("/items/42?include=all", "GET", {{"X-Request-Id", "chosen-id"}});
    CHECK_EQ(response.status, 201);
    CHECK_EQ(response.body, "42");
    CHECK_EQ(server.request("/missing").status, 404);
    CHECK_EQ(server.request("/items/42", "POST").status, 405);
    server.stop();
    const auto records = logs.copy();
    REQUIRE_EQ(records.size(), 3);
    CHECK_EQ(records[0].method, "GET");
    CHECK_EQ(records[0].target, "/items/42?include=all");
    CHECK_EQ(records[0].route_pattern, "/items/:id");
    CHECK_EQ(records[0].status_code, 201);
    CHECK_GE(records[0].duration.count(), 1000);
    CHECK_EQ(records[0].request_id, "chosen-id");
    CHECK_FALSE(records[0].downstream_exception);
    CHECK_FALSE(records[0].response_committed);
    CHECK_EQ(records[1].status_code, 404);
    CHECK(records[1].route_pattern.empty());
    CHECK_EQ(records[1].request_id, "generated-log-id");
    CHECK_EQ(records[2].method, "POST");
    CHECK_EQ(records[2].status_code, 405);
    CHECK(records[2].route_pattern.empty());
    // Every string remains independently owned after request/Context destruction.
    auto retained = records[0];
    CHECK_EQ(retained.target, "/items/42?include=all");
    CHECK_EQ(retained.route_pattern, "/items/:id");
}

TEST_CASE("access log never infers request IDs from unchecked incoming or response headers") {
    LogStore logs;
    MiddlewareServer server;
    server->use(kale::access_log([&logs](kale::AccessLogRecord record) { logs.push(std::move(record)); }));
    server->GET("/unselected", [](kale::Context &ctx) {
        CHECK(kale::get_request_id(ctx).empty());
        ctx.set_header("X-Request-Id", "response-only");
        ctx.string("ok");
    });
    server.start();
    CHECK_EQ(server.request("/unselected", "GET", {{"X-Request-Id", "untrusted"}}).status, 200);
    server.stop();
    const auto records = logs.copy();
    REQUIRE_EQ(records.size(), 1);
    CHECK(records.front().request_id.empty());
}

TEST_CASE("access log sink failures preserve success and original HttpError and unknown exceptions") {
    CHECK_THROWS_AS(kale::access_log({}), std::invalid_argument);
    std::atomic<int> records{0};
    MiddlewareServer server;
    server->use(kale::access_log([&records](kale::AccessLogRecord) {
        ++records;
        throw std::logic_error("sink failure");
    }));
    server->GET("/ok", [](kale::Context &ctx) { ctx.set_status(202).string("accepted"); });
    server->GET("/http-error", [](kale::Context &) { throw kale::HttpError(418, "application error"); });
    server->GET("/std-error", [](kale::Context &) { throw std::runtime_error("application failure"); });
    server->GET("/unknown-error", [](kale::Context &) { throw 17; });
    server.start();
    auto response = server.request("/ok");
    CHECK_EQ(response.status, 202);
    CHECK_EQ(response.body, "accepted");
    CHECK_EQ(server.request("/http-error").status, 418);
    CHECK_EQ(server.request("/std-error").status, 500);
    CHECK_EQ(server.request("/unknown-error").status, 500);
    server.stop();
    CHECK_EQ(records.load(), 4);
}

TEST_CASE("access log reports escaping error policy and preserves the original exception for recovery") {
    LogStore logs;
    std::atomic<int> recovered{0};
    MiddlewareServer server;
    server->use([&recovered](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        try {
            co_await next();
        } catch (const kale::HttpError &error) {
            CHECK_EQ(error.status_code(), 409);
            CHECK_EQ(std::string(error.what()), "original error");
            ++recovered;
            ctx.set_status(error.status_code()).string("recovered");
        }
    });
    server->use(kale::access_log([&logs](kale::AccessLogRecord record) {
        logs.push(std::move(record));
        throw std::logic_error("must not mask");
    }));
    server->GET("/error", [](kale::Context &ctx) {
        ctx.set_status(202);
        throw kale::HttpError(409, "original error");
    });
    server->GET("/std", [](kale::Context &) { throw std::runtime_error("failure"); });
    server.start();
    auto response = server.request("/error");
    CHECK_EQ(response.status, 409);
    CHECK_EQ(response.body, "recovered");
    CHECK_EQ(server.request("/std").status, 500);
    server.stop();
    CHECK_EQ(recovered.load(), 1);
    const auto records = logs.copy();
    REQUIRE_EQ(records.size(), 2);
    CHECK_EQ(records[0].status_code, 409);
    CHECK_EQ(records[1].status_code, 500);
    CHECK(records[0].downstream_exception);
    CHECK(records[1].downstream_exception);
    CHECK_FALSE(records[0].response_committed);
}

TEST_CASE("access log outside recovery samples rendered status without claiming an escaping exception") {
    LogStore logs;
    MiddlewareServer server;
    server->use(kale::access_log([&logs](kale::AccessLogRecord record) { logs.push(std::move(record)); }));
    server->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        try { co_await next(); }
        catch (...) { ctx.set_status(422).string("rendered"); }
    });
    server->GET("/recovered", [](kale::Context &) { throw std::runtime_error("recovered"); });
    server.start();
    CHECK_EQ(server.request("/recovered").status, 422);
    server.stop();
    const auto records = logs.copy();
    REQUIRE_EQ(records.size(), 1);
    CHECK_EQ(records[0].status_code, 422);
    CHECK_FALSE(records[0].downstream_exception);
}

TEST_CASE("middleware observes short circuits early replies and stream commit at return") {
    LogStore logs;
    std::atomic<int> invoked{0};
    MiddlewareServer server;
    server->use(kale::access_log([&logs](kale::AccessLogRecord record) { logs.push(std::move(record)); }));
    server->use(kale::request_id());
    server->use(kale::security_headers());
    server->use(kale::cors());
    server->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        if (ctx.path() == "/short") { ctx.set_status(401).string("short"); co_return; }
        co_await next();
    });
    server->GET("/short", [&invoked](kale::Context &) { ++invoked; });
    server->GET("/early", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_status(202).string("early");
        (void)co_await ctx.reply();
    });
    server->GET("/stream", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_status(206);
        CHECK(co_await ctx.send_chunk("stream"));
        // No end_chunk: ordinary server finalization follows middleware return.
    });
    server.start();
    const Headers origin{{"Origin", "https://client.example"}};
    auto short_response = server.request("/short", "GET", origin);
    auto early_response = server.request("/early", "GET", origin);
    auto stream_response = server.request("/stream", "GET", origin);
    CHECK_EQ(short_response.status, 401);
    CHECK_EQ(early_response.status, 202);
    CHECK_EQ(early_response.body, "early");
    CHECK_EQ(stream_response.status, 206);
    CHECK_EQ(stream_response.body, "stream");
    for (const auto *response : {&short_response, &early_response, &stream_response}) {
        CHECK_EQ(response->header("X-Content-Type-Options"), "nosniff");
        CHECK_EQ(response->header("Access-Control-Allow-Origin"), "*");
        CHECK_FALSE(response->header("X-Request-Id").empty());
    }
    server.stop();
    CHECK_EQ(invoked.load(), 0);
    const auto records = logs.copy();
    REQUIRE_EQ(records.size(), 3);
    auto find_record = [&](const std::string &id) -> const kale::AccessLogRecord * {
        for (const auto &rec : records) {
            if (rec.request_id == id) return &rec;
        }
        return nullptr;
    };
    const auto short_id = short_response.header("X-Request-Id");
    const auto early_id = early_response.header("X-Request-Id");
    const auto stream_id = stream_response.header("X-Request-Id");
    const auto *short_rec = find_record(short_id);
    const auto *early_rec = find_record(early_id);
    const auto *stream_rec = find_record(stream_id);
    REQUIRE_NE(short_rec, nullptr);
    REQUIRE_NE(early_rec, nullptr);
    REQUIRE_NE(stream_rec, nullptr);
    CHECK_EQ(short_rec->status_code, 401);
    CHECK_FALSE(short_rec->response_committed);
    CHECK_EQ(short_rec->request_id, short_id);
    CHECK_EQ(early_rec->status_code, 202);
    CHECK(early_rec->response_committed);
    CHECK_EQ(early_rec->request_id, early_id);
    CHECK_EQ(stream_rec->status_code, 206);
    CHECK(stream_rec->response_committed);
    CHECK_EQ(stream_rec->request_id, stream_id);
}

TEST_CASE("access log records committed status while preserving a later downstream exception") {
    LogStore logs;
    MiddlewareServer server;
    server->use(kale::access_log([&logs](kale::AccessLogRecord record) { logs.push(std::move(record)); }));
    server->GET("/sent", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_status(202).string("already sent");
        (void)co_await ctx.reply();
        throw kale::HttpError(409, "after reply");
    });
    server.start();
    auto response = server.request("/sent");
    CHECK_EQ(response.status, 202);
    CHECK_EQ(response.body, "already sent");
    server.stop();
    const auto records = logs.copy();
    REQUIRE_EQ(records.size(), 1);
    CHECK_EQ(records.front().status_code, 202);
    CHECK(records.front().downstream_exception);
    CHECK(records.front().response_committed);
}

TEST_CASE("CORS preserves explicit fields and downstream grant removal without losing Vary") {
    MiddlewareServer server;
    server->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        ctx.set_header("Access-Control-Allow-Origin", "https://application.example");
        ctx.set_header("Access-Control-Expose-Headers", "X-App");
        co_await next();
    });
    kale::CorsConfig config;
    config.expose_headers = {"X-Default"};
    server->use(kale::cors(config));
    server->GET("/preserve", [](kale::Context &ctx) { ctx.string("preserved"); });
    server->GET("/override", [](kale::Context &ctx) {
        ctx.set_header("Access-Control-Allow-Origin", "https://handler.example");
        ctx.erase_resp_header("Access-Control-Expose-Headers");
    });
    server.start();
    const Headers origin{{"Origin", "https://client.example"}};
    auto preserved = server.request("/preserve", "GET", origin);
    CHECK_EQ(preserved.header("Access-Control-Allow-Origin"), "https://application.example");
    CHECK_EQ(preserved.header("Access-Control-Expose-Headers"), "X-App");
    auto overridden = server.request("/override", "GET", origin);
    CHECK_EQ(overridden.header("Access-Control-Allow-Origin"), "https://handler.example");
    CHECK_FALSE(overridden.has("Access-Control-Expose-Headers"));
    CHECK(token_present(overridden.header("Vary"), "Origin"));
}

TEST_CASE("middleware validates embedded bytes and length-aware header and origin boundaries") {
    for (unsigned int byte = 0; byte <= 127; ++byte) {
        if (byte >= 32 && byte != 127) continue;
        kale::CorsConfig cors;
        cors.allow_origins = {"https://good.example" + std::string(1, static_cast<char>(byte)) + "evil"};
        CHECK_THROWS_AS(kale::cors(cors), std::invalid_argument);
        kale::RequestIdConfig id;
        id.header_name = "X-ID" + std::string(1, static_cast<char>(byte));
        CHECK_THROWS_AS(kale::request_id(id), std::invalid_argument);
    }
    // A real HTTP request enters these tests. Native mutation deliberately places
    // bytes that an HTTP parser may reject before middleware into native fields,
    // proving the factories also reject unsafe internal/native values.
    MiddlewareServer server;
    server->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        if (ctx.path() == "/origin") {
            const std::string origin = std::string("https://good.example") + char(0) + "evil";
            ctx.raw_req().add_header_pair("Origin", origin);
        } else if (ctx.path() == "/id") {
            const char byte = ctx.raw_uri() == "/id?high" ? char(128) : char(0);
            const std::string value = std::string("unsafe") + byte + "id";
            ctx.raw_req().add_header_pair("X-Request-Id", value);
        }
        co_await next();
    });
    server->use(kale::cors());
    kale::RequestIdConfig id;
    id.generator = [] { return std::string("replacement"); };
    server->use(kale::request_id(id));
    server->GET("/origin", [](kale::Context &ctx) { ctx.string("ordinary"); });
    install_id_echo(server);
    server.start();
    auto response = server.request("/origin");
    CHECK_EQ(response.status, 200);
    CHECK_FALSE(response.has("Access-Control-Allow-Origin"));
    CHECK_EQ(response.body, "ordinary");
    // Both NUL and high bytes must be replaced if native application code
    // introduces them after the wire parser has already accepted the request.
    for (const auto *target : {"/id", "/id?high"}) {
        auto replaced = server.request(target);
        CHECK_EQ(replaced.status, 200);
        CHECK_EQ(replaced.header("X-Request-Id"), "replacement");
        CHECK_EQ(replaced.body, "replacement");
    }
}

} // TEST_SUITE
