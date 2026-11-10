#include "doctest/doctest.h"
#include "kale/http/json.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

TEST_SUITE("core") {

TEST_CASE("JSON root scalar assignment and replacement") {
    using kale::Json;

    // bool
    Json j_bool;
    j_bool = true;
    CHECK(j_bool.is_boolean());
    CHECK(j_bool.get<bool>());
    CHECK_EQ(j_bool.dump(), "true");
    j_bool = false;
    CHECK_FALSE(j_bool.get<bool>());
    CHECK_EQ(j_bool.dump(), "false");

    // integer / floating-point
    Json j_num;
    j_num = 42;
    CHECK(j_num.is_number());
    CHECK_EQ(j_num.get<int>(), 42);
    CHECK_EQ(j_num.dump(), "42");
    j_num = 3.5;
    CHECK_EQ(j_num.get<double>(), 3.5);

    // string
    Json j_str;
    j_str = "hello";
    CHECK(j_str.is_string());
    CHECK_EQ(j_str.get<std::string>(), "hello");
    CHECK_EQ(j_str.dump(), "\"hello\"");

    std::string_view sv = "world";
    j_str = sv;
    CHECK_EQ(j_str.get<std::string_view>(), "world");
    CHECK_EQ(j_str.dump(), "\"world\"");

    std::string s = "std_string";
    j_str = s;
    CHECK_EQ(j_str.get<std::string>(), "std_string");

    // nullptr
    Json j_null;
    j_null = 100;
    CHECK_FALSE(j_null.is_null());
    j_null = nullptr;
    CHECK(j_null.is_null());
    CHECK_EQ(j_null.dump(), "null");

    // object / move
    Json j_obj;
    j_obj = Json::Object{{"k", 1}};
    CHECK(j_obj.is_object());
    CHECK_EQ(j_obj["k"].get<int>(), 1);
}

TEST_CASE("JSON object proxy scalar assignments and repeated overwrite") {
    using kale::Json;

    Json val;
    val["active"] = true;
    val["count"] = 42;
    val["rate"] = 1.25;
    val["message"] = "Hello, World!";
    val["empty"] = nullptr;

    CHECK(val.is_object());
    CHECK_EQ(val.size(), 5);
    CHECK(val["active"].is_boolean());
    CHECK(val["active"].get<bool>());
    CHECK_EQ(val["count"].get<int>(), 42);
    CHECK_EQ(val["rate"].get<double>(), 1.25);
    CHECK_EQ(val["message"].get<std::string>(), "Hello, World!");
    CHECK(val["empty"].is_null());

    // Repeated assignment through same proxy
    auto p = val["count"];
    p = 100;
    CHECK_EQ(val["count"].get<int>(), 100);
    p = 200;
    CHECK_EQ(val["count"].get<int>(), 200);
    CHECK_EQ(val.size(), 5);

    // Repeated assignment through indexing (duplicate key overwrite)
    val["active"] = false;
    CHECK_FALSE(val["active"].get<bool>());
    CHECK_EQ(val.size(), 5);

    // Overwriting null field
    auto null_obj = Json::parse("{\"val\": null}");
    CHECK(null_obj["val"].is_null());
    null_obj["val"] = 999;
    CHECK_EQ(null_obj.size(), 1);
    CHECK_EQ(null_obj["val"].get<int>(), 999);
    CHECK_EQ(null_obj.dump(), "{\"val\":999}");

    // Preserving order on key overwrite
    Json ordered = Json::Object{};
    ordered["a"] = 1;
    ordered["b"] = 2;
    ordered["c"] = 3;
    ordered["b"] = 42;
    CHECK_EQ(ordered.size(), 3);
    CHECK_EQ(ordered.dump(), "{\"a\":1,\"b\":42,\"c\":3}");
}

TEST_CASE("JSON object builder and typed push_back") {
    using kale::Json;

    Json obj = Json::Object{};
    obj.push_back("active", true)
       .push_back("count", 42)
       .push_back("rate", 0.5)
       .push_back("message", "chain")
       .push_back("none", nullptr);

    CHECK_EQ(obj.size(), 5);
    CHECK(obj["active"].get<bool>());
    CHECK_EQ(obj["count"].get<int>(), 42);
    CHECK_EQ(obj["rate"].get<double>(), 0.5);
    CHECK_EQ(obj["message"].get<std::string>(), "chain");
    CHECK(obj["none"].is_null());

    // Deduplication via push_back
    obj.push_back("count", 100);
    CHECK_EQ(obj.size(), 5);
    CHECK_EQ(obj["count"].get<int>(), 100);

    // Array push_back chaining
    Json arr = Json::Array{};
    arr.push_back(10).push_back("foo").push_back(true).push_back(nullptr);
    CHECK_EQ(arr.size(), 4);
    CHECK_EQ(arr[0].get<int>(), 10);
    CHECK_EQ(arr[1].get<std::string>(), "foo");
    CHECK(arr[2].get<bool>());
    CHECK(arr[3].is_null());

    // Array proxy assignment
    arr[0] = 99;
    arr[1] = "bar";
    arr[2] = false;
    CHECK_EQ(arr[0].get<int>(), 99);
    CHECK_EQ(arr[1].get<std::string>(), "bar");
    CHECK_FALSE(arr[2].get<bool>());
}

TEST_CASE("JSON nested object creation and duplicate semantics") {
    using kale::Json;

    Json val;
    val["sub"]["child"] = 123;
    val["sub"]["name"] = "nested";
    CHECK_EQ(val.dump(), "{\"sub\":{\"child\":123,\"name\":\"nested\"}}");
    CHECK_EQ(val["sub"]["child"].get<int>(), 123);
    CHECK_EQ(val["sub"]["name"].get<std::string>(), "nested");

    // Overwrite nested field
    val["sub"]["child"] = 456;
    CHECK_EQ(val.dump(), "{\"sub\":{\"child\":456,\"name\":\"nested\"}}");
    CHECK_EQ(val["sub"]["child"].get<int>(), 456);

    // 3 levels deep
    Json deep;
    deep["l1"]["l2"]["l3"] = "deep_val";
    CHECK_EQ(deep["l1"]["l2"]["l3"].get<std::string>(), "deep_val");
    CHECK_EQ(deep.dump(), "{\"l1\":{\"l2\":{\"l3\":\"deep_val\"}}}");
}

TEST_CASE("JSON string_view constructors: short, long, and embedded-NUL") {
    using kale::Json;

    // Short string (< 256)
    std::string_view short_sv = "hello world";
    Json j_short(short_sv);
    CHECK(j_short.is_string());
    CHECK_EQ(j_short.get<std::string_view>(), short_sv);
    CHECK_EQ(j_short.dump(), "\"hello world\"");

    // Long string (>= 256)
    std::string long_str(400, 'z');
    std::string_view long_sv(long_str);
    Json j_long(long_sv);
    CHECK(j_long.is_string());
    CHECK_EQ(j_long.get<std::string_view>(), long_sv);
    CHECK_EQ(j_long.dump(), "\"" + long_str + "\"");

    // Short string with embedded NUL (workflow parser stops at first NUL)
    std::string_view nul_short("abc\0def", 7);
    Json j_nul_short(nul_short);
    CHECK(j_nul_short.is_string());
    CHECK_EQ(j_nul_short.get<std::string_view>(), "abc");
    CHECK_EQ(j_nul_short.dump(), "\"abc\"");

    // Long string with embedded NUL (workflow parser stops at first NUL)
    std::string nul_long(400, 'w');
    nul_long[150] = '\0';
    std::string_view nul_long_sv(nul_long);
    Json j_nul_long(nul_long_sv);
    CHECK(j_nul_long.is_string());
    CHECK_EQ(j_nul_long.get<std::string_view>(), std::string(150, 'w'));
    CHECK_EQ(j_nul_long.dump(), "\"" + std::string(150, 'w') + "\"");
}

TEST_CASE("Exact dynamic benchmark JSON payload and dump capacity") {
    using kale::Json;

    Json val;
    val["active"] = true;
    val["count"] = 42;
    val["message"] = "Hello, World!";

    std::string dumped = val.dump();
    CHECK_EQ(dumped, "{\"active\":true,\"count\":42,\"message\":\"Hello, World!\"}");
    CHECK_EQ(dumped.size(), 52);

    // Verify round-trip parsing
    Json parsed = Json::parse(dumped);
    CHECK(parsed["active"].get<bool>());
    CHECK_EQ(parsed["count"].get<int>(), 42);
    CHECK_EQ(parsed["message"].get<std::string>(), "Hello, World!");
}

TEST_CASE("JSON numeric formatting boundaries and exactness") {
    using kale::Json;

    // Zero and negative zero
    Json j0(0);
    CHECK_EQ(j0.dump(), "0");

    // Small integers
    Json j1(1);
    CHECK_EQ(j1.dump(), "1");
    Json j_neg(-42);
    CHECK_EQ(j_neg.dump(), "-42");

    // Large 64-bit numbers
    Json j_large(1234567890123456LL);
    CHECK_EQ(j_large.dump(), "1234567890123456");
    CHECK_EQ(j_large.get<int64_t>(), 1234567890123456LL);

    Json j_max_safe(9007199254740991LL); // 2^53 - 1
    CHECK_EQ(j_max_safe.dump(), "9007199254740991");

    // Floating-point values
    Json j_float(3.141592653589793);
    std::string f_dump = j_float.dump();
    CHECK(f_dump.starts_with("3.14159265358979"));
    CHECK_EQ(j_float.get<double>(), doctest::Approx(3.141592653589793));

    Json j_frac(0.125);
    CHECK_EQ(j_frac.dump(), "0.125");
    CHECK_EQ(j_frac.get<double>(), 0.125);

    Json j_sci(1e-5);
    Json j_sci_parsed = Json::parse(j_sci.dump());
    CHECK_EQ(j_sci_parsed.get<double>(), doctest::Approx(1e-5));
}

TEST_CASE("JSON wrong-type child push_back on object child must fail safely") {
    using kale::Json;
    Json root = Json::Object{};
    root["scalar"] = 42;

    bool threw = false;
    try {
        root["scalar"].push_back("extra", 100);
    } catch (...) {
        threw = true;
    }

    if (!threw) {
        CHECK(root["scalar"].is_number());
        CHECK_EQ(root["scalar"].get<int>(), 42);
    }
}

TEST_CASE("JSON wrong-type child push_back on array child must fail safely") {
    using kale::Json;
    Json arr = Json::Array{};
    arr.push_back(42);

    bool threw = false;
    try {
        arr[0].push_back(99);
    } catch (...) {
        threw = true;
    }

    if (!threw) {
        CHECK(arr[0].is_number());
        CHECK_EQ(arr[0].get<int>(), 42);
    }
}

TEST_CASE("JSON root assignment from object descendant preserves copied value") {
    using kale::Json;
    Json root = Json::Object{};
    root["nested"] = Json::Object{{"k1", 10}, {"k2", 20}};

    root = root["nested"];

    CHECK(root.is_object());
    CHECK_EQ(root["k1"].get<int>(), 10);
    CHECK_EQ(root["k2"].get<int>(), 20);
}

TEST_CASE("JSON root assignment from scalar descendant preserves copied value") {
    using kale::Json;
    Json root = Json::Object{};
    root["val"] = 42;

    root = root["val"];

    CHECK(root.is_number());
    CHECK_EQ(root.get<int>(), 42);
}

TEST_CASE("JSON self-aliased string_view assignment preserves content") {
    using kale::Json;
    Json j("hello self-aliased string");
    std::string_view sv = j.get<std::string_view>();

    j = sv;

    CHECK(j.is_string());
    CHECK_EQ(j.get<std::string_view>(), "hello self-aliased string");
    CHECK_EQ(j.dump(), "\"hello self-aliased string\"");
}

TEST_CASE("JSON self-aliased long string_view assignment preserves content") {
    using kale::Json;
    std::string long_str(300, 'k');
    Json j(long_str);
    std::string_view sv = j.get<std::string_view>();

    j = sv;

    CHECK(j.is_string());
    CHECK_EQ(j.get<std::string_view>(), long_str);
}

TEST_CASE("JSON clear through child proxy updates parent object") {
    using kale::Json;
    Json root = Json::Object{};
    root["nested"] = Json::Object{{"a", 1}, {"b", 2}};
    CHECK_EQ(root["nested"].size(), 2);

    auto child = root["nested"];
    child.clear();

    CHECK(child.empty());
    CHECK_EQ(child.size(), 0);
    CHECK(root["nested"].empty());
    CHECK_EQ(root["nested"].size(), 0);
    CHECK_EQ(root.dump(), "{\"nested\":{}}");
}

TEST_CASE("JSON clear through child proxy updates parent array") {
    using kale::Json;
    Json root = Json::Object{};
    root["arr"] = Json::Array{1, 2, 3};
    CHECK_EQ(root["arr"].size(), 3);

    auto child = root["arr"];
    child.clear();

    CHECK(child.empty());
    CHECK_EQ(child.size(), 0);
    CHECK(root["arr"].empty());
    CHECK_EQ(root["arr"].size(), 0);
    CHECK_EQ(root.dump(), "{\"arr\":[]}");
}

TEST_CASE("JSON mutable iterator assignment updates array container and remains iterable") {
    using kale::Json;
    Json arr = Json::Array{1, 2, 3};
    auto it = arr.begin();
    REQUIRE(it != arr.end());

    *it = 100;
    CHECK_EQ(arr[0].get<int>(), 100);

    ++it;
    REQUIRE(it != arr.end());
    CHECK_EQ((*it).get<int>(), 2);
}

TEST_CASE("JSON mutable iterator assignment updates object container and remains iterable") {
    using kale::Json;
    Json obj = Json::Object{{"first", 1}, {"second", 2}};
    auto it = obj.begin();
    REQUIRE(it != obj.end());
    CHECK_EQ(it->key(), "first");

    *it = 100;
    CHECK_EQ(it->key(), "first");
    CHECK_EQ((*it).key(), "first");
    CHECK_EQ((*it).get<int>(), 100);
    CHECK_EQ(obj["first"].get<int>(), 100);

    // Re-assignment at same position
    *it = 105;
    CHECK_EQ(it->key(), "first");
    CHECK_EQ(obj["first"].get<int>(), 105);

    ++it;
    REQUIRE(it != obj.end());
    CHECK_EQ(it->key(), "second");
    CHECK_EQ((*it).get<int>(), 2);

    *it = 200;
    CHECK_EQ(it->key(), "second");
    CHECK_EQ(obj["second"].get<int>(), 200);

    ++it;
    CHECK(it == obj.end());
}

TEST_CASE("JSON mutable reverse iterator assignment updates container and remains iterable") {
    using kale::Json;

    // Array reverse iteration
    Json arr = Json::Array{1, 2, 3};
    auto r_arr = arr.rbegin();
    REQUIRE(r_arr != arr.rend());
    *r_arr = 300;
    CHECK_EQ(arr[2].get<int>(), 300);
    ++r_arr;
    REQUIRE(r_arr != arr.rend());
    CHECK_EQ((*r_arr).get<int>(), 2);
    *r_arr = 200;
    CHECK_EQ(arr[1].get<int>(), 200);
    ++r_arr;
    REQUIRE(r_arr != arr.rend());
    CHECK_EQ((*r_arr).get<int>(), 1);
    ++r_arr;
    CHECK(r_arr == arr.rend());

    // Object reverse iteration
    Json obj = Json::Object{{"a", 1}, {"b", 2}};
    auto r_obj = obj.rbegin();
    REQUIRE(r_obj != obj.rend());
    CHECK_EQ(r_obj->key(), "b");
    *r_obj = 20;
    CHECK_EQ(r_obj->key(), "b");
    CHECK_EQ((*r_obj).key(), "b");
    CHECK_EQ(obj["b"].get<int>(), 20);
    ++r_obj;
    REQUIRE(r_obj != obj.rend());
    CHECK_EQ(r_obj->key(), "a");
    *r_obj = 10;
    CHECK_EQ(r_obj->key(), "a");
    CHECK_EQ(obj["a"].get<int>(), 10);
    ++r_obj;
    CHECK(r_obj == obj.rend());
}

TEST_CASE("JSON const-object child proxy assignment preserves key") {
    using kale::Json;
    Json obj = Json::Object{};
    obj["target"] = 42;
    const Json &c_obj = obj;

    auto proxy = c_obj["target"];
    CHECK_EQ(proxy.key(), "target");

    proxy = 99;
    CHECK_EQ(proxy.key(), "target");
    CHECK_EQ(obj["target"].get<int>(), 99);
    CHECK_FALSE(obj.has(""));
}

TEST_CASE("JSON native nonfinite numeric representation and get parity") {
    using kale::Json;

    // Positive infinity
    const double pos_inf = std::numeric_limits<double>::infinity();
    Json j_pos(pos_inf);
    CHECK(j_pos.is_number());
    CHECK(std::isinf(j_pos.get<double>()));
    CHECK(j_pos.get<double>() > 0);
    CHECK(std::isinf(j_pos.get<float>()));
    CHECK_EQ(j_pos.get<int>(), 0);

    // Negative infinity
    const double neg_inf = -std::numeric_limits<double>::infinity();
    Json j_neg(neg_inf);
    CHECK(j_neg.is_number());
    CHECK(std::isinf(j_neg.get<double>()));
    CHECK(j_neg.get<double>() < 0);
    CHECK(std::isinf(j_neg.get<float>()));
    CHECK_EQ(j_neg.get<int>(), 0);

    // Quiet NaN
    const double nan_val = std::numeric_limits<double>::quiet_NaN();
    Json j_nan(nan_val);
    CHECK(j_nan.is_number());
    CHECK(std::isnan(j_nan.get<double>()));
    CHECK(std::isnan(j_nan.get<float>()));
    CHECK_EQ(j_nan.get<int64_t>(), 0);

    // Native Workflow parse parity for huge exponent (exp > 291 -> INFINITY)
    Json parsed_overflow = Json::parse("{\"val\": 1e999}");
    CHECK(parsed_overflow["val"].is_number());
    CHECK(std::isinf(parsed_overflow["val"].get<double>()));
    CHECK(parsed_overflow["val"].get<double>() > 0);

    Json parsed_neg_overflow = Json::parse("{\"val\": -1e999}");
    CHECK(parsed_neg_overflow["val"].is_number());
    CHECK(std::isinf(parsed_neg_overflow["val"].get<double>()));
    CHECK(parsed_neg_overflow["val"].get<double>() < 0);

    // Native Workflow parser rejects non-standard JSON literals per RFC 8259
    CHECK_THROWS_AS(Json::parse("inf"), kale::JsonParseError);
    CHECK_THROWS_AS(Json::parse("-inf"), kale::JsonParseError);
    CHECK_THROWS_AS(Json::parse("nan"), kale::JsonParseError);
    CHECK_THROWS_AS(Json::parse("NaN"), kale::JsonParseError);
}

} // TEST_SUITE("core")
