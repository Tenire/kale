#include "doctest/doctest.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "kale/http/field_desc.h"
#include "kale/http/json_reader.h"
#include "kale/http/binding.h"
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
namespace {

// ============================================================================
// DTO Definitions with Declarative KALE_BIND Reflection
// ============================================================================

struct GenericItemDTO {
    std::string title;
    int code{0};

    KALE_BIND(
        KALE_FIELD(title),
        KALE_FIELD(code)
    )
};

struct GenericMasterDTO {
    std::string name;
    int age{0};
    uint32_t count{0};
    int64_t large_id{0};
    uint64_t large_counter{0};
    double score{0.0};
    bool active{false};
    std::optional<std::string> note;
    std::optional<int> extra_code;
    GenericItemDTO item;
    std::vector<std::string> tags;
    std::vector<int> numbers;
    std::vector<GenericItemDTO> items;

    KALE_BIND(
        KALE_FIELD(name),
        KALE_FIELD(age),
        KALE_FIELD(count),
        KALE_FIELD(large_id),
        KALE_FIELD(large_counter),
        KALE_FIELD(score),
        KALE_FIELD(active),
        KALE_FIELD(note),
        KALE_FIELD(extra_code),
        KALE_FIELD(item),
        KALE_FIELD(tags),
        KALE_FIELD(numbers),
        KALE_FIELD(items)
    )
};

struct IntegerTargetsDTO {
    int8_t i8{0};
    uint8_t u8{0};
    int16_t i16{0};
    uint16_t u16{0};
    int32_t i32{0};
    uint32_t u32{0};
    int64_t i64{0};
    uint64_t u64{0};

    KALE_BIND(
        KALE_FIELD(i8),
        KALE_FIELD(u8),
        KALE_FIELD(i16),
        KALE_FIELD(u16),
        KALE_FIELD(i32),
        KALE_FIELD(u32),
        KALE_FIELD(i64),
        KALE_FIELD(u64)
    )
};

struct FloatTargetsDTO {
    float f32{0.0f};
    double f64{0.0};

    KALE_BIND(
        KALE_FIELD(f32),
        KALE_FIELD(f64)
    )
};

template <typename J>
struct RawPayloadDTO {
    std::string tag;
    J payload;

    KALE_BIND(
        KALE_FIELD(tag),
        KALE_FIELD(payload)
    )
};

struct SimpleAccountDTO {
    std::string username;
    int age{0};

    KALE_BIND(
        KALE_FIELD(username),
        KALE_FIELD(age)
    )
};

struct CountingDumpWrapper {
    nlohmann::json inner;
    int *dump_count{nullptr};

    std::string dump() const {
        if (dump_count) {
            ++(*dump_count);
        }
        return inner.dump();
    }
};

// ============================================================================
// HTTP Test Helpers & Server Fixture
// ============================================================================

struct GenericTestResponse {
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

