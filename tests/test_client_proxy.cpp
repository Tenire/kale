#include "doctest/doctest.h"

#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#include "coke/sleep.h"
#include "coke/task.h"
#include "coke/wait.h"
#include "kale/core/http_server.h"
#include "kale/core/logger.h"
#include "kale/client/http_client.h"
#include "kale/client/reverse_proxy.h"

namespace {

class TestServer {
public:
    explicit TestServer(kale::HttpServerParams params = {})
        : server_(params) {
        server_.set_logger(kale::Logger::off());
        int ret = server_.start("127.0.0.1", 0);
        REQUIRE_EQ(ret, 0);
        REQUIRE(server_.is_running());
        port_ = static_cast<unsigned short>(server_.listen_port());
        REQUIRE_GT(port_, 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(port_);
    }

    ~TestServer() {
        if (server_.is_running()) {
            server_.stop();
        }
    }

    TestServer(const TestServer &) = delete;
    TestServer &operator=(const TestServer &) = delete;

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
class FdGuard {
public:
    explicit FdGuard(int fd = -1) noexcept : fd_(fd) {}
    ~FdGuard() { reset(); }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
    FdGuard(FdGuard &&other) noexcept : fd_(other.release()) {}
    FdGuard &operator=(FdGuard &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    int get() const noexcept { return fd_; }
    int release() noexcept {
        int ret = fd_;
        fd_ = -1;
        return ret;
    }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }
private:
    int fd_{-1};
};

struct RawWireResponse {
    int status{0};
    std::string status_line;
    std::string headers;
    std::string body;

    bool has_header(std::string_view name) const {
        std::string needle = std::string(name) + ":";
        size_t pos = 0;
        size_t first_crlf = headers.find("\r\n");
        if (first_crlf == std::string::npos) return false;
        pos = first_crlf + 2;

        while (pos < headers.size()) {
            size_t line_end = headers.find("\r\n", pos);
            if (line_end == std::string::npos) line_end = headers.size();
            std::string_view line(&headers[pos], line_end - pos);
            if (line.size() >= needle.size() &&
                strncasecmp(line.data(), needle.data(), needle.size()) == 0) {
                return true;
            }
            if (line_end == headers.size()) break;
            pos = line_end + 2;
        }
        return false;
    }

    std::string header(std::string_view name) const {
        std::string needle = std::string(name) + ":";
        size_t pos = 0;
        size_t first_crlf = headers.find("\r\n");
        if (first_crlf == std::string::npos) return "";
        pos = first_crlf + 2;

        while (pos < headers.size()) {
            size_t line_end = headers.find("\r\n", pos);
            if (line_end == std::string::npos) line_end = headers.size();
            std::string_view line(&headers[pos], line_end - pos);
            if (line.size() >= needle.size() &&
                strncasecmp(line.data(), needle.data(), needle.size()) == 0) {
                std::string_view val = line.substr(needle.size());
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
                    val.remove_prefix(1);
                }
                while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) {
                    val.remove_suffix(1);
                }
                return std::string(val);
            }
            if (line_end == headers.size()) break;
            pos = line_end + 2;
        }
        return "";
    }
};

inline RawWireResponse send_raw_http(unsigned short port, std::string_view request) {
    int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE_GE(raw_fd, 0);
    FdGuard guard(raw_fd);

    struct timeval tv{.tv_sec = 5, .tv_usec = 0};
    ::setsockopt(raw_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(raw_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    int ret = ::connect(raw_fd, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
    REQUIRE_EQ(ret, 0);

    size_t total_sent = 0;
    while (total_sent < request.size()) {
        ssize_t n = ::send(raw_fd, request.data() + total_sent, request.size() - total_sent, 0);
        if (n < 0 && errno == EINTR) continue;
        REQUIRE_GT(n, 0);
        total_sent += static_cast<size_t>(n);
    }

    std::string response_data;
    char buf[2048];
    size_t expected_total_size = std::string::npos;

    while (true) {
        ssize_t n = ::recv(raw_fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        response_data.append(buf, static_cast<size_t>(n));

        if (expected_total_size == std::string::npos) {
            size_t header_end = response_data.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string hdrs = response_data.substr(0, header_end + 2);
                static const std::string cl_needle = "content-length:";
                size_t p = 0;
                while (p < hdrs.size()) {
                    size_t le = hdrs.find("\r\n", p);
                    if (le == std::string::npos) break;
                    std::string_view l(&hdrs[p], le - p);
                    if (l.size() >= cl_needle.size() && strncasecmp(l.data(), cl_needle.data(), cl_needle.size()) == 0) {
                        std::string_view v = l.substr(cl_needle.size());
                        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
                        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
                        size_t cl_val = 0;
                        auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), cl_val);
                        if (ec == std::errc()) {
                            expected_total_size = header_end + 4 + cl_val;
                        }
                        break;
                    }
                    p = le + 2;
                }
            }
        }
        if (expected_total_size != std::string::npos && response_data.size() >= expected_total_size) {
            break;
        }
    }

    RawWireResponse out;
    size_t header_end = response_data.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        out.headers = response_data.substr(0, header_end);
        out.body = response_data.substr(header_end + 4);
        size_t first_line_end = out.headers.find("\r\n");
        out.status_line = out.headers.substr(0, first_line_end);
        size_t sp1 = out.status_line.find(' ');
        if (sp1 != std::string::npos) {
            size_t sp2 = out.status_line.find(' ', sp1 + 1);
            std::string code_str = out.status_line.substr(sp1 + 1, sp2 != std::string::npos ? sp2 - (sp1 + 1) : std::string::npos);
            std::from_chars(code_str.data(), code_str.data() + code_str.size(), out.status);
        }
    } else {
        out.body = response_data;
    }
    return out;
}

class RawDelayedResponseServer {
public:
    explicit RawDelayedResponseServer(std::chrono::milliseconds response_delay)
        : delay_(response_delay) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE_GE(listen_fd_, 0);

