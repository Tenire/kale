#include "doctest/doctest.h"
#include "kale/http/json.h"
#include "kale/core/compress.h"
#include "kale/core/cookie.h"
#include "kale/security/signed_cookie.h"
#include "kale/http/sse.h"
#include "kale/http/field_desc.h"
#include "kale/http/json_reader.h"
#include "kale/http/binding.h"
#include "kale/core/context.h"
#include "http/base64.h"
#include "security/crypto_util.h"
#include "http/mime_type.h"
#include <future>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <zstd.h>

#include "coke/sleep.h"
#include "coke/task.h"
#include "coke/wait.h"
#include "kale/core/http_server.h"
#include "kale/client/http_client.h"

namespace {

class AuditTestServer {
public:
    explicit AuditTestServer(kale::HttpServerParams params = {})
        : server_(params) {
        int ret = server_.start("127.0.0.1", 0);
        REQUIRE_EQ(ret, 0);
        REQUIRE(server_.is_running());
        port_ = static_cast<unsigned short>(server_.listen_port());
        REQUIRE_GT(port_, 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(port_);
    }

    ~AuditTestServer() {
        if (server_.is_running()) {
            server_.stop();
        }
    }

    AuditTestServer(const AuditTestServer &) = delete;
    AuditTestServer &operator=(const AuditTestServer &) = delete;

    kale::HttpServer &server() noexcept { return server_; }
    unsigned short port() const noexcept { return port_; }
    const std::string &base_url() const noexcept { return base_url_; }

    std::string url(std::string_view path) const {
        if (path.empty() || path.front() != '/') {
            return base_url_ + "/" + std::string(path);
        }
        return base_url_ + std::string(path);
    }

private:
    kale::HttpServer server_;
    unsigned short port_{0};
    std::string base_url_;
};


struct RawJsonPayloadDTO {
    kale::Json payload;
    KALE_BIND(
        KALE_FIELD(payload)
    )
};

struct SimpleAccountDTO {
    std::string username;
    KALE_BIND(
        KALE_FIELD(username)
    )
};

struct AgeUserDTO {
    int age{0};
    KALE_BIND(
        KALE_FIELD(age)
    )
};
} // namespace


TEST_SUITE("core") {
TEST_CASE("JSON C++20 access preserves Workflow-backed values and defaults") {
    using kale::Json;
    auto value = Json::parse("{\"count\":42,\"name\":\"kale\",\"enabled\":true}");
    CHECK(value.is_object());
    CHECK_EQ(value["count"].get<int>(), 42);
    CHECK_EQ(value["name"].get<std::string>(), "kale");
    CHECK(value["enabled"].get<bool>());
    CHECK_EQ(value["missing"].get_or(7), 7);
    CHECK_EQ(value["name"].get_or(7), 7);
    auto assigned = Json::Object{{"value", 1}};
    assigned["value"] = 2;
    CHECK_EQ(assigned["value"].get<int>(), 2);
    CHECK_EQ(Json::parse("[1,2,3]").size(), 3);
    CHECK_EQ(Json::parse("null").dump(), "null");
}
TEST_CASE("Crypto rejects malformed encodings and authenticates standard vectors") {
    using kale::Base64;
    CHECK_EQ(*Base64::decode("Zg=="), "f");
    CHECK_EQ(*Base64::decode_url("Zg"), "f");
    CHECK_EQ(*Base64::decode_url("Zg=="), "f");
    CHECK_EQ(*Base64::decode("Zm9v"), "foo");
    for (const auto bad : {"A", "Zg", "Zg=", "Zg===", "Zg==tail", "Zh==", "Zm9=", "AA_A", "AA-A", "AA A"})
        CHECK_FALSE(Base64::decode(bad).has_value());
    for (const auto bad : {"A", "Zh", "Zm9", "Zg=", "Zg===", "Zg==tail", "AA/A", "AA+A"})
        CHECK_FALSE(Base64::decode_url(bad).has_value());
    CHECK_EQ(kale::CryptoUtil::hmac_sha256_base64url("The quick brown fox jumps over the lazy dog", "key"),
             "97yD9DBThCSxMpjmqm-xQ-9NWaFJRhdZl0edvC0aPNg");
    CHECK_EQ(kale::CryptoUtil::sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    auto signed_value = kale::sign_cookie_value("session", "secret");
    CHECK_EQ(*kale::unsign_cookie_value(signed_value, "secret"), "session");
    CHECK_FALSE(kale::unsign_cookie_value(signed_value, "wrong").has_value());
    CHECK_FALSE(kale::CryptoUtil::random_bytes(nullptr, 1));
    CHECK(kale::CryptoUtil::random_bytes(nullptr, 0));
    kale::Cookie cookie("a", "safe");
    cookie.set_path("/\r\nInjected: yes");
    CHECK_THROWS_AS(cookie.to_string(), std::invalid_argument);
}
TEST_CASE("Decompression bounds output and rejects incomplete streams") {
    const std::string payload(128 * 1024, 'x');
    std::string encoded, output;
    REQUIRE(kale::Compress::gzip(payload, encoded));
    CHECK_FALSE(kale::Compress::ungzip(encoded, output, payload.size() - 1));
    CHECK(output.empty());
    REQUIRE(kale::Compress::ungzip(encoded, output, payload.size()));
    CHECK_EQ(output, payload);
    CHECK_FALSE(kale::Compress::ungzip(std::string_view(encoded).substr(0, encoded.size() - 1), output));
    CHECK_FALSE(kale::Compress::ungzip(encoded + "garbage", output));
    REQUIRE(kale::Compress::ungzip(encoded + encoded, output, payload.size() * 2));
    CHECK_EQ(output, payload + payload);
    REQUIRE(kale::Compress::zstd(payload, encoded));
    CHECK_FALSE(kale::Compress::unzstd(encoded, output, payload.size() - 1));
    REQUIRE(kale::Compress::unzstd(encoded, output, payload.size()));
    CHECK_EQ(output, payload);
    CHECK_FALSE(kale::Compress::unzstd(std::string_view(encoded).substr(0, encoded.size() - 1), output));
    CHECK_FALSE(kale::Compress::unzstd(encoded + "garbage", output));
    REQUIRE(kale::Compress::unzstd(encoded + encoded, output, payload.size() * 2));
    CHECK_EQ(output, payload + payload);
    auto *context = ZSTD_createCCtx();
    REQUIRE(context);
    REQUIRE_FALSE(ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 0)));
    encoded.resize(ZSTD_compressBound(payload.size()));
    const size_t size = ZSTD_compress2(context, encoded.data(), encoded.size(), payload.data(), payload.size());
    ZSTD_freeCCtx(context);
    REQUIRE_FALSE(ZSTD_isError(size));
    encoded.resize(size);
    CHECK_FALSE(kale::Compress::unzstd(std::string_view(encoded).substr(0, encoded.size() - 1), output));
    REQUIRE(kale::Compress::unzstd(encoded, output));
    CHECK_EQ(output, payload);
    REQUIRE(kale::Compress::gzip("", encoded));
    REQUIRE(kale::Compress::ungzip(encoded, output, 0));
    CHECK_EQ(output, "");
    CHECK_EQ(kale::Compress::negotiate("gzip;q=2,zstd;q=0"), kale::CompressionEncoding::None);
}
TEST_CASE("SSE preserves data trailing line endings and MIME uses basename") {
    kale::SseEvent event;
    event.data = "a\n";
    CHECK_EQ(event.format(), "data: a\ndata: \n\n");
    event.data = "a\r\nb\r";
    CHECK_EQ(event.format(), "data: a\ndata: b\ndata: \n\n");
    CHECK_EQ(kale::get_mime_type("/www.v2/readme"), "application/octet-stream");
    CHECK_EQ(kale::get_mime_type("/www/INDEX.HTML"), "text/html; charset=utf-8");
    CHECK_EQ(kale::get_mime_type(".woff2"), "font/woff2");
}

TEST_CASE("HttpClient C2: non-2xx reception stops at configured wire limit") {
    AuditTestServer server;
    std::atomic<int> chunks_sent{0};
    std::atomic<bool> server_peer_closed{false};
    std::promise<void> server_finished_prom;
    auto server_finished_fut = server_finished_prom.get_future();

    server.server().GET("/c2_oversized", [&](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_status(500);
        // Upstream attempts to stream 10 chunks of 10KB (total 100KB)
        for (int i = 0; i < 10; ++i) {
            bool ok = co_await ctx.send_chunk(std::string(10 * 1024, 'e'));
            if (!ok) {
                server_peer_closed.store(true);
                break;
            }
            chunks_sent.fetch_add(1);
            co_await coke::sleep(std::chrono::milliseconds(10));
        }
        co_await ctx.end_chunk();
        server_finished_prom.set_value();
    });

    server.server().GET("/c2_small_500", [](kale::Context &ctx) {
        ctx.set_status(500).string("error: small payload");
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;

        // 1. Verify small non-2xx response within limit works and delivers status/body
        auto resp_small = co_await client.get(server.url("/c2_small_500"))
            .max_body_bytes(1024)
            .send();
        CHECK_EQ(resp_small.status(), 500);
        std::string small_body = co_await resp_small.read_body();
        CHECK_EQ(small_body, "error: small payload");

        // 2. Verify oversized non-2xx response actively stops reception on the wire
        // Configured limit: 5000 bytes. Parser limit: 5000 + 65536 = 70536 bytes.
        // Server sends 100KB in 10 chunks.
        bool caught_body_too_large = false;
        try {
            auto resp_over = co_await client.get(server.url("/c2_oversized"))
                .max_body_bytes(5000)
                .send();
            co_await resp_over.read_body();
        } catch (const kale::HttpBodyTooLargeError &e) {
            caught_body_too_large = true;
            CHECK_EQ(e.code(), kale::HttpClientError::Code::BodyTooLarge);
        }
        CHECK(caught_body_too_large);

        // Await server handler completion barrier deterministically
        server_finished_fut.wait();

        // Deterministic wire observation:
        // HttpMessage::append rejected the incoming stream with EMSGSIZE at the parser limit,
        // closing the socket and causing the server to observe peer close before all chunks were sent.
        CHECK_LT(chunks_sent.load(), 10);
        CHECK(server_peer_closed.load());
    }());
}
TEST_CASE("HttpClient C3: total_timeout clamps native chunk and non-chunked socket deadlines") {
    std::atomic<bool> chunked_peer_closed{false};
    std::atomic<bool> nonchunked_peer_closed{false};
    AuditTestServer server;

    // Subcase A: Chunked stream with initial chunk followed by delay past total_timeout
    server.server().GET("/c3_chunked_silent", [&](kale::Context &ctx) -> coke::Task<void> {
        bool ok = co_await ctx.send_chunk("first_chunk");
        if (!ok) co_return;
        co_await coke::sleep(std::chrono::milliseconds(250));
        ok = co_await ctx.send_chunk("second_chunk");
        if (!ok) {
            chunked_peer_closed.store(true);
        }
        co_await ctx.end_chunk();
    });

    // Subcase B: Non-chunked Content-Length response with delayed body
    server.server().GET("/c3_nonchunked_delay", [&](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_header("Content-Length", "100");
        co_await ctx.send_chunk(""); // flushes headers
        co_await coke::sleep(std::chrono::milliseconds(250));
        bool ok = co_await ctx.send_chunk(std::string(100, 'b'));
        if (!ok) {
            nonchunked_peer_closed.store(true);
        }
        co_await ctx.end_chunk();
    });

    // Subcase C: Shorter configured receive_timeout is preserved
    server.server().GET("/c3_shorter_control", [&](kale::Context &ctx) -> coke::Task<void> {
        co_await ctx.send_chunk("first");
        co_await coke::sleep(std::chrono::milliseconds(150));
        co_await ctx.send_chunk("late_chunk");
        co_await ctx.end_chunk();
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;

        // Subcase A: total_timeout 100ms with long native timeouts (10s)
        auto resp_a = co_await client.get(server.url("/c3_chunked_silent"))
            .receive_timeout(std::chrono::seconds(10))
            .total_timeout(std::chrono::milliseconds(100))
            .send();
        CHECK_EQ(resp_a.status(), 200);
        auto c1 = co_await resp_a.read_chunk();
        REQUIRE(c1.has_value());
        CHECK_EQ(*c1, "first_chunk");

        // Caller delays reading past total timeout (100ms)
        co_await coke::sleep(std::chrono::milliseconds(300));
        CHECK(chunked_peer_closed.load());

        bool caught_timeout_a = false;
        try {
            co_await resp_a.read_chunk();
        } catch (const kale::HttpTimeoutError &e) {
            caught_timeout_a = true;
            CHECK(e.is_timeout());
            CHECK_EQ(e.code(), kale::HttpClientError::Code::Timeout);
        }
        CHECK(caught_timeout_a);

        // Subcase B: non-chunked Content-Length with total_timeout 100ms
        bool caught_timeout_b = false;
        try {
            auto resp_b = co_await client.get(server.url("/c3_nonchunked_delay"))
                .receive_timeout(std::chrono::seconds(10))
                .total_timeout(std::chrono::milliseconds(100))
                .send();
            co_await resp_b.read_body();
        } catch (const kale::HttpTimeoutError &e) {
            caught_timeout_b = true;
            CHECK(e.is_timeout());
            CHECK_EQ(e.code(), kale::HttpClientError::Code::Timeout);
        }
        CHECK(caught_timeout_b);

        // Subcase C: shorter configured receive_timeout (50ms) preserved against longer total_timeout (1000ms)
        bool caught_timeout_c = false;
        try {
            auto resp_c = co_await client.get(server.url("/c3_shorter_control"))
                .receive_timeout(std::chrono::milliseconds(50))
                .connect_timeout(std::chrono::milliseconds(0))
                .total_timeout(std::chrono::milliseconds(1000))
                .send();
            co_await resp_c.read_chunk();
            co_await resp_c.read_chunk();
        } catch (const kale::HttpTimeoutError &e) {
            caught_timeout_c = true;
            CHECK(e.is_timeout());
            CHECK_EQ(e.code(), kale::HttpClientError::Code::Timeout);
        }
        CHECK(caught_timeout_c);
    }());
}

TEST_CASE("HttpClient C4: explicit cancel transitions stably to aborted state") {
    AuditTestServer server;

    server.server().GET("/c4_stream", [&](kale::Context &ctx) -> coke::Task<void> {
        for (int i = 0; i < 5; ++i) {
            bool ok = co_await ctx.send_chunk("payload_chunk_" + std::to_string(i));
            if (!ok) break;
            co_await coke::sleep(std::chrono::milliseconds(20));
        }
        co_await ctx.end_chunk();
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;

        auto resp = co_await client.get(server.url("/c4_stream")).send();
        CHECK_EQ(resp.status(), 200);

        auto c1 = co_await resp.read_chunk();
        REQUIRE(c1.has_value());

        // Cancel mid-stream
        resp.cancel();

        // Subsequent reads stably throw Aborted
        for (int i = 0; i < 3; ++i) {
            bool caught_aborted = false;
            try {
                co_await resp.read_chunk();
            } catch (const kale::HttpClientError &e) {
                caught_aborted = true;
                CHECK_EQ(e.code(), kale::HttpClientError::Code::Aborted);
            }
            CHECK(caught_aborted);
        }
    }());
}

TEST_CASE("Accept-Encoding negotiation handles mixed-case tokens and q-parameters") {
    SUBCASE("Uppercase Q=0 parameter must disable coding") {
        auto result = kale::Compress::negotiate("gzip;Q=0, zstd;q=0");
        CHECK_EQ(result, kale::CompressionEncoding::None);
    }

    SUBCASE("Mixed-case coding with q=0 must disable coding") {
        auto result = kale::Compress::negotiate("Gzip;q=0, Zstd;q=0");
        CHECK_EQ(result, kale::CompressionEncoding::None);

        auto result_upper = kale::Compress::negotiate("GZIP;Q=0, ZSTD;Q=0");
        CHECK_EQ(result_upper, kale::CompressionEncoding::None);
    }

    SUBCASE("Mixed-case content coding names without q=0 must be negotiated") {
        auto result_gzip = kale::Compress::negotiate("Gzip");
        CHECK_EQ(result_gzip, kale::CompressionEncoding::Gzip);

        auto result_zstd = kale::Compress::negotiate("Zstd");
        CHECK_EQ(result_zstd, kale::CompressionEncoding::Zstd);

        auto result_mixed = kale::Compress::negotiate("GZip;q=0.8, ZStd;q=0.9");
        CHECK_EQ(result_mixed, kale::CompressionEncoding::Zstd);
    }

    SUBCASE("Wildcard with uppercase Q=0 must disable fallbacks") {
        auto result = kale::Compress::negotiate("gzip;q=0.5, *;Q=0");
        CHECK_EQ(result, kale::CompressionEncoding::Gzip);

        auto result_none = kale::Compress::negotiate("*;Q=0");
        CHECK_EQ(result_none, kale::CompressionEncoding::None);
    }
}

TEST_CASE("Nested duplicate JSON keys under Json DTO and ignored fields") {
    AuditTestServer server;
    std::atomic<bool> caught_bind_error{false};

    server.server().POST("/bind_raw_payload", [&](kale::Context &ctx) {
        try {
            ctx.bind_json<RawJsonPayloadDTO>();
            ctx.string("ok");
        } catch (const kale::JsonBindError &) {
            caught_bind_error.store(true);
            ctx.set_status(400).string("bind_error");
        }
    });

    server.server().POST("/bind_ignored_nested", [&](kale::Context &ctx) {
        try {
            ctx.bind_json<SimpleAccountDTO>();
            ctx.string("ok");
        } catch (const kale::JsonBindError &) {
            caught_bind_error.store(true);
            ctx.set_status(400).string("bind_error");
        }
    });

    SUBCASE("Raw Json DTO field with duplicate keys inside nested object must be rejected") {
        caught_bind_error.store(false);
        coke::sync_wait([&]() -> coke::Task<void> {
            kale::HttpClient client;
            auto resp = co_await client.post(server.url("/bind_raw_payload"))
                .body(R"({"payload": {"role": "user", "role": "admin"}})", "application/json")
                .send();
            CHECK(caught_bind_error.load());
            CHECK_EQ(resp.status(), 400);
        }());
    }

    SUBCASE("Ignored nested object containing duplicate keys under DTO") {
        caught_bind_error.store(false);
        coke::sync_wait([&]() -> coke::Task<void> {
            kale::HttpClient client;
            auto resp = co_await client.post(server.url("/bind_ignored_nested"))
                .body(R"({"username": "alice", "ignored_nested": {"dup_key": 1, "dup_key": 2}})", "application/json")
                .send();
            CHECK(caught_bind_error.load());
            CHECK_EQ(resp.status(), 400);
        }());
    }
}

TEST_CASE("Wildcard suffix registration must reject or respect suffix") {
    kale::Router router;
    CHECK_THROWS_AS(router.GET("/files/*path/private", [](kale::Context &ctx) {
        ctx.string("private_content");
    }), std::invalid_argument);
}

TEST_CASE("any method registration set includes all supported standard HTTP methods") {
    kale::Router router;
    router.any("/any_resource", [](kale::Context &ctx) {
        ctx.string("any_ok");
    });

    const kale::HttpMethod standard_methods[] = {
        kale::HttpMethod::GET,
        kale::HttpMethod::POST,
        kale::HttpMethod::PUT,
        kale::HttpMethod::DELETE,
        kale::HttpMethod::PATCH,
        kale::HttpMethod::HEAD,
        kale::HttpMethod::OPTIONS,
        kale::HttpMethod::CONNECT,
        kale::HttpMethod::TRACE
    };

    for (kale::HttpMethod m : standard_methods) {
        auto res = router.route(m, "/any_resource");
        CHECK_MESSAGE(res.matched,
                      "Router::any must match supported standard HTTP method: ",
                      kale::method_to_string(m));
    }
}

TEST_CASE("Context::bind_json numeric overflow rejection for nonfinite/infinite numbers") {
    AuditTestServer server;
    std::atomic<bool> business_invoked{false};
    std::atomic<int> bound_age{0};

    server.server().POST("/user_age", [&](kale::Context &ctx) {
        auto dto = ctx.bind_json<AgeUserDTO>();
        business_invoked.store(true);
        bound_age.store(dto.age);
        ctx.string("ok_user_age");
    });

    SUBCASE("Positive finite control binds successfully") {
        business_invoked.store(false);
        coke::sync_wait([&]() -> coke::Task<void> {
            kale::HttpClient client;
            auto resp = co_await client.post(server.url("/user_age"))
                .body(R"({"age": 42})", "application/json")
                .send();
            CHECK_EQ(resp.status(), 200);
            CHECK(business_invoked.load());
            CHECK_EQ(bound_age.load(), 42);
        }());
    }

    SUBCASE("Positive overflow 1e999 must reject with 400 and not invoke business logic") {
        business_invoked.store(false);
        coke::sync_wait([&]() -> coke::Task<void> {
            kale::HttpClient client;
            auto resp = co_await client.post(server.url("/user_age"))
                .body(R"({"age": 1e999})", "application/json")
                .send();
            CHECK_EQ(resp.status(), 400);
            CHECK_FALSE(business_invoked.load());
        }());
    }

    SUBCASE("Negative overflow -1e999 must reject with 400 and not invoke business logic") {
        business_invoked.store(false);
        coke::sync_wait([&]() -> coke::Task<void> {
            kale::HttpClient client;
            auto resp = co_await client.post(server.url("/user_age"))
                .body(R"({"age": -1e999})", "application/json")
                .send();
            CHECK_EQ(resp.status(), 400);
            CHECK_FALSE(business_invoked.load());
        }());
    }
}
}