    static GenericTestResponse from_coke(coke::HttpResult &res) {
        GenericTestResponse out;
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

inline coke::Task<GenericTestResponse> do_http_request(
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
    co_return GenericTestResponse::from_coke(res);
}

class GenericServerFixture {
public:
    explicit GenericServerFixture(kale::HttpServerParams params = {})
        : server_(params) {
        int ret = server_.start("127.0.0.1", 0);
        REQUIRE_EQ(ret, 0);
        REQUIRE(server_.is_running());
        port_ = static_cast<unsigned short>(server_.listen_port());
        REQUIRE_GT(port_, 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(port_);
    }

    ~GenericServerFixture() {
        if (server_.is_running()) {
            server_.stop();
        }
    }

    GenericServerFixture(const GenericServerFixture &) = delete;
    GenericServerFixture &operator=(const GenericServerFixture &) = delete;

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

    GenericTestResponse request(
        std::string_view path,
        std::string method = "GET",
        HeaderPairList headers = {},
        std::string body = "") const {
        return coke::sync_wait(
            do_http_request(url(path), std::move(method), std::move(headers), std::move(body))
        );
    }

    GenericTestResponse post(std::string_view path, std::string body = "", HeaderPairList headers = {}) const {
        return request(path, "POST", std::move(headers), std::move(body));
    }

private:
    kale::HttpServer server_;
    unsigned short port_{0};
    std::string base_url_;
};

} // namespace

// ============================================================================
// Test Suite: Generic DTO Binding Engine (Dual Library: kale::Json and nlohmann::json)
// ============================================================================

TEST_SUITE("generic_binding") {

TEST_CASE("Generic DTO: Valid comprehensive payload binding and string ownership") {
    auto run_test = []<typename J>() {
        std::string valid_json = R"({
            "name": "Alex Mercer",
            "age": 32,
            "count": 500,
            "large_id": 9007199254740991,
            "large_counter": 9007199254740991,
            "score": 98.75,
            "active": true,
            "note": "senior engineer",
            "extra_code": 404,
            "item": {
                "title": "Hardware Token",
                "code": 1001
            },
            "tags": ["core", "networking", "distributed"],
            "numbers": [10, 20, 30, 40],
            "items": [
                {"title": "Alpha", "code": 1},
                {"title": "Beta", "code": 2}
            ],
            "unknown_root_key": "ignored_value",
            "unknown_object": {"nested_key": 999}
        })";

        GenericMasterDTO dto;
        {
            // Execute parse and bind inside temporary scope to test string ownership
            dto = kale::bind_json_body<GenericMasterDTO, J>(valid_json);
        }

        // Validate primitive and numeric fields
        CHECK_EQ(dto.name, "Alex Mercer");
        CHECK_EQ(dto.age, 32);
        CHECK_EQ(dto.count, 500u);
        CHECK_EQ(dto.large_id, 9007199254740991LL);
        CHECK_EQ(dto.large_counter, 9007199254740991ULL);
        CHECK_EQ(dto.score, doctest::Approx(98.75));
        CHECK_EQ(dto.active, true);

        // Validate optional fields
        REQUIRE(dto.note.has_value());
        CHECK_EQ(*dto.note, "senior engineer");
        REQUIRE(dto.extra_code.has_value());
        CHECK_EQ(*dto.extra_code, 404);

        // Validate nested DTO
        CHECK_EQ(dto.item.title, "Hardware Token");
        CHECK_EQ(dto.item.code, 1001);

        // Validate vector of strings and ownership
        REQUIRE_EQ(dto.tags.size(), 3);
        CHECK_EQ(dto.tags[0], "core");
        CHECK_EQ(dto.tags[1], "networking");
        CHECK_EQ(dto.tags[2], "distributed");

        // Validate vector of numbers
        REQUIRE_EQ(dto.numbers.size(), 4);
        CHECK_EQ(dto.numbers[0], 10);
        CHECK_EQ(dto.numbers[1], 20);
        CHECK_EQ(dto.numbers[2], 30);
        CHECK_EQ(dto.numbers[3], 40);

        // Validate vector of nested DTOs
        REQUIRE_EQ(dto.items.size(), 2);
        CHECK_EQ(dto.items[0].title, "Alpha");
        CHECK_EQ(dto.items[0].code, 1);
        CHECK_EQ(dto.items[1].title, "Beta");
        CHECK_EQ(dto.items[1].code, 2);
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Generic DTO: Optional fields handling (present, missing, null)") {
    auto run_test = []<typename J>() {
        // Case 1: Optional fields missing from JSON -> std::nullopt
        std::string missing_opt = R"({
            "name": "Bob",
            "age": 25,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "item": {"title": "T", "code": 0},
            "tags": [],
            "numbers": [],
            "items": []
        })";

        auto dto_missing = kale::bind_json_body<GenericMasterDTO, J>(missing_opt);
        CHECK_FALSE(dto_missing.note.has_value());
        CHECK_FALSE(dto_missing.extra_code.has_value());

        // Case 2: Optional fields set to null in JSON -> std::nullopt
        std::string null_opt = R"({
            "name": "Bob",
            "age": 25,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "note": null,
            "extra_code": null,
            "item": {"title": "T", "code": 0},
            "tags": [],
            "numbers": [],
            "items": []
        })";

