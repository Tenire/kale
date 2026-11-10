#include "doctest/doctest.h"
#include "kale/core/http_server.h"
#include "kale/core/logger.h"
#include "kale/core/context.h"
#include "kale/core/http_util.h"

#include <arpa/inet.h>
#include <charconv>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

struct SimpleHttpResponse {
    int status_code{0};
    std::string body;
};

SimpleHttpResponse send_raw_request(unsigned short port, std::string_view raw_path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }

    struct timeval tv{.tv_sec = 2, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&sin), sizeof(sin)) != 0) {
        ::close(fd);
        return {};
    }

    std::string req = "GET ";
    req.append(raw_path);
    req.append(" HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");

    ssize_t sent = ::send(fd, req.data(), req.size(), 0);
    if (sent != static_cast<ssize_t>(req.size())) {
        ::close(fd);
        return {};
    }

    std::string resp_data;
    char buf[1024];
    ssize_t n = 0;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
        resp_data.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    SimpleHttpResponse res;
    auto first_space = resp_data.find(' ');
    if (first_space != std::string::npos && first_space + 4 <= resp_data.size()) {
        std::from_chars(resp_data.data() + first_space + 1, resp_data.data() + first_space + 4, res.status_code);
    }

    auto header_end = resp_data.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        std::string raw_body = resp_data.substr(header_end + 4);
        if (resp_data.find("Transfer-Encoding: chunked") != std::string::npos ||
            resp_data.find("transfer-encoding: chunked") != std::string::npos) {
            std::string decoded;
            size_t pos = 0;
            while (pos < raw_body.size()) {
                auto crlf = raw_body.find("\r\n", pos);
                if (crlf == std::string::npos) {
                    break;
                }
                std::string hex_str = raw_body.substr(pos, crlf - pos);
                size_t chunk_sz = 0;
                try {
                    chunk_sz = std::stoul(hex_str, nullptr, 16);
                } catch (...) {
                    break;
                }
                if (chunk_sz == 0) {
                    break;
                }
                pos = crlf + 2;
                if (pos + chunk_sz > raw_body.size()) {
                    break;
                }
                decoded.append(raw_body.substr(pos, chunk_sz));
                pos += chunk_sz + 2;
            }
            res.body = std::move(decoded);
        } else {
            res.body = std::move(raw_body);
        }
    }
    return res;
}

class TestHttpServer {
public:
    explicit TestHttpServer(kale::HttpServerParams params = {})
        : server_(params) {
        server_.set_logger(kale::Logger::off());
        int ret = server_.start("127.0.0.1", 0);
        REQUIRE_EQ(ret, 0);
        REQUIRE(server_.is_running());
        port_ = static_cast<unsigned short>(server_.listen_port());
        REQUIRE_GT(port_, 0);
    }

    ~TestHttpServer() {
        server_.stop();
    }

    TestHttpServer(const TestHttpServer &) = delete;
    TestHttpServer &operator=(const TestHttpServer &) = delete;

    kale::HttpServer *operator->() noexcept { return &server_; }
    unsigned short port() const noexcept { return port_; }

    SimpleHttpResponse get(std::string_view raw_path) const {
        return send_raw_request(port_, raw_path);
    }

private:
    kale::HttpServer server_;
    unsigned short port_{0};
};

struct PathNormalizationCase {
    std::string_view path;
    bool needs_norm_clean;
    bool needs_norm_route;
    std::string_view expected_clean;
};

} // namespace

