#include "doctest/doctest.h"

#include <array>
#include <charconv>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "coke/http/http_client.h"
#include "coke/task.h"
#include "coke/wait.h"
#include "kale/core/blueprint.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/router.h"
#include "kale/core/http_method.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

namespace {

void enumeration_handler(kale::Context &) {}

constexpr std::array sorted_methods{
    kale::HttpMethod::CONNECT, kale::HttpMethod::DELETE, kale::HttpMethod::GET,
    kale::HttpMethod::HEAD, kale::HttpMethod::OPTIONS, kale::HttpMethod::PATCH,
    kale::HttpMethod::POST, kale::HttpMethod::PUT, kale::HttpMethod::TRACE
};

struct EnumerationResponse {
    int state{-1};
    int status{0};
    std::map<std::string, std::string, std::less<>> headers;
};

coke::Task<EnumerationResponse> enumeration_request(std::string url, std::string method) {
    auto *task = WFTaskFactory::create_http_task(url, 0, 0, nullptr);
    task->get_req()->set_method(method);
    task->get_req()->add_header_pair("Connection", "close");
    auto result = co_await coke::HttpAwaiter(task);
    EnumerationResponse response;
    response.state = result.state;
    if (result.state != 0) co_return response;
    if (const char *status = result.resp.get_status_code()) {
        const std::string_view value(status);
        std::from_chars(value.data(), value.data() + value.size(), response.status);
    }
    protocol::HttpHeaderCursor cursor(&result.resp);
    std::string name, value;
    while (cursor.next(name, value)) response.headers[name] = value;
    co_return response;
}

class EnumerationServer {
public:
    ~EnumerationServer() { server.stop(); }
    kale::HttpServer server;
};

} // namespace

TEST_CASE("Route enumeration returns an empty owning snapshot") {
    static_assert(std::is_same_v<decltype(std::declval<const kale::Router &>().routes()),
                                std::vector<kale::RouteInfo>>);
    static_assert(std::is_same_v<decltype(kale::RouteInfo::pattern), std::string>);
    static_assert(std::is_same_v<decltype(kale::RouteInfo::name), std::string>);
    static_assert(std::is_same_v<decltype(kale::RouteInfo::tags),
                                std::map<std::string, std::string, std::less<>>>);
    const kale::Router router;
    CHECK(router.routes().empty());
}

TEST_CASE("Route enumeration sorts patterns then all nine method names") {
    kale::Router router;
    router.GET("/z", enumeration_handler);
    for (auto it = sorted_methods.rbegin(); it != sorted_methods.rend(); ++it) {
        router.route(*it, "/same", enumeration_handler).name(std::string(kale::method_to_string(*it)));
    }
    router.GET("/a/:id", enumeration_handler);
    router.GET("/", enumeration_handler);
    router.GET("/a/*tail", enumeration_handler);
    const auto snapshot = router.routes();
    REQUIRE_EQ(snapshot.size(), 13);
    CHECK_EQ(snapshot[0].pattern, "/");
    CHECK_EQ(snapshot[1].pattern, "/a/*tail");
    CHECK_EQ(snapshot[2].pattern, "/a/:id");
    for (size_t i = 0; i < sorted_methods.size(); ++i) {
        CHECK(snapshot[i + 3].method == sorted_methods[i]);
        CHECK_EQ(snapshot[i + 3].pattern, "/same");
        CHECK_EQ(snapshot[i + 3].name, kale::method_to_string(sorted_methods[i]));
        CHECK(snapshot[i + 3].tags.empty());
    }
    CHECK_EQ(snapshot.back().pattern, "/z");

    kale::Router reverse;
    reverse.GET("/a/*tail", enumeration_handler);
    reverse.GET("/", enumeration_handler);
    reverse.GET("/a/:id", enumeration_handler);
    for (auto method : sorted_methods) reverse.route(method, "/same", enumeration_handler);
    reverse.GET("/z", enumeration_handler);
    const auto reordered = reverse.routes();
    REQUIRE_EQ(reordered.size(), snapshot.size());
    for (size_t i = 0; i < snapshot.size(); ++i) {
        CHECK_EQ(reordered[i].pattern, snapshot[i].pattern);
        CHECK(reordered[i].method == snapshot[i].method);
    }
}

TEST_CASE("Route enumeration includes every any method without implicit endpoints") {
    kale::Router router;
    router.any("/all", enumeration_handler);
    const auto snapshot = router.routes();
    REQUIRE_EQ(snapshot.size(), sorted_methods.size());
    for (size_t i = 0; i < sorted_methods.size(); ++i) {
        CHECK(snapshot[i].method == sorted_methods[i]);
        CHECK_EQ(snapshot[i].pattern, "/all");
        CHECK(router.route(snapshot[i].method, "/all").matched);
    }
    kale::Router explicit_get;
    explicit_get.GET("/only", enumeration_handler);
    REQUIRE_EQ(explicit_get.routes().size(), 1);
    CHECK(explicit_get.routes().front().method == kale::HttpMethod::GET);
}

