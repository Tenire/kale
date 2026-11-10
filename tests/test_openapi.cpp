#include "doctest/doctest.h"

#include <kale/core/context.h>
#include <kale/core/http_server.h>
#include <kale/core/router.h>
#include <kale/http/field_desc.h>
#include <kale/http/validator.h>
#include <kale/http/openapi.h>
#include <kale/http/json.h>

#include <coke/http/http_client.h>
#include <coke/task.h>
#include <coke/wait.h>
#include <nlohmann/json.hpp>
#include <workflow/HttpUtil.h>
#include <workflow/WFTaskFactory.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace kale::rules;

struct Reply {
    int state{-1};
    int status{0};
    std::string body;
    std::map<std::string, std::string, std::less<>> headers;
};

coke::Task<Reply> request(std::string url, std::string method = "GET") {
    auto *task = WFTaskFactory::create_http_task(url, 0, 0, nullptr);
    task->get_req()->set_method(method);
    task->get_req()->add_header_pair("Connection", "close");
    auto result = co_await coke::HttpAwaiter(task);
    Reply reply;
    reply.state = result.state;
    if (result.state != 0) co_return reply;
    if (const char *status = result.resp.get_status_code()) {
        const std::string_view code(status);
        std::from_chars(code.data(), code.data() + code.size(), reply.status);
    }
    if (result.resp.is_chunked()) {
        reply.body = protocol::HttpUtil::decode_chunked_body(&result.resp);
    } else {
        const void *body = nullptr;
        std::size_t size = 0;
        result.resp.get_parsed_body(&body, &size);
        if (body && size) reply.body.assign(static_cast<const char *>(body), size);
    }
    protocol::HttpHeaderCursor cursor(&result.resp);
    std::string name, value;
    while (cursor.next(name, value)) reply.headers[name] = value;
    co_return reply;
}

class TestServer {
public:
    ~TestServer() { server.stop(); }
    void start() { REQUIRE_EQ(server.start("127.0.0.1", 0), 0); }
    Reply get(std::string path, std::string method = "GET") {
        auto response = coke::sync_wait(request(
            "http://127.0.0.1:" + std::to_string(server.listen_port()) + path, method));
        REQUIRE_EQ(response.state, 0);
        return response;
    }
    kale::HttpServer server;
};

// DTO fixtures
struct AddressDTO {
    std::string street;
    std::string city;
    int zip_code{0};

    KALE_BIND(
        KALE_FIELD(street, min_len(2), max_len(100)),
        KALE_FIELD(city, min_len(1)),
        KALE_FIELD(zip_code, gte(10000), lte(99999))
    )
};

struct CreateUserRequest {
    std::string username;
    int age{0};
    int64_t account_id{0};
    uint64_t score{0};
    std::string email;
    std::string id_v7;
    std::optional<std::string> bio;
    std::optional<std::string> nickname;
    std::vector<std::string> tags;
    AddressDTO address;

    KALE_BIND(
        KALE_FIELD(username, min_len(3), max_len(20), pattern("^[a-zA-Z0-9_]+$")),
        KALE_FIELD(age, gte(18), lte(100)),
        KALE_FIELD(account_id, gt(10000000000LL)),
        KALE_FIELD(score, lte(18446744073709551615ULL)),
        KALE_FIELD(email, is_email()),
        KALE_FIELD(id_v7, is_uuid_v7()),
        KALE_FIELD(bio, max_len(120)),
        KALE_FIELD(nickname, required(), max_len(50)),
        KALE_FIELD(tags, min_len(1), max_len(10)),
        KALE_FIELD(address)
    )
};

struct QueryParamsDTO {
    int page{1};
    int limit{20};
    std::string status{"active"};
    std::vector<std::string> filters;

    KALE_BIND(
        KALE_FIELD(page, gte(1)),
        KALE_FIELD(limit, range(1, 100)),
        KALE_FIELD(status, one_of<std::string>({"active", "inactive", "pending"})),
        KALE_FIELD(filters, max_len(5))
    )
};

struct UserResponse {
    int id{0};
    std::string username;
    std::string email;

    KALE_BIND(
        KALE_FIELD(id, gt(0)),
        KALE_FIELD(username),
        KALE_FIELD(email)
    )
};

// Recursive DTO fixture
struct RecursiveDTO {
    std::string name;
    std::vector<RecursiveDTO> children;

    KALE_BIND(
        KALE_FIELD(name),
        KALE_FIELD(children)
    )
};

// Contradictory bounds fixtures
struct ContradictoryBoundsDTO {
    int val{0};
    KALE_BIND(
        KALE_FIELD(val, gte(100), lte(50))
    )
};

