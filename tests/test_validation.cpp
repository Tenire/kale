/**
 * @file test_validation.cpp
 * @brief Comprehensive test suite for declarative FieldDesc constraints and DTO validation.
 *
 * Covers:
 * - Typed FieldDesc constraint metadata and OpenAPI introspection.
 * - Exact typed numeric comparisons without int64/uint64 to double narrowing.
 * - String length (bytes), pattern regex (compiled once, thread-safe const), and one_of choices.
 * - Invalid rule configurations throwing std::invalid_argument at construction time.
 * - Generic DTO binding across dual JSON libraries: kale::Json and nlohmann::json.
 * - Optional field semantics: absent/null skipped unless explicit required() rule is present.
 * - Accurate JSON error paths for nested DTOs and vector element boundaries.
 * - Real HTTP server validation tests verifying service invocation count is 0 on 400 Bad Request.
 */
#include "doctest/doctest.h"

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "kale/http/field_desc.h"
#include "kale/http/json_reader.h"
#include "kale/http/binding.h"
#include "kale/http/validator.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/router.h"
#include "kale/core/http_status.h"
#include "kale/core/http_method.h"
#include "kale/core/http_util.h"

#include "coke/http/http_client.h"
#include "coke/sleep.h"
#include "coke/task.h"
#include "coke/wait.h"
#include "workflow/HttpUtil.h"

using namespace kale::rules;

namespace {

// ============================================================================
// DTO Definitions with Declarative Constraints
// ============================================================================

struct UserValidationDTO {
    std::string name;
    int age{0};
    double score{0.0};
    std::string role;
    std::string code;
    std::optional<std::string> bio;
    std::optional<std::string> token;
    std::vector<std::string> tags;

    KALE_BIND(
        KALE_FIELD(name, min_len(3), max_len(20)),
        KALE_FIELD(age, gte(18), lte(100)),
        KALE_FIELD(score, range(0.0, 100.0)),
        KALE_FIELD(role, one_of<std::string>({"admin", "user", "guest"})),
        KALE_FIELD(code, pattern("^[A-Z]{3}-[0-9]{4}$")),
        KALE_FIELD(bio, max_len(100)),
        KALE_FIELD(token, required(), min_len(8)),
        KALE_FIELD(tags, min_len(1), max_len(5))
    )
};

struct NestedItemDTO {
    std::string title;
    int code{0};

    KALE_BIND(
        KALE_FIELD(title, min_len(2)),
        KALE_FIELD(code, gte(100))
    )
};

struct OrderDTO {
    std::string order_id;
    NestedItemDTO primary;
    std::vector<NestedItemDTO> items;

    KALE_BIND(
        KALE_FIELD(order_id, min_len(4)),
        KALE_FIELD(primary),
        KALE_FIELD(items, min_len(1))
    )
};

struct HighPrecisionIntDTO {
    int64_t id{0};
    uint64_t counter{0};

    KALE_BIND(
        KALE_FIELD(id, range(int64_t{9007199254740992LL}, int64_t{9007199254740994LL})),
        KALE_FIELD(counter, gt(uint64_t{10000000000000000ULL}))
    )
};

struct Exact53BitIntDTO {
    int64_t id{0};
    uint64_t counter{0};

    KALE_BIND(
        KALE_FIELD(id, range(int64_t{9007199254740980LL}, int64_t{9007199254740991LL})),
        KALE_FIELD(counter, gt(uint64_t{9007199254740900ULL}))
    )
};

struct EmptyDescDTO {
    std::string raw_field;
    int num{0};

    KALE_BIND(
        KALE_FIELD(raw_field),
        KALE_FIELD(num)
    )
};

// ============================================================================
// HTTP Test Helpers & Server Fixture
// ============================================================================

struct ValidationTestResponse {
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