        auto dto_null = kale::bind_json_body<GenericMasterDTO, J>(null_opt);
        CHECK_FALSE(dto_null.note.has_value());
        CHECK_FALSE(dto_null.extra_code.has_value());
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Generic DTO: Missing required fields throw JsonBindError with exact paths") {
    auto run_test = []<typename J>() {
        // Root missing field
        std::string missing_root = R"({
            "age": 30,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "item": {"title": "T", "code": 0},
            "tags": [],
            "numbers": [],
            "items": []
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(missing_root);
            FAIL("Expected JsonBindError for missing root required field");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "name");
            CHECK_EQ(e.expected(), "present");
            CHECK_EQ(e.reason(), "required field is missing");
            CHECK_EQ(e.status_code(), 400);
        }

        // Nested missing field
        std::string missing_nested = R"({
            "name": "Bob",
            "age": 30,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "item": {"title": "T"},
            "tags": [],
            "numbers": [],
            "items": []
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(missing_nested);
            FAIL("Expected JsonBindError for missing nested required field");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "item.code");
            CHECK_EQ(e.expected(), "present");
            CHECK_EQ(e.reason(), "required field is missing");
            CHECK_EQ(e.status_code(), 400);
        }

        // Vector element missing field
        std::string missing_array_elem = R"({
            "name": "Bob",
            "age": 30,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "item": {"title": "T", "code": 0},
            "tags": [],
            "numbers": [],
            "items": [
                {"title": "Item1", "code": 1},
                {"title": "Item2"}
            ]
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(missing_array_elem);
            FAIL("Expected JsonBindError for missing field inside array element");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "items[1].code");
            CHECK_EQ(e.expected(), "present");
            CHECK_EQ(e.reason(), "required field is missing");
            CHECK_EQ(e.status_code(), 400);
        }
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Generic DTO: Null required fields throw JsonBindError with exact paths") {
    auto run_test = []<typename J>() {
        // Root null field
        std::string null_root = R"({
            "name": null,
            "age": 30,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "item": {"title": "T", "code": 0},
            "tags": [],
            "numbers": [],
            "items": []
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(null_root);
            FAIL("Expected JsonBindError for null root required field");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "name");
            CHECK_EQ(e.expected(), "non-null");
            CHECK_EQ(e.reason(), "required field is null");
            CHECK_EQ(e.status_code(), 400);
        }

        // Nested null field
        std::string null_nested = R"({
            "name": "Bob",
            "age": 30,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "item": {"title": null, "code": 0},
            "tags": [],
            "numbers": [],
            "items": []
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(null_nested);
            FAIL("Expected JsonBindError for null nested required field");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "item.title");
            CHECK_EQ(e.expected(), "non-null");
            CHECK_EQ(e.reason(), "required field is null");
            CHECK_EQ(e.status_code(), 400);
        }
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Generic DTO: Type mismatch errors throw JsonBindError with expected types") {
    auto run_test = []<typename J>() {
        // String expected, got number
        std::string type_mismatch_string = R"({
            "name": 12345,
            "age": 30,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "item": {"title": "T", "code": 0},
            "tags": [],
            "numbers": [],
            "items": []
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(type_mismatch_string);
            FAIL("Expected JsonBindError for string type mismatch");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "name");
            CHECK_EQ(e.expected(), "string");
            CHECK_EQ(e.reason(), "value is not a string");
        }

        // Bool expected, got string
        std::string type_mismatch_bool = R"({
            "name": "Bob",
            "age": 30,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": "not_a_bool",
            "item": {"title": "T", "code": 0},
            "tags": [],
            "numbers": [],
            "items": []
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(type_mismatch_bool);
            FAIL("Expected JsonBindError for boolean type mismatch");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "active");
            CHECK_EQ(e.expected(), "boolean");
            CHECK_EQ(e.reason(), "value is not a boolean");
        }

        // Object expected, got array
        std::string type_mismatch_object = R"({
            "name": "Bob",
            "age": 30,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "item": ["not", "an", "object"],
            "tags": [],
            "numbers": [],
            "items": []
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(type_mismatch_object);
            FAIL("Expected JsonBindError for object type mismatch");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "item");
            CHECK_EQ(e.expected(), "object");
            CHECK_EQ(e.reason(), "value is not an object");
        }

        // Array expected, got string
        std::string type_mismatch_array = R"({
            "name": "Bob",
            "age": 30,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "item": {"title": "T", "code": 0},
            "tags": "not_an_array",
            "numbers": [],
            "items": []
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(type_mismatch_array);
            FAIL("Expected JsonBindError for array type mismatch");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "tags");
            CHECK_EQ(e.expected(), "array");
            CHECK_EQ(e.reason(), "value is not an array");
        }

        // Array element type mismatch
        std::string array_elem_mismatch = R"({
            "name": "Bob",
            "age": 30,
            "count": 10,
            "large_id": 1,
            "large_counter": 2,
            "score": 1.0,
            "active": false,
            "item": {"title": "T", "code": 0},
            "tags": ["ok", 999],
            "numbers": [],
            "items": []
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(array_elem_mismatch);
            FAIL("Expected JsonBindError for array element type mismatch");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "tags[1]");
            CHECK_EQ(e.expected(), "string");
            CHECK_EQ(e.reason(), "value is not a string");
        }
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Generic DTO: Kind-aware numeric boundaries and target overflow") {
    auto run_test = []<typename J>() {
        // Valid integer targets
        std::string valid_ints = R"({
            "i8": 127,
            "u8": 255,
            "i16": 32767,
            "u16": 65535,
            "i32": 2147483647,
            "u32": 4294967295,
            "i64": 9007199254740991,
            "u64": 9007199254740991
        })";

        auto dto = kale::bind_json_body<IntegerTargetsDTO, J>(valid_ints);
        CHECK_EQ(dto.i8, 127);
        CHECK_EQ(dto.u8, 255);
        CHECK_EQ(dto.i16, 32767);
        CHECK_EQ(dto.u16, 65535);
        CHECK_EQ(dto.i32, 2147483647);
        CHECK_EQ(dto.u32, 4294967295U);
        CHECK_EQ(dto.i64, 9007199254740991LL);
        CHECK_EQ(dto.u64, 9007199254740991ULL);

        // int8_t positive overflow: 128
        std::string i8_overflow = R"({"i8": 128, "u8": 0, "i16": 0, "u16": 0, "i32": 0, "u32": 0, "i64": 0, "u64": 0})";
        try {
            (void)kale::bind_json_body<IntegerTargetsDTO, J>(i8_overflow);
            FAIL("Expected overflow error for i8");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "i8");
            CHECK_EQ(e.expected(), "integer");
            CHECK_EQ(e.reason(), "numeric value out of range");
        }

        // int8_t negative underflow: -129
        std::string i8_underflow = R"({"i8": -129, "u8": 0, "i16": 0, "u16": 0, "i32": 0, "u32": 0, "i64": 0, "u64": 0})";
        try {
            (void)kale::bind_json_body<IntegerTargetsDTO, J>(i8_underflow);
            FAIL("Expected underflow error for i8");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "i8");
            CHECK_EQ(e.expected(), "integer");
            CHECK_EQ(e.reason(), "numeric value out of range");
        }

        // uint8_t overflow: 256
        std::string u8_overflow = R"({"i8": 0, "u8": 256, "i16": 0, "u16": 0, "i32": 0, "u32": 0, "i64": 0, "u64": 0})";
        try {
            (void)kale::bind_json_body<IntegerTargetsDTO, J>(u8_overflow);
            FAIL("Expected overflow error for u8");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "u8");
            CHECK_EQ(e.expected(), "unsigned integer");
            CHECK_EQ(e.reason(), "numeric value out of range");
        }

        // uint8_t negative value rejection
        std::string u8_negative = R"({"i8": 0, "u8": -1, "i16": 0, "u16": 0, "i32": 0, "u32": 0, "i64": 0, "u64": 0})";
        try {
            (void)kale::bind_json_body<IntegerTargetsDTO, J>(u8_negative);
            FAIL("Expected negative unsigned error for u8");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "u8");
            CHECK_EQ(e.expected(), "unsigned integer");
            CHECK_EQ(e.reason(), "numeric value out of range");
        }

        // uint32_t negative value rejection
        std::string u32_negative = R"({"i8": 0, "u8": 0, "i16": 0, "u16": 0, "i32": 0, "u32": -50, "i64": 0, "u64": 0})";
        try {
            (void)kale::bind_json_body<IntegerTargetsDTO, J>(u32_negative);
            FAIL("Expected negative unsigned error for u32");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "u32");
            CHECK_EQ(e.expected(), "unsigned integer");
            CHECK_EQ(e.reason(), "numeric value out of range");
        }

        // uint64_t negative value rejection
        std::string u64_negative = R"({"i8": 0, "u8": 0, "i16": 0, "u16": 0, "i32": 0, "u32": 0, "i64": 0, "u64": -1})";
        try {
            (void)kale::bind_json_body<IntegerTargetsDTO, J>(u64_negative);
            FAIL("Expected negative unsigned error for u64");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "u64");
            CHECK_EQ(e.expected(), "unsigned integer");
            CHECK_EQ(e.reason(), "numeric value out of range");
        }

        // Fractional value for integer rejection
        std::string frac_for_int = R"({"i8": 0, "u8": 0, "i16": 0, "u16": 0, "i32": 42.5, "u32": 0, "i64": 0, "u64": 0})";
        try {
            (void)kale::bind_json_body<IntegerTargetsDTO, J>(frac_for_int);
            FAIL("Expected non-integer rejection for i32");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "i32");
            CHECK_EQ(e.expected(), "integer");
            CHECK_EQ(e.reason(), "floating point or non-finite value not allowed for integer");
        }

        // Fractional value for unsigned integer rejection
        std::string frac_for_uint = R"({"i8": 0, "u8": 0, "i16": 0, "u16": 0, "i32": 0, "u32": 10.25, "i64": 0, "u64": 0})";
        try {
            (void)kale::bind_json_body<IntegerTargetsDTO, J>(frac_for_uint);
            FAIL("Expected non-integer rejection for u32");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "u32");
            CHECK_EQ(e.expected(), "unsigned integer");
            CHECK_EQ(e.reason(), "floating point or non-finite value not allowed for integer");
        }
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Generic DTO: Floating point target boundaries and nonfinite handling") {
    auto run_test = []<typename J>() {
        std::string valid_floats = R"({
            "f32": 3.14159,
            "f64": 2.718281828459045
        })";

        auto dto = kale::bind_json_body<FloatTargetsDTO, J>(valid_floats);
        CHECK_EQ(dto.f32, doctest::Approx(3.14159f));
        CHECK_EQ(dto.f64, doctest::Approx(2.718281828459045));

        // float (f32) overflow: 1e39 exceeds float max (~3.4e38)
        std::string f32_overflow = R"({
            "f32": 1e39,
            "f64": 0.0
        })";

        try {
            (void)kale::bind_json_body<FloatTargetsDTO, J>(f32_overflow);
            FAIL("Expected float overflow error for f32");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "f32");
            CHECK_EQ(e.expected(), "float");
            const bool valid_reason = (e.reason() == "floating point value out of range" ||
                                       e.reason() == "non-finite floating point value");
            CHECK(valid_reason);
        }
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Native nlohmann: Full 64-bit integer precision beyond double 2^53 mantissa") {
    // 64-bit integers > 2^53 preserve exact 64-bit signed/unsigned fidelity in nlohmann::json
    std::string exact64_json = R"({
        "i64": 922337203685477580,
        "u64": 1844674407370955161,
        "i8": 0, "u8": 0, "i16": 0, "u16": 0, "i32": 0, "u32": 0
    })";

    auto dto = kale::bind_json_body<IntegerTargetsDTO, nlohmann::json>(exact64_json);
    CHECK_EQ(dto.i64, 922337203685477580LL);
    CHECK_EQ(dto.u64, 1844674407370955161ULL);

    // Max 64-bit integer limits
    std::string max64_json = R"({
        "i64": 9223372036854775807,
        "u64": 18446744073709551615,
        "i8": 0, "u8": 0, "i16": 0, "u16": 0, "i32": 0, "u32": 0
    })";
    auto dto_max = kale::bind_json_body<IntegerTargetsDTO, nlohmann::json>(max64_json);
    CHECK_EQ(dto_max.i64, std::numeric_limits<int64_t>::max());
    CHECK_EQ(dto_max.u64, std::numeric_limits<uint64_t>::max());
}

