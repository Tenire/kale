#include "doctest/doctest.h"

#include <charconv>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "coke/http/http_client.h"
#include "coke/sleep.h"
#include "coke/task.h"
#include "coke/wait.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/router.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

namespace {

struct MetadataResponse {
    int state{-1};
    int status{0};
    std::string body;
    std::map<std::string, std::string, kale::CaseInsensitiveLess> headers;
};

coke::Task<MetadataResponse> metadata_request(std::string url, std::string method) {
    auto *task = WFTaskFactory::create_http_task(url, 0, 0, nullptr);
    task->get_req()->set_method(method);
    task->get_req()->add_header_pair("Connection", "close");
    auto result = co_await coke::HttpAwaiter(task);
    MetadataResponse response;
    response.state = result.state;
    if (result.state != 0) co_return response;
    if (const char *status = result.resp.get_status_code()) {
        const std::string_view value(status);
        std::from_chars(value.data(), value.data() + value.size(), response.status);
    }
    if (result.resp.is_chunked()) {
        response.body = protocol::HttpUtil::decode_chunked_body(&result.resp);
    } else {
        const void *body = nullptr;
        size_t size = 0;
        result.resp.get_parsed_body(&body, &size);
        if (body && size) response.body.assign(static_cast<const char *>(body), size);
    }
    protocol::HttpHeaderCursor cursor(&result.resp);
    std::string name, value;
    while (cursor.next(name, value)) response.headers[name] = value;
    co_return response;
}

class MetadataServer {
public:
    explicit MetadataServer(kale::HttpServerParams params = {}) : server_(params) {}
    ~MetadataServer() { server_.stop(); }
    kale::HttpServer *operator->() noexcept { return &server_; }
    void start() {
        REQUIRE_EQ(server_.start("127.0.0.1", 0), 0);
        REQUIRE_GT(server_.listen_port(), 0);
    }
    MetadataResponse request(std::string_view path, std::string method = "GET") {
        return coke::sync_wait(metadata_request(
            "http://127.0.0.1:" + std::to_string(server_.listen_port()) + std::string(path),
            std::move(method)));
    }
private:
    kale::HttpServer server_;
};

void no_metadata_handler(kale::Context &) {}

} // namespace

TEST_CASE("Deferred route captures retain owning public copy and move behavior") {
    static_assert(std::is_same_v<decltype(kale::RouteResult::params),
                  std::vector<std::pair<std::string, std::string>>>);
    const std::string long_name(80, 'n');
    const std::string long_value(160, 'v');
    kale::RouteResult retained;
    {
        kale::Router router;
        std::string pattern = "/owned/:" + long_name + "/:short";
        std::string path = "/owned/" + long_value + "/a%20b+%252F";
        router.GET(pattern, no_metadata_handler);
        retained = router.route(kale::HttpMethod::GET, path);
        REQUIRE(retained.matched);
        CHECK_EQ(retained.params.size(), 2);
        CHECK_EQ(retained.params[0].first, long_name);
        CHECK_EQ(retained.param(long_name), long_value);
        CHECK_EQ(retained.param("short"), "a b+%2F");
        pattern.assign(pattern.size(), 'x');
        path.assign(path.size(), 'x');
    }
    // Only captures are owning; the endpoint pointer is deliberately not used
    // after the Router has been destroyed.
    CHECK_EQ(retained.param(long_name), long_value);
    CHECK_EQ(retained.param("short"), "a b+%2F");
    auto copied = retained;
    CHECK_NE(copied.params[0].second.data(), retained.params[0].second.data());
    retained.params[0].second = "changed";
    CHECK_EQ(copied.param(long_name), long_value);
    const char *long_address = copied.param(long_name).data();
    const char *short_address = copied.param("short").data();
    auto moved = std::move(copied);
    CHECK_EQ(moved.param(long_name).data(), long_address);
    CHECK_EQ(moved.param("short").data(), short_address);
    kale::RouteResult assigned;
    assigned = std::move(moved);
    CHECK_EQ(assigned.param(long_name).data(), long_address);
    CHECK_EQ(assigned.param("short").data(), short_address);
    assigned = assigned;
    CHECK_EQ(assigned.param("short"), "a b+%2F");
}

