#include "doctest/doctest.h"

#include "kale/core/context.h"
#include "kale/core/router.h"
#include "kale/core/http_server.h"
#include "kale/http/json.h"
#include "kale/core/cookie.h"
#include "kale/core/http_status.h"
#include "kale/core/http_method.h"
#include "kale/core/http_util.h"
#include "kale/http/sse.h"
#include "kale/core/blueprint.h"
#include "kale/middleware/error_handler.h"
#include "kale/core/logger.h"
#include "kale/http/field_desc.h"
#include "kale/http/json_reader.h"
#include "kale/http/binding.h"
#include <atomic>
#include <charconv>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <typeinfo>

#include "coke/http/http_client.h"
#include "coke/sleep.h"
#include "coke/task.h"
#include "coke/wait.h"
#include "workflow/HttpUtil.h"

namespace {

struct LayerTestResponse {
    int state{0};
    int error{0};
    int status{0};
    std::string status_str;
    std::string body;
    std::map<std::string, std::string, kale::CaseInsensitiveLess> headers;

    bool ok() const noexcept { return state == 0; }

    std::string header(std::string_view key) const {
        auto it = headers.find(key);
        return (it != headers.end()) ? it->second : std::string{};
    }

    kale::Json json() const {
        return kale::Json::parse(body);
    }

    static LayerTestResponse from_coke(coke::HttpResult &res) {
        LayerTestResponse out;
        out.state = res.state;
        out.error = res.error;
        if (res.state == 0) {
            const char *sc = res.resp.get_status_code();
            if (sc) {
                out.status_str = sc;
                std::from_chars(out.status_str.data(), out.status_str.data() + out.status_str.size(), out.status);
            }
            if (res.resp.is_chunked()) {
                out.body = protocol::HttpUtil::decode_chunked_body(&res.resp);
            } else {
                const void *body_ptr = nullptr;
                size_t body_len = 0;
                if (res.resp.get_parsed_body(&body_ptr, &body_len) && body_ptr && body_len > 0) {
                    out.body.assign(static_cast<const char *>(body_ptr), body_len);
                }
            }
            protocol::HttpHeaderCursor cursor(&res.resp);
            std::string name, val;
            while (cursor.next(name, val)) {
                out.headers[name] = val;
            }
        }
        return out;
    }
};

using HeaderPairList = std::vector<std::pair<std::string, std::string>>;

inline coke::Task<LayerTestResponse> do_http_request(
    std::string url,
    std::string method = "GET",
    HeaderPairList headers = {},
    std::string body = "") {
    auto *task = WFTaskFactory::create_http_task(url, 0, 1, nullptr);
    auto *req = task->get_req();
    req->set_method(method);
    for (const auto &pair : headers) {
        req->add_header_pair(pair.first, pair.second);
    }
    if (!body.empty()) {
        req->append_output_body(body.data(), body.size());
    }
    auto res = co_await coke::HttpAwaiter(task);
    co_return LayerTestResponse::from_coke(res);
}

class LayerServerFixture {
public:
    explicit LayerServerFixture(kale::HttpServerParams params = {})
        : server_(params) {
        server_.set_logger(kale::Logger::off());
        int ret = server_.start("127.0.0.1", 0);
        REQUIRE_EQ(ret, 0);
        REQUIRE(server_.is_running());
        port_ = static_cast<unsigned short>(server_.listen_port());
        REQUIRE_GT(port_, 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(port_);
    }

    ~LayerServerFixture() {
        if (server_.is_running()) {
            server_.stop();
        }
    }

    LayerServerFixture(const LayerServerFixture &) = delete;
    LayerServerFixture &operator=(const LayerServerFixture &) = delete;

    kale::HttpServer *operator->() noexcept { return &server_; }
    kale::HttpServer &server() noexcept { return server_; }
    unsigned short port() const noexcept { return port_; }
    const std::string &base_url() const noexcept { return base_url_; }

    std::string url(std::string_view path) const {
        if (path.empty() || path.front() != '/') {
            return base_url_ + "/" + std::string(path);
        }
        return base_url_ + std::string(path);
    }

    LayerTestResponse request(
        std::string_view path,
        std::string method = "GET",
        HeaderPairList headers = {},
        std::string body = "") const {
        return coke::sync_wait(
            do_http_request(url(path), std::move(method), std::move(headers), std::move(body))
        );
    }

    LayerTestResponse get(std::string_view path, HeaderPairList headers = {}) const {
        return request(path, "GET", std::move(headers), "");
    }

    LayerTestResponse post(std::string_view path, std::string body = "", HeaderPairList headers = {}) const {
        return request(path, "POST", std::move(headers), std::move(body));
    }

private:
    kale::HttpServer server_;
    unsigned short port_{0};
    std::string base_url_;
};

// Custom exception types for error handling and application middleware testing
struct BaseCustomError : public std::exception {
    std::string msg;
    explicit BaseCustomError(std::string m = "base") : msg(std::move(m)) {}
    const char *what() const noexcept override { return msg.c_str(); }
};

struct DerivedCustomError : public BaseCustomError {
    int code;
    explicit DerivedCustomError(std::string m = "derived", int c = 42)
        : BaseCustomError(std::move(m)), code(c) {}
};

struct UnhandledError : public std::exception {
    const char *what() const noexcept override { return "unhandled"; }
};

// DTO Definitions for Phase 4 Testing
struct AddressDTO {
    std::string city;
    int zip{0};