TEST_CASE("Route enumeration follows duplicate and path normalization semantics") {
    kale::Router router;
    router.GET("items", enumeration_handler).name("original");
    CHECK_THROWS_AS(router.GET("/items", enumeration_handler), std::logic_error);
    CHECK_THROWS_AS(router.GET("//items", enumeration_handler), std::logic_error);
    CHECK_THROWS_AS(router.GET("/ghost/*tail/invalid", enumeration_handler), std::invalid_argument);
    router.POST("/items", enumeration_handler).name("create");
    router.GET("/items/", enumeration_handler).name("trailing");
    router.GET("/files//:id", enumeration_handler);
    const auto snapshot = router.routes();
    REQUIRE_EQ(snapshot.size(), 4);
    CHECK_EQ(snapshot[0].pattern, "/files//:id");
    CHECK_EQ(snapshot[1].pattern, "/items");
    CHECK_EQ(snapshot[1].name, "original");
    CHECK(snapshot[1].method == kale::HttpMethod::GET);
    CHECK_EQ(snapshot[2].pattern, "/items");
    CHECK(snapshot[2].method == kale::HttpMethod::POST);
    CHECK_EQ(snapshot[3].pattern, "/items/");
    CHECK_EQ(snapshot[3].name, "trailing");
    auto result = router.route(kale::HttpMethod::GET, "/files/42");
    REQUIRE(result.matched);
    CHECK_EQ(result.route_pattern(), snapshot[0].pattern);
}

TEST_CASE("Route enumeration preserves nested mounted names and tag precedence") {
    kale::Blueprint child;
    child.tag("tier", "child").tag("child", "yes");
    child.GET("", enumeration_handler).name("index");
    child.GET("/", enumeration_handler).name("slash");
    auto declaration = child.GET("files/*tail", enumeration_handler);
    declaration.name("download").tag("tier", "route").tag("empty", "");
    kale::Blueprint parent;
    parent.tag("tier", "parent").tag("parent", "yes");
    parent.mount("//v1///", child);
    kale::Router router;
    router.mount("//api///", parent);
    router.mount("/mirror", child);
    declaration.name("changed declaration").tag("tier", "changed");
    const auto snapshot = router.routes();
    REQUIRE_EQ(snapshot.size(), 6);
    const std::array expected_patterns{"/api/v1", "/api/v1/", "/api/v1/files/*tail",
                                       "/mirror", "/mirror/", "/mirror/files/*tail"};
    for (size_t i = 0; i < snapshot.size(); ++i) {
        CHECK_EQ(snapshot[i].pattern, expected_patterns[i]);
        CHECK_EQ(snapshot[i].tags.at("child"), "yes");
        if (i < 3) CHECK_EQ(snapshot[i].tags.at("parent"), "yes");
        else CHECK_EQ(snapshot[i].tags.count("parent"), 0);
        const bool wildcard = i % 3 == 2;
        CHECK_EQ(snapshot[i].name, wildcard ? "download" : (i % 3 == 0 ? "index" : "slash"));
        CHECK_EQ(snapshot[i].tags.at("tier"), wildcard ? "route" : "child");
        if (wildcard) CHECK_EQ(snapshot[i].tags.at("empty"), "");
    }
    const auto matched = router.route(kale::HttpMethod::GET, "/api/v1/files/a/b");
    REQUIRE(matched.matched);
    CHECK_EQ(matched.route_pattern(), snapshot[2].pattern);
    CHECK_EQ(matched.route_name(), snapshot[2].name);
    CHECK_EQ(matched.route_tag("tier"), snapshot[2].tags.at("tier"));
}