TEST_CASE("Deferred captures preserve branch priority ordering and repeated names") {
    kale::Router router;
    router.GET("/tree/fixed/:discard/nope", no_metadata_handler);
    router.GET("/tree/:id/:tail/hit", no_metadata_handler);
    router.GET("/choose/:rejected/:discard/nope", no_metadata_handler);
    router.GET("/choose/:selected/:tail/hit", no_metadata_handler);
    router.GET("/repeat/:id/:id", no_metadata_handler);
    router.GET("/tree/static", no_metadata_handler);
    auto result = router.route(kale::HttpMethod::GET, "/tree/fixed/v%20x/hit");
    REQUIRE(result.matched);
    REQUIRE_EQ(result.params.size(), 2);
    CHECK_EQ(result.params[0].first, "id");
    CHECK_EQ(result.params[0].second, "fixed");
    CHECK_EQ(result.params[1].first, "tail");
    CHECK_EQ(result.params[1].second, "v x");
    CHECK_EQ(result.param("discard"), "");
    auto alternate = router.route(kale::HttpMethod::GET, "/choose/a%2520/b%2Fc/hit");
    REQUIRE(alternate.matched);
    REQUIRE_EQ(alternate.params.size(), 2);
    CHECK_EQ(alternate.params[0].first, "selected");
    CHECK_EQ(alternate.params[0].second, "a%20");
    CHECK_EQ(alternate.params[1].first, "tail");
    CHECK_EQ(alternate.params[1].second, "b/c");
    CHECK_EQ(alternate.param("rejected"), "");
    auto repeat = router.route(kale::HttpMethod::GET, "/repeat/first/second");
    REQUIRE_EQ(repeat.params.size(), 2);
    CHECK_EQ(repeat.params[0].second, "first");
    CHECK_EQ(repeat.params[1].second, "second");
    CHECK_EQ(repeat.param("id"), "first");
    auto missing = router.route(kale::HttpMethod::GET, "/choose/a/b/missing");
    CHECK_FALSE(missing.matched);
    CHECK_FALSE(missing.method_not_allowed);
    CHECK(missing.params.empty());
    CHECK(missing.allowed_methods.empty());
    CHECK(router.route(kale::HttpMethod::GET, "/tree/static").params.empty());
}

TEST_CASE("Deferred wildcard captures decode once and keep canonical slash semantics") {
    kale::Router router;
    router.GET("/repo/:prefix/*tail", no_metadata_handler);
    router.GET("/one/:id", no_metadata_handler);
    auto result = router.route(kale::HttpMethod::GET, "/repo/a%20b/x%252Fy//z+/%00/");
    REQUIRE(result.matched);
    REQUIRE_EQ(result.params.size(), 2);
    CHECK_EQ(result.params[0].first, "prefix");
    CHECK_EQ(result.param("prefix"), "a b");
    CHECK_EQ(result.params[1].first, "tail");
    const std::string expected("x%2Fy/z+/\0/", 11);
    CHECK_EQ(result.param("tail"), std::string_view(expected));
    auto empty = router.route(kale::HttpMethod::GET, "/repo/a/");
    CHECK(empty.matched);
    REQUIRE_EQ(empty.params.size(), 2);
    CHECK_EQ(empty.params[1].first, "tail");
    CHECK(empty.params[1].second.empty());
    CHECK_FALSE(router.route(kale::HttpMethod::GET, "/repo/a").matched);
    CHECK_FALSE(router.route(kale::HttpMethod::GET, "/one/").matched);
    auto malformed = router.route(kale::HttpMethod::GET, "/one/%Q0%2%");
    CHECK(malformed.matched);
    CHECK_EQ(malformed.param("id"), "%Q0%2%");
    auto null = router.route(kale::HttpMethod::GET, "/one/%00");
    REQUIRE_EQ(null.param("id").size(), 1);
    CHECK_EQ(null.param("id")[0], '\0');
}