    KALE_BIND(
        KALE_FIELD(city),
        KALE_FIELD(zip)
    )
};

struct UserProfileDTO {
    std::string name;
    std::string category;
    int age{0};
    double score{0.0};
    bool active{false};
    std::optional<std::string> nickname;
    std::optional<int> loyalty_points;
    AddressDTO address;
    std::vector<std::string> tags;
    std::vector<int> scores;

    KALE_BIND(
        KALE_FIELD(name),
        KALE_FIELD(category),
        KALE_FIELD(age),
        KALE_FIELD(score),
        KALE_FIELD(active),
        KALE_FIELD(nickname),
        KALE_FIELD(loyalty_points),
        KALE_FIELD(address),
        KALE_FIELD(tags),
        KALE_FIELD(scores)
    )
};

struct UserAccountDTO {
    std::string username;
    int age{0};

    KALE_BIND(
        KALE_FIELD(username),
        KALE_FIELD(age)
    )
};

} // namespace

// ============================================================================
// Phase 1 Tests: Precise Headers Self-Containment and Type Integrity
// ============================================================================
TEST_CASE("Phase 1: Precise headers provide required functional types") {
    // 1. Core precise headers provide Context, Router, HttpServer
    kale::HttpServer server;
    kale::Router &router = server.router();
    CHECK_FALSE(server.is_running());

    router.GET("/precise_core", [](kale::Context &ctx) {
        ctx.string("core_ok");
    });

    // 2. HTTP precise headers provide Json, HttpStatus, HttpMethod, Cookie, etc.
    kale::Json obj = kale::Json::Object{};
    obj["status"] = static_cast<int>(kale::HttpStatus::OK);
    obj["method"] = std::string(kale::method_to_string(kale::HttpMethod::POST));
    CHECK(obj["status"].get<int>() == 200);
    CHECK(obj["method"].get<std::string>() == "POST");

    kale::Cookie c("session", "abc123xyz");
    CHECK(c.name() == "session");
    CHECK(c.value() == "abc123xyz");

    // 3. kale/http/sse.h provides SSE classes
    kale::SseEvent evt{.id = "", .event = "ping", .data = "payload", .retry = -1, .comment = ""};
    CHECK(evt.event == "ping");
    CHECK(evt.data == "payload");

    // 4. kale/core/blueprint.h provides self-contained Blueprint
    kale::Blueprint bp;
    bp.GET("/bp_test", [](kale::Context &ctx) { ctx.string("bp"); });
    CHECK_EQ(bp.routes().size(), 1);
    // 5. kale/middleware/error_handler.h provides self-contained error_handler middleware
    auto eh_mw = kale::error_handler();
    // 6. kale/http/binding.h provides self-contained JsonBindError deriving JsonInputError and Context::bind_json
    kale::JsonBindError bind_err("test_field", "unique key", "duplicate object key in JSON");
    CHECK_EQ(bind_err.field(), "test_field");
    CHECK_EQ(bind_err.expected(), "unique key");
    CHECK_EQ(bind_err.reason(), "duplicate object key in JSON");
    CHECK_EQ(bind_err.status_code(), 400);
    const kale::JsonInputError &input_err = bind_err;
    CHECK_EQ(input_err.status_code(), 400);
    const kale::HttpError &http_err = bind_err;
    CHECK_EQ(http_err.status_code(), 400);
}

// ============================================================================
// Phase 2 Tests: Blueprint temporary mount, duplicate rejection, nesting
// ============================================================================
TEST_CASE("Phase 2: Blueprint temporary mount, duplicate rejection, and inheritance") {
    SUBCASE("Blueprint mount accepts temporary rvalue and registers routes") {
        LayerServerFixture fixture;
        std::vector<std::string> call_log;

        kale::Blueprint bp;
        bp.GET("/item", [&call_log](kale::Context &ctx) {
            call_log.push_back("item");
            ctx.string("item_ok");
        });

        // Mount as temporary rvalue
        fixture->mount("/api/v1", std::move(bp));

        // Mount another temporary directly inline
        fixture->mount("/temp", kale::Blueprint().GET("/ping", [](kale::Context &ctx) {
            ctx.string("pong");
        }));

        auto res1 = fixture.get("/api/v1/item");
        CHECK(res1.status == 200);
        CHECK(res1.body == "item_ok");

        auto res2 = fixture.get("/temp/ping");
        CHECK(res2.status == 200);
        CHECK(res2.body == "pong");
    }

    SUBCASE("Blueprint duplicate registration throws std::logic_error") {
        kale::Router router;
        kale::Blueprint bp;
        bp.GET("/duplicate", [](kale::Context &ctx) { ctx.string("first"); });
        bp.GET("/duplicate", [](kale::Context &ctx) { ctx.string("second"); });

        // Registering duplicate route on the same router must throw
        CHECK_THROWS_AS(bp.register_into(router, ""), std::logic_error);
    }

    SUBCASE("Duplicate mount collision between two blueprints throws std::logic_error") {
        kale::Router router;
        kale::Blueprint bp1;
        bp1.GET("/collision", [](kale::Context &ctx) { ctx.string("bp1"); });

        kale::Blueprint bp2;
        bp2.GET("/collision", [](kale::Context &ctx) { ctx.string("bp2"); });

        bp1.register_into(router, "/prefix");
        CHECK_THROWS_AS(bp2.register_into(router, "/prefix"), std::logic_error);
    }

    SUBCASE("Nested middleware order, tag, name inheritance and independent handler mounts") {
        LayerServerFixture fixture;
        std::vector<std::string> sequence;

        // Root Blueprint with mw1 and tag
        kale::Blueprint root_bp;
        root_bp.tag("tier", "root");
        root_bp.tag("env", "prod");
        root_bp.use([&sequence](kale::Context &, kale::Next next) -> coke::Task<void> {
            sequence.push_back("root_mw_in");
            co_await next();
            sequence.push_back("root_mw_out");
        });

        // Child Blueprint with mw2 and overriding tag
        kale::Blueprint child_bp;
        child_bp.tag("tier", "child"); // overrides root tier
        child_bp.tag("child_attr", "active");
        child_bp.use([&sequence](kale::Context &, kale::Next next) -> coke::Task<void> {
            sequence.push_back("child_mw_in");
            co_await next();
            sequence.push_back("child_mw_out");
        });

        // Route in child Blueprint with route-level middleware and tags
        child_bp.GET("/action",
                     [&sequence](kale::Context &, kale::Next next) -> coke::Task<void> {
                         sequence.push_back("route_mw_in");
                         co_await next();
                         sequence.push_back("route_mw_out");
                     },
                     [&sequence](kale::Context &ctx) {
                         sequence.push_back("handler");
                         std::string tier = std::string(ctx.route_tag("tier"));
                         std::string env = std::string(ctx.route_tag("env"));
                         std::string child_attr = std::string(ctx.route_tag("child_attr"));
                         std::string name = std::string(ctx.route_name());
                         ctx.json(kale::Json::Object{
                             {"tier", tier},
                             {"env", env},
                             {"child_attr", child_attr},
                             {"name", name}
                         });
                     })
                .name("child_action_route")
                .tag("tier", "route_specific"); // overrides child tier

        // Independent second route mounted on same child blueprint
        child_bp.GET("/independent", [](kale::Context &ctx) {
            ctx.string(std::string(ctx.route_name()));
        }).name("independent_route");

        root_bp.mount("/nested", std::move(child_bp));
        fixture->mount("/v1", std::move(root_bp));

        // Execute request to nested route
        sequence.clear();
        auto res = fixture.get("/v1/nested/action");
        CHECK(res.status == 200);

        // Verify onion middleware execution order:
        // root_mw_in -> child_mw_in -> route_mw_in -> handler -> route_mw_out -> child_mw_out -> root_mw_out
        REQUIRE_EQ(sequence.size(), 7);
        CHECK(sequence[0] == "root_mw_in");
        CHECK(sequence[1] == "child_mw_in");
        CHECK(sequence[2] == "route_mw_in");
        CHECK(sequence[3] == "handler");
        CHECK(sequence[4] == "route_mw_out");
        CHECK(sequence[5] == "child_mw_out");
        CHECK(sequence[6] == "root_mw_out");

        // Verify tag inheritance: route > child > root
        kale::Json data = res.json();
        CHECK(data["tier"].get<std::string>() == "route_specific");
        CHECK(data["env"].get<std::string>() == "prod");
        CHECK(data["child_attr"].get<std::string>() == "active");
        CHECK(data["name"].get<std::string>() == "child_action_route");

        // Verify independent route preserves its own name
        auto res2 = fixture.get("/v1/nested/independent");
        CHECK(res2.status == 200);
        CHECK(res2.body == "independent_route");
    }
}

// ============================================================================
// Phase 3 Tests: Error Handling & Application Exception Mapping Semantics
// ============================================================================
TEST_CASE("Phase 3: Error handling middleware framing, status, and async recovery") {
    SUBCASE("Mapped exception, framing headers cleared, application exception dispatch") {
        kale::HttpServerParams params;
        params.set_error_formatter([](kale::Context &c, const kale::ErrorContext &ec) {
            c.set_status(ec.status_code);
            c.string(ec.message);
        });
        LayerServerFixture fixture(params);
        // Domain exception mapping middleware using standard reply_error
        fixture->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            try {
                co_await next();
            } catch (const DerivedCustomError &e) {
                ctx.set_header("X-Error-Handled", "derived");
                auto err = kale::HttpError(kale::HttpStatus::UnprocessableEntity, "handled derived: " + std::to_string(e.code));
                kale::reply_error(ctx, err);
            } catch (const BaseCustomError &e) {
                ctx.set_header("X-Error-Handled", "base");
                auto err = kale::HttpError::bad_request("handled base: " + std::string(e.what()));
                kale::reply_error(ctx, err);
            }
        });