TEST_CASE("Native kale::Json: Numbers backed by Workflow double precision mantissa") {
    // Numbers within 2^53 - 1 (9007199254740991) are exactly representable in double
    std::string exact_double_json = R"({
        "i64": 9007199254740991,
        "u64": 9007199254740991,
        "i8": 0, "u8": 0, "i16": 0, "u16": 0, "i32": 0, "u32": 0
    })";
    auto dto_exact = kale::bind_json_body<IntegerTargetsDTO, kale::Json>(exact_double_json);
    CHECK_EQ(dto_exact.i64, 9007199254740991LL);
    CHECK_EQ(dto_exact.u64, 9007199254740991ULL);

    // For values beyond 2^53, Workflow double parse reflects native IEEE-754 double representation
    std::string large_json = R"({
        "i64": 922337203685477580,
        "u64": 1844674407370955161,
        "i8": 0, "u8": 0, "i16": 0, "u16": 0, "i32": 0, "u32": 0
    })";
    auto dto_large = kale::bind_json_body<IntegerTargetsDTO, kale::Json>(large_json);
    CHECK_EQ(dto_large.i64, static_cast<int64_t>(922337203685477580.0));
    CHECK_EQ(dto_large.u64, static_cast<uint64_t>(1844674407370955161.0));
}