TEST_CASE("Deferred capture misses preserve method metadata and direct node prefix") {
    kale::Router router;
    router.GET("/method/:id", no_metadata_handler);
    router.POST("/method/:id", no_metadata_handler);
    router.GET("/choice/:first", no_metadata_handler);
    router.POST("/choice/:second", no_metadata_handler);
    for (auto method : {kale::HttpMethod::PUT, kale::HttpMethod::HEAD, kale::HttpMethod::OPTIONS}) {
        auto mismatch = router.route(method, "/method/a%20b");
        CHECK_FALSE(mismatch.matched);
        CHECK(mismatch.method_not_allowed);
        CHECK(mismatch.endpoint == nullptr);
        CHECK(mismatch.params.empty());
        REQUIRE_EQ(mismatch.allowed_methods.size(), 2);
        CHECK_EQ(mismatch.allow_header(), "GET, POST");
    }
    // A failed branch may populate Allow before another named branch succeeds.
    auto success = router.route(kale::HttpMethod::POST, "/choice/value");
    CHECK(success.matched);
    CHECK_FALSE(success.method_not_allowed);
    CHECK(success.allowed_methods.empty());
    CHECK_EQ(success.param("second"), "value");
    CHECK_EQ(success.param("first"), "");

    kale::RadixNode root;
    root.add_route(kale::HttpMethod::GET, "/node/:id", kale::make_handler(no_metadata_handler));
    std::vector<std::pair<std::string, std::string>> params{{"prefix", "kept"}};
    const std::string_view segments[]{"node", "v%20x"};
    kale::RouteResult direct;
    CHECK(root.match_route(kale::HttpMethod::GET, segments, 0, params, direct));
    REQUIRE_EQ(params.size(), 2);
    CHECK_EQ(params[0].first, "prefix");
    CHECK_EQ(params[0].second, "kept");
    CHECK_EQ(params[1].first, "id");
    CHECK_EQ(params[1].second, "v x");
    const auto before = params;
    kale::RouteResult failure;
    CHECK_FALSE(root.match_route(kale::HttpMethod::POST, segments, 0, params, failure));
    CHECK(params == before);
    CHECK(failure.method_not_allowed);
}

TEST_CASE("Deferred captures handle heap path segments and stable endpoint metadata") {
    kale::Router router;
    std::string pattern, path;
    for (int i = 0; i < 40; ++i) {
        pattern += "/:n" + std::to_string(i);
        path += "/v" + std::to_string(i);
    }
    auto builder = router.GET(pattern, no_metadata_handler).name("deep").tag("kind", "many");
    auto result = router.route(kale::HttpMethod::GET, path);
    REQUIRE(result.matched);
    REQUIRE_EQ(result.params.size(), 40);
    for (int i = 0; i < 40; ++i) {
        CHECK_EQ(result.params[i].first, "n" + std::to_string(i));
        CHECK_EQ(result.params[i].second, "v" + std::to_string(i));
    }
    const auto *endpoint = result.endpoint;
    router.POST(pattern, no_metadata_handler);
    router.PUT(pattern, no_metadata_handler);
    builder.name("renamed");
    kale::Router moved(std::move(router));
    CHECK_EQ(result.endpoint, endpoint);
    CHECK_EQ(result.route_name(), "renamed");
    CHECK_EQ(result.route_tag("kind"), "many");
    CHECK_EQ(moved.route(kale::HttpMethod::GET, path).endpoint, endpoint);
}