        fixture->GET("/throw-derived", [](kale::Context &ctx) {
            // Set stale headers that must be cleared by reply_error
            ctx.set_header("Content-Type", "application/octet-stream");
            ctx.set_header("Content-Length", "999");
            throw DerivedCustomError("failed_derived", 101);
        });

        fixture->GET("/throw-base", [](kale::Context &ctx) {
            ctx.set_header("Content-Type", "text/csv");
            throw BaseCustomError("failed_base");
        });

        auto res_derived = fixture.get("/throw-derived");
        CHECK(res_derived.status == 422);
        CHECK(res_derived.header("X-Error-Handled") == "derived");
        CHECK(res_derived.body == "handled derived: 101");
        // Verify Content-Length reflects the error body, not stale 999
        CHECK(res_derived.header("Content-Length") == std::to_string(res_derived.body.size()));

        auto res_base = fixture.get("/throw-base");
        CHECK(res_base.status == 400);
        CHECK(res_base.header("X-Error-Handled") == "base");
        CHECK(res_base.body == "handled base: failed_base");
    }

    SUBCASE("Async error handler executes properly") {
        LayerServerFixture fixture;

        fixture->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            std::exception_ptr ep;
            try {
                co_await next();
            } catch (...) {
                ep = std::current_exception();
            }
            if (ep) {
                bool is_derived = false;
                try {
                    std::rethrow_exception(ep);
                } catch (const DerivedCustomError &) {
                    is_derived = true;
                }
                if (is_derived) {
                    co_await coke::sleep(std::chrono::milliseconds(1));
                    try {
                        std::rethrow_exception(ep);
                    } catch (const DerivedCustomError &e) {
                        ctx.set_status(kale::HttpStatus::ServiceUnavailable);
                        ctx.set_header("X-Async-Handler", "true");
                        ctx.string("async error handled: " + std::to_string(e.code));
                    }
                }
            }
        });
        fixture->GET("/throw-async", [](kale::Context &) {
            throw DerivedCustomError("async_failure", 503);
        });

        auto res = fixture.get("/throw-async");
        CHECK(res.status == 503);
        CHECK(res.header("X-Async-Handler") == "true");
        CHECK(res.body == "async error handled: 503");
    }

    SUBCASE("Mapper-throws fallback escapes non-recursively to server error boundary") {
        LayerServerFixture fixture;

        fixture->use([](kale::Context &, kale::Next next) -> coke::Task<void> {
            co_await next();
        });
        fixture->use([](kale::Context &, kale::Next next) -> coke::Task<void> {
            try {
                co_await next();
            } catch (const DerivedCustomError &) {
                throw std::runtime_error("error within error handler");
            }
        });

        fixture->GET("/mapper-fails", [](kale::Context &) {
            throw DerivedCustomError("trigger_mapper", 1);
        });

        // Must escape to outer server error handler producing 500 without crashing or looping
        auto res = fixture.get("/mapper-fails");
        CHECK(res.status == 500);
    }

    SUBCASE("Unhandled exception escapes to server default 500 boundary") {
        LayerServerFixture fixture;

        fixture->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            try {
                co_await next();
            } catch (const DerivedCustomError &) {
                ctx.string("caught derived");
            }
        });

        fixture->GET("/throw-unhandled", [](kale::Context &) {
            throw UnhandledError();
        });

        auto res = fixture.get("/throw-unhandled");
        CHECK(res.status == 500);
    }
}
// ============================================================================
// Phase 4 Tests: DTO Validation & Binding Semantics
// ============================================================================
TEST_CASE("Phase 4: DTO valid binding and full validation failure coverage") {
    SUBCASE("Valid DTO binding: required, optional present/missing, nested, arrays, string ownership") {
        LayerServerFixture fixture;

        fixture->POST("/users/bind", [](kale::Context &ctx) {
            // Parse DTO inside nested scope so any local JSON parser structures are destroyed
            UserProfileDTO parsed_dto;
            {
                parsed_dto = ctx.bind_json<UserProfileDTO>();
            }

            // Retain post-bind lifetime assertion: copy from parsed DTO after bind scope exits
            std::string name_val = parsed_dto.name;
            std::string cat_val = parsed_dto.category;
            std::string nickname_val = parsed_dto.nickname.value_or("none");
            std::string city_val = parsed_dto.address.city;

            ctx.json(kale::Json::Object{
                {"name", name_val},
                {"category", cat_val},
                {"age", parsed_dto.age},
                {"score", parsed_dto.score},
                {"active", parsed_dto.active},
                {"has_nickname", parsed_dto.nickname.has_value()},
                {"nickname", nickname_val},
                {"has_loyalty", parsed_dto.loyalty_points.has_value()},
                {"loyalty_points", parsed_dto.loyalty_points.value_or(0)},
                {"city", city_val},
                {"zip", parsed_dto.address.zip},
                {"tags_count", static_cast<int>(parsed_dto.tags.size())},
                {"scores_count", static_cast<int>(parsed_dto.scores.size())}
            });
        });
        std::string valid_payload = R"({
            "name": "Alice Developer",
            "category": "engineer",
            "age": 30,
            "score": 98.5,
            "active": true,
            "nickname": "ali",
            "loyalty_points": null,
            "address": {
                "city": "Metropolis",
                "zip": 12345
            },
            "tags": ["c++", "networking", "coroutines"],
            "scores": [10, 20, 30]
        })";

        auto res = fixture.post("/users/bind", valid_payload, {{"Content-Type", "application/json"}});
        CHECK(res.status == 200);

        kale::Json out = res.json();
        CHECK(out["name"].get<std::string>() == "Alice Developer");
        CHECK(out["category"].get<std::string>() == "engineer");
        CHECK(out["age"].get<int>() == 30);
        CHECK(out["score"].get<double>() == 98.5);
        CHECK(out["active"].get<bool>() == true);
        CHECK(out["has_nickname"].get<bool>() == true);
        CHECK(out["nickname"].get<std::string>() == "ali");
        CHECK(out["has_loyalty"].get<bool>() == false);
        CHECK(out["city"].get<std::string>() == "Metropolis");
        CHECK(out["zip"].get<int>() == 12345);
        CHECK(out["tags_count"].get<int>() == 3);
        CHECK(out["scores_count"].get<int>() == 3);
    }

    SUBCASE("Missing required field throws JsonBindError with field path") {
        LayerServerFixture fixture;
        fixture->POST("/bind", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserProfileDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"expected", err.expected()},
                    {"reason", err.reason()}
                });
            }
        });

        // 'age' is missing from payload
        std::string payload = R"({
            "name": "Bob",
            "category": "admin",
            "score": 50.0,
            "active": true,
            "address": {"city": "Town", "zip": 1000},
            "tags": [],
            "scores": []
        })";

        auto res = fixture.post("/bind", payload, {{"Content-Type", "application/json"}});
        CHECK(res.status == 400);
        kale::Json err = res.json();
        CHECK(err["field"].get<std::string>() == "age");
        CHECK(err["expected"].get<std::string>() == "present");
    }

    SUBCASE("Null for required field throws JsonBindError") {
        LayerServerFixture fixture;
        fixture->POST("/bind", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserProfileDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"expected", err.expected()},
                    {"reason", err.reason()}
                });
            }
        });

        // 'name' is required but given as null
        std::string payload = R"({
            "name": null,
            "category": "admin",
            "age": 25,
            "score": 50.0,
            "active": true,
            "address": {"city": "Town", "zip": 1000},
            "tags": [],
            "scores": []
        })";

        auto res = fixture.post("/bind", payload, {{"Content-Type", "application/json"}});
        CHECK(res.status == 400);
        kale::Json err = res.json();
        CHECK(err["field"].get<std::string>() == "name");
        CHECK(err["expected"].get<std::string>() == "non-null");
    }

    SUBCASE("Type mismatch throws JsonBindError") {
        LayerServerFixture fixture;
        fixture->POST("/bind", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserProfileDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"reason", err.reason()}
                });
            }
        });

        // 'age' expects int, provided as string
        std::string payload = R"({
            "name": "Charlie",
            "category": "admin",
            "age": "not_a_number",
            "score": 50.0,
            "active": true,
            "address": {"city": "Town", "zip": 1000},
            "tags": [],
            "scores": []
        })";

        auto res = fixture.post("/bind", payload, {{"Content-Type", "application/json"}});
        CHECK(res.status == 400);
        kale::Json err = res.json();
        CHECK(err["field"].get<std::string>() == "age");
        CHECK(err["reason"].get<std::string>() == "value is not a number");
    }

    SUBCASE("Numeric range overflow throws JsonBindError") {
        LayerServerFixture fixture;
        fixture->POST("/bind", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserProfileDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"reason", err.reason()}
                });
            }
        });

        // 1e20 exceeds 32-bit signed integer range for 'age'
        std::string payload = R"({
            "name": "Overflow",
            "category": "admin",
            "age": 1e20,
            "score": 50.0,
            "active": true,
            "address": {"city": "Town", "zip": 1000},
            "tags": [],
            "scores": []
        })";

        auto res = fixture.post("/bind", payload, {{"Content-Type", "application/json"}});
        CHECK(res.status == 400);
        kale::Json err = res.json();
        CHECK(err["field"].get<std::string>() == "age");
        CHECK(err["reason"].get<std::string>() == "numeric value out of range");
    }

    SUBCASE("Malformed JSON throws JsonBindError") {
        LayerServerFixture fixture;
        fixture->POST("/bind", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserProfileDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"expected", err.expected()}
                });
            }
        });

        std::string bad_json = "{ name: incomplete ";

        auto res = fixture.post("/bind", bad_json, {{"Content-Type", "application/json"}});
        CHECK(res.status == 400);
        kale::Json err = res.json();
        CHECK(err["field"].get<std::string>() == "_root");
        CHECK(err["expected"].get<std::string>() == "valid JSON");
    }

    SUBCASE("Non-object root throws JsonBindError") {
        LayerServerFixture fixture;
        fixture->POST("/bind", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserProfileDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"expected", err.expected()},
                    {"reason", err.reason()}
                });
            }
        });

        // Root is JSON array, not object
        std::string array_json = "[1, 2, 3]";

        auto res = fixture.post("/bind", array_json, {{"Content-Type", "application/json"}});
        CHECK(res.status == 400);
        kale::Json err = res.json();
        CHECK(err["field"].get<std::string>() == "_root");
        CHECK(err["expected"].get<std::string>() == "object");
        CHECK(err["reason"].get<std::string>() == "root JSON value must be an object");
    }

    SUBCASE("Empty request body throws JsonBindError") {
        LayerServerFixture fixture;
        fixture->POST("/bind", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserProfileDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"reason", err.reason()}
                });
            }
        });

        auto res = fixture.post("/bind", "", {{"Content-Type", "application/json"}});
        CHECK(res.status == 400);
        kale::Json err = res.json();
        CHECK(err["field"].get<std::string>() == "_root");
        CHECK(err["reason"].get<std::string>() == "empty request body");
    }

    SUBCASE("Nested object failure provides dotted field path") {
        LayerServerFixture fixture;
        fixture->POST("/bind", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserProfileDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"reason", err.reason()}
                });
            }
        });

        // address.zip is missing inside nested object
        std::string payload = R"({
            "name": "Nested Fail",
            "category": "dev",
            "age": 28,
            "score": 90.0,
            "active": true,
            "address": {"city": "Nowhere"},
            "tags": [],
            "scores": []
        })";

        auto res = fixture.post("/bind", payload, {{"Content-Type", "application/json"}});
        CHECK(res.status == 400);
        kale::Json err = res.json();
        CHECK(err["field"].get<std::string>() == "address.zip");
    }

    SUBCASE("Array element failure provides indexed path") {
        LayerServerFixture fixture;
        fixture->POST("/bind", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserProfileDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"reason", err.reason()}
                });
            }
        });

        // Element 1 of scores array is not a number
        std::string payload = R"({
            "name": "Array Fail",
            "category": "dev",
            "age": 28,
            "score": 90.0,
            "active": true,
            "address": {"city": "Nowhere", "zip": 123},
            "tags": ["ok"],
            "scores": [10, "bad_elem", 30]
        })";

        auto res = fixture.post("/bind", payload, {{"Content-Type", "application/json"}});
        CHECK(res.status == 400);
        kale::Json err = res.json();
        CHECK(err["field"].get<std::string>() == "scores[1]");
    }

    SUBCASE("Duplicate key at root throws JsonBindError and maps to HTTP 400 without invoking business service") {
        LayerServerFixture fixture;
        std::atomic<bool> service_invoked{false};

        fixture->POST("/users/account", [&service_invoked](kale::Context &ctx) {
            auto dto = ctx.bind_json<UserAccountDTO>();
            // Business service:
            service_invoked.store(true);
            ctx.string("user:" + dto.username);
        });

        // 1. Valid payload invokes business service and returns 200
        std::string valid_payload = R"({"username": "alice", "age": 30})";
        auto res_valid = fixture.post("/users/account", valid_payload, {{"Content-Type", "application/json"}});
        CHECK(res_valid.status == 200);
        CHECK(service_invoked.load() == true);
        CHECK(res_valid.body == "user:alice");

        // Reset invocation tracker
        service_invoked.store(false);

        // 2. Duplicate root key 'username': throws JsonBindError, maps to 400 at server boundary, business service never invoked
        std::string dup_root_payload = R"({"username":"alice","username":"bob","age":30})";
        auto res_dup = fixture.post("/users/account", dup_root_payload, {{"Content-Type", "application/json"}});
        CHECK(res_dup.status == 400);
        CHECK(service_invoked.load() == false);

        // 3. Inspect structured JsonBindError directly at root
        fixture->POST("/users/account/inspect", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserAccountDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"expected", err.expected()},
                    {"reason", err.reason()}
                });
            }
        });

        auto res_inspect = fixture.post("/users/account/inspect", dup_root_payload, {{"Content-Type", "application/json"}});
        CHECK(res_inspect.status == 400);
        kale::Json err = res_inspect.json();
        CHECK(err["field"].get<std::string>() == "username");
        CHECK(err["expected"].get<std::string>() == "unique key");
        CHECK(err["reason"].get<std::string>() == "duplicate object key in JSON");
    }

    SUBCASE("Duplicate key in nested DTO throws JsonBindError and maps to HTTP 400 without invoking business service") {
        LayerServerFixture fixture;
        std::atomic<bool> service_invoked{false};

        fixture->POST("/users/nested-dup", [&service_invoked](kale::Context &ctx) {
            auto dto = ctx.bind_json<UserProfileDTO>();
            // Business service:
            service_invoked.store(true);
            ctx.string("ok");
        });

        // Duplicate 'city' in nested 'address'
        std::string dup_nested_payload = R"({
            "name": "Charlie",
            "category": "engineer",
            "age": 28,
            "score": 95.0,
            "active": true,
            "address": {
                "city": "Metropolis",
                "city": "Gotham",
                "zip": 12345
            },
            "tags": [],
            "scores": []
        })";

        // Throws JsonBindError, maps to 400 at server boundary, business service never invoked
        auto res = fixture.post("/users/nested-dup", dup_nested_payload, {{"Content-Type", "application/json"}});
        CHECK(res.status == 400);
        CHECK(service_invoked.load() == false);

        // Inspect structured JsonBindError directly on nested object
        fixture->POST("/users/nested-dup/inspect", [](kale::Context &ctx) {
            try {
                auto dto = ctx.bind_json<UserProfileDTO>();
                ctx.string("ok");
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"field", err.field()},
                    {"expected", err.expected()},
                    {"reason", err.reason()}
                });
            }
        });

        auto res_inspect = fixture.post("/users/nested-dup/inspect", dup_nested_payload, {{"Content-Type", "application/json"}});
        CHECK(res_inspect.status == 400);
        kale::Json err = res_inspect.json();
        CHECK(err["field"].get<std::string>() == "address.city");
        CHECK(err["expected"].get<std::string>() == "unique key");
        CHECK(err["reason"].get<std::string>() == "duplicate object key in JSON");
    }

    SUBCASE("Unknown keys that are not duplicated remain ignored by default") {
        LayerServerFixture fixture;
        fixture->POST("/users/unknown-keys", [](kale::Context &ctx) {
            auto dto = ctx.bind_json<UserAccountDTO>();
            ctx.string("user:" + dto.username + ",age:" + std::to_string(dto.age));
        });

        std::string payload = R"({
            "username": "alice",
            "age": 30,
            "extra_field": "ignored",
            "another_extra": 12345
        })";

        auto res = fixture.post("/users/unknown-keys", payload, {{"Content-Type", "application/json"}});
        CHECK(res.status == 200);
        CHECK(res.body == "user:alice,age:30");
    }
}