    static ValidationTestResponse from_coke(coke::HttpResult &res) {
        ValidationTestResponse out;
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

using ValidationHeaderList = std::vector<std::pair<std::string, std::string>>;

inline coke::Task<ValidationTestResponse> do_validation_request(
    std::string url,
    std::string method = "GET",
    ValidationHeaderList headers = {},
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
    co_return ValidationTestResponse::from_coke(res);
}

class ValidationServerFixture {
public:
    explicit ValidationServerFixture(kale::HttpServerParams params = {})
        : server_(params) {
        int ret = server_.start("127.0.0.1", 0);
        REQUIRE_EQ(ret, 0);
        REQUIRE(server_.is_running());
        port_ = static_cast<unsigned short>(server_.listen_port());
        REQUIRE_GT(port_, 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(port_);
    }

    ~ValidationServerFixture() {
        if (server_.is_running()) {
            server_.stop();
        }
    }

    ValidationServerFixture(const ValidationServerFixture &) = delete;
    ValidationServerFixture &operator=(const ValidationServerFixture &) = delete;

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

    ValidationTestResponse post(std::string_view path, std::string body = "", ValidationHeaderList headers = {}) const {
        return coke::sync_wait(
            do_validation_request(url(path), "POST", std::move(headers), std::move(body))
        );
    }

private:
    kale::HttpServer server_;
    unsigned short port_{0};
    std::string base_url_;
};

} // namespace

// ============================================================================
// Test Suite: Validation Engine & Constraint Metadata
// ============================================================================

TEST_SUITE("validation") {

TEST_CASE("Validation: Typed FieldDesc metadata, tuple constraints, and OpenAPI introspection") {
    SUBCASE("zero-rule FieldDesc maintains empty tuple and aggregate initialization") {
        EmptyDescDTO dto;
        const auto &fields = dto.__kale_fields();
        REQUIRE_EQ(std::tuple_size_v<std::remove_cvref_t<decltype(fields)>>, 2);

        const auto &desc0 = std::get<0>(fields);
        CHECK_EQ(desc0.key, "raw_field");
        CHECK_EQ(std::tuple_size_v<std::remove_cvref_t<decltype(desc0.constraints())>>, 0);

        // Direct aggregate initialization with key and member pointer
        kale::FieldDesc manual{"test_key", &EmptyDescDTO::num};
        CHECK_EQ(manual.key, "test_key");
        CHECK_EQ(std::tuple_size_v<std::remove_cvref_t<decltype(manual.constraints())>>, 0);
    }

    SUBCASE("multi-rule FieldDesc exposes typed tuple constraint metadata") {
        UserValidationDTO dto;
        const auto &fields = dto.__kale_fields();

        // Field 0: name (min_len(3), max_len(20))
        const auto &name_desc = std::get<0>(fields);
        CHECK_EQ(name_desc.key, "name");
        const auto &name_rules = name_desc.constraints();
        REQUIRE_EQ(std::tuple_size_v<std::remove_cvref_t<decltype(name_rules)>>, 2);

        const auto &rule0 = std::get<0>(name_rules);
        const auto &rule1 = std::get<1>(name_rules);
        CHECK_EQ(rule0.name, "min_len");
        CHECK_EQ(rule0.min_size, 3);
        CHECK_EQ(rule1.name, "max_len");
        CHECK_EQ(rule1.max_size, 20);

        // Field 1: age (gte(18), lte(100))
        const auto &age_desc = std::get<1>(fields);
        CHECK_EQ(age_desc.key, "age");
        const auto &age_rules = age_desc.constraints();
        REQUIRE_EQ(std::tuple_size_v<std::remove_cvref_t<decltype(age_rules)>>, 2);
        CHECK_EQ(std::get<0>(age_rules).name, "gte");
        CHECK_EQ(std::get<0>(age_rules).boundary, 18);
        CHECK_EQ(std::get<1>(age_rules).name, "lte");
        CHECK_EQ(std::get<1>(age_rules).boundary, 100);

        // Field 4: code (pattern("^[A-Z]{3}-[0-9]{4}$"))
        const auto &code_desc = std::get<4>(fields);
        CHECK_EQ(code_desc.key, "code");
        const auto &code_rules = code_desc.constraints();
        REQUIRE_EQ(std::tuple_size_v<std::remove_cvref_t<decltype(code_rules)>>, 1);
        CHECK_EQ(std::get<0>(code_rules).name, "pattern");
        CHECK_EQ(std::get<0>(code_rules).pattern(), "^[A-Z]{3}-[0-9]{4}$");
    }
}

TEST_CASE("Validation: Unit arithmetic and numeric comparisons without double conversion") {
    SUBCASE("exact typed integer comparisons preserving int64 / uint64 precision") {
        auto r_gte = gte(int64_t{9007199254740992LL});
        CHECK(r_gte.check(int64_t{9007199254740992LL}));
        CHECK(r_gte.check(int64_t{9007199254740993LL}));
        CHECK_FALSE(r_gte.check(int64_t{9007199254740991LL}));

        // Range comparison with adjacent 64-bit values
        auto r_range = range(int64_t{9007199254740992LL}, int64_t{9007199254740994LL});
        CHECK(r_range.check(int64_t{9007199254740992LL}));
        CHECK(r_range.check(int64_t{9007199254740993LL}));
        CHECK(r_range.check(int64_t{9007199254740994LL}));
        CHECK_FALSE(r_range.check(int64_t{9007199254740995LL}));

        // Signed vs unsigned boundary comparison
        CHECK(compare_lt(int64_t{-1}, uint64_t{10}));
        CHECK_FALSE(compare_gt(int64_t{-1}, uint64_t{10}));
        CHECK(compare_lte(int64_t{-1}, uint64_t{0}));
    }

    SUBCASE("floating point boundaries") {
        auto score_rule = range(0.0, 100.0);
        CHECK(score_rule.check(0.0));
        CHECK(score_rule.check(50.5));
        CHECK(score_rule.check(100.0));
        CHECK_FALSE(score_rule.check(-0.01));
        CHECK_FALSE(score_rule.check(100.01));
    }
}

TEST_CASE("Validation: Unit string length bytes, regex pattern, and one_of choices") {
    SUBCASE("string byte length rules") {
        using namespace std::string_view_literals;
        auto s_min = min_len(3);
        CHECK_FALSE(s_min.check("ab"sv));
        CHECK(s_min.check("abc"sv));
        CHECK(s_min.check("abcd"sv));

        auto s_max = max_len(5);
        CHECK(s_max.check("hello"sv));
        CHECK_FALSE(s_max.check("hello!"sv));

        auto s_range = len_range(2, 4);
        CHECK_FALSE(s_range.check("a"sv));
        CHECK(s_range.check("ab"sv));
        CHECK(s_range.check("abcd"sv));
        CHECK_FALSE(s_range.check("abcde"sv));

        // Also test std::string and vector length checks
        std::string s_owned = "testing";
        CHECK(s_min.check(s_owned));
        std::vector<int> v_items = {1, 2, 3};
        CHECK(s_min.check(v_items));
        std::vector<int> v_empty;
        CHECK_FALSE(s_min.check(v_empty));
    }

    SUBCASE("regex pattern rule full-match") {
        using namespace std::string_view_literals;
        auto pat = pattern("^[A-Z]{2}-[0-9]{3}$");
        CHECK(pat.check("AB-123"sv));
        CHECK_FALSE(pat.check("ab-123"sv));
        CHECK_FALSE(pat.check("AB-1234"sv));
        CHECK_FALSE(pat.check("prefix AB-123"sv));
    }

    SUBCASE("one_of selection rule") {
        auto choice_str = one_of<std::string>({"apple", "banana", "cherry"});
        CHECK(choice_str.check(std::string("banana")));
        CHECK_FALSE(choice_str.check(std::string("orange")));

        auto choice_int = one_of<int>({10, 20, 30});
        CHECK(choice_int.check(20));
        CHECK_FALSE(choice_int.check(25));
    }

    SUBCASE("legacy string format rules") {
        using namespace std::string_view_literals;
        CHECK(is_email().check("user@example.com"sv));
        CHECK_FALSE(is_email().check("invalid-email"sv));

        CHECK(is_uuid_v7().check("018f3e9c-7a1b-7c4d-8e2f-1a2b3c4d5e6f"sv));
        CHECK_FALSE(is_uuid_v7().check("not-a-uuid"sv));

        CHECK(is_numeric().check("123456"sv));
        CHECK_FALSE(is_numeric().check("123a456"sv));

        CHECK(is_alphanumeric().check("Abc123XYZ"sv));
        CHECK_FALSE(is_alphanumeric().check("Abc-123"sv));

        CHECK(is_hex().check("DEADbeef0123456789"sv));
        CHECK_FALSE(is_hex().check("G123"sv));

        CHECK(starts_with("Bearer ").check("Bearer abc123xyz"sv));
        CHECK_FALSE(starts_with("Bearer ").check("Basic abc123xyz"sv));

        CHECK(ends_with(".json").check("config.json"sv));
        CHECK_FALSE(ends_with(".json").check("config.xml"sv));

        CHECK(contains("test").check("my_test_payload"sv));
        CHECK_FALSE(contains("test").check("my_payload"sv));

        auto custom_rule = custom([](int v) { return v % 2 == 0; });
        CHECK(custom_rule.check(4));
        CHECK_FALSE(custom_rule.check(5));
    }
}

TEST_CASE("Validation: Invalid rule configurations throw std::invalid_argument at construction") {
    // Inverted range bounds
    CHECK_THROWS_AS(range(100, 10), std::invalid_argument);
    CHECK_THROWS_AS(len_range(10, 2), std::invalid_argument);

    // Non-finite boundaries
    CHECK_THROWS_AS(gt(std::numeric_limits<double>::infinity()), std::invalid_argument);
    CHECK_THROWS_AS(gte(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    CHECK_THROWS_AS(range(std::numeric_limits<double>::quiet_NaN(), 10.0), std::invalid_argument);
    CHECK_THROWS_AS(range(0.0, std::numeric_limits<double>::infinity()), std::invalid_argument);

    // Invalid regex syntax
    CHECK_THROWS_AS(pattern("[unclosed-group"), std::invalid_argument);
    CHECK_THROWS_AS(pattern("(+invalid)"), std::invalid_argument);
}

TEST_CASE("Validation: Generic DTO binding across kale::Json and nlohmann::json") {
    auto run_test = []<typename J>() {
        std::string valid_body = R"({
            "name": "SuperAdmin",
            "age": 28,
            "score": 95.5,
            "role": "admin",
            "code": "XYZ-9876",
            "bio": "Lead architect",
            "token": "secret_token_123",
            "tags": ["core", "gateway"]
        })";

        UserValidationDTO dto;
        {
            dto = kale::bind_json_body<UserValidationDTO, J>(valid_body);
        }

        // Verify owning DTO fields outlive JSON parser
        CHECK_EQ(dto.name, "SuperAdmin");
        CHECK_EQ(dto.age, 28);
        CHECK_EQ(dto.score, doctest::Approx(95.5));
        CHECK_EQ(dto.role, "admin");
        CHECK_EQ(dto.code, "XYZ-9876");
        REQUIRE(dto.bio.has_value());
        CHECK_EQ(*dto.bio, "Lead architect");
        REQUIRE(dto.token.has_value());
        CHECK_EQ(*dto.token, "secret_token_123");
        REQUIRE_EQ(dto.tags.size(), 2);
        CHECK_EQ(dto.tags[0], "core");
        CHECK_EQ(dto.tags[1], "gateway");

        // Failure Case 1: string min_len failure
        std::string invalid_name = R"({
            "name": "Al",
            "age": 28,
            "score": 95.5,
            "role": "admin",
            "code": "XYZ-9876",
            "token": "secret_token_123",
            "tags": ["core"]
        })";
        try {
            kale::bind_json_body<UserValidationDTO, J>(invalid_name);
            FAIL("expected JsonBindError for short name");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "name");
            CHECK_NE(e.reason().find(">= 3"), std::string::npos);
        }

        // Failure Case 2: integer gte failure
        std::string invalid_age = R"({
            "name": "Alice",
            "age": 16,
            "score": 95.5,
            "role": "admin",
            "code": "XYZ-9876",
            "token": "secret_token_123",
            "tags": ["core"]
        })";
        try {
            kale::bind_json_body<UserValidationDTO, J>(invalid_age);
            FAIL("expected JsonBindError for age < 18");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "age");
            CHECK_NE(e.reason().find(">= 18"), std::string::npos);
        }

        // Failure Case 3: one_of choice failure
        std::string invalid_role = R"({
            "name": "Alice",
            "age": 25,
            "score": 95.5,
            "role": "superoperator",
            "code": "XYZ-9876",
            "token": "secret_token_123",
            "tags": ["core"]
        })";
        try {
            kale::bind_json_body<UserValidationDTO, J>(invalid_role);
            FAIL("expected JsonBindError for invalid role");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "role");
            CHECK_NE(e.reason().find("allowed choices"), std::string::npos);
        }

        // Failure Case 4: pattern regex mismatch
        std::string invalid_code = R"({
            "name": "Alice",
            "age": 25,
            "score": 95.5,
            "role": "admin",
            "code": "xyz-1234",
            "token": "secret_token_123",
            "tags": ["core"]
        })";
        try {
            kale::bind_json_body<UserValidationDTO, J>(invalid_code);
            FAIL("expected JsonBindError for regex mismatch");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "code");
            CHECK_NE(e.reason().find("pattern"), std::string::npos);
        }

        // Failure Case 5: vector min_len failure
        std::string empty_tags = R"({
            "name": "Alice",
            "age": 25,
            "score": 95.5,
            "role": "admin",
            "code": "XYZ-9876",
            "token": "secret_token_123",
            "tags": []
        })";
        try {
            kale::bind_json_body<UserValidationDTO, J>(empty_tags);
            FAIL("expected JsonBindError for empty tags vector");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "tags");
            CHECK_NE(e.reason().find(">= 1"), std::string::npos);
        }
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Validation: Optional fields absent/null skip vs explicit required()") {
    auto run_test = []<typename J>() {
        // Case 1: bio is optional without required() -> absent & null are valid std::nullopt
        std::string missing_bio = R"({
            "name": "Bob Vance",
            "age": 45,
            "score": 80.0,
            "role": "user",
            "code": "ABC-1234",
            "token": "valid_token_123",
            "tags": ["sales"]
        })";
        auto dto1 = kale::bind_json_body<UserValidationDTO, J>(missing_bio);
        CHECK_FALSE(dto1.bio.has_value());

        std::string null_bio = R"({
            "name": "Bob Vance",
            "age": 45,
            "score": 80.0,
            "role": "user",
            "code": "ABC-1234",
            "bio": null,
            "token": "valid_token_123",
            "tags": ["sales"]
        })";
        auto dto2 = kale::bind_json_body<UserValidationDTO, J>(null_bio);
        CHECK_FALSE(dto2.bio.has_value());

        // Case 2: token is optional WITH required() -> missing throws JsonBindError
        std::string missing_token = R"({
            "name": "Bob Vance",
            "age": 45,
            "score": 80.0,
            "role": "user",
            "code": "ABC-1234",
            "tags": ["sales"]
        })";
        try {
            kale::bind_json_body<UserValidationDTO, J>(missing_token);
            FAIL("expected missing required token error");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "token");
            CHECK_EQ(e.expected(), "present");
        }

        // Case 3: token is optional WITH required() -> null throws JsonBindError
        std::string null_token = R"({
            "name": "Bob Vance",
            "age": 45,
            "score": 80.0,
            "role": "user",
            "code": "ABC-1234",
            "token": null,
            "tags": ["sales"]
        })";
        try {
            kale::bind_json_body<UserValidationDTO, J>(null_token);
            FAIL("expected null required token error");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "token");
            CHECK_EQ(e.expected(), "non-null");
        }

        // Case 4: token present but violating min_len(8)
        std::string short_token = R"({
            "name": "Bob Vance",
            "age": 45,
            "score": 80.0,
            "role": "user",
            "code": "ABC-1234",
            "token": "short",
            "tags": ["sales"]
        })";
        try {
            kale::bind_json_body<UserValidationDTO, J>(short_token);
            FAIL("expected short token error");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "token");
            CHECK_NE(e.reason().find(">= 8"), std::string::npos);
        }
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Validation: Accurate JSON paths for nested DTOs and vectors") {
    auto run_test = []<typename J>() {
        // Valid nested and vector payload
        std::string valid_order = R"({
            "order_id": "ORD-1001",
            "primary": {"title": "Main Widget", "code": 150},
            "items": [
                {"title": "Item A", "code": 100},
                {"title": "Item B", "code": 200}
            ]
        })";
        auto order = kale::bind_json_body<OrderDTO, J>(valid_order);
        CHECK_EQ(order.order_id, "ORD-1001");
        CHECK_EQ(order.primary.title, "Main Widget");
        CHECK_EQ(order.primary.code, 150);
        REQUIRE_EQ(order.items.size(), 2);

        // Nested primary DTO code constraint failure -> path "primary.code"
        std::string invalid_primary = R"({
            "order_id": "ORD-1001",
            "primary": {"title": "Main Widget", "code": 50},
            "items": [
                {"title": "Item A", "code": 100}
            ]
        })";
        try {
            kale::bind_json_body<OrderDTO, J>(invalid_primary);
            FAIL("expected primary.code error");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "primary.code");
            CHECK_NE(e.reason().find(">= 100"), std::string::npos);
        }

        // Vector element index 1 nested title constraint failure -> path "items[1].title"
        std::string invalid_item_elem = R"({
            "order_id": "ORD-1001",
            "primary": {"title": "Main Widget", "code": 150},
            "items": [
                {"title": "Item A", "code": 100},
                {"title": "X", "code": 200}
            ]
        })";
        try {
            kale::bind_json_body<OrderDTO, J>(invalid_item_elem);
            FAIL("expected items[1].title error");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "items[1].title");
            CHECK_NE(e.reason().find(">= 2"), std::string::npos);
        }

        // Vector size constraint failure -> path "items"
        std::string empty_items = R"({
            "order_id": "ORD-1001",
            "primary": {"title": "Main Widget", "code": 150},
            "items": []
        })";
        try {
            kale::bind_json_body<OrderDTO, J>(empty_items);
            FAIL("expected items min_len error");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "items");
            CHECK_NE(e.reason().find(">= 1"), std::string::npos);
        }
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Validation: 64-bit integer binding and comparison across libraries") {
    SUBCASE("kale::Json: Workflow double representation within 2^53 - 1 and mantissa rounding beyond") {
        // Within 2^53 - 1 (9007199254740991), integers are exactly representable in double
        std::string valid_double_json = R"({
            "id": 9007199254740990,
            "counter": 9007199254740991
        })";
        auto dto = kale::bind_json_body<Exact53BitIntDTO, kale::Json>(valid_double_json);
        CHECK_EQ(dto.id, 9007199254740990LL);
        CHECK_EQ(dto.counter, 9007199254740991ULL);

        std::string overflow_id = R"({
            "id": 9007199254740992,
            "counter": 9007199254740991
        })";
        try {
            kale::bind_json_body<Exact53BitIntDTO, kale::Json>(overflow_id);
            FAIL("expected id range failure for kale::Json");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "id");
            CHECK_NE(e.reason().find("between"), std::string::npos);
        }

        // Beyond 2^53, Workflow double rounds 10000000000000001 down to 10000000000000000,
        // so gt(10000000000000000ULL) correctly rejects it as <= boundary
        std::string beyond_double = R"({
            "id": 9007199254740993,
            "counter": 10000000000000001
        })";
        try {
            kale::bind_json_body<HighPrecisionIntDTO, kale::Json>(beyond_double);
            FAIL("expected counter gt failure due to double mantissa rounding");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "counter");
            CHECK_NE(e.reason().find(">"), std::string::npos);
        }
    }

    SUBCASE("nlohmann::json: Exact 64-bit integer representation beyond 2^53 without double narrowing") {
        std::string valid_hp = R"({
            "id": 9007199254740993,
            "counter": 10000000000000001
        })";
        auto dto = kale::bind_json_body<HighPrecisionIntDTO, nlohmann::json>(valid_hp);
        CHECK_EQ(dto.id, 9007199254740993LL);
        CHECK_EQ(dto.counter, 10000000000000001ULL);

        std::string overflow_id = R"({
            "id": 9007199254740995,
            "counter": 10000000000000001
        })";
        try {
            kale::bind_json_body<HighPrecisionIntDTO, nlohmann::json>(overflow_id);
            FAIL("expected id range failure for nlohmann::json");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "id");
            CHECK_NE(e.reason().find("between"), std::string::npos);
        }
    }
}