TEST_CASE("Deferred owning captures survive normalized HTTP Context moves and suspension") {
    MetadataServer server;
    server->GET("/move/:id/:tail", [](kale::Context &ctx) -> coke::Task<void> {
        CHECK_EQ(ctx.param("id"), "a b");
        CHECK_EQ(ctx.param("tail"), "v+%2F");
        const char *id = ctx.param("id").data();
        const char *tail = ctx.param("tail").data();
        auto moved = std::make_unique<kale::Context>(std::move(ctx));
        co_await coke::sleep(0.001);
        CHECK_EQ(moved->param("id"), "a b");
        CHECK_EQ(moved->param("tail"), "v+%2F");
        CHECK_EQ(moved->param("id").data(), id);
        CHECK_EQ(moved->param("tail").data(), tail);
        ctx = std::move(*moved);
        CHECK_EQ(ctx.param("id").data(), id);
        CHECK_EQ(ctx.param("tail").data(), tail);
        ctx.string(std::string(ctx.param("id")) + "|" + std::string(ctx.param("tail")));
    });
    server.start();
    for (const char *path : {"/move/a%20b/v+%252F", "/move//a%20b/./v+%252F"}) {
        auto response = server.request(path);
        CHECK_EQ(response.state, 0);
        CHECK_EQ(response.status, 200);
        CHECK_EQ(response.body, "a b|v+%2F");
    }
}

TEST_CASE("Deferred captures retain actual HTTP fallback and decoded traversal boundaries") {
    kale::HttpServerParams params;
    params.clean_path = false;
    MetadataServer server(params);
    server->GET("/back/:discard/nope", no_metadata_handler);
    server->GET("/back/:id/hit", [](kale::Context &ctx) { ctx.string(ctx.param("id")); });
    server->GET("/repeat/:id/:id", [](kale::Context &ctx) { ctx.string(ctx.param("id")); });
    server->GET("/repo/*tail", [](kale::Context &ctx) { ctx.string(ctx.param("tail")); });
    server->GET("/binary/:id", [](kale::Context &ctx) { ctx.string(ctx.param("id")); });
    server->GET("/method/:id", [](kale::Context &ctx) { ctx.string(ctx.param("id")); });
    server->OPTIONS("/explicit/:id", [](kale::Context &ctx) { ctx.string(ctx.param("id")); });
    server->set_not_found_handler([](kale::Context &ctx) {
        CHECK_FALSE(ctx.has_param("discard"));
        CHECK_FALSE(ctx.has_param("id"));
        ctx.set_status(404).string("missing");
    });
    server->set_method_not_allowed_handler([](kale::Context &ctx) {
        CHECK_FALSE(ctx.has_param("id"));
        ctx.set_status(405).string("method");
    });
    server.start();
    CHECK_EQ(server.request("/back/a%20b/hit").body, "a b");
    CHECK_EQ(server.request("/repeat/first/second").body, "first");
    CHECK_EQ(server.request("/repo/a%20b//v+/%252F/").body, "a b/v+/%2F/");
    auto binary = server.request("/binary/a%00b");
    CHECK_EQ(binary.status, 200);
    CHECK_EQ(binary.body, std::string("a\0b", 3));
    auto missing = server.request("/back/value/missing");
    CHECK_EQ(missing.status, 404);
    CHECK_EQ(missing.body, "missing");
    for (const char *method : {"POST", "HEAD", "OPTIONS"}) {
        auto mismatch = server.request("/method/a%20b", method);
        CHECK_EQ(mismatch.status, 405);
        CHECK_EQ(mismatch.headers["Allow"], "GET");
        if (std::string_view(method) != "HEAD") CHECK_EQ(mismatch.body, "method");
    }
    auto options = server.request("/explicit/value", "OPTIONS");
    CHECK_EQ(options.status, 200);
    CHECK_EQ(options.body, "value");
    CHECK_EQ(server.request("/repo/%2e%2e/private").status, 403);
}