TEST_CASE("Phase 1: Temporary Blueprint chain with group tag/use/mount builder lifetime") {
    SUBCASE("Temporary Blueprint with group tag chained into route") {
        LayerServerFixture fixture;
        auto builder = kale::Blueprint().tag("group_tag", "group_val").GET("/chained_temp", [](kale::Context &ctx) {
            std::string gt(ctx.route_tag("group_tag"));
            std::string rt(ctx.route_tag("route_tag"));
            std::string rn(ctx.route_name());
            ctx.string("name:" + rn + ",gt:" + gt + ",rt:" + rt);
        });
        builder.name("temp_name");
        builder.tag("route_tag", "route_val");

        kale::Blueprint bp = std::move(builder);
        fixture->mount("/test", std::move(bp));

        // Observable verification:
        // 1. Router route matching observes endpoint, name, and inherited tags
        auto match = fixture->router().route(kale::HttpMethod::GET, "/test/chained_temp");
        CHECK(match.matched);
        CHECK_EQ(match.route_name(), "temp_name");
        CHECK_EQ(match.route_tag("group_tag"), "group_val");
        CHECK_EQ(match.route_tag("route_tag"), "route_val");

        // 2. HTTP invocation executes handler and verifies metadata exposed in Context
        auto res = fixture.get("/test/chained_temp");
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, "name:temp_name,gt:group_val,rt:route_val");
    }

    SUBCASE("Temporary Blueprint with group use chained into route") {
        LayerServerFixture fixture;
        std::vector<std::string> sequence;
        auto mw = [&sequence](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            sequence.push_back("group_mw_in");
            ctx.set_header("X-Group-MW", "active");
            co_await next();
            sequence.push_back("group_mw_out");
        };
        auto builder = kale::Blueprint().use(mw).GET("/chained_mw", [&sequence](kale::Context &ctx) {
            sequence.push_back("handler");
            ctx.string("mw_handler_ok");
        });
        builder.name("temp_mw_name");
        kale::Blueprint bp = std::move(builder);
        fixture->mount("/mw_test", std::move(bp));

        // Observable verification of route and middleware execution
        auto match = fixture->router().route(kale::HttpMethod::GET, "/mw_test/chained_mw");
        CHECK(match.matched);
        CHECK_EQ(match.route_name(), "temp_mw_name");

        sequence.clear();
        auto res = fixture.get("/mw_test/chained_mw");
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, "mw_handler_ok");
        CHECK_EQ(res.header("X-Group-MW"), "active");
        REQUIRE_EQ(sequence.size(), 3);
        CHECK_EQ(sequence[0], "group_mw_in");
        CHECK_EQ(sequence[1], "handler");
        CHECK_EQ(sequence[2], "group_mw_out");
    }

    SUBCASE("Temporary Blueprint with group mount chained into route") {
        LayerServerFixture fixture;
        kale::Blueprint child;
        child.GET("/child_route", [](kale::Context &ctx) { ctx.string("child_ok"); });
        auto builder = kale::Blueprint().mount("/sub", std::move(child)).GET("/parent_route", [](kale::Context &ctx) {
            ctx.string("parent_ok");
        });
        builder.name("parent_name");
        kale::Blueprint bp = std::move(builder);
        fixture->mount("/api", std::move(bp));

        // Observable verification of both parent and child mounted routes
        auto match_parent = fixture->router().route(kale::HttpMethod::GET, "/api/parent_route");
        CHECK(match_parent.matched);
        CHECK_EQ(match_parent.route_name(), "parent_name");

        auto match_child = fixture->router().route(kale::HttpMethod::GET, "/api/sub/child_route");
        CHECK(match_child.matched);

        auto res_parent = fixture.get("/api/parent_route");
        CHECK_EQ(res_parent.status, 200);
        CHECK_EQ(res_parent.body, "parent_ok");

        auto res_child = fixture.get("/api/sub/child_route");
        CHECK_EQ(res_child.status, 200);
        CHECK_EQ(res_child.body, "child_ok");
    }
}