TEST_CASE("Generic DTO: Duplicate key rejection across root, nested, array, and ignored subtrees") {
    auto run_test = []<typename J>() {
        // 1. Duplicate key at root
        std::string dup_root = R"({
            "username": "alice",
            "username": "bob",
            "age": 30
        })";

        try {
            (void)kale::bind_json_body<SimpleAccountDTO, J>(dup_root);
            FAIL("Expected duplicate key error at root");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "username");
            CHECK_EQ(e.expected(), "unique key");
            CHECK_EQ(e.reason(), "duplicate object key in JSON");
            CHECK_EQ(e.status_code(), 400);
        }

        // 2. Duplicate key in nested object
        std::string dup_nested = R"({
            "name": "Alex",
            "age": 30,
            "count": 1,
            "large_id": 1,
            "large_counter": 1,
            "score": 1.0,
            "active": true,
            "item": {
                "title": "T1",
                "title": "T2",
                "code": 100
            },
            "tags": [],
            "numbers": [],
            "items": []
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(dup_nested);
            FAIL("Expected duplicate key error in nested object");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "item.title");
            CHECK_EQ(e.expected(), "unique key");
            CHECK_EQ(e.reason(), "duplicate object key in JSON");
            CHECK_EQ(e.status_code(), 400);
        }

        // 3. Duplicate key in array elements
        std::string dup_array = R"({
            "name": "Alex",
            "age": 30,
            "count": 1,
            "large_id": 1,
            "large_counter": 1,
            "score": 1.0,
            "active": true,
            "item": {"title": "T", "code": 1},
            "tags": [],
            "numbers": [],
            "items": [
                {"title": "ValidItem", "code": 1},
                {"title": "DupItem", "title": "Conflict", "code": 2}
            ]
        })";

        try {
            (void)kale::bind_json_body<GenericMasterDTO, J>(dup_array);
            FAIL("Expected duplicate key error in array element");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "items[1].title");
            CHECK_EQ(e.expected(), "unique key");
            CHECK_EQ(e.reason(), "duplicate object key in JSON");
            CHECK_EQ(e.status_code(), 400);
        }

        // 4. Duplicate key in unknown/ignored root field
        std::string dup_unknown_root = R"({
            "username": "alice",
            "age": 30,
            "ignored_extra": 10,
            "ignored_extra": 20
        })";

        try {
            (void)kale::bind_json_body<SimpleAccountDTO, J>(dup_unknown_root);
            FAIL("Expected duplicate key error for unknown root key");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "ignored_extra");
            CHECK_EQ(e.expected(), "unique key");
            CHECK_EQ(e.reason(), "duplicate object key in JSON");
            CHECK_EQ(e.status_code(), 400);
        }

        // 5. Duplicate key in unknown/ignored nested subtree
        std::string dup_ignored_subtree = R"({
            "username": "alice",
            "age": 30,
            "ignored_subtree": {
                "sub_dup": "first",
                "sub_dup": "second"
            }
        })";

        try {
            (void)kale::bind_json_body<SimpleAccountDTO, J>(dup_ignored_subtree);
            FAIL("Expected duplicate key error in ignored subtree");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "ignored_subtree.sub_dup");
            CHECK_EQ(e.expected(), "unique key");
            CHECK_EQ(e.reason(), "duplicate object key in JSON");
            CHECK_EQ(e.status_code(), 400);
        }

        // 6. Valid unknown keys without duplicates remain ignored and succeed
        std::string valid_unknowns = R"({
            "username": "alice",
            "age": 30,
            "extra_field_1": "ignored_val",
            "extra_field_2": 12345,
            "extra_obj": {"a": 1, "b": 2}
        })";

        auto dto_valid = kale::bind_json_body<SimpleAccountDTO, J>(valid_unknowns);
        CHECK_EQ(dto_valid.username, "alice");
        CHECK_EQ(dto_valid.age, 30);
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Generic DTO: Raw JSON field survival and independent lifetime") {
    auto run_test = []<typename J>() {
        RawPayloadDTO<J> dto;
        {
            std::string raw_input = R"({
                "tag": "audit_event",
                "payload": {
                    "event_id": 9999,
                    "event_name": "deployment_triggered",
                    "nested": {
                        "cluster": "prod-us-east"
                    }
                }
            })";

            dto = kale::bind_json_body<RawPayloadDTO<J>, J>(raw_input);
        }

        // After raw_input is destroyed, dto.payload must survive and remain valid
        CHECK_EQ(dto.tag, "audit_event");

        if constexpr (std::is_same_v<J, kale::Json>) {
            CHECK(dto.payload.is_object());
            CHECK_EQ(dto.payload["event_id"].template get<int>(), 9999);
            CHECK_EQ(dto.payload["event_name"].template get<std::string>(), "deployment_triggered");
            CHECK_EQ(dto.payload["nested"]["cluster"].template get<std::string>(), "prod-us-east");
        } else {
            CHECK(dto.payload.is_object());
            CHECK_EQ(dto.payload["event_id"].template get<int>(), 9999);
            CHECK_EQ(dto.payload["event_name"].template get<std::string>(), "deployment_triggered");
            CHECK_EQ(dto.payload["nested"]["cluster"].template get<std::string>(), "prod-us-east");
        }
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