struct ContradictoryOneOfDTO {
    std::string role;
    KALE_BIND(
        KALE_FIELD(role, one_of<std::string>({"admin", "user"}), one_of<std::string>({"guest"}))
    )
};

struct CustomRuleDTO {
    std::string code;
    KALE_BIND(
        KALE_FIELD(code, custom([](std::string_view s) { return s.size() == 4; }))
    )
};

struct InvalidQueryDTO {
    AddressDTO addr;
    KALE_BIND(
        KALE_FIELD(addr)
    )
};

} // namespace

TEST_SUITE("openapi") {

TEST_CASE("DTO Schema Reflection with faithful types, constraints and extensions") {
    auto schema = kale::reflect_schema<CreateUserRequest>();
    CHECK_EQ(schema.type, "object");
    CHECK_EQ(schema.properties.size(), 10);
    // Non-optional fields + optional field with required() are in required list
    // (username, age, account_id, score, email, id_v7, nickname, tags, address) -> 9 fields
    CHECK_EQ(schema.required.size(), 9);

    SUBCASE("String constraints and extensions") {
        REQUIRE_EQ(schema.properties.count("username"), 1);
        const auto &u = schema.properties["username"];
        CHECK_EQ(u.type, "string");
        CHECK_FALSE(u.nullable);
        REQUIRE_EQ(u.extensions.count("x-kale-min-byte-length"), 1);
        CHECK_EQ(u.extensions.at("x-kale-min-byte-length").u64_val, 3);
        REQUIRE_EQ(u.extensions.count("x-kale-max-byte-length"), 1);
        CHECK_EQ(u.extensions.at("x-kale-max-byte-length").u64_val, 20);
        REQUIRE_EQ(u.extensions.count("x-kale-patterns"), 1);
        CHECK_EQ(u.extensions.at("x-kale-patterns").array_val.size(), 1);
        CHECK_EQ(u.extensions.at("x-kale-patterns").array_val[0].str_val, "^[a-zA-Z0-9_]+$");
    }

    SUBCASE("Exact integer bounds without double narrowing") {
        REQUIRE_EQ(schema.properties.count("age"), 1);
        const auto &age = schema.properties["age"];
        CHECK_EQ(age.type, "integer");
        CHECK_EQ(age.format, "int32");
        REQUIRE(age.minimum.has_value());
        CHECK_EQ(age.minimum->as_double(), doctest::Approx(18.0));
        CHECK_FALSE(age.minimum->exclusive);
        REQUIRE(age.maximum.has_value());
        CHECK_EQ(age.maximum->as_double(), doctest::Approx(100.0));
        CHECK_FALSE(age.maximum->exclusive);

        REQUIRE_EQ(schema.properties.count("account_id"), 1);
        const auto &acc = schema.properties["account_id"];
        CHECK_EQ(acc.type, "integer");
        CHECK_EQ(acc.format, "int64");
        REQUIRE(acc.minimum.has_value());
        CHECK_EQ(acc.minimum->i64, 10000000000LL);
        CHECK(acc.minimum->exclusive);

        REQUIRE_EQ(schema.properties.count("score"), 1);
        const auto &score = schema.properties["score"];
        CHECK_EQ(score.type, "integer");
        CHECK_EQ(score.format, "uint64");
        REQUIRE(score.maximum.has_value());
        CHECK_EQ(score.maximum->u64, 18446744073709551615ULL);
    }

    SUBCASE("Formats and version-specific extensions") {
        REQUIRE_EQ(schema.properties.count("email"), 1);
        const auto &email = schema.properties["email"];
        CHECK_EQ(email.format, "email");
        REQUIRE_EQ(email.extensions.count("x-kale-format"), 1);
        CHECK_EQ(email.extensions.at("x-kale-format").str_val, "email");

        REQUIRE_EQ(schema.properties.count("id_v7"), 1);
        const auto &id_v7 = schema.properties["id_v7"];
        CHECK_EQ(id_v7.format, "uuid");
        REQUIRE_EQ(id_v7.extensions.count("x-kale-uuid-version"), 1);
        CHECK_EQ(id_v7.extensions.at("x-kale-uuid-version").i64_val, 7);
    }

    SUBCASE("Optional fields vs RequiredRule on optional") {
        REQUIRE_EQ(schema.properties.count("bio"), 1);
        const auto &bio = schema.properties["bio"];
        CHECK(bio.nullable);
        CHECK(std::find(schema.required.begin(), schema.required.end(), "bio") == schema.required.end());
        CHECK_EQ(bio.extensions.at("x-kale-max-byte-length").u64_val, 120);

        REQUIRE_EQ(schema.properties.count("nickname"), 1);
        const auto &nick = schema.properties["nickname"];
        CHECK_FALSE(nick.nullable);
        CHECK(std::find(schema.required.begin(), schema.required.end(), "nickname") != schema.required.end());
    }

    SUBCASE("Vector length maps to minItems / maxItems") {
        REQUIRE_EQ(schema.properties.count("tags"), 1);
        const auto &tags = schema.properties["tags"];
        CHECK_EQ(tags.type, "array");
        REQUIRE(tags.items);
        CHECK_EQ(tags.items->type, "string");
        CHECK_EQ(tags.min_items.value_or(0), 1);
        CHECK_EQ(tags.max_items.value_or(0), 10);
    }

    SUBCASE("Nested DTO reflection") {
        REQUIRE_EQ(schema.properties.count("address"), 1);
        const auto &addr = schema.properties["address"];
        CHECK_EQ(addr.type, "object");
        CHECK_EQ(addr.properties.size(), 3);
        CHECK_EQ(addr.required.size(), 3);
        REQUIRE_EQ(addr.properties.count("street"), 1);
        REQUIRE_EQ(addr.properties.count("city"), 1);
        REQUIRE_EQ(addr.properties.count("zip_code"), 1);
        CHECK_EQ(addr.properties.at("zip_code").minimum->as_double(), doctest::Approx(10000.0));
        CHECK_EQ(addr.properties.at("zip_code").maximum->as_double(), doctest::Approx(99999.0));
    }

    SUBCASE("Discrete Enum and Range reflection in QueryParamsDTO") {
        auto q_schema = kale::reflect_schema<QueryParamsDTO>();
        REQUIRE_EQ(q_schema.properties.count("status"), 1);
        const auto &status = q_schema.properties["status"];
        CHECK_EQ(status.enum_values.size(), 3);
        CHECK_EQ(status.enum_values[0].str_val, "active");
        CHECK_EQ(status.enum_values[1].str_val, "inactive");
        CHECK_EQ(status.enum_values[2].str_val, "pending");

        REQUIRE_EQ(q_schema.properties.count("limit"), 1);
        const auto &limit = q_schema.properties["limit"];
        CHECK_EQ(limit.minimum->as_double(), doctest::Approx(1.0));
        CHECK_EQ(limit.maximum->as_double(), doctest::Approx(100.0));
    }
}

TEST_CASE("Schema reflection edge cases: custom rule, contradictory bounds, recursive DTO") {
    SUBCASE("Custom rule is opaque and annotated without fabricated schema") {
        auto s = kale::reflect_schema<CustomRuleDTO>();
        REQUIRE_EQ(s.properties.count("code"), 1);
        const auto &code = s.properties["code"];
        REQUIRE_EQ(code.extensions.count("x-kale-custom-validation"), 1);
        CHECK(code.extensions.at("x-kale-custom-validation").bool_val);
    }

    SUBCASE("Contradictory numeric bounds throw std::invalid_argument") {
        CHECK_THROWS_AS(kale::reflect_schema<ContradictoryBoundsDTO>(), std::invalid_argument);
    }

    SUBCASE("Contradictory one_of choices throw std::invalid_argument") {
        CHECK_THROWS_AS(kale::reflect_schema<ContradictoryOneOfDTO>(), std::invalid_argument);
    }

    SUBCASE("Recursive DTO terminates reflection with clear error") {
        CHECK_THROWS_AS(kale::reflect_schema<RecursiveDTO>(), std::invalid_argument);
    }
}

TEST_CASE("OpenApiRegistry and Specification Generation invariants") {
    kale::Router router;
    router.POST("/api/v1/users", [](kale::Context &ctx) { ctx.string("created"); }).name("users.create");
    router.GET("/api/v1/users/:user_id", [](kale::Context &ctx) { ctx.string("user"); }).name("users.get");
    router.GET("/api/v1/items", [](kale::Context &ctx) { ctx.string("items"); }).name("items.list");
    router.GET("/files/*filepath", [](kale::Context &ctx) { ctx.string("file"); }).name("files.get");

    auto snapshot = router.routes();
    REQUIRE_EQ(snapshot.size(), 4);

    kale::OpenApiConfig config{
        .title = "Test Kale API",
        .version = "2.5.0",
        .description = "Test documentation for OpenAPI 3.0",
        .undocumented_policy = kale::UndocumentedRoutePolicy::GenerateDefault
    };

    kale::OpenApiRegistry registry;
    registry.associate(kale::HttpMethod::POST, "/api/v1/users")
        .set_summary("Create a new user")
        .set_description("Validates user payload and creates account")
        .add_tag("User Management")
        .body<CreateUserRequest>("User registration details")
        .response<UserResponse>(201, "User created successfully")
        .response(422, "Validation error");

    registry.associate(kale::HttpMethod::GET, "/api/v1/users/:user_id")
        .set_summary("Get user by ID")
        .add_tag("User Management")
        .response(200, "Found user");

    registry.associate(kale::HttpMethod::GET, "/api/v1/items")
        .set_summary("List items with pagination")
        .add_tag("Inventory")
        .query<QueryParamsDTO>()
        .response(200, "Item list");

    SUBCASE("Compliant OpenAPI 3.0.3 generation") {
        kale::OpenApiDocument doc = kale::generate_openapi(config, snapshot, registry);
        std::string json_text = doc.dump();
        auto j = nlohmann::json::parse(json_text);

        CHECK_EQ(j["openapi"], "3.0.3");
        CHECK_EQ(j["info"]["title"], "Test Kale API");
        CHECK_EQ(j["info"]["version"], "2.5.0");
        CHECK_EQ(j["info"]["description"], "Test documentation for OpenAPI 3.0");

        const auto &paths = j["paths"];
        REQUIRE(paths.contains("/api/v1/users/{user_id}"));
        const auto &user_get = paths["/api/v1/users/{user_id}"]["get"];
        CHECK_EQ(user_get["summary"], "Get user by ID");
        CHECK_EQ(user_get["operationId"], "users.get");
        CHECK_EQ(user_get["parameters"][0]["name"], "user_id");
        CHECK_EQ(user_get["parameters"][0]["in"], "path");
        CHECK_EQ(user_get["parameters"][0]["required"], true);

        REQUIRE(paths.contains("/api/v1/users"));
        const auto &user_post = paths["/api/v1/users"]["post"];
        CHECK_EQ(user_post["summary"], "Create a new user");
        const auto &body_schema = user_post["requestBody"]["content"]["application/json"]["schema"];
        CHECK_EQ(body_schema["type"], "object");
        CHECK_EQ(body_schema["properties"]["age"]["minimum"], 18);
        CHECK_EQ(body_schema["properties"]["account_id"]["minimum"], 10000000000LL);
        CHECK(user_post["responses"].contains("201"));
        CHECK(user_post["responses"].contains("422"));

        REQUIRE(paths.contains("/api/v1/items"));
        const auto &items_get = paths["/api/v1/items"]["get"];
        bool found_status_enum = false;
        bool found_filters_array = false;
        for (const auto &param : items_get["parameters"]) {
            if (param["name"] == "status") {
                found_status_enum = param["schema"].contains("enum");
            }
            if (param["name"] == "filters") {
                found_filters_array = (param["schema"]["type"] == "array" && param["style"] == "form" && param["explode"] == true);
            }
        }
        CHECK(found_status_enum);
        CHECK(found_filters_array);

        REQUIRE(paths.contains("/files/{filepath}"));
        const auto &files_get = paths["/files/{filepath}"]["get"];
        CHECK_EQ(files_get["parameters"][0]["name"], "filepath");
        CHECK_EQ(files_get["parameters"][0]["description"], "Catch-all wildcard path segment");
    }

    SUBCASE("Orphan RouteDoc rejects with std::invalid_argument") {
        kale::OpenApiRegistry bad_registry;
        bad_registry.associate(kale::HttpMethod::DELETE, "/non/existent/route");
        CHECK_THROWS_AS(kale::generate_openapi(config, snapshot, bad_registry), std::invalid_argument);
    }

    SUBCASE("Duplicate RouteDoc association rejects with std::invalid_argument") {
        kale::OpenApiRegistry dup_registry;
        dup_registry.associate(kale::HttpMethod::GET, "/api/v1/items");
        CHECK_THROWS_AS(dup_registry.associate(kale::HttpMethod::GET, "/api/v1/items"), std::invalid_argument);
    }

    SUBCASE("Path template collision across routes throws std::invalid_argument") {
        kale::Router collision_router;
        collision_router.GET("/users/:id", [](kale::Context &c) { c.string("1"); });
        collision_router.GET("/users/:userId", [](kale::Context &c) { c.string("2"); });
        auto col_snapshot = collision_router.routes();
        CHECK_THROWS_AS(kale::generate_openapi(config, col_snapshot), std::invalid_argument);
    }

    SUBCASE("HttpMethod::CONNECT throws std::invalid_argument") {
        kale::Router connect_router;
        connect_router.route(kale::HttpMethod::CONNECT, "/tunnel", [](kale::Context &c) { c.string("ok"); });
        auto conn_snapshot = connect_router.routes();
        CHECK_THROWS_AS(kale::generate_openapi(config, conn_snapshot), std::invalid_argument);
    }

    SUBCASE("Duplicate operationId throws std::invalid_argument") {
        kale::Router dup_op_router;
        dup_op_router.GET("/route1", [](kale::Context &c) { c.string("1"); }).name("same_id");
        dup_op_router.GET("/route2", [](kale::Context &c) { c.string("2"); }).name("same_id");
        auto dup_snapshot = dup_op_router.routes();
        CHECK_THROWS_AS(kale::generate_openapi(config, dup_snapshot), std::invalid_argument);
    }

    SUBCASE("Query parameter with nested object throws std::invalid_argument") {
        kale::Router q_router;
        q_router.GET("/search", [](kale::Context &c) { c.string("ok"); });
        kale::OpenApiRegistry q_reg;
        q_reg.associate(kale::HttpMethod::GET, "/search").query<InvalidQueryDTO>();
        CHECK_THROWS_AS(kale::generate_openapi(config, q_router.routes(), q_reg), std::invalid_argument);
    }

    SUBCASE("RequireExplicit policy throws for undocumented routes") {
        kale::OpenApiConfig strict_cfg = config;
        strict_cfg.undocumented_policy = kale::UndocumentedRoutePolicy::RequireExplicit;
        kale::OpenApiRegistry empty_reg;
        CHECK_THROWS_AS(kale::generate_openapi(strict_cfg, snapshot, empty_reg), std::invalid_argument);
    }

    SUBCASE("Exclude policy omits undocumented routes") {
        kale::OpenApiConfig exclude_cfg = config;
        exclude_cfg.undocumented_policy = kale::UndocumentedRoutePolicy::Exclude;
        kale::OpenApiRegistry partial_reg;
        partial_reg.associate(kale::HttpMethod::GET, "/api/v1/items").set_summary("Items only");
        auto doc = kale::generate_openapi(exclude_cfg, snapshot, partial_reg);
        auto j = nlohmann::json::parse(doc.dump());
        CHECK_EQ(j["paths"].size(), 1);
        CHECK(j["paths"].contains("/api/v1/items"));
        CHECK_FALSE(j["paths"].contains("/api/v1/users"));
    }
}

TEST_CASE("E2E HTTP Server serving OpenAPI specification with detached ownership") {
    TestServer srv;

    srv.server.GET("/api/v1/users/:user_id", [](kale::Context &ctx) {
        ctx.string("user info");
    }).name("users.get");

    srv.server.POST("/api/v1/users", [](kale::Context &ctx) {
        ctx.set_status(201).json(kale::Json::Object{{"status", "created"}});
    }).name("users.post");

    // Build OpenAPI document from snapshot during registration
    auto snapshot = srv.server.router().routes();
    kale::OpenApiConfig config{
        .title = "E2E Kale OpenAPI",
        .version = "1.0.0",
        .description = "Live HTTP specification verification"
    };

    kale::OpenApiRegistry registry;
    registry.associate(kale::HttpMethod::GET, "/api/v1/users/:user_id")
        .set_summary("Get user info")
        .response(200, "User returned");

    registry.associate(kale::HttpMethod::POST, "/api/v1/users")
        .set_summary("Create user")
        .body<CreateUserRequest>()
        .response<UserResponse>(201, "Created");

    kale::OpenApiDocument doc = kale::generate_openapi(config, snapshot, registry);

    // Mount spec handler
    srv.server.GET("/openapi.json", kale::make_openapi_handler(std::move(doc)));

    srv.start();

    SUBCASE("GET /openapi.json returns valid 200 with application/json") {
        auto r = srv.get("/openapi.json");
        CHECK_EQ(r.status, 200);
        auto ct = r.headers.find("Content-Type");
        REQUIRE(ct != r.headers.end());
        CHECK_NE(ct->second.find("application/json"), std::string::npos);

        auto j = nlohmann::json::parse(r.body);
        CHECK_EQ(j["openapi"], "3.0.3");
        CHECK_EQ(j["info"]["title"], "E2E Kale OpenAPI");
        CHECK(j["paths"].contains("/api/v1/users/{user_id}"));
        CHECK(j["paths"].contains("/api/v1/users"));
    }
}

} // TEST_SUITE("openapi")