TEST_SUITE("core") {

TEST_CASE("PathUtil: Compact table-driven normalization predicate boundaries and clean_path parity") {
    const PathNormalizationCase cases[] = {
        // 1. Canonical root and normal nested paths
        {"/", false, false, "/"},
        {"/a", false, false, "/a"},
        {"/a/b", false, false, "/a/b"},
        {"/api/v1/users", false, false, "/api/v1/users"},
        {"/users/123/profile", false, false, "/users/123/profile"},
        {"/deep/nested/path/to/resource", false, false, "/deep/nested/path/to/resource"},

        // 2. Trailing slashes (clean_path strips trailing slash; route preserves single trailing slash)
        {"/a/", true, false, "/a"},
        {"/a/b/", true, false, "/a/b"},
        {"/api/v1/users/", true, false, "/api/v1/users"},
        {"/users/123/profile/", true, false, "/users/123/profile"},

        // 3. Redundant trailing slashes
        {"//", true, true, "/"},
        {"///", true, true, "/"},
        {"/a//", true, true, "/a"},
        {"/a/b///", true, true, "/a/b"},

        // 4. Dot-like ordinary file and directory names
        {"/file.ext", false, false, "/file.ext"},
        {"/style.css", false, false, "/style.css"},
        {"/archive.tar.gz", false, false, "/archive.tar.gz"},
        {"/..hidden", false, false, "/..hidden"},
        {"/.../x", false, false, "/.../x"},
        {"/x...", false, false, "/x..."},
        {"/foo.bar/baz", false, false, "/foo.bar/baz"},
        {"/api/v1.0/resource", false, false, "/api/v1.0/resource"},
        {"/.well-known/acme-challenge", false, false, "/.well-known/acme-challenge"},
        {"/api/v1.0/resource/", true, false, "/api/v1.0/resource"},

        // 5. Duplicate slashes
        {"/a//b", true, true, "/a/b"},
        {"/a///b", true, true, "/a/b"},
        {"//a/b", true, true, "/a/b"},
        {"/a//b//c", true, true, "/a/b/c"},

        // 6. Single dot segments /./
        {"/.", true, true, "/"},
        {"/./", true, true, "/"},
        {"/./a", true, true, "/a"},
        {"/a/./b", true, true, "/a/b"},
        {"/a/.", true, true, "/a"},
        {"/a/./", true, true, "/a"},
        {"/a/b/./c", true, true, "/a/b/c"},

        // 7. Dotdot traversal segments
        {"/..", true, true, "/"},
        {"/../", true, true, "/"},
        {"/a/..", true, true, "/"},
        {"/a/../b", true, true, "/b"},
        {"/a/b/../c", true, true, "/a/c"},

        // 8. Relative and empty paths
        {"", true, true, "."},
        {".", false, false, "."},
        {"..", false, false, ".."},
        {"../..", false, false, "../.."},
        {"../../foo", false, false, "../../foo"},
        {"a", false, false, "a"},
        {"a/b", false, false, "a/b"},
        {"a/b/", true, false, "a/b"},
        {"./a", true, true, "a"},
        {"a/.", true, true, "a"},
        {"a//b", true, true, "a/b"},
        {"a/..", true, true, "."},
        {"a/../b", true, true, "b"}
    };

    for (const auto &c : cases) {
        CHECK_MESSAGE(kale::PathUtil::needs_normalization(c.path) == c.needs_norm_clean,
                      "needs_normalization(path) mismatch for: ", c.path);
        CHECK_MESSAGE(kale::PathUtil::needs_normalization(c.path, true) == c.needs_norm_route,
                      "needs_normalization(path, true) mismatch for: ", c.path);
        CHECK_MESSAGE(kale::PathUtil::clean_path(c.path) == std::string(c.expected_clean),
                      "clean_path(path) mismatch for: ", c.path);
    }
}

TEST_CASE("HttpServer routing: /a/b, /a//b, and /a/./b route identically under clean_path=true") {
    TestHttpServer server;
    server->GET("/a/b", [](kale::Context &ctx) {
        ctx.string("route:/a/b");
    });

    auto res1 = server.get("/a/b");
    CHECK_EQ(res1.status_code, 200);
    CHECK_EQ(res1.body, "route:/a/b");

    auto res2 = server.get("/a//b");
    CHECK_EQ(res2.status_code, 200);
    CHECK_EQ(res2.body, "route:/a/b");

    auto res3 = server.get("/a/./b");
    CHECK_EQ(res3.status_code, 200);
    CHECK_EQ(res3.body, "route:/a/b");
}

TEST_CASE("HttpServer routing: slash-sensitive route behavior distinguishes /items and /items/") {
    TestHttpServer server;
    server->GET("/items", [](kale::Context &ctx) {
        ctx.string("target:no-slash");
    });
    server->GET("/items/", [](kale::Context &ctx) {
        ctx.string("target:with-slash");
    });

    auto res_no_slash = server.get("/items");
    CHECK_EQ(res_no_slash.status_code, 200);
    CHECK_EQ(res_no_slash.body, "target:no-slash");

    auto res_with_slash = server.get("/items/");
    CHECK_EQ(res_with_slash.status_code, 200);
    CHECK_EQ(res_with_slash.body, "target:with-slash");

    // Redundant trailing slashes normalize to single trailing slash
    auto res_redundant_trailing = server.get("/items//");
    CHECK_EQ(res_redundant_trailing.status_code, 200);
    CHECK_EQ(res_redundant_trailing.body, "target:with-slash");

    auto res_dot_trailing = server.get("/items/./");
    CHECK_EQ(res_dot_trailing.status_code, 200);
    CHECK_EQ(res_dot_trailing.body, "target:with-slash");

    // Trailing dot resolves to no-slash
    auto res_dot_no_slash = server.get("/items/.");
    CHECK_EQ(res_dot_no_slash.status_code, 200);
    CHECK_EQ(res_dot_no_slash.body, "target:no-slash");
}

TEST_CASE("HttpServer routing: clean_path=false retains literal unnormalized paths") {
    kale::HttpServerParams params;
    params.clean_path = false;
    TestHttpServer server(params);

    server->GET("/a/b", [](kale::Context &ctx) {
        ctx.string("route:/a/b");
    });

    auto res_canonical = server.get("/a/b");
    CHECK_EQ(res_canonical.status_code, 200);
    CHECK_EQ(res_canonical.body, "route:/a/b");

    // Under Router's PathSegments tokenizer, consecutive slashes skip empty segments
    // and match /a/b even with clean_path=false
    auto res_dup = server.get("/a//b");
    CHECK_EQ(res_dup.status_code, 200);
    CHECK_EQ(res_dup.body, "route:/a/b");

    // In contrast, dot segments are preserved literally when clean_path=false,
    // so /a/./b and /a/b/. do not match /a/b
    auto res_dot_mid = server.get("/a/./b");
    CHECK_EQ(res_dot_mid.status_code, 404);

    auto res_dot_end = server.get("/a/b/.");
    CHECK_EQ(res_dot_end.status_code, 404);
}

TEST_CASE("HttpServer routing: path traversal /../ and /.. rejected with 403 Forbidden") {
    TestHttpServer server;
    server->GET("/a/b", [](kale::Context &ctx) {
        ctx.string("route:/a/b");
    });

    auto res_traversal_mid = server.get("/a/../b");
    CHECK_EQ(res_traversal_mid.status_code, 403);

    auto res_traversal_root = server.get("/..");
    CHECK_EQ(res_traversal_root.status_code, 403);

    auto res_traversal_end = server.get("/a/..");
    CHECK_EQ(res_traversal_end.status_code, 403);
}

} // TEST_SUITE("core")