        int opt = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = 0;
        ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
        REQUIRE_EQ(::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)), 0);
        REQUIRE_EQ(::listen(listen_fd_, 5), 0);

        socklen_t len = sizeof(sin);
        REQUIRE_EQ(::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&sin), &len), 0);
        port_ = ntohs(sin.sin_port);

        server_thread_ = std::thread([this]() {
            struct sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            int conn_fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
            if (conn_fd < 0) return;

            tcp_connected_.store(true);

            char buf[1024];
            (void)::recv(conn_fd, buf, sizeof(buf), 0);

            if (delay_.count() > 0) {
                std::this_thread::sleep_for(delay_);
            }

            const char resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
            (void)::send(conn_fd, resp, sizeof(resp) - 1, MSG_NOSIGNAL);

            ::close(conn_fd);
        });
    }

    ~RawDelayedResponseServer() {
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    unsigned short port() const noexcept { return port_; }
    bool tcp_connected() const noexcept { return tcp_connected_.load(); }

private:
    int listen_fd_{-1};
    unsigned short port_{0};
    std::chrono::milliseconds delay_;
    std::atomic<bool> tcp_connected_{false};
    std::thread server_thread_;
};

} // namespace

TEST_SUITE("core") {

// =========================================================================
// 1. HttpClient Tests
// =========================================================================

TEST_CASE("HttpClient status codes 200, 404, 500") {
    TestServer server;
    server.server().GET("/ok", [](kale::Context &ctx) {
        ctx.set_status(200).string("all good");
    });
    server.server().GET("/notfound", [](kale::Context &ctx) {
        ctx.set_status(404).string("page not found");
    });
    server.server().GET("/error", [](kale::Context &ctx) {
        ctx.set_status(500).string("server exploded");
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;

        auto resp200 = co_await client.get(server.url("/ok")).send();
        CHECK_EQ(resp200.status(), 200);
        std::string body200 = co_await resp200.read_body();
        CHECK_EQ(body200, "all good");

        auto resp404 = co_await client.get(server.url("/notfound")).send();
        CHECK_EQ(resp404.status(), 404);
        std::string body404 = co_await resp404.read_body();
        CHECK_EQ(body404, "page not found");

        auto resp500 = co_await client.get(server.url("/error")).send();
        CHECK_EQ(resp500.status(), 500);
        std::string body500 = co_await resp500.read_body();
        CHECK_EQ(body500, "server exploded");
    }());
}

TEST_CASE("HttpClient connection failures throw HttpClientError") {
    coke::sync_wait([]() -> coke::Task<void> {
        kale::HttpClient client;

        // 1. Invalid port (> 65535)
        bool caught_invalid_port = false;
        try {
            co_await client.get("http://127.0.0.1:99999/bad").send();
        } catch (const kale::HttpClientError &e) {
            caught_invalid_port = true;
            CHECK(!e.is_timeout());
            CHECK_EQ(e.code(), kale::HttpClientError::Code::InvalidUrl);
        }
        CHECK(caught_invalid_port);

        // 2. Connection refused (port 1 usually not listening)
        bool caught_conn_refused = false;
        try {
            co_await client.get("http://127.0.0.1:1/refused")
                .connect_timeout(std::chrono::milliseconds(500))
                .send();
        } catch (const kale::HttpClientError &e) {
            caught_conn_refused = true;
            CHECK(!e.is_timeout());
        }
        CHECK(caught_conn_refused);

        // 3. DNS failure
        bool caught_dns = false;
        try {
            co_await client.get("http://this-domain-does-not-exist-123456789.test.invalid/").send();
        } catch (const kale::HttpClientError &e) {
            caught_dns = true;
            CHECK(!e.is_timeout());
        }
        CHECK(caught_dns);
    }());
}

TEST_CASE("HttpClient slow upstream exceeding timeout throws HttpTimeoutError") {
    TestServer server;
    server.server().GET("/slow", [](kale::Context &ctx) -> coke::Task<void> {
        co_await coke::sleep(std::chrono::milliseconds(400));
        ctx.string("too late");
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;
        bool caught_timeout = false;
        try {
            co_await client.get(server.url("/slow"))
                .timeout(std::chrono::milliseconds(50))
                .send();
        } catch (const kale::HttpTimeoutError &e) {
            caught_timeout = true;
            CHECK(e.is_timeout());
            CHECK_EQ(e.code(), kale::HttpClientError::Code::Timeout);
        }
        CHECK(caught_timeout);
    }());
}

TEST_CASE("HttpClient Content-Length exceeding max_body_bytes fails during reception") {
    TestServer server;
    server.server().GET("/oversized_cl", [](kale::Context &ctx) {
        std::string payload(2000, 'a');
        ctx.set_header("Content-Length", std::to_string(payload.size()));
        ctx.body(payload);
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;
        bool caught_too_large = false;
        try {
            auto resp = co_await client.get(server.url("/oversized_cl"))
                .max_body_bytes(500)
                .send();
            co_await resp.read_body();
        } catch (const kale::HttpBodyTooLargeError &e) {
            caught_too_large = true;
            CHECK_EQ(e.code(), kale::HttpClientError::Code::BodyTooLarge);
        }
        CHECK(caught_too_large);
    }());
}

TEST_CASE("HttpClient chunked reception immediately aborts on offending chunk without waiting for delayed chunks") {
    TestServer server;
    server.server().GET("/slow_chunks", [](kale::Context &ctx) -> coke::Task<void> {
        // Chunk 1: 100 bytes
        co_await ctx.send_chunk(std::string(100, 'x'));
        // Delay 500ms before chunk 2
        co_await coke::sleep(std::chrono::milliseconds(500));
        co_await ctx.end_chunk();
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;
        auto start = std::chrono::steady_clock::now();
        bool caught_too_large = false;

        try {
            auto resp = co_await client.get(server.url("/slow_chunks"))
                .max_body_bytes(50)
                .send();
            co_await resp.read_body(50);
        } catch (const kale::HttpBodyTooLargeError &e) {
            caught_too_large = true;
            CHECK_EQ(e.code(), kale::HttpClientError::Code::BodyTooLarge);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        CHECK(caught_too_large);
        // Reception must abort immediately on chunk 1 without waiting for the 500ms chunk 2!
        CHECK_LT(elapsed.count(), 300);
    }());
}

TEST_CASE("HttpClient incremental read_chunk and cancel with lifetime safety") {
    TestServer server;
    server.server().GET("/stream_cancel", [](kale::Context &ctx) -> coke::Task<void> {
        co_await ctx.send_chunk("chunk_alpha");
        co_await ctx.send_chunk("chunk_beta");
        co_await ctx.send_chunk("chunk_gamma");
        co_await ctx.end_chunk();
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;
        auto resp = co_await client.get(server.url("/stream_cancel")).send();
        CHECK_EQ(resp.status(), 200);

        auto chunk1 = co_await resp.read_chunk();
        REQUIRE(chunk1.has_value());
        CHECK_EQ(*chunk1, "chunk_alpha");

        // Cancel mid-stream
        resp.cancel();

        bool caught_cancel = false;
        try {
            co_await resp.read_chunk();
        } catch (const kale::HttpClientError &e) {
            caught_cancel = true;
            CHECK_EQ(e.code(), kale::HttpClientError::Code::Aborted);
        }
        CHECK(caught_cancel);
    }());
}

TEST_CASE("HttpClient idempotent GET retry vs non-idempotent POST non-retry") {
    TestServer server;
    std::atomic<int> get_attempts{0};
    server.server().GET("/idempotent_test", [&](kale::Context &ctx) {
        int attempt = ++get_attempts;
        if (attempt == 1) {
            // Drop connection on first attempt without replying to trigger network retry
            ctx.coke_context().get_task()->noreply();
            ctx.mark_replied();
            return;
        }
        ctx.string("retry success");
    });

    std::atomic<int> post_attempts{0};
    server.server().POST("/non_idempotent_test", [&](kale::Context &ctx) {
        ++post_attempts;
        ctx.coke_context().get_task()->noreply();
        ctx.mark_replied();
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;

        // GET is idempotent: retries when attempt 1 drops connection
        auto resp_get = co_await client.get(server.url("/idempotent_test"))
            .retry(2)
            .send();
        CHECK_EQ(resp_get.status(), 200);
        std::string body_get = co_await resp_get.read_body();
        CHECK_EQ(body_get, "retry success");
        CHECK_EQ(get_attempts.load(), 2);

        // POST is non-idempotent: effective retries forced to 0
        bool post_failed = false;
        try {
            co_await client.post(server.url("/non_idempotent_test"))
                .retry(2)
                .send();
        } catch (const kale::HttpClientError &) {
            post_failed = true;
        }
        CHECK(post_failed);
        CHECK_EQ(post_attempts.load(), 1);
    }());
}

TEST_CASE("HttpClient default max_redirects == 0 does not follow 302") {
    TestServer server;
    server.server().GET("/redirect", [](kale::Context &ctx) {
        ctx.redirect("/target", 302);
    });
    server.server().GET("/target", [](kale::Context &ctx) {
        ctx.string("target reached");
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;
        auto resp = co_await client.get(server.url("/redirect")).send();
        CHECK_EQ(resp.status(), 302);
        CHECK_EQ(resp.header("Location"), "/target");
    }());
}

// =========================================================================
// 2. kale::proxy Tests
// =========================================================================

TEST_CASE("kale::proxy forwards 200, 404, 500 status and bodies") {
    TestServer upstream;
    upstream.server().GET("/backend/items", [](kale::Context &ctx) {
        ctx.set_status(200).string("backend items ok");
    });
    upstream.server().GET("/backend/notfound", [](kale::Context &ctx) {
        ctx.set_status(404).string("item not found");
    });
    upstream.server().GET("/backend/crash", [](kale::Context &ctx) {
        ctx.set_status(500).string("upstream crashed");
    });

    kale::HttpClient client;
    kale::ProxyOptions opts(upstream.url("/backend"));
    opts.strip_prefix = "/catalog";

    TestServer proxy_server;
    proxy_server.server().GET("/catalog/*path", kale::proxy(client, opts));

    coke::sync_wait([&]() -> coke::Task<void> {
        auto r200 = co_await client.get(proxy_server.url("/catalog/items")).send();
        CHECK_EQ(r200.status(), 200);
        CHECK_EQ(co_await r200.read_body(), "backend items ok");

        auto r404 = co_await client.get(proxy_server.url("/catalog/notfound")).send();
        CHECK_EQ(r404.status(), 404);
        CHECK_EQ(co_await r404.read_body(), "item not found");

        auto r500 = co_await client.get(proxy_server.url("/catalog/crash")).send();
        CHECK_EQ(r500.status(), 500);
        CHECK_EQ(co_await r500.read_body(), "upstream crashed");
    }());
}

TEST_CASE("kale::proxy preserves multiple upstream Set-Cookie headers") {
    TestServer upstream;
    upstream.server().GET("/cookies", [](kale::Context &ctx) {
        ctx.set_cookie("first", "one");
        ctx.set_cookie("second", "two");
        ctx.string("ok");
    });

    kale::HttpClient client;
    TestServer proxy_server;
    proxy_server.server().GET("/proxy/*path", kale::proxy(client, upstream.base_url()));

    coke::sync_wait([&]() -> coke::Task<void> {
        auto resp = co_await client.get(proxy_server.url("/proxy/cookies")).send();
        CHECK_EQ(resp.status(), 200);
        CHECK_EQ(co_await resp.read_body(), "ok");

        std::vector<std::string> cookies;
        for (const auto &[name, value] : resp.headers()) {
            if (strcasecmp(name.c_str(), "Set-Cookie") == 0) {
                cookies.push_back(value);
            }
        }
        REQUIRE_EQ(cookies.size(), 2);
        CHECK(cookies[0].find("first=one") != std::string::npos);
        CHECK(cookies[1].find("second=two") != std::string::npos);
    }());
}

// Total timeout covers the end-to-end request from send() start through response body completion.
TEST_CASE("HttpClient positive total_timeout bounds end-to-end response body completion") {
    TestServer server;
    server.server().GET("/delayed_body", [](kale::Context &ctx) -> coke::Task<void> {
        co_await ctx.send_chunk("first");
        co_await coke::sleep(std::chrono::milliseconds(250));
        co_await ctx.send_chunk("last");
        co_await ctx.end_chunk();
    });

    server.server().GET("/fast_chunks", [](kale::Context &ctx) -> coke::Task<void> {
        co_await ctx.send_chunk("chunk1");
        co_await ctx.send_chunk("chunk2");
        co_await ctx.end_chunk();
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;

        // 1. Timeout after headers and partial chunk
        {
            auto response = co_await client.get(server.url("/delayed_body"))
                .total_timeout(std::chrono::milliseconds(100))
                .receive_timeout(std::chrono::milliseconds(2000))
                .send();
            CHECK_EQ(response.status(), 200);

            // Read the first chunk successfully before the delay
            auto first_chunk = co_await response.read_chunk();
            REQUIRE(first_chunk.has_value());
            CHECK_EQ(*first_chunk, "first");

            // Second chunk exceeds total timeout
            bool caught_timeout = false;
            try {
                co_await response.read_chunk();
            } catch (const kale::HttpTimeoutError &e) {
                caught_timeout = true;
                CHECK(e.is_timeout());
                CHECK_EQ(e.code(), kale::HttpClientError::Code::Timeout);
                CHECK_EQ(e.sys_error(), ETIMEDOUT);
            }
            CHECK(caught_timeout);

            // Assert terminal state: future read calls also fail with timeout
            bool subsequent_timeout = false;
            try {
                co_await response.read_chunk();
            } catch (const kale::HttpTimeoutError &e) {
                subsequent_timeout = true;
                CHECK(e.is_timeout());
                CHECK_EQ(e.code(), kale::HttpClientError::Code::Timeout);
            }
            CHECK(subsequent_timeout);
        }

        // 2. Delayed body exceeding positive total timeout using read_body()
        {
            auto response = co_await client.get(server.url("/delayed_body"))
                .total_timeout(std::chrono::milliseconds(100))
                .receive_timeout(std::chrono::milliseconds(2000))
                .send();
            CHECK_EQ(response.status(), 200);

            bool caught_timeout = false;
            try {
                co_await response.read_body();
            } catch (const kale::HttpTimeoutError &e) {
                caught_timeout = true;
                CHECK(e.is_timeout());
                CHECK_EQ(e.code(), kale::HttpClientError::Code::Timeout);
            }
            CHECK(caught_timeout);
        }

        // 3. Unconsumed buffered chunks do not yield once total timeout expires
        {
            auto response = co_await client.get(server.url("/fast_chunks"))
                .total_timeout(std::chrono::milliseconds(80))
                .receive_timeout(std::chrono::milliseconds(2000))
                .send();
            CHECK_EQ(response.status(), 200);

            // Wait until total deadline has passed before consuming buffered chunks
            co_await coke::sleep(std::chrono::milliseconds(120));

            bool caught_buffered_timeout = false;
            try {
                co_await response.read_chunk();
            } catch (const kale::HttpTimeoutError &e) {
                caught_buffered_timeout = true;
                CHECK(e.is_timeout());
                CHECK_EQ(e.code(), kale::HttpClientError::Code::Timeout);
            }
            CHECK(caught_buffered_timeout);
        }

        // 4. Disabled total_timeout allows delayed response body completion
        {
            auto response = co_await client.get(server.url("/delayed_body"))
                .total_timeout(std::chrono::milliseconds(0))
                .receive_timeout(std::chrono::milliseconds(2000))
                .send();
            CHECK_EQ(response.status(), 200);
            auto body = co_await response.read_body();
            CHECK_EQ(body, "firstlast");
        }
    }());
}

TEST_CASE("kale::proxy hop-by-hop header stripping including custom Connection tokens") {
    TestServer upstream;
    upstream.server().GET("/inspect_headers", [](kale::Context &ctx) {
        // Assert incoming hop-by-hop headers and custom Connection tokens stripped
        CHECK_EQ(ctx.header("Connection").find("X-Custom-Hop"), std::string::npos);
        CHECK_EQ(ctx.header("Connection").find("close"), std::string::npos);
        CHECK(!ctx.has_header("X-Custom-Hop"));
        CHECK(!ctx.has_header("Keep-Alive"));
        CHECK(!ctx.has_header("Upgrade"));
        CHECK(ctx.has_header("X-Legit-Header"));

        // Reply with upstream hop-by-hop headers
        ctx.set_header("Connection", "close, X-Upstream-Hop");
        ctx.set_header("X-Upstream-Hop", "strip_me");
        ctx.set_header("X-Preserved-Header", "keep_me");
        ctx.string("headers inspected");
    });

    kale::HttpClient client;
    TestServer proxy_server;
    proxy_server.server().GET("/proxy/*path", kale::proxy(client, upstream.base_url()));

    coke::sync_wait([&]() -> coke::Task<void> {
        auto resp = co_await client.get(proxy_server.url("/proxy/inspect_headers"))
            .header("Connection", "close, X-Custom-Hop")
            .header("X-Custom-Hop", "secret-hop")
            .header("Keep-Alive", "timeout=5")
            .header("Upgrade", "websocket")
            .header("X-Legit-Header", "allowed-val")
            .send();

        CHECK_EQ(resp.status(), 200);
        CHECK_EQ(co_await resp.read_body(), "headers inspected");

        // Assert upstream hop-by-hop headers and custom Connection tokens stripped from response
        CHECK_EQ(resp.header("Connection").find("X-Upstream-Hop"), std::string::npos);
        CHECK_EQ(resp.header("Connection").find("close"), std::string::npos);
        CHECK(!resp.has_header("X-Upstream-Hop"));
        CHECK_EQ(resp.header("X-Preserved-Header"), "keep_me");
    }());
}

TEST_CASE("kale::proxy Host and X-Forwarded-* headers trusted vs untrusted") {
    TestServer upstream;
    std::string received_host;
    std::string received_xff;
    std::string received_proto;
    std::string received_xfh;

    upstream.server().GET("/forwarded_test", [&](kale::Context &ctx) {
        received_host = ctx.header("Host");
        received_xff = ctx.header("X-Forwarded-For");
        received_proto = ctx.header("X-Forwarded-Proto");
        received_xfh = ctx.header("X-Forwarded-Host");
        ctx.string("ok");
    });

    kale::HttpClient client;

    SUBCASE("Untrusted downstream (default trust_downstream_forwarded = false)") {
        kale::ProxyOptions opts(upstream.base_url());
        opts.trust_downstream_forwarded = false;

        TestServer proxy_server;
        proxy_server.server().GET("/untrusted/*path", kale::proxy(client, opts));

        coke::sync_wait([&]() -> coke::Task<void> {
            auto resp = co_await client.get(proxy_server.url("/untrusted/forwarded_test"))
                .header("Host", "client.spoofed.org")
                .header("X-Forwarded-For", "1.2.3.4")
                .header("X-Forwarded-Proto", "https")
                .header("X-Forwarded-Host", "spoofed.host.org")
                .send();

            CHECK_EQ(resp.status(), 200);
            CHECK_EQ(co_await resp.read_body(), "ok");

            // Untrusted ignores incoming X-Forwarded-For (does not append to 1.2.3.4)
            CHECK(received_xff.find("1.2.3.4") == std::string::npos);
            CHECK_EQ(received_proto, "http");
            CHECK_EQ(received_xfh, "client.spoofed.org");
            // Upstream host authority
            CHECK_EQ(received_host, "127.0.0.1:" + std::to_string(upstream.port()));
        }());
    }

    SUBCASE("Trusted downstream (trust_downstream_forwarded = true)") {
        kale::ProxyOptions opts(upstream.base_url());
        opts.trust_downstream_forwarded = true;

        TestServer proxy_server;
        proxy_server.server().GET("/trusted/*path", kale::proxy(client, opts));

        coke::sync_wait([&]() -> coke::Task<void> {
            auto resp = co_await client.get(proxy_server.url("/trusted/forwarded_test"))
                .header("X-Forwarded-For", "10.0.0.1")
                .header("X-Forwarded-Proto", "https")
                .header("X-Forwarded-Host", "trusted.entry.org")
                .send();

            CHECK_EQ(resp.status(), 200);
            CHECK_EQ(co_await resp.read_body(), "ok");

            // Trusted appends peer_addr to existing X-Forwarded-For
            CHECK(received_xff.starts_with("10.0.0.1, "));
            CHECK_EQ(received_proto, "https");
            CHECK_EQ(received_xfh, "trusted.entry.org");
        }());
    }
}

TEST_CASE("kale::proxy upstream timeout returns 504 without leaking internals") {
    TestServer upstream;
    upstream.server().GET("/delay", [](kale::Context &ctx) -> coke::Task<void> {
        co_await coke::sleep(std::chrono::milliseconds(300));
        ctx.string("done");
    });

    kale::HttpClient client;
    kale::ProxyOptions opts(upstream.base_url());
    opts.timeout = std::chrono::milliseconds(50);

    TestServer proxy_server;
    proxy_server.server().GET("/timeout_proxy/*path", kale::proxy(client, opts));

    coke::sync_wait([&]() -> coke::Task<void> {
        auto resp = co_await client.get(proxy_server.url("/timeout_proxy/delay")).send();
        CHECK_EQ(resp.status(), 504);
        std::string body = co_await resp.read_body();
        CHECK_EQ(body, "Gateway Timeout");
    }());
}

TEST_CASE("kale::proxy upstream connection error returns 502 without leaking internals") {
    kale::HttpClient client;
    // Target dead port 1
    kale::ProxyOptions opts("http://127.0.0.1:1");

    TestServer proxy_server;
    proxy_server.server().GET("/dead_proxy/*path", kale::proxy(client, opts));

    coke::sync_wait([&]() -> coke::Task<void> {
        auto resp = co_await client.get(proxy_server.url("/dead_proxy/test"))
            .connect_timeout(std::chrono::milliseconds(500))
            .send();
        CHECK_EQ(resp.status(), 502);
        std::string body = co_await resp.read_body();
        CHECK_EQ(body, "Bad Gateway");
    }());
}

TEST_CASE("kale::proxy streaming response and downstream disconnect cancellation") {
    SUBCASE("Case 1: Complete streaming consumption") {
        TestServer upstream;
        std::atomic<bool> upstream_completed{false};
        std::promise<void> upstream_done;
        auto upstream_done_fut = upstream_done.get_future();

        upstream.server().GET("/stream_source", [&](kale::Context &ctx) -> coke::Task<void> {
            for (int i = 0; i < 5; ++i) {
                bool ok = co_await ctx.send_chunk("chunk_" + std::to_string(i));
                if (!ok) {
                    break;
                }
                co_await coke::sleep(std::chrono::milliseconds(20));
            }
            co_await ctx.end_chunk();
            upstream_completed = true;
            upstream_done.set_value();
        });

        kale::HttpClient client;
        kale::ProxyOptions opts(upstream.base_url());
        opts.stream_response = true;

        TestServer proxy_server;
        proxy_server.server().GET("/stream_proxy/*path", kale::proxy(client, opts));

        coke::sync_wait([&]() -> coke::Task<void> {
            auto resp = co_await client.get(proxy_server.url("/stream_proxy/stream_source")).send();
            CHECK_EQ(resp.status(), 200);

            std::string full_body = co_await resp.read_body();
            CHECK_EQ(full_body, "chunk_0chunk_1chunk_2chunk_3chunk_4");
            upstream_done_fut.wait();
            CHECK(upstream_completed.load());
        }());
    }

    SUBCASE("Case 2: Downstream disconnect tears down proxy without stack use-after-free") {
        TestServer upstream;
        std::promise<void> stream_started;
        auto stream_started_fut = stream_started.get_future();
        std::atomic<bool> started_set{false};
        std::promise<void> upstream_done;
        auto upstream_done_fut = upstream_done.get_future();

        upstream.server().GET("/stream_disconnect", [&](kale::Context &ctx) -> coke::Task<void> {
            if (!started_set.exchange(true)) {
                stream_started.set_value();
            }
            for (int i = 0; i < 10; ++i) {
                bool ok = co_await ctx.send_chunk("bounded_chunk_" + std::to_string(i) + "\n");
                if (!ok) {
                    break;
                }
                co_await coke::sleep(std::chrono::milliseconds(25));
            }
            co_await ctx.end_chunk();
            upstream_done.set_value();
        });

        kale::HttpClient client;
        kale::ProxyOptions opts(upstream.base_url());
        opts.stream_response = true;

        TestServer proxy_server;
        proxy_server.server().GET("/stream_proxy/*path", kale::proxy(client, opts));

        int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE_GE(raw_fd, 0);
        FdGuard client_fd(raw_fd);

        struct timeval tv{.tv_sec = 5, .tv_usec = 0};
        ::setsockopt(raw_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(raw_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(proxy_server.port());
        ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
        int ret = ::connect(raw_fd, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
        REQUIRE_EQ(ret, 0);

        std::string req = "GET /stream_proxy/stream_disconnect HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Connection: close\r\n\r\n";
        size_t sent = 0;
        while (sent < req.size()) {
            ssize_t n = ::send(raw_fd, req.data() + sent, req.size() - sent, 0);
            if (n < 0 && errno == EINTR) continue;
            REQUIRE_GT(n, 0);
            sent += static_cast<size_t>(n);
        }

        stream_started_fut.wait();

        char buf[1024];
        ssize_t r = ::recv(raw_fd, buf, sizeof(buf), 0);
        REQUIRE_GT(r, 0);
        std::string received(buf, static_cast<size_t>(r));
        CHECK(received.find("200 OK") != std::string::npos);

        // Deterministically close downstream connection
        client_fd.reset();

        // Bounded wait for upstream producer to finish cleanly without hanging or crashing
        auto done_status = upstream_done_fut.wait_for(std::chrono::seconds(5));
        CHECK_EQ(done_status, std::future_status::ready);
    }
}

TEST_CASE("kale::proxy forwards chunked incoming request body decoded without framing or stale headers") {
    TestServer upstream;
    std::string received_body;
    std::string received_ct;
    std::string received_cl;
    std::string received_te;

    upstream.server().POST("/upload_chunked", [&](kale::Context &ctx) {
        received_body = std::string(ctx.req_body());
        received_ct = ctx.header("Content-Type");
        received_cl = ctx.header("Content-Length");
        received_te = ctx.header("Transfer-Encoding");
        ctx.set_header("X-Echo-Length", std::to_string(received_body.size()));
        ctx.string("chunks received");
    });

    kale::HttpClient client;
    TestServer proxy_server;
    proxy_server.server().POST("/proxy/*path", kale::proxy(client, upstream.base_url()));

    // Construct raw chunked HTTP request with binary data, multiple chunks, terminal chunk
    static constexpr char kBinaryChunk1[] = "binary\0data\xFF" "chunk1";
    std::string binary_chunk1(kBinaryChunk1, sizeof(kBinaryChunk1) - 1);
    std::string binary_chunk2 = "second_chunk_payload_text";
    std::string full_expected = binary_chunk1 + binary_chunk2;

    std::ostringstream raw_req;
    raw_req << "POST /proxy/upload_chunked HTTP/1.1\r\n"
            << "Host: 127.0.0.1\r\n"
            << "Content-Type: application/octet-stream\r\n"
            << "Transfer-Encoding: chunked\r\n"
            << "Connection: close\r\n\r\n";
    // Chunk 1
    raw_req << std::hex << binary_chunk1.size() << "\r\n";
    raw_req.write(binary_chunk1.data(), binary_chunk1.size());
    raw_req << "\r\n";
    // Chunk 2
    raw_req << std::hex << binary_chunk2.size() << "\r\n";
    raw_req.write(binary_chunk2.data(), binary_chunk2.size());
    raw_req << "\r\n";
    // Terminal chunk
    raw_req << "0\r\n\r\n";

    auto raw_res = send_raw_http(proxy_server.port(), raw_req.str());
    CHECK_EQ(raw_res.status, 200);
    CHECK_EQ(raw_res.body, "chunks received");
    CHECK_EQ(received_body, full_expected);
    CHECK_EQ(received_body.size(), full_expected.size());
    CHECK_EQ(received_ct, "application/octet-stream");
    CHECK_EQ(received_cl, std::to_string(full_expected.size()));
    CHECK_EQ(received_te, ""); // Transfer-Encoding stripped from incoming request
}

TEST_CASE("kale::proxy upstream failure resets representation headers on fallback 502 and 504") {
    kale::HttpClient client;

    SUBCASE("Upstream truncated body falls back to 502 Bad Gateway without representation headers") {
        TestServer upstream;
        upstream.server().GET("/fail_with_rep_headers", [](kale::Context &ctx) -> coke::Task<void> {
            ctx.set_header("Content-Encoding", "gzip");
            ctx.set_header("Content-Range", "bytes 0-100/500");
            ctx.set_header("ETag", "\"upstream-etag-12345\"");
            ctx.set_header("Content-Type", "application/octet-stream");
            ctx.set_header("Content-MD5", "Q2hlY2sgSW50ZWdyaXR5IQ==");
            ctx.set_header("Trailer", "X-Checksum");
            ctx.set_cookie("app_sess", "token_val_99");
            ctx.set_header("X-App-Service", "inventory");

            // Send partial chunked response then abruptly abort stream
            co_await ctx.send_chunk("partial_broken_payload");
            ctx.abort_stream();
        });

        TestServer proxy_server;
        proxy_server.server().GET("/proxy/*path", kale::proxy(client, upstream.base_url()));

        auto wire_res = send_raw_http(proxy_server.port(),
            "GET /proxy/fail_with_rep_headers HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n");

        CHECK_EQ(wire_res.status, 502);
        CHECK_EQ(wire_res.body, "Bad Gateway");

        // Representation / framing headers MUST NOT be present
        CHECK_FALSE(wire_res.has_header("Content-Encoding"));
        CHECK_FALSE(wire_res.has_header("Content-Range"));
        CHECK_FALSE(wire_res.has_header("ETag"));
        CHECK_FALSE(wire_res.has_header("Content-MD5"));
        CHECK_FALSE(wire_res.has_header("Trailer"));

        // Content-Length must reflect fallback body bytes ("Bad Gateway" -> 11)
        CHECK_EQ(wire_res.header("Content-Length"), "11");

        // Unrelated headers and cookie MUST be preserved
        CHECK(wire_res.headers.find("Set-Cookie:") != std::string::npos);
        CHECK(wire_res.headers.find("app_sess=token_val_99") != std::string::npos);
        CHECK_EQ(wire_res.header("X-App-Service"), "inventory");
    }

    SUBCASE("Upstream body read timeout falls back to 504 Gateway Timeout without representation headers") {
        TestServer upstream;
        upstream.server().GET("/timeout_with_rep_headers", [](kale::Context &ctx) -> coke::Task<void> {
            ctx.set_header("Content-Encoding", "gzip");
            ctx.set_header("Content-Range", "bytes 0-50/200");
            ctx.set_header("ETag", "\"timeout-etag-67890\"");
            ctx.set_header("Content-Type", "application/json");
            ctx.set_cookie("timeout_sess", "keep_this_cookie");
            ctx.set_header("X-Trace-Id", "trace-504-xyz");

            co_await ctx.send_chunk("chunk_before_timeout");
            co_await coke::sleep(std::chrono::milliseconds(300));
            co_await ctx.send_chunk("chunk_after_timeout");
            co_await ctx.end_chunk();
        });

        kale::ProxyOptions opts(upstream.base_url());
        opts.timeout = std::chrono::milliseconds(50);

        TestServer proxy_server;
        proxy_server.server().GET("/proxy/*path", kale::proxy(client, opts));

        auto wire_res = send_raw_http(proxy_server.port(),
            "GET /proxy/timeout_with_rep_headers HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n");

        CHECK_EQ(wire_res.status, 504);
        CHECK_EQ(wire_res.body, "Gateway Timeout");

        CHECK_FALSE(wire_res.has_header("Content-Encoding"));
        CHECK_FALSE(wire_res.has_header("Content-Range"));
        CHECK_FALSE(wire_res.has_header("ETag"));

        CHECK_EQ(wire_res.header("Content-Length"), "15");

        CHECK(wire_res.headers.find("Set-Cookie:") != std::string::npos);
        CHECK(wire_res.headers.find("timeout_sess=keep_this_cookie") != std::string::npos);
        CHECK_EQ(wire_res.header("X-Trace-Id"), "trace-504-xyz");
    }
}

TEST_CASE("kale::proxy rejects invalid target_base_url with std::invalid_argument") {
    kale::HttpClient client;
    CHECK_THROWS_AS(kale::proxy(client, "not_a_valid_url"), std::invalid_argument);
    CHECK_THROWS_AS(kale::proxy(client, "ftp://example.com"), std::invalid_argument);
    CHECK_THROWS_AS(kale::proxy(client, "http://127.0.0.1:99999"), std::invalid_argument);
}

TEST_CASE("HttpClient proxy_url loopback routing routes to configured HTTP proxy") {
    TestServer target_server;
    std::atomic<bool> target_received{false};
    target_server.server().GET("/resource", [&](kale::Context &ctx) {
        target_received.store(true);
        ctx.string("direct_target_response");
    });

    TestServer proxy_server;
    std::atomic<bool> proxy_received{false};
    proxy_server.server().any("/*rest", [&](kale::Context &ctx) {
        proxy_received.store(true);
        ctx.string("proxy_response");
    });
    proxy_server.server().GET("/resource", [&](kale::Context &ctx) {
        proxy_received.store(true);
        ctx.string("proxy_response");
    });

    kale::HttpClientOptions opts;
    opts.proxy_url = proxy_server.base_url();
    kale::HttpClient client(opts);

    coke::sync_wait([&]() -> coke::Task<void> {
        auto resp = co_await client.get(target_server.url("/resource")).send();
        CHECK(proxy_received.load());
        CHECK_FALSE(target_received.load());
        CHECK_EQ(resp.status(), 200);
        std::string body = co_await resp.read_body();
        CHECK_EQ(body, "proxy_response");
    }());
}

TEST_CASE("HttpClient connect_timeout does not abort response wait on already-connected socket") {
    // A silent local TCP server accepts the TCP handshake immediately (<1ms),
    // then delays its HTTP response for 200ms before sending 200 OK.
    // Client configures .connect_timeout(50ms) and .receive_timeout(2000ms).
    RawDelayedResponseServer server(std::chrono::milliseconds(200));

    coke::sync_wait([&]() -> coke::Task<void> {
        bool caught_timeout = false;
        int status = 0;
        try {
            kale::HttpClient client;
            auto resp = co_await client.get("http://127.0.0.1:" + std::to_string(server.port()) + "/delayed_response")
                .connect_timeout(std::chrono::milliseconds(50))
                .receive_timeout(std::chrono::milliseconds(2000))
                .send();
            status = resp.status();
        } catch (const kale::HttpTimeoutError &) {
            caught_timeout = true;
        }

        // TCP connection was accepted immediately
        CHECK(server.tcp_connected());

        // Under correct TCP connect_timeout semantics:
        // Handshake succeeded within 50ms connect_timeout.
        // Response took 200ms (<= 2000ms receive_timeout).
        // The request must succeed with 200 OK without timing out.
        // Under buggy watch_timeout semantics:
        // connect_timeout is mistakenly passed to chunked_task->set_watch_timeout(50ms),
        // which incorrectly times out the response wait after ~50ms.
        CHECK_FALSE(caught_timeout);
        CHECK_EQ(status, 200);
    }());
}

TEST_CASE("HttpClient rejects HTTPS targets through proxy_url with ProtocolError") {
    kale::HttpClientOptions opts;
    opts.proxy_url = "http://127.0.0.1:8080";
    kale::HttpClient client(opts);

    coke::sync_wait([&]() -> coke::Task<void> {
        bool caught = false;
        try {
            co_await client.get("https://127.0.0.1:8443/secure").send();
        } catch (const kale::HttpClientError &e) {
            caught = true;
            CHECK_EQ(e.code(), kale::HttpClientError::Code::ProtocolError);
        }
        CHECK(caught);
    }());
}

TEST_CASE("HttpClient rejects redirect following that would bypass its forward proxy") {
    kale::HttpClientOptions options;
    options.proxy_url = "http://127.0.0.1:8080";
    kale::HttpClient client(options);
    coke::sync_wait([&]() -> coke::Task<void> {
        try {
            co_await client.get("http://127.0.0.1:8081/resource").follow_redirects(1).send();
            FAIL("Proxy redirect configuration must be rejected before network dispatch");
        } catch (const kale::HttpClientError &error) {
            CHECK_EQ(error.code(), kale::HttpClientError::Code::ProtocolError);
        }
    }());
}


TEST_CASE("HttpClient cancellation distinguishes observer wake and buffer clearing") {
    TestServer server;
    server.server().GET("/stream_cancel_obs", [](kale::Context &ctx) -> coke::Task<void> {
        co_await ctx.send_chunk("part1");
        co_await coke::sleep(std::chrono::milliseconds(200));
        co_await ctx.send_chunk("part2");
        co_await ctx.end_chunk();
    });

    coke::sync_wait([&]() -> coke::Task<void> {
        kale::HttpClient client;
        auto resp = co_await client.get(server.url("/stream_cancel_obs")).send();
        CHECK_EQ(resp.status(), 200);

        auto chunk1 = co_await resp.read_chunk();
        REQUIRE(chunk1.has_value());
        CHECK_EQ(*chunk1, "part1");

        // Cancel mid-stream
        resp.cancel();

        // Immediate observer wake: subsequent read throws Aborted immediately
        bool caught_abort = false;
        try {
            co_await resp.read_chunk();
        } catch (const kale::HttpClientError &e) {
            caught_abort = true;
            CHECK_EQ(e.code(), kale::HttpClientError::Code::Aborted);
        }
        CHECK(caught_abort);
    }());
}

} // TEST_SUITE("core")