TEST_CASE("Phase 2: Blueprint RouteBuilder conversion to RouteBuilder lifetime") {
    LayerServerFixture fixture;

    // Disallowing temporary Blueprint route builder conversion to non-owning router RouteBuilder
    static_assert(!std::is_convertible_v<decltype(std::declval<kale::Blueprint>().GET("/temp", [](kale::Context &) {})), kale::RouteBuilder>,
                  "Rvalue Blueprint::RouteBuilder must not convert to non-owning kale::RouteBuilder");

    // Safe idiomatic pattern: auto retains the owning Blueprint::RouteBuilder
    auto rb = kale::Blueprint().GET("/convert_temp", [](kale::Context &ctx) {
        ctx.string("converted_ok");
    });
    rb.name("assigned_name");
    rb.tag("tag_k", "tag_v");
    rb.max_body_size(4096);
    kale::Blueprint bp_temp = std::move(rb);
    fixture->mount("/temp_bp", std::move(bp_temp));

    auto match_temp = fixture->router().route(kale::HttpMethod::GET, "/temp_bp/convert_temp");
    CHECK(match_temp.matched);
    CHECK_EQ(match_temp.route_name(), "assigned_name");
    CHECK_EQ(match_temp.route_tag("tag_k"), "tag_v");

    auto res_temp = fixture.get("/temp_bp/convert_temp");
    CHECK_EQ(res_temp.status, 200);
    CHECK_EQ(res_temp.body, "converted_ok");

    // Control case with lvalue Blueprint to observe expected builder behavior
    kale::Blueprint bp_ctrl;
    auto ctrl_builder = bp_ctrl.GET("/ctrl", [](kale::Context &ctx) {
        ctx.string("ctrl_ok");
    });
    kale::RouteBuilder rb_ctrl = ctrl_builder;
    rb_ctrl.name("ctrl_name");
    rb_ctrl.tag("ctrl_k", "ctrl_v");
    fixture->mount("/bp", std::move(bp_ctrl));

    auto match_ctrl = fixture->router().route(kale::HttpMethod::GET, "/bp/ctrl");
    CHECK(match_ctrl.matched);
    CHECK_EQ(match_ctrl.route_name(), "ctrl_name");
    CHECK_EQ(match_ctrl.route_tag("ctrl_k"), "ctrl_v");

    auto res_ctrl = fixture.get("/bp/ctrl");
    CHECK_EQ(res_ctrl.status, 200);
    CHECK_EQ(res_ctrl.body, "ctrl_ok");
}