TEST_CASE("Generic DTO: Empty body, non-object root, and syntax error rejection") {
    auto run_test = []<typename J>() {
        // Empty body
        try {
            (void)kale::bind_json_body<SimpleAccountDTO, J>("");
            FAIL("Expected error on empty body");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "_root");
            CHECK_EQ(e.expected(), "object");
            CHECK_EQ(e.reason(), "empty request body");
        }

        // Non-object root: array
        try {
            (void)kale::bind_json_body<SimpleAccountDTO, J>("[1, 2, 3]");
            FAIL("Expected error on array root");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "_root");
            CHECK_EQ(e.expected(), "object");
            CHECK_EQ(e.reason(), "root JSON value must be an object");
        }

        // Non-object root: string
        try {
            (void)kale::bind_json_body<SimpleAccountDTO, J>("\"just a string\"");
            FAIL("Expected error on string root");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "_root");
            CHECK_EQ(e.expected(), "object");
            CHECK_EQ(e.reason(), "root JSON value must be an object");
        }

        // Malformed JSON syntax
        try {
            (void)kale::bind_json_body<SimpleAccountDTO, J>("{broken_json: 123");
            FAIL("Expected error on malformed syntax");
        } catch (const kale::JsonBindError &e) {
            CHECK_EQ(e.field(), "_root");
            CHECK_EQ(e.expected(), "valid JSON");
            CHECK_EQ(e.status_code(), 400);
        }
    };

    SUBCASE("kale::Json") {
        run_test.template operator()<kale::Json>();
    }
    SUBCASE("nlohmann::json") {
        run_test.template operator()<nlohmann::json>();
    }
}

// ============================================================================
// Real HTTP Server Binding: Service-Zero Verification across Dual Libraries
// ============================================================================

