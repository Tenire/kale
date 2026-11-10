#include "doctest/doctest.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coke/http/http_client.h"
#include "coke/wait.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "workflow/HttpMessage.h"
#include "workflow/WFTaskFactory.h"

namespace {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

inline coke::Task<coke::HttpResult> send_test_request(
    const std::string &url,
    const HeaderList &headers = {}) {
    auto *task = WFTaskFactory::create_http_task(url, 0, 1, nullptr);
    auto *req = task->get_req();
    for (const auto &[k, v] : headers) {
        if (k == "Host") {
            req->set_header_pair(k.c_str(), v.c_str());
        } else {
            req->add_header_pair(k, v);
        }
    }
    co_return co_await coke::HttpAwaiter(task);
}

} // namespace

TEST_SUITE("core") {

TEST_CASE("Metadata batch: Context::headers validation and semantics") {
    kale::HttpServerParams params;
    kale::HttpServer server(params);

    server.GET("/test_batch_headers", [](kale::Context &ctx) {
        // 1. Span size mismatch throws std::invalid_argument BEFORE mutating output
        {
            std::array<std::string_view, 2> keys = {"Host", "Accept"};
            std::array<std::string, 3> out = {"sentinel1", "sentinel2", "sentinel3"};
            CHECK_THROWS_AS(ctx.headers(keys, out), std::invalid_argument);
            // Output must not be touched or cleared
            CHECK_EQ(out[0], "sentinel1");
            CHECK_EQ(out[1], "sentinel2");
            CHECK_EQ(out[2], "sentinel3");
        }

        // 2. Empty spans: no-op
        {
            std::span<const std::string_view> empty_keys;
            std::span<std::string> empty_out;
            CHECK_NOTHROW(ctx.headers(empty_keys, empty_out));
        }

        // 3. Basic extraction & case-insensitivity
        {
            std::array<std::string_view, 4> keys = {
                "hOsT", "user-AGENT", "X-cUsToM", "x-missing-header"
            };
            std::array<std::string, 4> out;
            ctx.headers(keys, out);

            CHECK_EQ(out[0], ctx.header("Host"));
            CHECK_EQ(out[1], "BatchTester/1.0");
            CHECK_EQ(out[2], "custom-val-1");
            CHECK_EQ(out[3], ""); // missing
        }

        // 4. Duplicate header folding (standard header with ", ")
        {
            std::array<std::string_view, 1> keys = {"X-Repeated"};
            std::array<std::string, 1> out;
            ctx.headers(keys, out);
            CHECK_EQ(out[0], "val_alpha, val_beta, val_gamma");
            CHECK_EQ(out[0], ctx.header("X-Repeated"));
        }

        // 5. Duplicate Cookie header folding (with "; ")
        {
            std::array<std::string_view, 1> keys = {"Cookie"};
            std::array<std::string, 1> out;
            ctx.headers(keys, out);
            CHECK_EQ(out[0], "c1=v1; c2=v2; c3=v3");
            CHECK_EQ(out[0], ctx.header("Cookie"));
        }

        // 6. Repeated empty headers folding (must not confuse empty value with unseen)
        {
            std::array<std::string_view, 1> keys = {"X-Empty-Repeated"};
            std::array<std::string, 1> out;
            ctx.headers(keys, out);
            // First header is empty string "", second is empty string "".
            // Folded with ", ": "" + ", " + "" = ", "
            CHECK_EQ(out[0], ", ");
            CHECK_EQ(out[0], ctx.header("X-Empty-Repeated"));
        }

        // 7. Duplicate requested keys in keys span: both must receive identical scalar value
        {
            std::array<std::string_view, 4> keys = {
                "X-Repeated", "X-Repeated", "Cookie", "Cookie"
            };
            std::array<std::string, 4> out;
            ctx.headers(keys, out);
            CHECK_EQ(out[0], "val_alpha, val_beta, val_gamma");
            CHECK_EQ(out[1], "val_alpha, val_beta, val_gamma");
            CHECK_EQ(out[2], "c1=v1; c2=v2; c3=v3");
            CHECK_EQ(out[3], "c1=v1; c2=v2; c3=v3");
        }

        // 8. Large key count (> 64) triggering dynamic RAII fallback
        {
            std::vector<std::string_view> large_keys;
            large_keys.reserve(70);
            large_keys.push_back("User-Agent");
            for (int i = 1; i < 70; ++i) {
                large_keys.push_back("X-Nonexistent");
            }
            std::vector<std::string> large_out(70);
            ctx.headers(large_keys, large_out);
            CHECK_EQ(large_out[0], "BatchTester/1.0");
            for (int i = 1; i < 70; ++i) {
                CHECK_EQ(large_out[i], "");
            }
        }

        ctx.string("ok_headers");
    });

    int ret = server.start("127.0.0.1", 0);
    REQUIRE_EQ(ret, 0);
    const unsigned short port = static_cast<unsigned short>(server.listen_port());
    REQUIRE_GT(port, 0);

    HeaderList headers = {
        {"User-Agent", "BatchTester/1.0"},
        {"X-Custom", "custom-val-1"},
        {"X-Repeated", "val_alpha"},
        {"X-Repeated", "val_beta"},
        {"X-Repeated", "val_gamma"},
        {"Cookie", "c1=v1"},
        {"Cookie", "c2=v2"},
        {"Cookie", "c3=v3"},
        {"X-Empty-Repeated", ""},
        {"X-Empty-Repeated", ""}
    };

    auto res = coke::sync_wait(send_test_request("http://127.0.0.1:" + std::to_string(port) + "/test_batch_headers", headers));
    CHECK_EQ(res.state, 0);
    const void *body_ptr = nullptr;
    size_t body_len = 0;
    res.resp.get_parsed_body(&body_ptr, &body_len);
    CHECK_EQ(std::string_view(static_cast<const char*>(body_ptr), body_len), "ok_headers");

    server.stop();
}

TEST_CASE("Metadata batch: Context::queries validation and semantics") {
    kale::HttpServerParams params;
    kale::HttpServer server(params);

    server.GET("/test_batch_queries", [](kale::Context &ctx) {
        // 1. Span size mismatch throws std::invalid_argument BEFORE mutating output
        {
            std::array<std::string_view, 2> keys = {"filter", "page"};
            std::array<std::string, 1> out = {"sentinel"};
            CHECK_THROWS_AS(ctx.queries(keys, out), std::invalid_argument);
            CHECK_EQ(out[0], "sentinel");
        }

        // 2. Empty spans: no-op
        {
            std::span<const std::string_view> empty_keys;
            std::span<std::string> empty_out;
            CHECK_NOTHROW(ctx.queries(empty_keys, empty_out));
        }

        // 3. Basic extraction and scalar parity
        {
            std::array<std::string_view, 5> keys = {
                "filter", "page", "limit", "sort", "region"
            };
            std::array<std::string, 5> out;
            ctx.queries(keys, out);

            CHECK_EQ(out[0], "active");
            CHECK_EQ(out[1], "2");
            CHECK_EQ(out[2], "50");
            CHECK_EQ(out[3], "desc");
            CHECK_EQ(out[4], "us-east");

            for (size_t i = 0; i < keys.size(); ++i) {
                CHECK_EQ(out[i], ctx.query(keys[i]));
            }
        }

        // 4. URL-decoded keys and values with spaces and plus
        {
            std::array<std::string_view, 3> keys = {
                "user name", "greeting message", "plus symbol"
            };
            std::array<std::string, 3> out;
            ctx.queries(keys, out);

            CHECK_EQ(out[0], "Alice Smith");
            CHECK_EQ(out[1], "Hello World");
            CHECK_EQ(out[2], "+");

            for (size_t i = 0; i < keys.size(); ++i) {
                CHECK_EQ(out[i], ctx.query(keys[i]));
            }
        }

        // 5. First-occurrence wins on duplicates (matching scalar query())
        {
            std::array<std::string_view, 2> keys = {"dup_key", "dup_empty_first"};
            std::array<std::string, 2> out;
            ctx.queries(keys, out);

            // In URI: ?dup_key=first_val&dup_key=second_val
            CHECK_EQ(out[0], "first_val");
            CHECK_EQ(out[0], ctx.query("dup_key"));

            // In URI: ?dup_empty_first=&dup_empty_first=later_val
            // First occurrence is empty value "", which must win over later_val!
            CHECK_EQ(out[1], "");
            CHECK_EQ(out[1], ctx.query("dup_empty_first"));
        }

        // 6. Bare key without '=' vs empty value vs missing key
        {
            std::array<std::string_view, 3> keys = {"bare_flag", "empty_val", "missing_param"};
            std::array<std::string, 3> out;
            ctx.queries(keys, out);

            CHECK_EQ(out[0], "");
            CHECK(ctx.has_query("bare_flag"));
            CHECK_EQ(out[0], ctx.query("bare_flag"));

            CHECK_EQ(out[1], "");
            CHECK(ctx.has_query("empty_val"));
            CHECK_EQ(out[1], ctx.query("empty_val"));

            CHECK_EQ(out[2], "");
            CHECK_FALSE(ctx.has_query("missing_param"));
            CHECK_EQ(out[2], ctx.query("missing_param"));
        }

        // 7. Duplicate requested keys in keys span
        {
            std::array<std::string_view, 4> keys = {
                "filter", "filter", "page", "page"
            };
            std::array<std::string, 4> out;
            ctx.queries(keys, out);

            CHECK_EQ(out[0], "active");
            CHECK_EQ(out[1], "active");
            CHECK_EQ(out[2], "2");
            CHECK_EQ(out[3], "2");
        }

        // 8. Large key count (> 64) triggering dynamic RAII fallback
        {
            std::vector<std::string_view> large_keys;
            large_keys.reserve(70);
            large_keys.push_back("page");
            for (int i = 1; i < 70; ++i) {
                large_keys.push_back("nonexistent_q");
            }
            std::vector<std::string> large_out(70);
            ctx.queries(large_keys, large_out);
            CHECK_EQ(large_out[0], "2");
            for (int i = 1; i < 70; ++i) {
                CHECK_EQ(large_out[i], "");
            }
        }

        ctx.string("ok_queries");
    });

    server.GET("/test_empty_uri_query", [](kale::Context &ctx) {
        std::array<std::string_view, 2> keys = {"a", "b"};
        std::array<std::string, 2> out = {"sentinel_a", "sentinel_b"};
        ctx.queries(keys, out);
        CHECK_EQ(out[0], "");
        CHECK_EQ(out[1], "");
        ctx.string("ok_empty_query");
    });

    int ret = server.start("127.0.0.1", 0);
    REQUIRE_EQ(ret, 0);
    const unsigned short port = static_cast<unsigned short>(server.listen_port());
    REQUIRE_GT(port, 0);

    const std::string query_uri = "http://127.0.0.1:" + std::to_string(port) +
        "/test_batch_queries?"
        "filter=active&page=2&limit=50&sort=desc&region=us-east"
        "&user%20name=Alice%20Smith&greeting+message=Hello+World&plus%20symbol=%2B"
        "&dup_key=first_val&dup_key=second_val"
        "&dup_empty_first=&dup_empty_first=later_val"
        "&bare_flag&empty_val=";

    auto res = coke::sync_wait(send_test_request(query_uri));
    CHECK_EQ(res.state, 0);
    const void *body_ptr = nullptr;
    size_t body_len = 0;
    res.resp.get_parsed_body(&body_ptr, &body_len);
    CHECK_EQ(std::string_view(static_cast<const char*>(body_ptr), body_len), "ok_queries");

    // Test request without query string
    auto res_empty = coke::sync_wait(send_test_request("http://127.0.0.1:" + std::to_string(port) + "/test_empty_uri_query"));
    CHECK_EQ(res_empty.state, 0);

    // Test request with bare '?'
    auto res_bare = coke::sync_wait(send_test_request("http://127.0.0.1:" + std::to_string(port) + "/test_empty_uri_query?"));
    CHECK_EQ(res_bare.state, 0);

    server.stop();
}

TEST_CASE("Metadata batch: combined 10 headers, 5 queries, 1 cookie end-to-end integration") {
    kale::HttpServerParams params;
    kale::HttpServer server(params);

    server.GET("/metadata/combined", [](kale::Context &ctx) {
        static constexpr std::array<std::string_view, 10> kHeaderKeys = {
            "Host", "User-Agent", "Accept", "X-Request-Id",
            "X-Client-Version", "X-Forwarded-For", "X-Trace-Id",
            "X-Auth-Token", "X-Env", "X-Region"
        };
        std::array<std::string, 10> header_vals;
        ctx.headers(kHeaderKeys, header_vals);

        static constexpr std::array<std::string_view, 5> kQueryKeys = {
            "filter", "page", "limit", "sort", "region"
        };
        std::array<std::string, 5> query_vals;
        ctx.queries(kQueryKeys, query_vals);

        const std::string_view session_id = ctx.cookie("bench_session");

        std::string response_body;
        response_body.reserve(256);
        response_body.append("H:").append(header_vals[0])
                     .append("|").append(header_vals[1])
                     .append("|").append(header_vals[2])
                     .append("|").append(header_vals[3])
                     .append("|").append(header_vals[4])
                     .append("|").append(header_vals[5])
                     .append("|").append(header_vals[6])
                     .append("|").append(header_vals[7])
                     .append("|").append(header_vals[8])
                     .append("|").append(header_vals[9])
                     .append(";Q:").append(query_vals[0])
                     .append("|").append(query_vals[1])
                     .append("|").append(query_vals[2])
                     .append("|").append(query_vals[3])
                     .append("|").append(query_vals[4])
                     .append(";C:").append(session_id);

        ctx.string(response_body);
    });

    int ret = server.start("127.0.0.1", 0);
    REQUIRE_EQ(ret, 0);
    const unsigned short port = static_cast<unsigned short>(server.listen_port());
    REQUIRE_GT(port, 0);

    HeaderList headers = {
        {"Host", "benchmark.kale.internal"},
        {"User-Agent", "BenchmarkWorker/2.0"},
        {"Accept", "application/json, text/plain"},
        {"X-Request-Id", "req-uuid-12345"},
        {"X-Client-Version", "v1.5.0"},
        {"X-Forwarded-For", "192.168.1.100"},
        {"X-Trace-Id", "trace-777-abc"},
        {"X-Auth-Token", "bearer-token-secret"},
        {"X-Env", "production"},
        {"X-Region", "ap-southeast-1"},
        {"Cookie", "bench_session=sess-jwt-val-xyz"}
    };

    const std::string url = "http://127.0.0.1:" + std::to_string(port) +
        "/metadata/combined?filter=active&page=3&limit=25&sort=asc&region=apac";

    auto res = coke::sync_wait(send_test_request(url, headers));
    CHECK_EQ(res.state, 0);
    const void *body_ptr = nullptr;
    size_t body_len = 0;
    res.resp.get_parsed_body(&body_ptr, &body_len);
    std::string_view body(static_cast<const char*>(body_ptr), body_len);

    const std::string expected =
        "H:benchmark.kale.internal|BenchmarkWorker/2.0|application/json, text/plain|req-uuid-12345|v1.5.0|192.168.1.100|trace-777-abc|bearer-token-secret|production|ap-southeast-1"
        ";Q:active|3|25|asc|apac"
        ";C:sess-jwt-val-xyz";

    CHECK_EQ(body, expected);

    server.stop();
}

} // TEST_SUITE("core")