TEST_CASE("Phase 3: Error handling middleware execution and const qualification") {
    LayerServerFixture fixture;

    int sync_run_count = 0;
    int async_run_count = 0;

    fixture->use([&sync_run_count, &async_run_count](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        std::exception_ptr ep;
        try {
            co_await next();
        } catch (...) {
            ep = std::current_exception();
        }
        if (ep) {
            bool is_invalid_arg = false;
            bool is_range_err = false;
            try {
                std::rethrow_exception(ep);
            } catch (const std::invalid_argument &) {
                is_invalid_arg = true;
            } catch (const std::range_error &) {
                is_range_err = true;
            }

            if (is_invalid_arg) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::invalid_argument &e) {
                    ++sync_run_count;
                    ctx.set_status(kale::HttpStatus::BadRequest);
                    ctx.set_header("X-Handled-By", "const_sync");
                    ctx.string("sync_handled:" + std::string(e.what()));
                }
            } else if (is_range_err) {
                ++async_run_count;
                co_await coke::sleep(std::chrono::milliseconds(1));
                try {
                    std::rethrow_exception(ep);
                } catch (const std::range_error &e) {
                    ctx.set_status(kale::HttpStatus::UnprocessableEntity);
                    ctx.set_header("X-Handled-By", "const_async");
                    ctx.string("async_handled:" + std::string(e.what()));
                }
            }
        }
    });
    fixture->GET("/throw_sync_const", [](kale::Context &) {
        throw std::invalid_argument("invalid_arg_payload");
    });

    fixture->GET("/throw_async_const", [](kale::Context &) {
        throw std::range_error("range_error_payload");
    });

    auto res_sync = fixture.get("/throw_sync_const");
    CHECK(sync_run_count == 1);
    CHECK(res_sync.status == 400);
    CHECK(res_sync.header("X-Handled-By") == "const_sync");

    auto res_async = fixture.get("/throw_async_const");
    CHECK(async_run_count == 1);
    CHECK(res_async.status == 422);
    CHECK(res_async.header("X-Handled-By") == "const_async");
}