TEST_CASE("Generic HTTP Context Binding: Service zero execution on validation failures") {
    GenericServerFixture fixture;
    std::atomic<int> kale_service_invoked{0};
    std::atomic<int> nloh_service_invoked{0};

    // Route 1: kale::Json binding via Context::bind_json<DTO, kale::Json>()
    fixture->POST("/api/bind/kale", [&kale_service_invoked](kale::Context &ctx) {
        auto dto = ctx.bind_json<SimpleAccountDTO, kale::Json>();
        kale_service_invoked.fetch_add(1);
        ctx.string("kale_user:" + dto.username + ":" + std::to_string(dto.age));
    });

    // Route 2: nlohmann::json binding via Context::bind_json<DTO, nlohmann::json>()
    fixture->POST("/api/bind/nloh", [&nloh_service_invoked](kale::Context &ctx) {
        auto dto = ctx.bind_json<SimpleAccountDTO, nlohmann::json>();
        nloh_service_invoked.fetch_add(1);
        ctx.string("nloh_user:" + dto.username + ":" + std::to_string(dto.age));
    });

    auto test_service_zero = [&](std::string_view endpoint, std::atomic<int> &invoked_counter, std::string prefix) {
        invoked_counter.store(0);

        // 1. Valid request invokes business service
        std::string valid_body = R"({"username": "charlie", "age": 28})";
        auto res_valid = fixture.post(std::string(endpoint), valid_body, {{"Content-Type", "application/json"}});
        CHECK_EQ(res_valid.status, 200);
        CHECK_EQ(invoked_counter.load(), 1);
        CHECK_EQ(res_valid.body, prefix + ":charlie:28");

        // 2. Missing required field: returns 400, business service NOT invoked (count remains 1)
        std::string missing_body = R"({"username": "charlie"})";
        auto res_missing = fixture.post(std::string(endpoint), missing_body, {{"Content-Type", "application/json"}});
        CHECK_EQ(res_missing.status, 400);
        CHECK_EQ(invoked_counter.load(), 1);

        // 3. Null required field: returns 400, business service NOT invoked (count remains 1)
        std::string null_body = R"({"username": null, "age": 28})";
        auto res_null = fixture.post(std::string(endpoint), null_body, {{"Content-Type", "application/json"}});
        CHECK_EQ(res_null.status, 400);
        CHECK_EQ(invoked_counter.load(), 1);

        // 4. Type mismatch: returns 400, business service NOT invoked (count remains 1)
        std::string mismatch_body = R"({"username": "charlie", "age": "twenty_eight"})";
        auto res_mismatch = fixture.post(std::string(endpoint), mismatch_body, {{"Content-Type", "application/json"}});
        CHECK_EQ(res_mismatch.status, 400);
        CHECK_EQ(invoked_counter.load(), 1);

        // 5. Duplicate key: returns 400, business service NOT invoked (count remains 1)
        std::string dup_body = R"({"username": "alice", "username": "bob", "age": 30})";
        auto res_dup = fixture.post(std::string(endpoint), dup_body, {{"Content-Type", "application/json"}});
        CHECK_EQ(res_dup.status, 400);
        CHECK_EQ(invoked_counter.load(), 1);

        // 6. Malformed JSON: returns 400, business service NOT invoked (count remains 1)
        auto res_syntax = fixture.post(std::string(endpoint), "{not_valid_json", {{"Content-Type", "application/json"}});
        CHECK_EQ(res_syntax.status, 400);
        CHECK_EQ(invoked_counter.load(), 1);

        // 7. Empty body: returns 400, business service NOT invoked (count remains 1)
        auto res_empty = fixture.post(std::string(endpoint), "", {{"Content-Type", "application/json"}});
        CHECK_EQ(res_empty.status, 400);
        CHECK_EQ(invoked_counter.load(), 1);
    };

    SUBCASE("kale::Json HTTP endpoint") {
        test_service_zero("/api/bind/kale", kale_service_invoked, "kale_user");
    }
    SUBCASE("nlohmann::json HTTP endpoint") {
        test_service_zero("/api/bind/nloh", nloh_service_invoked, "nloh_user");
    }
}

// ============================================================================
// Native nlohmann roundtrip serialization using CountingDumpWrapper
// ============================================================================

TEST_CASE("Native nlohmann: parse_json and roundtrip serialization dumps once over HTTP") {
    GenericServerFixture fixture;
    std::atomic<int> observed_dump_count{0};

    fixture->POST("/api/nloh/roundtrip", [&observed_dump_count](kale::Context &ctx) {
        // Parse incoming request body using native nlohmann::json
        auto doc = ctx.parse_json<nlohmann::json>();

        // Augment document
        doc["status"] = "processed";
        doc["server_stamp"] = 123456789;

        // Wrap in CountingDumpWrapper to verify single-pass serialization
        int local_dumps = 0;
        CountingDumpWrapper wrapper{std::move(doc), &local_dumps};

        // Emit JSON response
        ctx.json(wrapper);

        observed_dump_count.store(local_dumps);
        ctx.set_header("X-Dump-Count", std::to_string(local_dumps));
    });

    std::string payload = R"({"action": "compute", "client_id": 42})";
    auto res = fixture.post("/api/nloh/roundtrip", payload, {{"Content-Type", "application/json"}});

    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.header("Content-Type"), "application/json; charset=utf-8");
    CHECK_EQ(res.header("X-Dump-Count"), "1");
    CHECK_EQ(observed_dump_count.load(), 1);

    // Verify response body is valid JSON with expected augmented fields
    auto res_doc = nlohmann::json::parse(res.body);
    CHECK_EQ(res_doc["action"].get<std::string>(), "compute");
    CHECK_EQ(res_doc["client_id"].get<int>(), 42);
    CHECK_EQ(res_doc["status"].get<std::string>(), "processed");
    CHECK_EQ(res_doc["server_stamp"].get<int64_t>(), 123456789LL);
}