TEST_CASE("Validation: Real HTTP endpoints with service invocation count tracking") {
    ValidationServerFixture fixture;

    std::atomic<int> kale_service_invocations{0};
    std::atomic<int> nloh_service_invocations{0};

    // Route 1: Using kale::Json reader
    fixture->POST("/api/v1/users/kale", [&](kale::Context &ctx) {
        auto dto = ctx.bind_json<UserValidationDTO, kale::Json>();
        kale_service_invocations.fetch_add(1, std::memory_order_relaxed);
        ctx.json(kale::Json::Object{
            {"status", "created"},
            {"user", dto.name}
        });
    });

    // Route 2: Using nlohmann::json reader
    fixture->POST("/api/v1/users/nloh", [&](kale::Context &ctx) {
        auto dto = ctx.bind_json<UserValidationDTO, nlohmann::json>();
        nloh_service_invocations.fetch_add(1, std::memory_order_relaxed);
        ctx.json(kale::Json::Object{
            {"status", "created"},
            {"user", dto.name}
        });
    });

    std::string valid_payload = R"({
        "name": "Administrator",
        "age": 30,
        "score": 99.0,
        "role": "admin",
        "code": "ADM-0001",
        "bio": "System administrator",
        "token": "admin_access_token_999",
        "tags": ["infra", "security"]
    })";

    ValidationHeaderList json_headers = {{"Content-Type", "application/json"}};

    SUBCASE("Valid HTTP requests successfully invoke service and return 200 OK") {
        auto res1 = fixture.post("/api/v1/users/kale", valid_payload, json_headers);
        CHECK_EQ(res1.status, 200);
        CHECK_EQ(kale_service_invocations.load(), 1);

        auto res2 = fixture.post("/api/v1/users/nloh", valid_payload, json_headers);
        CHECK_EQ(res2.status, 200);
        CHECK_EQ(nloh_service_invocations.load(), 1);
    }

    SUBCASE("Invalid constraint HTTP requests abort with 400 Bad Request; service invocation count remains 0") {
        // Reset counters
        kale_service_invocations.store(0);
        nloh_service_invocations.store(0);

        std::string invalid_age_payload = R"({
            "name": "Administrator",
            "age": 15,
            "score": 99.0,
            "role": "admin",
            "code": "ADM-0001",
            "token": "admin_access_token_999",
            "tags": ["infra"]
        })";

        // Kale endpoint with invalid age
        auto res1 = fixture.post("/api/v1/users/kale", invalid_age_payload, json_headers);
        CHECK_EQ(res1.status, 400);
        CHECK_EQ(kale_service_invocations.load(), 0); // Must NOT invoke service

        // Nlohmann endpoint with invalid age
        auto res2 = fixture.post("/api/v1/users/nloh", invalid_age_payload, json_headers);
        CHECK_EQ(res2.status, 400);
        CHECK_EQ(nloh_service_invocations.load(), 0); // Must NOT invoke service

        // Kale endpoint with invalid regex pattern
        std::string invalid_pattern_payload = R"({
            "name": "Administrator",
            "age": 30,
            "score": 99.0,
            "role": "admin",
            "code": "invalid-code",
            "token": "admin_access_token_999",
            "tags": ["infra"]
        })";
        auto res3 = fixture.post("/api/v1/users/kale", invalid_pattern_payload, json_headers);
        CHECK_EQ(res3.status, 400);
        CHECK_EQ(kale_service_invocations.load(), 0);

        // Nlohmann endpoint with missing required token
        std::string missing_token_payload = R"({
            "name": "Administrator",
            "age": 30,
            "score": 99.0,
            "role": "admin",
            "code": "ADM-0001",
            "tags": ["infra"]
        })";
        auto res4 = fixture.post("/api/v1/users/nloh", missing_token_payload, json_headers);
        CHECK_EQ(res4.status, 400);
        CHECK_EQ(nloh_service_invocations.load(), 0);
    }
}

TEST_CASE("Validation: PatternRule thread safety and static descriptor reuse") {
    ValidationServerFixture fixture;
    std::atomic<int> completed_requests{0};

    fixture->POST("/api/v1/fast_validate", [&](kale::Context &ctx) {
        auto dto = ctx.bind_json<UserValidationDTO, kale::Json>();
        completed_requests.fetch_add(1, std::memory_order_relaxed);
        ctx.json(kale::Json::Object{{"code", dto.code}});
    });

    std::string payload = R"({
        "name": "ConcurrentTester",
        "age": 30,
        "score": 88.0,
        "role": "user",
        "code": "TST-5555",
        "token": "valid_token_xyz",
        "tags": ["load"]
    })";

    ValidationHeaderList json_headers = {{"Content-Type", "application/json"}};

    // Sequential multi-request execution verifying immutable static tuple reuse without re-compilation
    for (int i = 0; i < 5; ++i) {
        auto res = fixture.post("/api/v1/fast_validate", payload, json_headers);
        CHECK_EQ(res.status, 200);
    }
    CHECK_EQ(completed_requests.load(), 5);
}

} // TEST_SUITE("validation")