TEST_CASE("Phase 3: Base exception mapping dynamic derived info preservation") {
    LayerServerFixture fixture;

    bool dynamic_cast_succeeded = false;
    bool typeid_matched_derived = false;
    int derived_code_observed = 0;

    fixture->use([&](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        std::exception_ptr ep;
        try {
            co_await next();
        } catch (...) {
            ep = std::current_exception();
        }
        if (ep) {
            bool is_base = false;
            try {
                std::rethrow_exception(ep);
            } catch (const BaseCustomError &) {
                is_base = true;
            }
            if (is_base) {
                co_await coke::sleep(std::chrono::milliseconds(1));
                try {
                    std::rethrow_exception(ep);
                } catch (const BaseCustomError &e) {
                    const auto *derived = dynamic_cast<const DerivedCustomError*>(&e);
                    if (derived) {
                        dynamic_cast_succeeded = true;
                        derived_code_observed = derived->code;
                    }
                    if (typeid(e) == typeid(DerivedCustomError)) {
                        typeid_matched_derived = true;
                    }
                    ctx.set_status(kale::HttpStatus::InternalServerError);
                    ctx.string("handled base: " + std::string(e.what()));
                }
            }
        }
    });

    fixture->GET("/throw_derived_for_base_mapper", [](kale::Context &) {
        throw DerivedCustomError("derived_failure_msg", 999);
    });

    auto res = fixture.get("/throw_derived_for_base_mapper");
    CHECK(res.status == 500);
    CHECK(dynamic_cast_succeeded);
    CHECK(typeid_matched_derived);
    CHECK(derived_code_observed == 999);
}
TEST_CASE("Phase 4: Context::bind_json exception classification and standard 400 Bad Request mapping") {
    SUBCASE("Uncaught JsonBindError maps to standard 400 Bad Request with framing length 11") {
        LayerServerFixture fixture;
        fixture->POST("/bind_uncaught", [](kale::Context &ctx) {
            auto dto = ctx.bind_json<UserAccountDTO>();
            ctx.string("ok:" + dto.username);
        });

        // Empty body triggers JsonBindError which derives from JsonInputError -> HTTP 400
        auto res = fixture.post("/bind_uncaught", "", {{"Content-Type", "application/json"}});
        CHECK_EQ(res.status, 400);
        CHECK_EQ(res.body, "Bad Request");
        CHECK_EQ(res.body.size(), 11);
        CHECK_EQ(res.header("Content-Length"), "11");
    }

    SUBCASE("Direct throw of JsonInputError in handler maps to standard 400 Bad Request") {
        LayerServerFixture fixture;
        fixture->POST("/direct_input_error", [](kale::Context &) {
            throw kale::JsonInputError("direct validation failure");
        });

        auto res = fixture.post("/direct_input_error", "{}");
        CHECK_EQ(res.status, 400);
        CHECK_EQ(res.body, "Bad Request");
        CHECK_EQ(res.body.size(), 11);
        CHECK_EQ(res.header("Content-Length"), "11");
    }
}