TEST_CASE("Third User-Defined Type: Alternate basic_json instantiation and HTTP service") {
    // 1. Comprehensive DTO binding via generic primary JsonReader<nlohmann::ordered_json>
    std::string valid_json = R"({
        "name": "Ordered User",
        "age": 30,
        "count": 100,
        "large_id": 922337203685477580,
        "large_counter": 18446744073709551615,
        "score": 98.6,
        "active": true,
        "note": "preserved order",
        "extra_code": 42,
        "item": {"title": "nested", "code": 7},
        "tags": ["one", "two"],
        "numbers": [10, 20],
        "items": [{"title": "i1", "code": 1}]
    })";
    auto dto = kale::bind_json_body<GenericMasterDTO, nlohmann::ordered_json>(valid_json);
    CHECK_EQ(dto.name, "Ordered User");
    CHECK_EQ(dto.age, 30);
    CHECK_EQ(dto.count, 100);
    CHECK_EQ(dto.large_id, 922337203685477580LL);
    CHECK_EQ(dto.large_counter, 18446744073709551615ULL);
    CHECK_EQ(dto.score, 98.6);
    CHECK(dto.active);
    CHECK_EQ(dto.note.value_or(""), "preserved order");
    CHECK_EQ(dto.extra_code.value_or(0), 42);
    CHECK_EQ(dto.item.title, "nested");
    CHECK_EQ(dto.item.code, 7);
    CHECK_EQ(dto.tags.size(), 2);
    CHECK_EQ(dto.numbers.size(), 2);
    CHECK_EQ(dto.items.size(), 1);
    CHECK_EQ(dto.items[0].title, "i1");
    CHECK_EQ(dto.items[0].code, 1);

    // 2. Duplicate key rejection across root, nested, array, and ignored subtrees
    std::string dup_root = R"({"username":"alice","username":"bob","age":30})";
    CHECK_THROWS_AS((kale::bind_json_body<SimpleAccountDTO, nlohmann::ordered_json>(dup_root)), kale::JsonBindError);

    std::string dup_nested = R"({"name":"test","item":{"title":"a","title":"b","code":2}})";
    CHECK_THROWS_AS((kale::bind_json_body<GenericMasterDTO, nlohmann::ordered_json>(dup_nested)), kale::JsonBindError);

    std::string dup_array = R"({"name":"test","items":[{"title":"t1","code":1},{"title":"a","title":"b","code":2}]})";
    CHECK_THROWS_AS((kale::bind_json_body<GenericMasterDTO, nlohmann::ordered_json>(dup_array)), kale::JsonBindError);

    std::string dup_ignored = R"({"title":"alice","code":30,"ignored":{"dup":1,"dup":2}})";
    CHECK_THROWS_AS((kale::bind_json_body<GenericItemDTO, nlohmann::ordered_json>(dup_ignored)), kale::JsonBindError);

    // 3. HTTP Server route with actual parse, bind, and JSON output (dump)
    GenericServerFixture fixture;
    std::atomic<int> ordered_service_invoked{0};
    fixture->POST("/api/ordered_json/test", [&ordered_service_invoked](kale::Context &ctx) {
        auto item = ctx.bind_json<GenericItemDTO, nlohmann::ordered_json>();
        ordered_service_invoked.fetch_add(1);
        nlohmann::ordered_json resp;
        resp["echo_title"] = item.title;
        resp["echo_code"] = item.code;
        resp["status"] = "success";
        ctx.json(resp);
    });

    std::string req_body = R"({"title":"ordered_work","code":777})";
    auto res = fixture.post("/api/ordered_json/test", req_body, {{"Content-Type", "application/json"}});
    CHECK_EQ(res.status, 200);
    CHECK_EQ(ordered_service_invoked.load(), 1);
    auto res_json = nlohmann::ordered_json::parse(res.body);
    CHECK_EQ(res_json["echo_title"].get<std::string>(), "ordered_work");
    CHECK_EQ(res_json["echo_code"].get<int>(), 777);
    CHECK_EQ(res_json["status"].get<std::string>(), "success");

    // 4. Error on duplicate key over HTTP: status 400, service not invoked
    std::string dup_http = R"({"title":"w1","title":"w2","code":777})";
    auto err_res = fixture.post("/api/ordered_json/test", dup_http, {{"Content-Type", "application/json"}});
    CHECK_EQ(err_res.status, 400);
    CHECK_EQ(ordered_service_invoked.load(), 1);
}
} // TEST_SUITE("generic_binding")