TEST_CASE("Route snapshots detach builder changes and retain stable endpoints") {
    kale::Router router;
    auto builder = router.GET("/stable", enumeration_handler);
    builder.name("before").tag("state", "before");
    auto matched = router.route(kale::HttpMethod::GET, "/stable");
    REQUIRE(matched.matched);
    const auto *endpoint = matched.endpoint;
    const auto before = router.routes();
    for (auto method : sorted_methods) {
        if (method != kale::HttpMethod::GET) router.route(method, "/stable", enumeration_handler);
    }
    for (int i = 0; i < 32; ++i) router.GET("/other/" + std::to_string(i), enumeration_handler);
    builder.name("after").tag("state", "after").tag("new", "yes");
    REQUIRE_EQ(before.size(), 1);
    CHECK_EQ(before.front().name, "before");
    CHECK_EQ(before.front().tags.at("state"), "before");
    CHECK_EQ(before.front().tags.count("new"), 0);
    CHECK_EQ(router.route(kale::HttpMethod::GET, "/stable").endpoint, endpoint);
    CHECK_EQ(matched.route_name(), "after");
    CHECK_EQ(matched.route_tag("state"), "after");
    auto after = router.routes();
    REQUIRE_EQ(after.size(), 41);
    for (auto &info : after) {
        if (info.pattern == "/stable" && info.method == kale::HttpMethod::GET) {
            CHECK_EQ(info.name, "after");
            CHECK_EQ(info.tags.at("new"), "yes");
            info.name = "local edit";
            info.tags["state"] = "local edit";
        }
    }
    CHECK_EQ(matched.route_name(), "after");
    CHECK_EQ(matched.route_tag("state"), "after");
}

TEST_CASE("Copied route snapshots outlive router moves and destruction") {
    const std::string pattern = "/" + std::string(96, char(112)) + "/:id";
    const std::string name(128, char(110));
    const std::string key(80, char(107));
    const std::string value(192, char(118));
    std::vector<kale::RouteInfo> retained;
    {
        kale::Router router;
        router.GET(pattern, enumeration_handler).name(name).tag(key, value);
        auto snapshot = router.routes();
        retained = snapshot;
        snapshot.front().pattern = "local";
        snapshot.front().tags.clear();
        kale::Router moved(std::move(router));
        const auto moved_snapshot = moved.routes();
        REQUIRE_EQ(moved_snapshot.size(), 1);
        CHECK_EQ(moved_snapshot.front().pattern, pattern);
        kale::Router assigned;
        assigned = std::move(moved);
        REQUIRE_EQ(assigned.routes().size(), 1);
    }
    REQUIRE_EQ(retained.size(), 1);
    CHECK(retained.front().method == kale::HttpMethod::GET);
    CHECK_EQ(retained.front().pattern, pattern);
    CHECK_EQ(retained.front().name, name);
    CHECK_EQ(retained.front().tags.at(key), value);
}

TEST_CASE("Enumerated mounted route metadata matches actual HTTP Context metadata") {
    EnumerationServer fixture;
    auto &server = fixture.server;
    auto handler = [](kale::Context &ctx) {
        ctx.set_header("X-Route-Method", ctx.method_string());
        ctx.set_header("X-Route-Pattern", ctx.route_pattern());
        ctx.set_header("X-Route-Name", ctx.route_name());
        ctx.set_header("X-Route-Tier", ctx.route_tag("tier"));
        ctx.set_header("X-Route-Scope", ctx.route_tag("scope"));
        ctx.string("ok");
    };
    kale::Blueprint child;
    child.tag("tier", "child");
    child.GET("/users/:id", handler).name("users.show").tag("tier", "route");
    child.POST("/users/:id", handler).name("users.update");
    child.HEAD("/users/:id", handler).name("users.head");
    child.OPTIONS("/users/:id", handler).name("users.options");
    child.GET("/files/*tail", handler).name("files");
    kale::Blueprint group;
    group.tag("scope", "api");
    group.mount("/v1/", std::move(child));
    server.mount("/api/", std::move(group));
    server.GET("/", handler).name("root").tag("tier", "root").tag("scope", "public");
    const kale::HttpServer &view = server;
    const auto snapshot = view.router().routes();
    REQUIRE_EQ(snapshot.size(), 6);
    REQUIRE_EQ(server.start("127.0.0.1", 0), 0);
    REQUIRE_GT(server.listen_port(), 0);
    const auto base = "http://127.0.0.1:" + std::to_string(server.listen_port());
    for (const auto &info : snapshot) {
        const std::string path = info.pattern == "/" ? "/" :
            (info.pattern == "/api/v1/files/*tail" ? "/api/v1/files/a/b.txt" : "/api/v1/users/42");
        const auto response = coke::sync_wait(enumeration_request(
            base + path, std::string(kale::method_to_string(info.method))));
        REQUIRE_EQ(response.state, 0);
        REQUIRE_EQ(response.status, 200);
        CHECK_EQ(response.headers.at("X-Route-Method"), kale::method_to_string(info.method));
        CHECK_EQ(response.headers.at("X-Route-Pattern"), info.pattern);
        CHECK_EQ(response.headers.at("X-Route-Name"), info.name);
        CHECK_EQ(response.headers.at("X-Route-Tier"), info.tags.at("tier"));
        CHECK_EQ(response.headers.at("X-Route-Scope"), info.tags.at("scope"));
    }
}
