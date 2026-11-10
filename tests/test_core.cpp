#include "doctest/doctest.h"

#include <arpa/inet.h>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <fstream>
#include <map>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "coke/sleep.h"
#include "coke/http/http_client.h"
#include "coke/wait.h"
#include "coke/condition.h"
#include "coke/queue.h"
#include "kale/core/http_server.h"
#include "kale/core/context.h"
#include "kale/http/json.h"
#include "kale/core/router.h"
#include "kale/http/sse.h"
#include "kale/security/signed_cookie.h"
#include "kale/http/file_response.h"
#include "kale/http/form.h"
#include "kale/http/upload.h"
#include "kale/core/http_status.h"
#include "kale/core/http_error.h"
#include "kale/core/logger.h"
#include "kale/core/compress.h"
#include "kale/middleware/compression.h"
#include "http/base64.h"
#include "http/multipart_parser.h"
#include "workflow/HttpUtil.h"
namespace {
inline std::string get_resp_body(const kale::Context &ctx) {
    std::string out;
    ctx.get_resp_body_merged(out);
    return out;
}
struct LifecycleTestAttachment {
    static inline int alive_count = 0;
    LifecycleTestAttachment() { alive_count++; }
    ~LifecycleTestAttachment() { alive_count--; }
    LifecycleTestAttachment(const LifecycleTestAttachment&) { alive_count++; }
    LifecycleTestAttachment& operator=(const LifecycleTestAttachment&) = default;
};

struct DestructionWitness {
    std::shared_ptr<std::atomic<bool>> destroyed;
    std::shared_ptr<std::promise<void>> destruction_promise;

    explicit DestructionWitness(std::shared_ptr<std::atomic<bool>> d,
                                std::shared_ptr<std::promise<void>> p)
        : destroyed(std::move(d)), destruction_promise(std::move(p)) {}

    ~DestructionWitness() {
        if (destroyed) *destroyed = true;
        if (destruction_promise) {
            destruction_promise->set_value();
        }
    }
    DestructionWitness(const DestructionWitness &) = delete;
    DestructionWitness &operator=(const DestructionWitness &) = delete;
};

struct NotifyingAttachment {
    std::shared_ptr<DestructionWitness> witness;

    explicit NotifyingAttachment(std::shared_ptr<DestructionWitness> w)
        : witness(std::move(w)) {}
};

struct UserPayloadWitness {
    std::shared_ptr<std::promise<void>> destruction_promise;

    explicit UserPayloadWitness(std::shared_ptr<std::promise<void>> p)
        : destruction_promise(std::move(p)) {}

    ~UserPayloadWitness() {
        if (destruction_promise) {
            destruction_promise->set_value();
        }
    }
    UserPayloadWitness(const UserPayloadWitness &) = delete;
    UserPayloadWitness &operator=(const UserPayloadWitness &) = delete;
};

struct UserPayload {
    std::shared_ptr<UserPayloadWitness> witness;

    explicit UserPayload(std::shared_ptr<UserPayloadWitness> w)
        : witness(std::move(w)) {}
};


struct TestResponse {
    int state{0};
    int error{0};
    int status{0};
    std::string status_str;
    std::string body;
    std::map<std::string, std::string, kale::CaseInsensitiveLess> headers;

    bool ok() const noexcept { return state == 0; }

    bool has_header(std::string_view key) const noexcept {
        return headers.find(key) != headers.end();
    }

    std::string header(std::string_view key) const {
        auto it = headers.find(key);
        return (it != headers.end()) ? it->second : std::string{};
    }

    kale::Json json() const {
        return kale::Json::parse(body);
    }

    static TestResponse from_coke(coke::HttpResult &res) {
        TestResponse out;
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

using HeaderList = std::vector<std::pair<std::string, std::string>>;

inline coke::Task<TestResponse> do_request(
    coke::HttpClient &cli,
    std::string url,
    std::string method = "GET",
    HeaderList headers = {},
    std::string body = "") {
    (void)cli;
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
    co_return TestResponse::from_coke(res);
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix = "kale_test_") {
        static std::atomic<uint64_t> counter{0};
        uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
                (std::string(prefix) + std::to_string(::getpid()) + "_" + std::to_string(id));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir &) = delete;
    ScopedTempDir &operator=(const ScopedTempDir &) = delete;

    const std::filesystem::path &path() const noexcept { return path_; }
    std::string str() const { return path_.string(); }
    std::string file_path(std::string_view rel) const {
        return (path_ / rel).string();
    }
    std::string write_file(std::string_view rel, std::string_view content) const {
        auto full = path_ / rel;
        if (full.has_parent_path()) {
            std::filesystem::create_directories(full.parent_path());
        }
        std::ofstream ofs(full, std::ios::binary);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        return full.string();
    }

private:
    std::filesystem::path path_;
};

class CoreTestServer {
public:
    explicit CoreTestServer(kale::HttpServerParams params = {})
        : server_(params) {
        server_.set_logger(kale::Logger::off());
        int ret = server_.start("127.0.0.1", 0);
        REQUIRE_EQ(ret, 0);
        REQUIRE(server_.is_running());
        port_ = static_cast<unsigned short>(server_.listen_port());
        REQUIRE_GT(port_, 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(port_);
    }
    CoreTestServer &operator=(const CoreTestServer &) = delete;

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

    TestResponse request(
        std::string_view path,
        std::string method = "GET",
        HeaderList headers = {},
        std::string body = "") const {
        coke::HttpClientParams cli_params;
        cli_params.redirect_max = 0;
        coke::HttpClient cli(cli_params);
        return coke::sync_wait(
            do_request(cli, url(path), std::move(method), std::move(headers), std::move(body))
        );
    }

    TestResponse get(std::string_view path, HeaderList headers = {}) const {
        return request(path, "GET", std::move(headers), "");
    }

    TestResponse post(std::string_view path, std::string body = "", HeaderList headers = {}) const {
        return request(path, "POST", std::move(headers), std::move(body));
    }

private:
    kale::HttpServer server_;
    unsigned short port_{0};
    std::string base_url_;
};

class RawStreamClient {
public:
    explicit RawStreamClient(unsigned short port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE_GE(fd_, 0);

        struct timeval tv{.tv_sec = 2, .tv_usec = 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
        int ret = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
        REQUIRE_EQ(ret, 0);
    }

    ~RawStreamClient() {
        close();
    }

    RawStreamClient(const RawStreamClient &) = delete;
    RawStreamClient &operator=(const RawStreamClient &) = delete;
    int fd() const noexcept { return fd_; }

    void send_request(std::string_view path, bool close_connection = true) {
        std::string req = "GET " + std::string(path) + " HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Accept: text/event-stream\r\n"
                          "Connection: " + (close_connection ? "close" : "keep-alive") + "\r\n\r\n";
        ssize_t sent = ::send(fd_, req.data(), req.size(), 0);
        REQUIRE_EQ(sent, static_cast<ssize_t>(req.size()));
    }
    void send_raw(std::string_view req) {
        ssize_t sent = ::send(fd_, req.data(), req.size(), 0);
        REQUIRE_EQ(sent, static_cast<ssize_t>(req.size()));
    }

    std::string read_headers() {
        std::string headers;
        char buf[1];
        while (true) {
            ssize_t n = ::recv(fd_, buf, 1, 0);
            if (n <= 0) break;
            headers.push_back(buf[0]);
            if (headers.size() >= 4 && headers.substr(headers.size() - 4) == "\r\n\r\n") {
                break;
            }
        }
        return headers;
    }
    std::string read_body_until_close(std::string_view headers) {
        const auto marker = headers.find("Content-Length:");
        if (marker == std::string_view::npos) return {};
        const auto value_start = headers.find_first_not_of(" \t", marker + std::string_view("Content-Length:").size());
        const auto value_end = headers.find("\r\n", value_start);
        size_t length = 0;
        auto [ptr, ec] = std::from_chars(headers.data() + value_start, headers.data() + value_end, length);
        if (ec != std::errc{} || ptr != headers.data() + value_end) return {};
        return read_exact(length);
    }
    std::string read_chunk() {
        std::string len_str;
        char buf[1];
        while (true) {
            ssize_t n = ::recv(fd_, buf, 1, 0);
            if (n <= 0) return "";
            len_str.push_back(buf[0]);
            if (len_str.size() >= 2 && len_str.substr(len_str.size() - 2) == "\r\n") {
                break;
            }
        }
        len_str.resize(len_str.size() - 2);
        size_t chunk_len = 0;
        auto [p, ec] = std::from_chars(len_str.data(), len_str.data() + len_str.size(), chunk_len, 16);
        if (ec != std::errc{}) return "";
        if (chunk_len == 0) {
            char trailing[2];
            ::recv(fd_, trailing, 2, MSG_WAITALL);
            return "";
        }

        std::string chunk_data;
        chunk_data.resize(chunk_len);
        size_t total_read = 0;
        while (total_read < chunk_len) {
            ssize_t n = ::recv(fd_, chunk_data.data() + total_read, chunk_len - total_read, 0);
            if (n <= 0) break;
            total_read += static_cast<size_t>(n);
        }

        char trailing[2];
        ::recv(fd_, trailing, 2, MSG_WAITALL);

        return chunk_data;
    }
    std::string read_exact(size_t size) {
        std::string data(size, '\0');
        size_t total = 0;
        while (total < size) {
            ssize_t n = ::recv(fd_, data.data() + total, size - total, 0);
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        data.resize(total);
        return data;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_{-1};
};

struct RawResponse {
    int status{0};
    std::string status_line;
    std::string headers;
    std::string body;
};

inline RawResponse send_raw_http(unsigned short port, std::string_view request) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE_GE(fd, 0);

    struct timeval tv{.tv_sec = 3, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
    REQUIRE_EQ(ret, 0);

    size_t total_sent = 0;
    while (total_sent < request.size()) {
        ssize_t n = ::send(fd, request.data() + total_sent, request.size() - total_sent, 0);
        REQUIRE_GT(n, 0);
        total_sent += static_cast<size_t>(n);
    }

    std::string response_data;
    char buf[2048];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response_data.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    RawResponse out;
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
} // namespace

TEST_SUITE("core") {
TEST_CASE("Compression middleware configurable compression policy") {
    std::string payload(4096, 'k');

    kale::CompressConfig config;
    config.set_min_length(128)
          .set_max_length(8192)
          .set_preferred_encodings({kale::CompressionEncoding::Gzip})
          .add_mime_type("application/x-test");
    CoreTestServer server;
    server.server().use(kale::compression(config));
    server->GET("/compress", [payload](kale::Context &ctx) {
        ctx.set_content_type("application/x-test");
        ctx.string(payload);
    });

    auto compressed = server.get("/compress", {{"Accept-Encoding", "gzip, zstd;q=1"}});
    CHECK_EQ(compressed.status, 200);
    CHECK_EQ(compressed.header("Content-Encoding"), "gzip");
    CHECK(compressed.header("Vary").find("Accept-Encoding") != std::string::npos);
    std::string decoded;
    REQUIRE(kale::Compress::ungzip(compressed.body, decoded));
    CHECK_EQ(decoded, payload);

    config.set_max_length(2048);
    CoreTestServer limited;
    limited.server().use(kale::compression(config));
    limited->GET("/compress", [payload](kale::Context &ctx) {
        ctx.set_content_type("application/x-test");
        ctx.string(payload);
    });
    auto too_large = limited.get("/compress", {{"Accept-Encoding", "gzip"}});
    CHECK_FALSE(too_large.has_header("Content-Encoding"));
    CHECK_EQ(too_large.body, payload);

    config.set_max_length(0);
    config.mime_types = {"application/x-test"};
    CoreTestServer mime_server;
    mime_server.server().use(kale::compression(config));
    mime_server->GET("/compress", [payload](kale::Context &ctx) {
        ctx.set_content_type("application/octet-stream");
        ctx.string(payload);
    });
    auto wrong_type = mime_server.get("/compress", {{"Accept-Encoding", "gzip"}});
    CHECK_FALSE(wrong_type.has_header("Content-Encoding"));
    CHECK_EQ(wrong_type.body, payload);
}

TEST_CASE("Chunked response completion preserves keep-alive connection") {
    CoreTestServer server;
    server->GET("/stream", [](kale::Context &ctx) -> coke::Task<void> {
        co_await ctx.send_chunk("done");
        co_await ctx.end_chunk();
    });
    server->GET("/next", [](kale::Context &ctx) { ctx.string("reused"); });

    RawStreamClient client(server.port());
    client.send_request("/stream", false);
    const auto first_headers = client.read_headers();
    REQUIRE(first_headers.find("200 OK") != std::string::npos);
    CHECK_EQ(client.read_chunk(), "done");
    CHECK_EQ(client.read_chunk(), "");
    client.send_request("/next", true);
    const auto second_headers = client.read_headers();
    REQUIRE(second_headers.find("200 OK") != std::string::npos);
    CHECK_EQ(client.read_body_until_close(second_headers), "reused");
}


TEST_CASE("HttpServer direct Context processing and basic routing") {
    CoreTestServer srv;

    // 1. Basic GET with Context direct response
    srv->GET("/hello", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.string("Hello from Kale Core Context!");
        co_return;
    });
    srv->GET("/custom-body", [](kale::Context &ctx) {
        ctx.set_content_type("application/octet-stream");
        ctx.body(std::string("raw\0payload", 11));
    });

    // 2. Synchronous handler adaptation
    srv->GET("/sync", [](kale::Context &ctx) {
        ctx.string("sync response");
    });

    // 3. Value-returning handler
    srv->GET("/val-str", []() -> std::string {
        return "value string";
    });

    srv->GET("/val-json", []() -> kale::Json {
        return kale::Json::Object{{"engine", "kale_core"}, {"status", "ok"}};
    });

    // 4. Path parameter and query extraction
    srv->GET("/users/:id", [](kale::Context &ctx) {
        std::string uid(ctx.param("id"));
        std::string filter(ctx.query("filter"));
        ctx.json(kale::Json::Object{{"user_id", uid}, {"filter", filter}});
    });

    // 5. POST body reading
    srv->POST("/echo", [](kale::Context &ctx) {
        ctx.set_header("X-Echo-Length", std::to_string(ctx.req_body().size()));
        ctx.string(std::string(ctx.req_body()));
        CHECK_EQ(get_resp_body(ctx), "direct payload");
    });
    // 6. PUT / DELETE / PATCH / HEAD / OPTIONS methods
    srv->PUT("/resource", [](kale::Context &ctx) {
        ctx.string("put_ok");
    });
    srv->DELETE("/resource", [](kale::Context &ctx) {
        ctx.string("delete_ok");
    });
    srv->PATCH("/resource", [](kale::Context &ctx) {
        ctx.string("patch_ok");
    });
    srv->HEAD("/resource", [](kale::Context &ctx) {
        ctx.set_header("X-Custom-Head", "head_ok");
        ctx.set_status(200);
    });
    srv->OPTIONS("/resource", [](kale::Context &ctx) {
        ctx.set_header("Allow", "GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS");
        ctx.string("options_ok");
    });

    // Verify GET /hello
    auto r1 = srv.get("/hello");
    CHECK_EQ(r1.status, 200);
    CHECK_EQ(r1.body, "Hello from Kale Core Context!");
    CHECK(r1.header("Content-Type").find("text/plain") != std::string::npos);

    // Verify custom raw response body preserves explicit media type and bytes
    auto custom = srv.get("/custom-body");
    CHECK_EQ(custom.status, 200);
    CHECK_EQ(custom.body, std::string("raw\0payload", 11));
    CHECK_EQ(custom.header("Content-Type"), "application/octet-stream");

    // Verify GET /sync
    auto r2 = srv.get("/sync");

    CHECK_EQ(r2.status, 200);
    CHECK_EQ(r2.body, "sync response");

    // Verify GET /val-str
    auto r3 = srv.get("/val-str");
    CHECK_EQ(r3.status, 200);
    CHECK_EQ(r3.body, "value string");

    // Verify GET /val-json
    auto r4 = srv.get("/val-json");
    CHECK_EQ(r4.status, 200);
    CHECK_EQ(r4.json()["engine"].get<std::string>(), "kale_core");

    // Verify path params and query
    auto r5 = srv.get("/users/42?filter=active");
    CHECK_EQ(r5.status, 200);
    CHECK_EQ(r5.json()["user_id"].get<std::string>(), "42");
    CHECK_EQ(r5.json()["filter"].get<std::string>(), "active");

    // Verify POST echo
    auto r6 = srv.post("/echo", "direct payload");
    CHECK_EQ(r6.status, 200);
    CHECK_EQ(r6.body, "direct payload");
    CHECK_EQ(r6.header("X-Echo-Length"), "14");

    // Verify 404 Not Found
    auto r7 = srv.get("/non_existent_route");
    CHECK_EQ(r7.status, 404);

    // Verify PUT / DELETE / PATCH / HEAD / OPTIONS execution
    auto r_put = srv.request("/resource", "PUT");
    CHECK_EQ(r_put.status, 200);
    CHECK_EQ(r_put.body, "put_ok");

    auto r_del = srv.request("/resource", "DELETE");
    CHECK_EQ(r_del.status, 200);
    CHECK_EQ(r_del.body, "delete_ok");

    auto r_patch = srv.request("/resource", "PATCH");
    CHECK_EQ(r_patch.status, 200);
    CHECK_EQ(r_patch.body, "patch_ok");

    auto r_head = srv.request("/resource", "HEAD");
    CHECK_EQ(r_head.status, 200);
    CHECK_EQ(r_head.header("X-Custom-Head"), "head_ok");

    auto r_opt = srv.request("/resource", "OPTIONS");
    CHECK_EQ(r_opt.status, 200);
    CHECK_EQ(r_opt.header("Allow"), "GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS");
}
TEST_CASE("HttpServer middlewares and TypeMap context injection") {
    CoreTestServer srv;

    struct TraceInfo {
        std::string trace_id;
    };

    // Global middleware injecting typed context into Context
    srv->use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        ctx.set_header("X-Global-MW", "hit");
        ctx.set_attachment(TraceInfo{.trace_id = "trace-uuid-999"});
        co_await next();
    });

    // Route-level middleware
    auto auth_mw = [](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        if (ctx.header("Authorization") != "Bearer secret_token") {
            ctx.set_status(kale::HttpStatus::Unauthorized);
            ctx.string("Unauthorized");
            co_return;
        }
        ctx.set_header("X-Route-MW", "authorized");
        co_await next();
    };

    srv->GET("/secure", auth_mw, [](kale::Context &ctx) {
        const auto *trace = ctx.get_attachment<TraceInfo>();
        std::string tid = trace ? trace->trace_id : "none";
        ctx.json(kale::Json::Object{{"access", "granted"}, {"trace_id", tid}});
    });

    // Request without auth
    auto r1 = srv.get("/secure");
    CHECK_EQ(r1.status, 401);
    CHECK_EQ(r1.header("X-Global-MW"), "hit");
    CHECK_EQ(r1.body, "Unauthorized");

    // Request with valid auth
    auto r2 = srv.get("/secure", {{"Authorization", "Bearer secret_token"}});
    CHECK_EQ(r2.status, 200);
    CHECK_EQ(r2.header("X-Global-MW"), "hit");
    CHECK_EQ(r2.header("X-Route-MW"), "authorized");
    CHECK_EQ(r2.json()["access"].get<std::string>(), "granted");
    CHECK_EQ(r2.json()["trace_id"].get<std::string>(), "trace-uuid-999");
}

TEST_CASE("HttpServer file sending with Range support") {
    ScopedTempDir tmp("kale_core_test_");
    tmp.write_file("data.txt", "0123456789abcdefghijklmnopqrstuvwxyz");

    CoreTestServer srv;

    srv->GET("/download", [filepath = tmp.file_path("data.txt")](kale::Context &ctx) -> coke::Task<void> {
        co_await kale::send_file_range(ctx, filepath, ctx.header("Range"));
    });

    // 1. Full file download
    auto r1 = srv.get("/download");
    CHECK_EQ(r1.status, 200);
    CHECK_EQ(r1.body, "0123456789abcdefghijklmnopqrstuvwxyz");
    CHECK_EQ(r1.header("Accept-Ranges"), "bytes");

    // 2. Partial Range request
    auto r2 = srv.get("/download", {{"Range", "bytes=0-9"}});
    CHECK_EQ(r2.status, 206);
    CHECK_EQ(r2.body, "0123456789");
    CHECK_EQ(r2.header("Content-Range"), "bytes 0-9/36");
}

TEST_CASE("HttpServer file saving via save_body") {
    ScopedTempDir tmp("kale_save_test_");
    CoreTestServer srv;

    srv->POST("/upload-body", [&tmp](kale::Context &ctx) -> coke::Task<void> {
        std::string target = tmp.file_path("sub/nested/saved_body.txt");
        int ret = co_await kale::save_body(ctx, target);
        if (ret == 0) {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.string("saved");
        } else {
            ctx.set_status(kale::HttpStatus::InternalServerError);
            ctx.string(std::to_string(ret));
        }
    });

    srv->POST("/upload-append", [&tmp](kale::Context &ctx) -> coke::Task<void> {
        std::string target = tmp.file_path("sub/nested/saved_body.txt");
        int ret = co_await kale::save_body(ctx, target, true);
        if (ret == 0) {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.string("appended");
        } else {
            ctx.set_status(kale::HttpStatus::InternalServerError);
            ctx.string(std::to_string(ret));
        }
    });

    auto r1 = srv.post("/upload-body", "Hello Kale Body!");
    CHECK_EQ(r1.status, 200);
    CHECK_EQ(r1.body, "saved");

    // Verify file content on disk and nested directory creation
    std::ifstream ifs1(tmp.file_path("sub/nested/saved_body.txt"));
    std::string content1((std::istreambuf_iterator<char>(ifs1)), std::istreambuf_iterator<char>());
    CHECK_EQ(content1, "Hello Kale Body!");

    // Test append
    auto r2 = srv.post("/upload-append", " Append More!");
    CHECK_EQ(r2.status, 200);
    CHECK_EQ(r2.body, "appended");

    std::ifstream ifs2(tmp.file_path("sub/nested/saved_body.txt"));
    std::string content2((std::istreambuf_iterator<char>(ifs2)), std::istreambuf_iterator<char>());
    CHECK_EQ(content2, "Hello Kale Body! Append More!");
}

TEST_CASE("HttpServer save_upload and save_form_file for multipart uploads") {
    ScopedTempDir tmp("kale_upload_test_");
    CoreTestServer srv;

    srv->POST("/upload-field", [&tmp](kale::Context &ctx) -> coke::Task<void> {
        std::string dest = tmp.file_path("uploaded_field.txt");
        int ret = co_await kale::save_upload(ctx, "file_doc", dest);
        if (ret == 0) {
            ctx.string("upload_field:ok");
        } else {
            ctx.set_status(kale::HttpStatus::InternalServerError).string(std::to_string(ret));
        }
    });

    srv->POST("/upload-form-file", [&tmp](kale::Context &ctx) -> coke::Task<void> {
        auto ff = kale::form_file(ctx, "manual_doc");
        if (!ff.has_value()) {
            ctx.set_status(kale::HttpStatus::BadRequest).string("missing_file");
            co_return;
        }
        std::string dest = tmp.file_path("saved_manual.txt");
        int ret = co_await kale::save_form_file(*ff, dest);
        if (ret == 0) {
            ctx.string("save_form_file:ok");
        } else {
            ctx.set_status(kale::HttpStatus::InternalServerError).string(std::to_string(ret));
        }
    });

    std::string boundary = "----TestUploadBoundaryXYZ";
    std::string mp_body =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file_doc\"; filename=\"doc.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "file_doc_contents_12345\r\n"
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"manual_doc\"; filename=\"manual.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "manual_doc_contents_67890\r\n"
        "--" + boundary + "--\r\n";

    auto r1 = srv.post("/upload-field", mp_body, {{"Content-Type", "multipart/form-data; boundary=" + boundary}});
    CHECK_EQ(r1.status, 200);
    CHECK_EQ(r1.body, "upload_field:ok");

    std::ifstream ifs1(tmp.file_path("uploaded_field.txt"));
    std::string content1((std::istreambuf_iterator<char>(ifs1)), std::istreambuf_iterator<char>());
    CHECK_EQ(content1, "file_doc_contents_12345");

    auto r2 = srv.post("/upload-form-file", mp_body, {{"Content-Type", "multipart/form-data; boundary=" + boundary}});
    CHECK_EQ(r2.status, 200);
    CHECK_EQ(r2.body, "save_form_file:ok");

    std::ifstream ifs2(tmp.file_path("saved_manual.txt"));
    std::string content2((std::istreambuf_iterator<char>(ifs2)), std::istreambuf_iterator<char>());
    CHECK_EQ(content2, "manual_doc_contents_67890");
}

TEST_CASE("HttpServer middleware onion order") {
    CoreTestServer srv;
    std::vector<std::string> order;

    // Global middleware 1
    srv->use([&order](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        order.push_back("global_1_pre");
        ctx.set_header("X-Global-1", "pre");
        co_await next();
        order.push_back("global_1_post");
        ctx.set_header("X-Global-1-Post", "ok");
    });

    // Route-level middleware
    auto route_mw = [&order](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        order.push_back("route_mw_pre");
        ctx.set_header("X-Route-MW", "pre");
        co_await next();
        order.push_back("route_mw_post");
    };

    srv->GET("/onion-flow", route_mw, [&order](kale::Context &ctx) {
        order.push_back("handler");
        ctx.string("onion reached");
    });

    auto r = srv.get("/onion-flow");
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, "onion reached");
    CHECK_EQ(r.header("X-Global-1"), "pre");
    CHECK_EQ(r.header("X-Global-1-Post"), "ok");
    CHECK_EQ(r.header("X-Route-MW"), "pre");

    REQUIRE_EQ(order.size(), 5);
    CHECK_EQ(order[0], "global_1_pre");
    CHECK_EQ(order[1], "route_mw_pre");
    CHECK_EQ(order[2], "handler");
    CHECK_EQ(order[3], "route_mw_post");
    CHECK_EQ(order[4], "global_1_post");
}

TEST_CASE("HttpServer chunked streaming and SSE engine") {
    CoreTestServer srv;

    // 1. Raw chunked streaming
    srv->GET("/raw-chunked", [](kale::Context &ctx) -> coke::Task<void> {
        CHECK(ctx.is_connected());
        co_await ctx.send_chunk("chunk_1_");
        co_await ctx.send_chunk("chunk_2_");
        co_await ctx.send_chunk("chunk_3");
        co_await ctx.end_chunk();
    });

    // 2. Server-Sent Events (SSE)
    srv->GET("/sse-stream", [](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        CHECK(sse.is_connected());
        co_await sse.send("first message");
        co_await sse.send("custom_event", "second message");
        co_await sse.send_event(kale::SseEvent{
            .id = "msg-101",
            .event = "status",
            .data = "running",
            .retry = 2000,
            .comment = "keepalive"
        });
        co_await sse.close();
    });

    // Verify raw chunked
    auto r1 = srv.get("/raw-chunked");
    CHECK_EQ(r1.status, 200);
    CHECK_EQ(r1.body, "chunk_1_chunk_2_chunk_3");
    CHECK_EQ(r1.header("Transfer-Encoding"), "chunked");

    // Verify SSE stream
    auto r2 = srv.get("/sse-stream");
    CHECK_EQ(r2.status, 200);
    CHECK_EQ(r2.header("Content-Type"), "text/event-stream");
    CHECK_EQ(r2.header("Transfer-Encoding"), "chunked");
    CHECK(r2.body.find("data: first message\n\n") != std::string::npos);
    CHECK(r2.body.find("event: custom_event\ndata: second message\n\n") != std::string::npos);
    CHECK(r2.body.find("id: msg-101\n") != std::string::npos);
    CHECK(r2.body.find("retry: 2000\n") != std::string::npos);
    CHECK(r2.body.find(": keepalive\n") != std::string::npos);
}

TEST_CASE("SSE concurrent sends complete as intact ordered events") {
    CoreTestServer srv;
    srv->GET("/sse-concurrent", [](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        std::vector<coke::Task<bool>> sends;
        sends.emplace_back(sse.send("one", "alpha"));
        sends.emplace_back(sse.send("two", "beta"));
        sends.emplace_back(sse.send("three", "gamma"));
        auto results = co_await coke::async_wait(std::move(sends));
        CHECK(results.size() == 3);
        CHECK(std::all_of(results.begin(), results.end(), [](bool result) { return result; }));
        co_await sse.close();
    });

    auto response = srv.get("/sse-concurrent");
    CHECK_EQ(response.status, 200);
    CHECK_EQ(response.body, "event: one\ndata: alpha\n\nevent: two\ndata: beta\n\nevent: three\ndata: gamma\n\n");
}

TEST_CASE("SSE slow clients retain complete event order through backpressure") {
    CoreTestServer srv;
    std::atomic<int> completed{0};
    srv->GET("/sse-backpressure", [&completed](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        for (int i = 0; i < 160; ++i) {
            std::string payload(64 * 1024, static_cast<char>('a' + (i % 26)));
            if (!co_await sse.send(std::to_string(i), payload)) co_return;
            completed.fetch_add(1);
        }
        co_await sse.close();
    });
    RawStreamClient client(srv.port());
    client.send_request("/sse-backpressure");
    CHECK(client.read_headers().find("200 OK") != std::string::npos);
    CHECK(client.read_chunk().starts_with("event: 0\ndata: "));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    int before_drain = completed.load();
    CHECK_LT(before_drain, 160);
    std::vector<std::string> events;
    events.reserve(160);
    events.emplace_back("event: 0\ndata: ");
    for (int i = 1; i < 160; ++i) {
        auto chunk = client.read_chunk();
        REQUIRE(chunk.starts_with("event: " + std::to_string(i) + "\ndata: "));
        REQUIRE(chunk.ends_with("\n\n"));
        events.push_back(std::move(chunk));
    }
    CHECK_EQ(events.size(), 160);
    CHECK_EQ(client.read_chunk(), "");
    CHECK_EQ(completed.load(), 160);
}

TEST_CASE("HttpServer real-time SSE incremental streaming and disconnection") {
    CoreTestServer srv;

    // 1. Incremental streaming endpoint: emits events across timeline
    srv->GET("/sse-timeline", [](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        CHECK(sse.is_connected());

        co_await sse.send("step1", "initial");
        co_await coke::sleep(0.02);
        co_await sse.send("step2", "progressing");
        co_await coke::sleep(0.02);
        co_await sse.send_event(kale::SseEvent{
            .id = "final-id",
            .event = "step3",
            .data = "complete",
            .retry = 5000,
            .comment = "heartbeat"
        });
        co_await sse.close();
    });

    // 2. Client disconnection detection endpoint
    std::atomic<bool> client_disconnected{false};
    srv->GET("/sse-disconnect", [&client_disconnected](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        co_await sse.send("welcome", "connected");

        // Wait for client socket to drop
        for (int i = 0; i < 50; ++i) {
            co_await coke::sleep(0.02);
            if (!sse.is_connected()) {
                client_disconnected = true;
                break;
            }
        }
    });

    // 3. Pure comment: must NOT contain spurious data:\n
    srv->GET("/sse-comment", [](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        co_await sse.send_comment("keepalive-ping");
        co_await sse.close();
    });
    // Test 1: Real-time incremental event-by-event consumption
    {
        RawStreamClient client(srv.port());
        client.send_request("/sse-timeline");

        std::string headers = client.read_headers();
        CHECK(headers.find("200 OK") != std::string::npos);
        CHECK(headers.find("Content-Type: text/event-stream") != std::string::npos);
        CHECK(headers.find("Transfer-Encoding: chunked") != std::string::npos);

        // First event chunk arrives incrementally
        std::string chunk1 = client.read_chunk();
        CHECK_EQ(chunk1, "event: step1\ndata: initial\n\n");

        // Second event chunk arrives after server delay
        std::string chunk2 = client.read_chunk();
        CHECK_EQ(chunk2, "event: step2\ndata: progressing\n\n");

        // Third event chunk with ID, retry, comment
        std::string chunk3 = client.read_chunk();
        CHECK(chunk3.find(": heartbeat\n") != std::string::npos);
        CHECK(chunk3.find("retry: 5000\n") != std::string::npos);
        CHECK(chunk3.find("id: final-id\n") != std::string::npos);
        CHECK(chunk3.find("event: step3\n") != std::string::npos);
        CHECK(chunk3.find("data: complete\n\n") != std::string::npos);

        // Final zero-length chunk / EOF
        std::string eof_chunk = client.read_chunk();
        CHECK(eof_chunk.empty());
    }

    // Test 2: Active client disconnect detection
    {
        RawStreamClient client(srv.port());
        client.send_request("/sse-disconnect");

        std::string headers = client.read_headers();
        CHECK(headers.find("200 OK") != std::string::npos);

        std::string first_chunk = client.read_chunk();
        CHECK_EQ(first_chunk, "event: welcome\ndata: connected\n\n");

        // Client abruptly closes connection while server stream is still alive
        client.close();

        // Wait for server to sense connection close
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(client_disconnected.load());
    }

    // Test 3: Pure comment formatting without spurious data:\n
    {
        RawStreamClient client(srv.port());
        client.send_request("/sse-comment");
        client.read_headers();
        std::string comment_chunk = client.read_chunk();
        CHECK_EQ(comment_chunk, ": keepalive-ping\n\n");
        CHECK(comment_chunk.find("data:") == std::string::npos);
    }
}

TEST_CASE("HttpServer dead stream runaway loop trap") {
    CoreTestServer srv;
    std::atomic<bool> loop_terminated{false};
    std::atomic<int> send_attempts{0};

    // An endpoint with an intentional while(true) loop that ignores send return value
    srv->GET("/sse-runaway", [&loop_terminated, &send_attempts](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        while (true) {
            send_attempts.fetch_add(1);
            co_await sse.send("tick");
            co_await coke::sleep(0.01);
        }
        loop_terminated = true;
    });

    RawStreamClient client(srv.port());
    client.send_request("/sse-runaway");

    std::string headers = client.read_headers();
    CHECK(headers.find("200 OK") != std::string::npos);

    std::string first_chunk = client.read_chunk();
    CHECK_EQ(first_chunk, "data: tick\n\n");

    // Abruptly close client socket
    client.close();

    // Wait briefly for server to execute next iteration
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The runaway loop must have been killed by DeadStreamLoopException!
    int attempts_after_wait = send_attempts.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int attempts_later = send_attempts.load();

    CHECK_EQ(attempts_after_wait, attempts_later);
    CHECK_LE(attempts_after_wait, 5);
}

TEST_CASE("Security and Core Defect Regressions") {
    // 1. Cookie CRLF injection prevention
    kale::Cookie evil_cookie("bad\r\nHeader: evil", "val");
    CHECK(!evil_cookie.is_valid());
    CHECK_THROWS_AS(evil_cookie.to_string(), std::invalid_argument);

    kale::Cookie evil_val("user", "val\r\nSet-Cookie: evil=1");
    CHECK(!evil_val.is_valid());
    CHECK_THROWS_AS(evil_val.to_string(), std::invalid_argument);

    kale::Cookie good_cookie("session", "abc123");
    CHECK(good_cookie.is_valid());
    CHECK_NE(good_cookie.to_string(), "");

    // 2. Base64 strict character validation
    CHECK(!kale::Base64::decode_url("!!@@##").has_value());
    CHECK(!kale::Base64::decode("!!@@##").has_value());
    auto b64_valid = kale::Base64::encode(reinterpret_cast<const unsigned char*>("hello"), 5);
    auto b64_dec = kale::Base64::decode(b64_valid);
    CHECK(b64_dec.has_value());
    CHECK_EQ(*b64_dec, "hello");

    // 3. Json number serialization, push_back deduplication, and get_or type fallback
    kale::Json js_obj = kale::Json::Object{};
    js_obj.push_back("k", 1);
    js_obj.push_back("k", 2);
    CHECK_EQ(js_obj.size(), 1);
    CHECK_EQ(js_obj["k"].get<int>(), 2);

    kale::Json js_str("not_a_number");
    CHECK_EQ(js_str.get_or<int>(42), 42);

    kale::Json js_num(1234567890123456LL);
    std::string dumped = js_num.dump();
    CHECK(dumped.find("1234567890123456") != std::string::npos);

    // 4. HttpStatus phrase 300, 302, 415, and 422
    CHECK_EQ(kale::status_phrase(300), "Multiple Choices");
    CHECK_EQ(kale::status_phrase(302), "Found");
    CHECK_EQ(kale::status_phrase(304), "Not Modified");
    CHECK_EQ(kale::status_phrase(415), "Unsupported Media Type");
    CHECK_EQ(kale::status_phrase(422), "Unprocessable Entity");
    // 5. Multipart boundary and disposition extraction
    std::string_view ct = "multipart/form-data; boundary=\"my_bound\"; charset=utf-8";
    std::string_view bound = kale::MultiPartParser::extract_boundary(ct);
    CHECK_EQ(bound, "my_bound");

    std::string mp_body = "--my_bound\r\n"
                          "Content-Disposition: form-data; filename=\"photo.png\"; name=\"upload\"\r\n"
                          "Content-Type: image/png\r\n\r\n"
                          "fake_data_--my_bound_inside"
                          "\r\n--my_bound--\r\n";
    auto files = kale::MultiPartParser::parse(mp_body, bound);
    REQUIRE_EQ(files.size(), 1);
    CHECK_EQ(files[0].name, "upload");
    CHECK_EQ(files[0].filename, "photo.png");
    CHECK_EQ(files[0].data, "fake_data_--my_bound_inside");

    // 5b. Multi-part consecutive parsing (verifying no alternating part drop)
    std::string multi_mp_body = "--my_bound\r\n"
                                "Content-Disposition: form-data; name=\"field1\"\r\n\r\n"
                                "val1\r\n"
                                "--my_bound\r\n"
                                "Content-Disposition: form-data; name=\"field2\"\r\n\r\n"
                                "val2\r\n"
                                "--my_bound\r\n"
                                "Content-Disposition: form-data; name=\"field3\"\r\n\r\n"
                                "val3\r\n"
                                "--my_bound--\r\n";
    auto multi_files = kale::MultiPartParser::parse(multi_mp_body, bound);
    REQUIRE_EQ(multi_files.size(), 3);
    CHECK_EQ(multi_files[0].name, "field1");
    CHECK_EQ(multi_files[0].data, "val1");
    CHECK_EQ(multi_files[1].name, "field2");
    CHECK_EQ(multi_files[1].data, "val2");
    CHECK_EQ(multi_files[2].name, "field3");
    CHECK_EQ(multi_files[2].data, "val3");
    // 6. Router percent-decode, empty trailing slash separation, and tag overwrite
    kale::Router router;
    auto builder = router.GET("/files/:name", [](kale::Context &ctx) {
        ctx.string(std::string(ctx.param("name")));
    }).tag("version", "v1").tag("version", "v2");
    const auto *saved_endpoint = router.route(kale::HttpMethod::GET, "/files/item").endpoint;
    CHECK_THROWS_AS(router.GET("/files/:name", [](kale::Context &) {}), std::logic_error);
    router.POST("/files/:name", [](kale::Context &) {});
    router.PUT("/files/:name", [](kale::Context &) {});
    router.DELETE("/files/:name", [](kale::Context &) {});
    router.PATCH("/files/:name", [](kale::Context &) {});
    router.HEAD("/files/:name", [](kale::Context &) {});
    router.OPTIONS("/files/:name", [](kale::Context &) {});
    router.GET("/other/:name", [](kale::Context &) {});
    builder.name("file-route");
    auto stable_endpoint = router.route(kale::HttpMethod::GET, "/files/item");
    CHECK_EQ(stable_endpoint.endpoint, saved_endpoint);
    CHECK_EQ(stable_endpoint.route_name(), "file-route");
    CHECK(router.route(kale::HttpMethod::GET, "/files/item").matched);
    CHECK(router.route(kale::HttpMethod::POST, "/files/item").matched);
    CHECK(router.route(kale::HttpMethod::PUT, "/files/item").matched);
    CHECK(router.route(kale::HttpMethod::DELETE, "/files/item").matched);
    CHECK(router.route(kale::HttpMethod::PATCH, "/files/item").matched);
    CHECK(router.route(kale::HttpMethod::HEAD, "/files/item").matched);
    CHECK(router.route(kale::HttpMethod::OPTIONS, "/files/item").matched);
    CHECK(router.route(kale::HttpMethod::GET, "/other/item").matched);
    auto res = router.route(kale::HttpMethod::GET, "/files/hello%20world");
    CHECK(res.matched);
    CHECK_EQ(res.param("name"), "hello world");
    CHECK_EQ(res.route_tag("version"), "v2");

    // Verify '+' in path is preserved and not converted to space
    auto res_plus = router.route(kale::HttpMethod::GET, "/files/a+b.txt");
    CHECK(res_plus.matched);
    CHECK_EQ(res_plus.param("name"), "a+b.txt");

    auto res_empty = router.route(kale::HttpMethod::GET, "/files/");
    CHECK(!res_empty.matched);

    // Wildcard trailing slash matching empty path
    router.GET("/static/*path", [](kale::Context &ctx) {
        ctx.string(std::string(ctx.param("path")));
    });
    auto res_wild_empty = router.route(kale::HttpMethod::GET, "/static/");
    CHECK(res_wild_empty.matched);
    CHECK_EQ(res_wild_empty.param("path"), "");
    CHECK(!router.route(kale::HttpMethod::GET, "/static").matched);
    router.GET("/users", [](kale::Context &) {});
    CHECK(!router.route(kale::HttpMethod::GET, "/users/").matched);
    router.GET("/users/", [](kale::Context &) {});
    CHECK(router.route(kale::HttpMethod::GET, "/users/").matched);
    // Router 200 match leaves allowed_methods unpopulated, while 405 populates allowed_methods
    auto res_match = router.route(kale::HttpMethod::GET, "/users");
    CHECK(res_match.matched);
    CHECK(!res_match.method_not_allowed);
    CHECK(res_match.allowed_methods.empty());
    auto res_mna = router.route(kale::HttpMethod::POST, "/users");
    CHECK(!res_mna.matched);
    CHECK(res_mna.method_not_allowed);
    CHECK_EQ(res_mna.allowed_methods.size(), 1);
    CHECK_EQ(res_mna.allowed_methods[0], kale::HttpMethod::GET);
    CHECK_EQ(res_mna.allow_header(), "GET");

    // Wildcard 200 match leaves allowed_methods unpopulated, while 405 populates allowed_methods
    auto res_wild_match = router.route(kale::HttpMethod::GET, "/static/css/app.css");
    CHECK(res_wild_match.matched);
    CHECK(!res_wild_match.method_not_allowed);
    CHECK(res_wild_match.allowed_methods.empty());
    auto res_wild_mna = router.route(kale::HttpMethod::POST, "/static/css/app.css");
    CHECK(!res_wild_mna.matched);
    CHECK(res_wild_mna.method_not_allowed);
    CHECK_EQ(res_wild_mna.allowed_methods.size(), 1);
    CHECK_EQ(res_wild_mna.allowed_methods[0], kale::HttpMethod::GET);
    CHECK_EQ(res_wild_mna.allow_header(), "GET");

    // Router allocation & matching semantics: static, params, wildcard, backtracking, 405
    kale::Router test_router;
    test_router.GET("/api/v1/health", [](kale::Context &) {});
    test_router.GET("/users/:id/profile", [](kale::Context &) {});
    test_router.GET("/users/settings", [](kale::Context &) {});
    test_router.GET("/repo/*rest", [](kale::Context &) {});

    // 1. Static no-param match has empty params
    auto static_res = test_router.route(kale::HttpMethod::GET, "/api/v1/health");
    CHECK(static_res.matched);
    CHECK(static_res.params.empty());
    CHECK_EQ(static_res.param("id"), "");

    // 2. Parameter match decoded values
    auto param_res = test_router.route(kale::HttpMethod::GET, "/users/john%20doe/profile");
    CHECK(param_res.matched);
    CHECK_EQ(param_res.params.size(), 1);
    CHECK_EQ(param_res.param("id"), "john doe");

    // 3. Failed parameter branch / backtracking to static route or other branch
    // "/users/settings" matches static child rather than :id child failing at second segment
    auto static_branch_res = test_router.route(kale::HttpMethod::GET, "/users/settings");
    CHECK(static_branch_res.matched);
    CHECK(static_branch_res.params.empty());

    // Failed parameter branch that does not match any route restores clean empty params
    auto fail_branch_res = test_router.route(kale::HttpMethod::GET, "/users/john/unknown");
    CHECK(!fail_branch_res.matched);
    CHECK(fail_branch_res.params.empty());

    // 4. Wildcard match decoded and multi-segment
    auto wild_res = test_router.route(kale::HttpMethod::GET, "/repo/docs%20v1/guide/start.html");
    CHECK(wild_res.matched);
    CHECK_EQ(wild_res.params.size(), 1);
    CHECK_EQ(wild_res.param("rest"), "docs v1/guide/start.html");

    // 5. 405 allowed methods
    auto mna_res = test_router.route(kale::HttpMethod::POST, "/api/v1/health");
    CHECK(!mna_res.matched);
    CHECK(mna_res.method_not_allowed);
    CHECK_EQ(mna_res.allow_header(), "GET");
    // 7. SSE comment formatting does not send empty data event
    kale::SseEvent ev_comment;
    ev_comment.comment = "heartbeat";
    std::string formatted_comm = ev_comment.format();
    CHECK(formatted_comm.starts_with(": heartbeat"));
    CHECK(formatted_comm.find("data:") == std::string::npos);

    // 8. SSE event with only event/id still outputs data line per SSE spec
    kale::SseEvent ev_only_event;
    ev_only_event.event = "ping";
    std::string formatted_event = ev_only_event.format();
    CHECK(formatted_event.find("event: ping\n") != std::string::npos);
    CHECK(formatted_event.find("data:\n") != std::string::npos);

    // 9. Real HTTP requests reject traversal after path percent-decoding and preserve '+'.
    std::atomic<bool> handler_called{false};
    CoreTestServer http_srv;
    http_srv->GET("/files/*path", [&handler_called](kale::Context &ctx) {
        handler_called.store(true);
        ctx.string(std::string(ctx.param("path")));
    });
    auto safe_path = http_srv.get("/files/a+b.txt");
    CHECK_EQ(safe_path.status, 200);
    CHECK_EQ(safe_path.body, "a+b.txt");
    CHECK(handler_called.load());

    handler_called.store(false);
    auto traversal = http_srv.get("/files/%2e%2e/%2e%2e/secret.txt");
    CHECK_EQ(traversal.status, 403);
    CHECK(!handler_called.load());
}

TEST_CASE("Range boundaries, JSON failures and one-shot middleware over HTTP") {
    ScopedTempDir tmp("kale_audit_");
    tmp.write_file("file.TXT", "0123456789");
    tmp.write_file("empty", "");
    CoreTestServer server;
    auto download = [path = tmp.file_path("file.TXT")](kale::Context &ctx) -> coke::Task<void> {
        co_await kale::send_file_range(ctx, path, ctx.header("Range"));
    };
    server->GET("/file", download);
    server->HEAD("/file", download);
    server->GET("/empty", [path = tmp.file_path("empty")](kale::Context &ctx) -> coke::Task<void> {
        co_await kale::send_file_range(ctx, path, ctx.header("Range"));
    });
    for (const auto &[range, expected] : std::vector<std::pair<std::string, std::string>>{{"bytes=-3", "789"}, {"bytes=7-", "789"}, {"bytes=3-5", "345"}, {"bytes=0-9999999999999999999999999", "0123456789"}}) {
        auto response = server.get("/file", {{"Range", range}});
        CHECK_EQ(response.status, 206);
        CHECK_EQ(response.body, expected);
        CHECK_EQ(response.header("Content-Length"), std::to_string(expected.size()));
        CHECK_FALSE(response.has_header("Transfer-Encoding"));
    }
    for (const auto range : {"bytes=5-2", "bytes=-0", "bytes=10-", "bytes=0-1,3-4", "bytes=abc-2", "bytes=99999999999999999999999999-"}) {
        auto response = server.get("/file", {{"Range", range}});
        CHECK_EQ(response.status, 416);
        CHECK_EQ(response.header("Content-Range"), "bytes */10");
        CHECK_EQ(response.body, "");
    }
    auto head = server.request("/file", "HEAD", {{"Range", "bytes=2-4"}});
    CHECK_EQ(head.status, 200);
    CHECK_EQ(head.header("Content-Length"), "10");
    CHECK_EQ(head.body, "");
    CHECK_EQ(server.get("/empty").status, 200);
    CHECK_EQ(server.get("/empty", {{"Range", "bytes=0-"}}).status, 416);
    server->POST("/json", [](kale::Context &ctx) { ctx.json(ctx.parse_json()); });
    CHECK_EQ(server.post("/json", "{broken", {{"Content-Type", "application/json"}}).status, 400);
    CHECK_EQ(server.post("/json", "{\"n\":9007199254740992}", {{"Content-Type", "application/json"}}).body, "{\"n\":9007199254740992}");
    std::atomic<int> calls{0};
    server->GET("/next", [](kale::Context &, kale::Next next) -> coke::Task<void> {
        co_await next();
        CHECK_THROWS_AS(next(), std::logic_error);
    }, [&calls](kale::Context &ctx) { calls.fetch_add(1); ctx.string("once"); });
    CHECK_EQ(server.get("/next").body, "once");
    CHECK_EQ(calls.load(), 1);
    server->POST("/multipart", [](kale::Context &ctx) {
            const kale::Context &request = ctx;
            const auto part = kale::form_file(request, "upload");
            REQUIRE(part);
            CHECK_EQ(part->filename, "a.txt");
            ctx.string(kale::form(request, "field") + ":" + std::string(part->data));
        });
    auto multipart = server.post("/multipart", "--B\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\nvalue\r\n--B\r\nContent-Disposition: form-data; name=\"upload\"; filename=\"a.txt\"\r\n\r\nfile\r\n--B--\r\n", {{"Content-Type", "multipart/form-data; boundary=B"}});
    CHECK_EQ(multipart.body, "value:file");
}

TEST_CASE("FileResponse regression: lazy coroutine task evaluation with temporary path and range") {
    ScopedTempDir tmp("kale_lazy_file_test_");
    std::string test_data = "abcdefghijklmnopqrstuvwxyz0123456789";
    tmp.write_file("target_payload.txt", test_data);

    CoreTestServer server;

    // 1. send_file with slice (start=5, end=15), evaluated lazily after full expression ends -> 206 PartialContent
    server->GET("/lazy_send_file_slice", [&tmp](kale::Context &ctx) -> coke::Task<void> {
        coke::Task<int> task;
        {
            std::string temp_path = tmp.file_path("target_payload.txt");
            // Obtain task from temporary std::string path, then end full expression and destruct temporary
            task = kale::send_file(ctx, std::move(temp_path), 5, 15);
            // Churn / overwrite memory
            volatile char churn[128];
            std::fill_n(churn, 128, 'Z');
        }
        co_await coke::sleep(0.005);
        int ret = co_await std::move(task);
        CHECK_EQ(ret, 0);
    });

    // 2. send_file full file (defaults start=0, end=max), evaluated lazily -> 200 OK
    server->GET("/lazy_send_file_full", [&tmp](kale::Context &ctx) -> coke::Task<void> {
        coke::Task<int> task;
        {
            std::string temp_path = tmp.file_path("target_payload.txt");
            task = kale::send_file(ctx, std::move(temp_path));
            volatile char churn[128];
            std::fill_n(churn, 128, 'X');
        }
        co_await coke::sleep(0.005);
        int ret = co_await std::move(task);
        CHECK_EQ(ret, 0);
    });
    // 2. send_file_range with temporary path and range strings, evaluated lazily
    server->GET("/lazy_send_range", [&tmp](kale::Context &ctx) -> coke::Task<void> {
        coke::Task<int> task;
        {
            std::string temp_path = tmp.file_path("target_payload.txt");
            std::string temp_range = "bytes=10-19";
            task = kale::send_file_range(ctx, std::move(temp_path), std::move(temp_range));
            volatile char churn[128];
            std::fill_n(churn, 128, 'Y');
        }
        co_await coke::sleep(0.005);
        int ret = co_await std::move(task);
        CHECK_EQ(ret, 0);
    });

    auto r0 = server.get("/lazy_send_file_full");
    CHECK_EQ(r0.status, 200);
    CHECK_EQ(r0.body, test_data);

    auto r1 = server.get("/lazy_send_file_slice");
    CHECK_EQ(r1.status, 206);
    CHECK_EQ(r1.body, test_data.substr(5, 10)); // indices 5 to 14, length 10
    CHECK_EQ(r1.header("Content-Range"), "bytes 5-14/36");
    auto r2 = server.get("/lazy_send_range");
    CHECK_EQ(r2.status, 206);
    CHECK_EQ(r2.body, test_data.substr(10, 10));
    CHECK_EQ(r2.header("Content-Range"), "bytes 10-19/36");
}
TEST_CASE("Request Metadata Optimization and Lazy Parsing") {
    CoreTestServer server;

    server->GET("/test_header", [](kale::Context &ctx) {
        // Normal header checks
        CHECK(ctx.has_header("X-Direct"));
        CHECK_EQ(ctx.header("X-Direct"), "direct_value");
        CHECK_EQ(ctx.header("x-direct"), "direct_value");
        CHECK(!ctx.has_header("X-NonExistent"));
        CHECK_EQ(ctx.header("X-NonExistent"), "");

        // Non-null-terminated string_view keys
        std::string buffer = "X-Direct-Padding";
        std::string_view non_null_term_key(buffer.data(), 8); // "X-Direct", byte 8 is '-'
        CHECK(ctx.has_header(non_null_term_key));
        CHECK_EQ(ctx.header(non_null_term_key), "direct_value");

        std::string_view partial_prefix(buffer.data(), 7); // "X-Direc", shorter
        CHECK(!ctx.has_header(partial_prefix));
        CHECK_EQ(ctx.header(partial_prefix), "");

        std::string_view longer_key(buffer.data(), 16); // "X-Direct-Padding", longer
        CHECK(!ctx.has_header(longer_key));
        CHECK_EQ(ctx.header(longer_key), "");

        // Empty header key
        CHECK(!ctx.has_header(""));
        CHECK_EQ(ctx.header(""), "");

        // Multi-header joining
        CHECK(ctx.has_header("X-Multi"));
        CHECK_EQ(ctx.header("X-Multi"), "part1, part2");

        const auto all = ctx.headers();
        CHECK_EQ(all.at("X-Direct"), "direct_value");
        CHECK_EQ(all.at("X-Multi"), "part1, part2");

        const auto all2 = ctx.headers();
        CHECK_EQ(all, all2);
        CHECK_EQ(ctx.header("X-Direct"), "direct_value");
        CHECK_EQ(ctx.header("X-Multi"), "part1, part2");

        ctx.string("ok_header");
    });

    server->GET("/test_query", [](kale::Context &ctx) {
        // Direct lookups and view capture before full-map materialization
        CHECK(ctx.has_query("id"));
        std::string v_id = ctx.query("id");
        CHECK_EQ(v_id, "123");
        CHECK(ctx.has_query("type"));
        std::string v_type = ctx.query("type");
        CHECK_EQ(v_type, "admin");

        // Percent and plus decoding
        CHECK(ctx.has_query("msg"));
        std::string v_msg = ctx.query("msg");
        CHECK_EQ(v_msg, "hello world");
        CHECK(ctx.has_query("a b"));
        CHECK_EQ(ctx.query("a b"), "plus key");
        CHECK(ctx.has_query("x y"));
        CHECK_EQ(ctx.query("x y"), "pct key");

        // Duplicate keys (first occurrence wins)
        CHECK(ctx.has_query("dup"));
        CHECK_EQ(ctx.query("dup"), "first");
        CHECK(ctx.has_query("dup_enc"));
        std::string v_dup_enc = ctx.query("dup_enc");
        CHECK_EQ(v_dup_enc, "first val");
        // Empty values: with '=' and without '=' (flag)
        CHECK(ctx.has_query("flag"));
        CHECK_EQ(ctx.query("flag"), "");
        CHECK_EQ(ctx.default_query("flag", std::string_view("def")), "");
        CHECK(ctx.has_query("empty"));
        CHECK_EQ(ctx.query("empty"), "");
        CHECK_EQ(ctx.default_query("empty", std::string_view("def")), "");

        // Empty key (?=ignore&=) should not match
        CHECK(!ctx.has_query(""));
        CHECK_EQ(ctx.query(""), "");

        // Malformed percent encodings
        CHECK(ctx.has_query("m%ZZ"));
        CHECK_EQ(ctx.query("m%ZZ"), "raw_pct");
        CHECK(ctx.has_query("m%"));
        CHECK_EQ(ctx.query("m%"), "trailing_pct");
        CHECK_EQ(ctx.query("bad_val"), "foo%ZZbar");

        // Missing query
        CHECK(!ctx.has_query("missing"));
        CHECK_EQ(ctx.query("missing"), "");
        CHECK_EQ(ctx.default_query("missing", std::string_view("def_val")), "def_val");
        CHECK_EQ(ctx.default_query("missing", std::string("def_string")), "def_string");

        // Entire map materialization check
        const auto qmap = ctx.query_params();
        CHECK_EQ(qmap.at("id"), "123");
        CHECK_EQ(qmap.at("type"), "admin");
        CHECK_EQ(qmap.at("msg"), "hello world");
        CHECK_EQ(qmap.at("a b"), "plus key");
        CHECK_EQ(qmap.at("x y"), "pct key");
        CHECK_EQ(qmap.at("dup"), "first");
        CHECK_EQ(qmap.at("dup_enc"), "first val");
        CHECK_EQ(qmap.at("flag"), "");
        CHECK_EQ(qmap.at("empty"), "");
        CHECK_EQ(qmap.at("m%ZZ"), "raw_pct");
        CHECK_EQ(qmap.at("m%"), "trailing_pct");
        CHECK_EQ(qmap.at("bad_val"), "foo%ZZbar");
        CHECK(qmap.find("") == qmap.end());

        // Whole-map access returns owning map by value (calling twice produces independent copies)
        const auto qmap2 = ctx.query_params();
        CHECK_EQ(qmap, qmap2);

        // Values match between direct scalar lookups and whole-map
        CHECK_EQ(v_msg, "hello world");
        CHECK_EQ(v_msg, qmap.at("msg"));
        CHECK_EQ(v_id, "123");
        CHECK_EQ(v_id, qmap.at("id"));
        CHECK_EQ(v_type, "admin");
        CHECK_EQ(v_type, qmap.at("type"));
        CHECK_EQ(v_dup_enc, "first val");
        CHECK_EQ(v_dup_enc, qmap.at("dup_enc"));

        // Subsequent direct queries return owning strings with matching values
        std::string v_msg2 = ctx.query("msg");
        CHECK_EQ(v_msg2, qmap.at("msg"));
        // Consistent behavior after parsing
        CHECK(!ctx.has_query(""));
        CHECK_EQ(ctx.query(""), "");
        CHECK_EQ(ctx.query("id"), "123");
        ctx.string("ok_query");
    });

    server->GET("/test_cookie", [](kale::Context &ctx) {
        // Basic cookies
        // Basic cookies and view capture before full-map materialization
        CHECK(ctx.has_cookie("user"));
        std::string_view cv_user = ctx.cookie("user");
        CHECK_EQ(cv_user, "alice");
        CHECK(ctx.has_cookie("theme"));
        std::string_view cv_theme = ctx.cookie("theme");
        CHECK_EQ(cv_theme, "dark");

        // Multiple Cookie headers and duplicate cookie names
        CHECK(ctx.has_cookie("split_cookie"));
        std::string_view cv_split = ctx.cookie("split_cookie");
        CHECK_EQ(cv_split, "from_second_header");
        CHECK(ctx.has_cookie("c_dup"));
        std::string_view cv_cdup = ctx.cookie("c_dup");
        CHECK_EQ(cv_cdup, "first_val"); // First occurrence wins

        // Value containing '='
        CHECK(ctx.has_cookie("token"));
        std::string_view cv_token = ctx.cookie("token");
        CHECK_EQ(cv_token, "abc=123==");
        // Empty cookie value
        CHECK(ctx.has_cookie("empty_val"));
        CHECK_EQ(ctx.cookie("empty_val"), "");
        CHECK_EQ(ctx.default_cookie("empty_val", std::string_view("def")), "");

        // Empty cookie key (=bad)
        CHECK(!ctx.has_cookie(""));
        CHECK_EQ(ctx.cookie(""), "");

        // Missing cookie
        CHECK(!ctx.has_cookie("missing"));
        CHECK_EQ(ctx.cookie("missing"), "");
        CHECK_EQ(ctx.default_cookie("missing", std::string_view("default_val")), "default_val");

        // Entire map materialization check
        const auto cmap = ctx.cookies();
        CHECK_EQ(cmap.at("user"), "alice");
        CHECK_EQ(cmap.at("theme"), "dark");
        CHECK_EQ(cmap.at("split_cookie"), "from_second_header");
        CHECK_EQ(cmap.at("c_dup"), "first_val");
        CHECK_EQ(cmap.at("token"), "abc=123==");
        CHECK_EQ(cmap.at("empty_val"), "");
        CHECK(cmap.find("") == cmap.end());

        // Repeated cookies() returns independent owning map by value
        const auto cmap2 = ctx.cookies();
        CHECK_EQ(cmap, cmap2);

        // View stability: string_view points into raw Workflow request buffer
        CHECK_EQ(cv_user, "alice");
        CHECK_EQ(cv_user, cmap.at("user"));
        CHECK_EQ(cv_theme, "dark");
        CHECK_EQ(cv_theme, cmap.at("theme"));
        CHECK_EQ(cv_split, "from_second_header");
        CHECK_EQ(cv_split, cmap.at("split_cookie"));
        CHECK_EQ(cv_cdup, "first_val");
        CHECK_EQ(cv_cdup, cmap.at("c_dup"));
        CHECK_EQ(cv_token, "abc=123==");
        CHECK_EQ(cv_token, cmap.at("token"));

        // Sequential cookies also point to the same raw request buffer
        std::string_view cv_user2 = ctx.cookie("user");
        CHECK_EQ(cv_user2, "alice");
        CHECK_EQ(cv_user.data(), cv_user2.data());
        // Consistent behavior after parsing
        CHECK(!ctx.has_cookie(""));
        CHECK_EQ(ctx.cookie(""), "");

        ctx.string("ok_cookie");
    });
    server->GET("/test_quoted_cookie", [](kale::Context &ctx) {
        // Quoted cookie value unquoting (RFC 6265)
        CHECK(ctx.has_cookie("quoted"));
        CHECK_EQ(ctx.cookie("quoted"), "hello world");
        CHECK(ctx.has_cookie("empty_quoted"));
        CHECK_EQ(ctx.cookie("empty_quoted"), "");
        CHECK(ctx.has_cookie("token_quoted"));
        CHECK_EQ(ctx.cookie("token_quoted"), "abc=123==");
        CHECK(ctx.has_cookie("spaced"));
        CHECK_EQ(ctx.cookie("spaced"), "  inner spaced  ");

        // Malformed / asymmetric quotes remain unchanged
        CHECK(ctx.has_cookie("unclosed"));
        CHECK_EQ(ctx.cookie("unclosed"), "\"unclosed_val");
        CHECK(ctx.has_cookie("trailing"));
        CHECK_EQ(ctx.cookie("trailing"), "trailing_val\"");
        CHECK(ctx.has_cookie("single"));
        CHECK_EQ(ctx.cookie("single"), "\"");

        // First occurrence wins with quoted cookies
        CHECK(ctx.has_cookie("dup_q"));
        CHECK_EQ(ctx.cookie("dup_q"), "first");

        // Signed cookie with surrounding quotes
        auto signed_val = kale::signed_cookie(ctx, "signed_q", "test_secret_key");
        REQUIRE(signed_val.has_value());
        CHECK_EQ(*signed_val, "secret_session_payload");

        // Whole map materialization preserves unquoted semantics
        const auto cmap = ctx.cookies();
        CHECK_EQ(cmap.at("quoted"), "hello world");
        CHECK_EQ(cmap.at("empty_quoted"), "");
        CHECK_EQ(cmap.at("token_quoted"), "abc=123==");
        CHECK_EQ(cmap.at("spaced"), "  inner spaced  ");
        CHECK_EQ(cmap.at("unclosed"), "\"unclosed_val");
        CHECK_EQ(cmap.at("trailing"), "trailing_val\"");
        CHECK_EQ(cmap.at("single"), "\"");
        CHECK_EQ(cmap.at("dup_q"), "first");

        ctx.string("ok_quoted_cookie");
    });

    auto res_header = server.get("/test_header", {
        {"X-Direct", "direct_value"},
        {"X-Multi", "part1"},
        {"X-Multi", "part2"}
    });
    CHECK_EQ(res_header.status, 200);
    CHECK_EQ(res_header.body, "ok_header");

    auto res_query = server.get("/test_query?id=123&type=admin&msg=hello%20world&a+b=plus%20key&x%20y=pct%20key&dup=first&dup=second&dup_enc=first%20val&dup_enc=second&flag&empty=&m%ZZ=raw_pct&m%=trailing_pct&bad_val=foo%ZZbar&=ignored&=");
    CHECK_EQ(res_query.status, 200);
    CHECK_EQ(res_query.body, "ok_query");

    auto res_cookie = server.get("/test_cookie", {
        {"Cookie", "user=alice; theme=dark; c_dup=first_val; token=abc=123==; empty_val=; =bad_entry"},
        {"Cookie", "split_cookie=from_second_header; c_dup=second_val"}
    });
    CHECK_EQ(res_cookie.status, 200);
    CHECK_EQ(res_cookie.body, "ok_cookie");

    std::string signed_cookie_str = kale::sign_cookie_value("secret_session_payload", "test_secret_key");
    auto res_quoted = server.get("/test_quoted_cookie", {
        {"Cookie", "quoted=\"hello world\"; empty_quoted=\"\"; token_quoted=\"abc=123==\"; spaced=\"  inner spaced  \"; unclosed=\"unclosed_val; trailing=trailing_val\"; single=\"; dup_q=\"first\""},
        {"Cookie", "signed_q=\"" + signed_cookie_str + "\"; dup_q=\"second\""}
    });
    CHECK_EQ(res_quoted.status, 200);
    CHECK_EQ(res_quoted.body, "ok_quoted_cookie");

    // Regression: long (>SSO) valid signed-cookie roundtrip and tampered rejection
    const std::string long_secret = "super_secure_production_secret_key_1234567890_abcdefghijklmnopqrstuvwxyz";
    const std::string long_payload(256, 'K'); // 256 bytes > 15-byte SSO buffer
    std::string valid_long_signed = kale::sign_cookie_value(long_payload, long_secret);
    std::string tampered_long_signed = valid_long_signed;
    tampered_long_signed.back() = (tampered_long_signed.back() == 'A' ? 'B' : 'A');

    server->GET("/test_signed_cookie_long", [&long_secret, &long_payload](kale::Context &ctx) {
        auto valid_opt = kale::signed_cookie(ctx, "long_valid", long_secret);
        REQUIRE(valid_opt.has_value());
        CHECK_EQ(*valid_opt, long_payload);

        auto tampered_opt = kale::signed_cookie(ctx, "long_tampered", long_secret);
        CHECK_FALSE(tampered_opt.has_value());

        auto missing_opt = kale::signed_cookie(ctx, "non_existent_cookie", long_secret);
        CHECK_FALSE(missing_opt.has_value());

        auto wrong_secret_opt = kale::signed_cookie(ctx, "long_valid", "wrong_secret");
        CHECK_FALSE(wrong_secret_opt.has_value());

        ctx.string("ok_signed_cookie_long");
    });

    auto res_signed_long = server.get("/test_signed_cookie_long", {
        {"Cookie", "long_valid=" + valid_long_signed + "; long_tampered=" + tampered_long_signed}
    });
    CHECK_EQ(res_signed_long.status, 200);
    CHECK_EQ(res_signed_long.body, "ok_signed_cookie_long");
    server->GET("/test_no_metadata", [](kale::Context &ctx) {
        // Query on request without '?'
        CHECK(!ctx.has_query("foo"));
        CHECK_EQ(ctx.query("foo"), "");
        CHECK_EQ(ctx.default_query("foo", std::string_view("fallback")), "fallback");
        CHECK_EQ(ctx.default_query("foo", std::string("owned_fb")), "owned_fb");
        CHECK(ctx.query_params().empty());
        CHECK(!ctx.has_query("foo"));
        CHECK_EQ(ctx.query("foo"), "");

        // Cookie on request without Cookie header
        CHECK(!ctx.has_cookie("bar"));
        CHECK_EQ(ctx.cookie("bar"), "");
        CHECK_EQ(ctx.default_cookie("bar", std::string_view("fb_cookie")), "fb_cookie");
        CHECK(ctx.cookies().empty());
        CHECK(!ctx.has_cookie("bar"));
        CHECK_EQ(ctx.cookie("bar"), "");

        ctx.string("ok_no_metadata");
    });

    server->GET("/test_move", [](kale::Context &ctx) {
        std::string q_before = ctx.query("param");
        std::string_view c_before = ctx.cookie("ck");
        CHECK_EQ(q_before, "qval");
        CHECK_EQ(c_before, "cval");

        kale::Context moved = std::move(ctx);
        // Check moved-to instance
        CHECK_EQ(moved.query("param"), "qval");
        CHECK_EQ(moved.cookie("ck"), "cval");
        CHECK(moved.has_query("param"));
        CHECK(moved.has_cookie("ck"));
        // Views into map strings remain stable after Context move
        CHECK_EQ(q_before, "qval");
        CHECK_EQ(c_before.data(), moved.cookie("ck").data());

        // Move back to allow normal reply
        ctx = std::move(moved);
        CHECK_EQ(ctx.query("param"), "qval");
        CHECK_EQ(ctx.cookie("ck"), "cval");
        CHECK_EQ(c_before.data(), ctx.cookie("ck").data());
        ctx.string("ok_move");
    });

    server->GET("/test_move_unparsed", [](kale::Context &ctx) {
        // Move before any query or cookie parsing
        kale::Context moved = std::move(ctx);

        // Parse on moved instance
        CHECK(moved.has_query("lazy_q"));
        CHECK_EQ(moved.query("lazy_q"), "hello");
        CHECK(moved.has_cookie("lazy_c"));
        CHECK_EQ(moved.cookie("lazy_c"), "world");

        ctx = std::move(moved);
        CHECK_EQ(ctx.query("lazy_q"), "hello");
        CHECK_EQ(ctx.cookie("lazy_c"), "world");
        ctx.string("ok_move_unparsed");
    });

    auto res_no_meta = server.get("/test_no_metadata");
    CHECK_EQ(res_no_meta.status, 200);
    CHECK_EQ(res_no_meta.body, "ok_no_metadata");

    auto res_move = server.get("/test_move?param=qval", {{"Cookie", "ck=cval"}});
    CHECK_EQ(res_move.status, 200);
    CHECK_EQ(res_move.body, "ok_move");

    auto res_move_unparsed = server.get("/test_move_unparsed?lazy_q=hello", {{"Cookie", "lazy_c=world"}});
    CHECK_EQ(res_move_unparsed.status, 200);
    CHECK_EQ(res_move_unparsed.body, "ok_move_unparsed");
}

TEST_CASE("Request Metadata Boundary and Lifetime Semantics") {
    CoreTestServer server;

    server->GET("/test_boundary", [](kale::Context &ctx) {
        // 1. Query prefix discrimination and duplicate prevention
        CHECK(ctx.has_query("user"));
        CHECK_EQ(ctx.query("user"), "alice");
        CHECK(ctx.has_query("user_id"));
        CHECK_EQ(ctx.query("user_id"), "456");
        CHECK(ctx.has_query("username"));
        CHECK_EQ(ctx.query("username"), "bob");
        CHECK(!ctx.has_query("use"));
        CHECK_EQ(ctx.query("use"), "");
        CHECK(!ctx.has_query("user_"));
        CHECK_EQ(ctx.query("user_"), "");

        // 2. Cookie prefix discrimination
        CHECK(ctx.has_cookie("auth"));
        CHECK_EQ(ctx.cookie("auth"), "yes");
        CHECK(ctx.has_cookie("auth_token"));
        CHECK_EQ(ctx.cookie("auth_token"), "secret123");
        CHECK(ctx.has_cookie("author"));
        CHECK_EQ(ctx.cookie("author"), "charlie");
        CHECK(!ctx.has_cookie("aut"));
        CHECK_EQ(ctx.cookie("aut"), "");

        // 3. Multi-header joining (3 duplicate headers)
        CHECK(ctx.has_header("X-Multi-3"));
        CHECK_EQ(ctx.header("X-Multi-3"), "one, two, three");

        // 4. Content-Type inspection
        CHECK_EQ(ctx.content_type(), "application/json");

        // 5. Independent whole-map calls
        auto headers_map = ctx.headers();
        auto query_map = ctx.query_params();
        auto cookie_map = ctx.cookies();

        CHECK_EQ(headers_map.at("X-Multi-3"), "one, two, three");
        CHECK_EQ(query_map.at("user"), "alice");
        CHECK_EQ(cookie_map.at("auth_token"), "secret123");

        ctx.string("ok_boundary");
    });

    server->GET("/test_bare_query", [](kale::Context &ctx) {
        // Query on URI with bare '?'
        CHECK(!ctx.has_query("foo"));
        CHECK_EQ(ctx.query("foo"), "");
        CHECK(ctx.query_params().empty());
        ctx.string("ok_bare_query");
    });

    auto res = server.get("/test_boundary?&&user_id=456&&&username=bob&user=alice&&user=ignored_dup&&", {
        {"Content-Type", "application/json"},
        {"X-Multi-3", "one"},
        {"X-Multi-3", "two"},
        {"X-Multi-3", "three"},
        {"Cookie", "  auth_token=secret123 ; author=charlie;  auth=yes ; auth=ignored_dup  "}
    });
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, "ok_boundary");

    auto res_bare = server.get("/test_bare_query?");
    CHECK_EQ(res_bare.status, 200);
    CHECK_EQ(res_bare.body, "ok_bare_query");
}

TEST_CASE("Workflow Native Parser Raw HTTP Request Metadata Integration") {
    CoreTestServer server;

    // 1. Consecutive duplicate headers
    server->GET("/raw_consecutive", [](kale::Context &ctx) {
        CHECK(ctx.has_header("X-Consecutive"));
        CHECK_EQ(ctx.header("X-Consecutive"), "first, second, third");
        CHECK_EQ(ctx.headers().at("X-Consecutive"), "first, second, third");
        // Map and scalar consistency
        CHECK_EQ(ctx.header("X-Consecutive"), ctx.headers().at("X-Consecutive"));
        ctx.string("ok_consecutive");
    });

    // 2. Interleaved duplicate headers (separated by other headers)
    server->GET("/raw_interleaved", [](kale::Context &ctx) {
        CHECK(ctx.has_header("X-Interleaved"));
        CHECK_EQ(ctx.header("X-Interleaved"), "part1, part2, part3");
        CHECK_EQ(ctx.header("x-interleaved"), "part1, part2, part3");
        CHECK_EQ(ctx.header("X-INTERLEAVED"), "part1, part2, part3");
        CHECK_EQ(ctx.headers().at("X-Interleaved"), "part1, part2, part3");
        CHECK_EQ(ctx.headers().at("x-interleaved"), "part1, part2, part3");
        CHECK_EQ(ctx.headers().at("X-INTERLEAVED"), "part1, part2, part3");
        CHECK_EQ(ctx.header("X-Other-A"), "dummy_a");
        CHECK_EQ(ctx.header("X-Other-B"), "dummy_b");
        CHECK_EQ(ctx.headers().at("X-Other-A"), "dummy_a");
        CHECK_EQ(ctx.headers().at("X-Other-B"), "dummy_b");
        // Map and scalar consistency
        CHECK_EQ(ctx.header("X-Interleaved"), ctx.headers().at("X-Interleaved"));
        ctx.string("ok_interleaved");
    });

    // 3. Mixed-case header names across multiple lines
    server->GET("/raw_mixed_case", [](kale::Context &ctx) {
        CHECK(ctx.has_header("x-custom-case"));
        CHECK(ctx.has_header("X-Custom-Case"));
        CHECK(ctx.has_header("X-CUSTOM-CASE"));
        CHECK_EQ(ctx.header("X-Custom-Case"), "first, second, third");
        CHECK_EQ(ctx.header("x-custom-case"), "first, second, third");
        CHECK_EQ(ctx.header("X-CUSTOM-CASE"), "first, second, third");
        CHECK_EQ(ctx.headers().at("X-Custom-Case"), "first, second, third");
        CHECK_EQ(ctx.headers().at("x-custom-case"), "first, second, third");
        CHECK_EQ(ctx.headers().at("X-CUSTOM-CASE"), "first, second, third");
        ctx.string("ok_mixed_case");
    });

    // 4. Multiple Cookie lines with different casing and RFC 6265 semicolon join
    server->GET("/raw_multiple_cookies", [](kale::Context &ctx) {
        CHECK(ctx.has_cookie("c1"));
        CHECK_EQ(ctx.cookie("c1"), "v1");
        CHECK(ctx.has_cookie("c2"));
        CHECK_EQ(ctx.cookie("c2"), "v2");
        CHECK(ctx.has_cookie("c3"));
        CHECK_EQ(ctx.cookie("c3"), "v3");
        CHECK(ctx.has_cookie("c4"));
        CHECK_EQ(ctx.cookie("c4"), "v4");
        CHECK(ctx.has_cookie("c5"));
        CHECK_EQ(ctx.cookie("c5"), "v5");
        CHECK(ctx.has_cookie("c6"));
        CHECK_EQ(ctx.cookie("c6"), "v6");

        // Header RFC 6265 semicolon joining for multiple Cookie headers
        CHECK_EQ(ctx.header("Cookie"), "c1=v1; c2=v2; c3=v3; c4=v4; c5=v5; c6=v6");
        CHECK_EQ(ctx.headers().at("Cookie"), "c1=v1; c2=v2; c3=v3; c4=v4; c5=v5; c6=v6");
        CHECK_EQ(ctx.header("Cookie"), ctx.headers().at("Cookie"));

        // Map vs scalar consistency
        const auto cmap = ctx.cookies();
        CHECK_EQ(cmap.at("c1"), ctx.cookie("c1"));
        CHECK_EQ(cmap.at("c2"), ctx.cookie("c2"));
        CHECK_EQ(cmap.at("c3"), ctx.cookie("c3"));
        CHECK_EQ(cmap.at("c4"), ctx.cookie("c4"));
        CHECK_EQ(cmap.at("c5"), ctx.cookie("c5"));
        CHECK_EQ(cmap.at("c6"), ctx.cookie("c6"));
        ctx.string("ok_multiple_cookies");
    });

    // 5. Duplicate cookie names (same line and across multiple lines: first occurrence wins)
    server->GET("/raw_duplicate_cookies", [](kale::Context &ctx) {
        CHECK(ctx.has_cookie("dup_same"));
        CHECK_EQ(ctx.cookie("dup_same"), "same_first");
        CHECK_EQ(ctx.cookies().at("dup_same"), "same_first");

        CHECK(ctx.has_cookie("dup_cross"));
        CHECK_EQ(ctx.cookie("dup_cross"), "cross_first");
        CHECK_EQ(ctx.cookies().at("dup_cross"), "cross_first");

        ctx.string("ok_duplicate_cookies");
    });

    // 6. Empty values in headers, cookies, and query
    server->GET("/raw_empty_values", [](kale::Context &ctx) {
        CHECK(ctx.has_header("X-Empty-Header"));
        CHECK_EQ(ctx.header("X-Empty-Header"), "");
        CHECK_EQ(ctx.headers().at("X-Empty-Header"), "");

        CHECK(ctx.has_header("X-Empty-Space"));
        CHECK_EQ(ctx.header("X-Empty-Space"), "");
        CHECK_EQ(ctx.headers().at("X-Empty-Space"), "");

        CHECK(ctx.has_header("X-Empty-Multi"));
        CHECK_EQ(ctx.header("X-Empty-Multi"), ", ");
        CHECK_EQ(ctx.headers().at("X-Empty-Multi"), ", ");

        CHECK(ctx.has_cookie("empty_c"));
        CHECK_EQ(ctx.cookie("empty_c"), "");
        CHECK_EQ(ctx.cookies().at("empty_c"), "");

        CHECK(ctx.has_cookie("quoted_empty"));
        CHECK_EQ(ctx.cookie("quoted_empty"), "");
        CHECK_EQ(ctx.cookies().at("quoted_empty"), "");

        CHECK(ctx.has_cookie("normal"));
        CHECK_EQ(ctx.cookie("normal"), "ok");
        CHECK_EQ(ctx.cookies().at("normal"), "ok");

        CHECK(ctx.has_query("empty_q"));
        CHECK_EQ(ctx.query("empty_q"), "");
        CHECK_EQ(ctx.query_params().at("empty_q"), "");

        CHECK(ctx.has_query("flag_q"));
        CHECK_EQ(ctx.query("flag_q"), "");
        CHECK_EQ(ctx.query_params().at("flag_q"), "");

        ctx.string("ok_empty_values");
    });

    // 7. Quoted cookie values and special characters
    server->GET("/raw_quoted_cookies", [](kale::Context &ctx) {
        CHECK(ctx.has_cookie("q_simple"));
        CHECK_EQ(ctx.cookie("q_simple"), "hello_world");
        CHECK_EQ(ctx.cookies().at("q_simple"), "hello_world");

        CHECK(ctx.has_cookie("q_spaces"));
        CHECK_EQ(ctx.cookie("q_spaces"), "  spaced  ");
        CHECK_EQ(ctx.cookies().at("q_spaces"), "  spaced  ");

        CHECK(ctx.has_cookie("q_eq"));
        CHECK_EQ(ctx.cookie("q_eq"), "k=v==1");
        CHECK_EQ(ctx.cookies().at("q_eq"), "k=v==1");

        CHECK(ctx.has_cookie("q_commas"));
        CHECK_EQ(ctx.cookie("q_commas"), "a,b,c");
        CHECK_EQ(ctx.cookies().at("q_commas"), "a,b,c");

        // Asymmetric/unclosed quotes remain raw
        CHECK(ctx.has_cookie("unclosed"));
        CHECK_EQ(ctx.cookie("unclosed"), "\"raw_val");
        CHECK_EQ(ctx.cookies().at("unclosed"), "\"raw_val");

        CHECK(ctx.has_cookie("lone_q"));
        CHECK_EQ(ctx.cookie("lone_q"), "\"");
        CHECK_EQ(ctx.cookies().at("lone_q"), "\"");

        ctx.string("ok_quoted_cookies");
    });

    // 8. Header values containing commas and semicolons
    server->GET("/raw_commas_semicolons", [](kale::Context &ctx) {
        CHECK(ctx.has_header("X-Comma-Val"));
        CHECK_EQ(ctx.header("X-Comma-Val"), "token1, token2, token3, token4");
        CHECK_EQ(ctx.headers().at("X-Comma-Val"), "token1, token2, token3, token4");

        CHECK(ctx.has_header("X-Semi-Val"));
        CHECK_EQ(ctx.header("X-Semi-Val"), "param=abc; other=def, extra=ghi; flag");
        CHECK_EQ(ctx.headers().at("X-Semi-Val"), "param=abc; other=def, extra=ghi; flag");

        CHECK(ctx.has_header("X-Mixed-Punct"));
        CHECK_EQ(ctx.header("X-Mixed-Punct"), "a,b;c=1, d;e=2");
        CHECK_EQ(ctx.headers().at("X-Mixed-Punct"), "a,b;c=1, d;e=2");

        CHECK(ctx.has_cookie("comma_in_cookie"));
        CHECK_EQ(ctx.cookie("comma_in_cookie"), "part1,part2");
        CHECK_EQ(ctx.cookies().at("comma_in_cookie"), "part1,part2");

        ctx.string("ok_commas_semicolons");
    });

    // 9. Full map vs scalar consistency and string_view buffer stability across Context moves
    server->GET("/raw_full_consistency", [](kale::Context &ctx) {
        // Content-Type zero-copy view check
        CHECK_EQ(ctx.content_type(), "application/x-custom");

        // Map vs scalar check for all headers
        const auto hmap = ctx.headers();
        for (const auto &[k, v] : hmap) {
            CHECK_MESSAGE(ctx.has_header(k), "Missing header: ", k);
            CHECK_EQ(ctx.header(k), v);
        }

        // Map vs scalar check for all cookies
        const auto cmap = ctx.cookies();
        for (const auto &[k, v] : cmap) {
            CHECK_MESSAGE(ctx.has_cookie(k), "Missing cookie: ", k);
            CHECK_EQ(ctx.cookie(k), v);
            CHECK_EQ(ctx.default_cookie(k, "fallback"), v);
        }

        // Map vs scalar check for all query parameters
        const auto qmap = ctx.query_params();
        for (const auto &[k, v] : qmap) {
            CHECK_MESSAGE(ctx.has_query(k), "Missing query: ", k);
            CHECK_EQ(ctx.query(k), v);
            CHECK_EQ(ctx.default_query(k, "fallback"), v);
        }

        // Check buffer stability of string_view across Context move on live raw request
        std::string_view c_ptr1 = ctx.cookie("user");
        std::string_view ct_ptr1 = ctx.content_type();
        CHECK_EQ(c_ptr1, "bob");
        CHECK_EQ(ct_ptr1, "application/x-custom");

        kale::Context moved = std::move(ctx);
        std::string_view c_ptr2 = moved.cookie("user");
        std::string_view ct_ptr2 = moved.content_type();
        CHECK_EQ(c_ptr1.data(), c_ptr2.data());
        CHECK_EQ(ct_ptr1.data(), ct_ptr2.data());

        ctx = std::move(moved);
        std::string_view c_ptr3 = ctx.cookie("user");
        std::string_view ct_ptr3 = ctx.content_type();
        CHECK_EQ(c_ptr1.data(), c_ptr3.data());
        CHECK_EQ(ct_ptr1.data(), ct_ptr3.data());

        ctx.string("ok_full_consistency");
    });

    // Send raw HTTP requests over real TCP socket
    auto resp1 = send_raw_http(server.port(),
        "GET /raw_consecutive HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "X-Consecutive: first\r\n"
        "X-Consecutive: second\r\n"
        "X-Consecutive: third\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(resp1.status, 200);
    CHECK_EQ(resp1.body, "ok_consecutive");

    auto resp2 = send_raw_http(server.port(),
        "GET /raw_interleaved HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "X-Interleaved: part1\r\n"
        "X-Other-A: dummy_a\r\n"
        "X-Interleaved: part2\r\n"
        "X-Other-B: dummy_b\r\n"
        "x-interleaved: part3\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(resp2.status, 200);
    CHECK_EQ(resp2.body, "ok_interleaved");

    auto resp3 = send_raw_http(server.port(),
        "GET /raw_mixed_case HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "x-custom-case: first\r\n"
        "X-CUSTOM-CASE: second\r\n"
        "X-Custom-Case: third\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(resp3.status, 200);
    CHECK_EQ(resp3.body, "ok_mixed_case");

    auto resp4 = send_raw_http(server.port(),
        "GET /raw_multiple_cookies HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Cookie: c1=v1; c2=v2\r\n"
        "Cookie: c3=v3\r\n"
        "cookie: c4=v4; c5=v5\r\n"
        "COOKIE: c6=v6\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(resp4.status, 200);
    CHECK_EQ(resp4.body, "ok_multiple_cookies");

    auto resp5 = send_raw_http(server.port(),
        "GET /raw_duplicate_cookies HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Cookie: dup_same=same_first; dup_same=same_second; dup_cross=cross_first\r\n"
        "Cookie: dup_same=same_third; dup_cross=cross_second\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(resp5.status, 200);
    CHECK_EQ(resp5.body, "ok_duplicate_cookies");

    auto resp6 = send_raw_http(server.port(),
        "GET /raw_empty_values?empty_q=&flag_q HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "X-Empty-Header:\r\n"
        "X-Empty-Space:   \r\n"
        "X-Empty-Multi:\r\n"
        "X-Empty-Multi:\r\n"
        "Cookie: empty_c=; quoted_empty=\"\"; normal=ok\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(resp6.status, 200);
    CHECK_EQ(resp6.body, "ok_empty_values");

    auto resp7 = send_raw_http(server.port(),
        "GET /raw_quoted_cookies HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Cookie: q_simple=\"hello_world\"; q_spaces=\"  spaced  \"; q_eq=\"k=v==1\"; q_commas=\"a,b,c\"; unclosed=\"raw_val; lone_q=\"\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(resp7.status, 200);
    CHECK_EQ(resp7.body, "ok_quoted_cookies");

    auto resp8 = send_raw_http(server.port(),
        "GET /raw_commas_semicolons HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "X-Comma-Val: token1, token2, token3\r\n"
        "X-Comma-Val: token4\r\n"
        "X-Semi-Val: param=abc; other=def\r\n"
        "X-Semi-Val: extra=ghi; flag\r\n"
        "X-Mixed-Punct: a,b;c=1, d;e=2\r\n"
        "Cookie: comma_in_cookie=part1,part2; normal=1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(resp8.status, 200);
    CHECK_EQ(resp8.body, "ok_commas_semicolons");

    auto resp9 = send_raw_http(server.port(),
        "GET /raw_full_consistency?search=hello%20world&filter=active&flag&page=2 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/x-custom\r\n"
        "X-Header-A: val1\r\n"
        "X-Header-B: val2\r\n"
        "x-header-a: val3\r\n"
        "Cookie: user=bob; session=\"active=true==\"; empty_val=\r\n"
        "Cookie: extra_cookie=present; user=ignored_second\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(resp9.status, 200);
    CHECK_EQ(resp9.body, "ok_full_consistency");
}

TEST_CASE("Deferred chunked request body decoding semantics and lifecycle") {
    CoreTestServer server;

    server->POST("/echo_chunked", [](kale::Context &ctx) {
        CHECK(ctx.raw_req().is_chunked());
        ctx.set_header("X-Body-Length", std::to_string(ctx.req_body().size()));
        ctx.string(std::string(ctx.req_body()));
    });

    server->POST("/bounded", [](kale::Context &ctx) {
        ctx.string("ok_bounded");
    }).max_body_size(10);

    server->POST("/param_traversal/:name", [](kale::Context &ctx) {
        ctx.string("reached");
    });

    // 1. Matched handler correctly decodes chunked body and preserves req_body() string_view
    auto echo_resp = send_raw_http(server.port(),
        "POST /echo_chunked HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n"
        "5\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\n\r\n");
    CHECK_EQ(echo_resp.status, 200);
    CHECK_EQ(echo_resp.body, "hello world");
    CHECK(echo_resp.headers.find("X-Body-Length: 11") != std::string::npos);

    // 2. max_body_size enforcement on decoded chunked body
    auto bounded_resp = send_raw_http(server.port(),
        "POST /bounded HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n"
        "b\r\n0123456789a\r\n"
        "0\r\n\r\n");
    CHECK_EQ(bounded_resp.status, 413);

    // 3. Unmatched 404 does not fail or hang with chunked body
    auto not_found_resp = send_raw_http(server.port(),
        "POST /unmatched_route HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n"
        "5\r\nhello\r\n"
        "0\r\n\r\n");
    CHECK_EQ(not_found_resp.status, 404);

    // 4. Path traversal rejects immediately with 403 without invoking handler or failing
    auto traversal_resp = send_raw_http(server.port(),
        "POST /echo_chunked/../forbidden HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n"
        "5\r\nhello\r\n"
        "0\r\n\r\n");
    CHECK_EQ(traversal_resp.status, 403);

    // 5. Parameter path traversal rejects with 403 before body preparation
    auto param_traversal_resp = send_raw_http(server.port(),
        "POST /param_traversal/%2e%2e HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n"
        "5\r\nhello\r\n"
        "0\r\n\r\n");
    CHECK_EQ(param_traversal_resp.status, 403);
}

TEST_CASE("Response header storage, case-insensitivity, overwrite, and wire emission") {
    CoreTestServer srv;

    // 1. Direct Context response header mutation and inspection
    srv->GET("/test_direct_headers", [](kale::Context &ctx) {
        // Basic insertion and case-insensitive retrieval
        ctx.set_header("X-Custom-Header", "value1");
        CHECK(ctx.has_resp_header("X-Custom-Header"));
        CHECK(ctx.has_resp_header("x-custom-header"));
        CHECK(ctx.has_resp_header("X-CUSTOM-HEADER"));
        CHECK_EQ(ctx.resp_header("X-Custom-Header"), "value1");
        CHECK_EQ(ctx.resp_header("x-custom-header"), "value1");
        CHECK_EQ(ctx.resp_header("X-CUSTOM-HEADER"), "value1");

        // Non-existent header
        CHECK(!ctx.has_resp_header("X-Non-Existent"));
        CHECK_EQ(ctx.resp_header("X-Non-Existent"), "");

        // Overwrite semantics with different casing
        ctx.set_header("x-custom-header", "value2");
        CHECK(ctx.has_resp_header("X-Custom-Header"));
        CHECK_EQ(ctx.resp_header("X-Custom-Header"), "value2");
        CHECK_EQ(ctx.resp_header("x-custom-header"), "value2");

        // Multiple distinct headers
        ctx.set_header("Content-Type", "application/json");
        ctx.set_header("X-Trace-Id", "trace-12345");
        CHECK_EQ(ctx.resp_header("Content-Type"), "application/json");
        CHECK_EQ(ctx.resp_header("content-type"), "application/json");
        CHECK_EQ(ctx.resp_header("X-Trace-Id"), "trace-12345");

        // Header validation: invalid header name rejected
        ctx.set_header("Bad Header Name", "val");
        ctx.set_header("Bad:Name", "val");
        ctx.set_header("Bad\r\nName", "val");
        ctx.set_header("", "val");
        CHECK(!ctx.has_resp_header("Bad Header Name"));
        CHECK(!ctx.has_resp_header("Bad:Name"));
        CHECK(!ctx.has_resp_header("Bad\r\nName"));
        CHECK(!ctx.has_resp_header(""));

        // Header validation: invalid header value rejected (CRLF injection prevention)
        ctx.set_header("X-Injected", "val\r\nInjected: evil");
        CHECK(!ctx.has_resp_header("X-Injected"));

        // Erase header
        CHECK(ctx.erase_resp_header("x-trace-id"));
        CHECK(!ctx.has_resp_header("X-Trace-Id"));
        CHECK_EQ(ctx.resp_header("X-Trace-Id"), "");
        CHECK(!ctx.erase_resp_header("X-Trace-Id")); // Already erased

        // Remaining headers intact
        CHECK(ctx.has_resp_header("X-Custom-Header"));
        CHECK_EQ(ctx.resp_header("X-Custom-Header"), "value2");
        CHECK(ctx.has_resp_header("Content-Type"));

        ctx.string("direct_ok");
    });

    auto direct_resp = srv.get("/test_direct_headers");
    CHECK_EQ(direct_resp.status, 200);
    CHECK_EQ(direct_resp.body, "direct_ok");
    CHECK_EQ(direct_resp.header("X-Custom-Header"), "value2");
    CHECK(!direct_resp.has_header("X-Trace-Id"));

    // 2. Full wire emission through HTTP server
    srv->GET("/test_resp_headers", [](kale::Context &ctx) {
        ctx.set_header("X-Service-Name", "KaleApp");
        ctx.set_header("x-service-name", "KaleCore"); // overwrite with lowercase
        ctx.set_header("X-Multi-Case-Test", "original");
        ctx.set_header("X-MULTI-CASE-TEST", "updated"); // overwrite with uppercase
        ctx.set_header("X-Preserved", "static_value");
        ctx.set_header("X-Erased-Header", "to_be_removed");
        ctx.erase_resp_header("x-erased-header");
        ctx.set_header("Invalid Header", "should_not_appear");
        ctx.set_header("X-Crlf", "bad\r\nvalue");
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string("headers_ok");
    });

    auto resp = srv.get("/test_resp_headers");
    CHECK_EQ(resp.status, 200);
    CHECK_EQ(resp.body, "headers_ok");

    // Verify wire values
    CHECK(resp.has_header("X-Service-Name"));
    CHECK_EQ(resp.header("X-Service-Name"), "KaleCore");
    CHECK_EQ(resp.header("x-service-name"), "KaleCore");

    CHECK(resp.has_header("X-Multi-Case-Test"));
    CHECK_EQ(resp.header("X-Multi-Case-Test"), "updated");
    CHECK_EQ(resp.header("x-multi-case-test"), "updated");

    CHECK(resp.has_header("X-Preserved"));
    CHECK_EQ(resp.header("X-Preserved"), "static_value");

    CHECK(!resp.has_header("X-Erased-Header"));
    CHECK(!resp.has_header("Invalid Header"));
    CHECK(!resp.has_header("X-Crlf"));
    CHECK(resp.header("Content-Type").find("text/plain") != std::string::npos);

    // Verify raw socket bytes
    auto raw_resp = send_raw_http(srv.port(),
        "GET /test_resp_headers HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(raw_resp.status, 200);
    CHECK_EQ(raw_resp.body, "headers_ok");
    CHECK(raw_resp.headers.find("KaleCore") != std::string::npos);
    CHECK(raw_resp.headers.find("KaleApp") == std::string::npos);
    CHECK(raw_resp.headers.find("updated") != std::string::npos);
    CHECK(raw_resp.headers.find("original") == std::string::npos);
    CHECK(raw_resp.headers.find("Invalid Header") == std::string::npos);
    CHECK(raw_resp.headers.find("to_be_removed") == std::string::npos);
    CHECK(raw_resp.headers.find("bad\r\nvalue") == std::string::npos);
}

TEST_CASE("Context borrowed URI/path views, move semantics, and wire status serialization") {
    CoreTestServer srv;

    // 1. Borrowed URI/path views with query and move semantics
    srv->GET("/api/v1/test_views", [](kale::Context &ctx) {
        const char *raw_req_uri = ctx.raw_req().get_request_uri();
        CHECK(raw_req_uri != nullptr);

        // Verify std::string_view return types and exact bytes
        std::string_view u = ctx.raw_uri();
        std::string_view p = ctx.path();
        CHECK_EQ(u, "/api/v1/test_views?param1=hello&param2=world%20test&flag");
        CHECK_EQ(p, "/api/v1/test_views");

        // Lifetime/ownership proof: pointers borrow directly from raw request URI buffer without copying
        CHECK_EQ(u.data(), raw_req_uri);
        CHECK_EQ(p.data(), raw_req_uri);

        // Query extraction from borrowed views
        CHECK(ctx.has_query("param1"));
        CHECK_EQ(ctx.query("param1"), "hello");
        CHECK(ctx.has_query("param2"));
        CHECK_EQ(ctx.query("param2"), "world test");
        CHECK(ctx.has_query("flag"));
        CHECK_EQ(ctx.default_query("missing", "default_val"), "default_val");

        auto qm = ctx.query_params();
        CHECK_EQ(qm.at("param1"), "hello");
        CHECK_EQ(qm.at("param2"), "world test");

        // Move constructor preserves views and raw request buffer anchoring
        kale::Context moved(std::move(ctx));
        CHECK_EQ(moved.raw_uri(), "/api/v1/test_views?param1=hello&param2=world%20test&flag");
        CHECK_EQ(moved.path(), "/api/v1/test_views");
        CHECK_EQ(moved.raw_uri().data(), raw_req_uri);
        CHECK_EQ(moved.path().data(), raw_req_uri);
        CHECK_EQ(moved.query("param1"), "hello");
        CHECK_EQ(moved.query("param2"), "world test");
        CHECK(ctx.raw_uri().empty());
        CHECK(ctx.path().empty());

        // Move assignment back restores views and allows normal completion
        ctx = std::move(moved);
        CHECK_EQ(ctx.raw_uri(), "/api/v1/test_views?param1=hello&param2=world%20test&flag");
        CHECK_EQ(ctx.path(), "/api/v1/test_views");
        CHECK_EQ(ctx.raw_uri().data(), raw_req_uri);
        CHECK_EQ(ctx.path().data(), raw_req_uri);
        CHECK_EQ(ctx.query("param1"), "hello");
        CHECK(moved.raw_uri().empty());
        CHECK(moved.path().empty());

        ctx.string("views_ok");
    });

    // 2. Bare path and bare question mark
    srv->GET("/bare_path", [](kale::Context &ctx) {
        const char *raw_req_uri = ctx.raw_req().get_request_uri();
        CHECK_EQ(ctx.path(), "/bare_path");
        CHECK_EQ(ctx.raw_uri(), "/bare_path");
        CHECK_EQ(ctx.path().data(), raw_req_uri);
        CHECK_EQ(ctx.raw_uri().data(), raw_req_uri);
        CHECK(!ctx.has_query("foo"));
        ctx.string("bare_ok");
    });

    srv->GET("/bare_query", [](kale::Context &ctx) {
        CHECK_EQ(ctx.path(), "/bare_query");
        CHECK_EQ(ctx.raw_uri(), "/bare_query?");
        CHECK(!ctx.has_query("foo"));
        ctx.string("bare_query_ok");
    });

    // 3. Status codes: standard literals, custom 3-digit codes, and invalid status fallbacks
    srv->GET("/status_200", [](kale::Context &ctx) {
        ctx.set_status(kale::HttpStatus::OK);
        ctx.string("ok_200");
    });

    srv->GET("/status_201", [](kale::Context &ctx) {
        ctx.set_status(201);
        ctx.string("created_201");
    });

    srv->GET("/status_204", [](kale::Context &ctx) {
        ctx.set_status(204);
    });

    srv->GET("/status_404", [](kale::Context &ctx) {
        ctx.set_status(404);
        ctx.string("not_found_404");
    });

    srv->GET("/status_418", [](kale::Context &ctx) {
        ctx.set_status(418); // Valid 3-digit custom code (formats via std::to_chars stack buffer)
        ctx.string("teapot_418");
    });

    srv->GET("/status_999", [](kale::Context &ctx) {
        ctx.set_status(999); // Upper bound valid 3-digit code (formats via std::to_chars stack buffer)
        ctx.string("edge_999");
    });

    srv->GET("/status_invalid_low", [](kale::Context &ctx) {
        ctx.set_status(42); // Invalid (<100) -> fallback 500
        ctx.string("fallback_low");
    });

    srv->GET("/status_invalid_high", [](kale::Context &ctx) {
        ctx.set_status(1000); // Invalid (>999) -> fallback 500
        ctx.string("fallback_high");
    });

    srv->GET("/status_invalid_neg", [](kale::Context &ctx) {
        ctx.set_status(-1); // Invalid negative -> fallback 500
        ctx.string("fallback_neg");
    });

    // Execute wire verification
    auto r1 = send_raw_http(srv.port(),
        "GET /api/v1/test_views?param1=hello&param2=world%20test&flag HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(r1.status, 200);
    CHECK(r1.status_line.starts_with("HTTP/1.1 200 OK"));
    CHECK_EQ(r1.body, "views_ok");

    auto r2 = send_raw_http(srv.port(),
        "GET /bare_path HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(r2.status, 200);
    CHECK_EQ(r2.body, "bare_ok");

    auto r3 = send_raw_http(srv.port(),
        "GET /bare_query? HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(r3.status, 200);
    CHECK_EQ(r3.body, "bare_query_ok");

    auto r_200 = send_raw_http(srv.port(), "GET /status_200 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK_EQ(r_200.status, 200);
    CHECK(r_200.status_line.starts_with("HTTP/1.1 200 OK"));
    CHECK_EQ(r_200.body, "ok_200");

    auto r_201 = send_raw_http(srv.port(), "GET /status_201 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK_EQ(r_201.status, 201);
    CHECK(r_201.status_line.starts_with("HTTP/1.1 201 Created"));
    CHECK_EQ(r_201.body, "created_201");

    auto r_204 = send_raw_http(srv.port(), "GET /status_204 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK_EQ(r_204.status, 204);
    CHECK(r_204.status_line.starts_with("HTTP/1.1 204 No Content"));

    auto r_404 = send_raw_http(srv.port(), "GET /status_404 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK_EQ(r_404.status, 404);
    CHECK(r_404.status_line.starts_with("HTTP/1.1 404 Not Found"));

    auto r_418 = send_raw_http(srv.port(), "GET /status_418 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK_EQ(r_418.status, 418);
    CHECK(r_418.status_line.starts_with("HTTP/1.1 418"));
    CHECK_EQ(r_418.body, "teapot_418");

    auto r_999 = send_raw_http(srv.port(), "GET /status_999 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK_EQ(r_999.status, 999);
    CHECK(r_999.status_line.starts_with("HTTP/1.1 999"));
    CHECK_EQ(r_999.body, "edge_999");

    auto r_low = send_raw_http(srv.port(), "GET /status_invalid_low HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK_EQ(r_low.status, 500);
    CHECK(r_low.status_line.starts_with("HTTP/1.1 500 Internal Server Error"));

    auto r_high = send_raw_http(srv.port(), "GET /status_invalid_high HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK_EQ(r_high.status, 500);
    CHECK(r_high.status_line.starts_with("HTTP/1.1 500 Internal Server Error"));

    auto r_neg = send_raw_http(srv.port(), "GET /status_invalid_neg HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK_EQ(r_neg.status, 500);
    CHECK(r_neg.status_line.starts_with("HTTP/1.1 500 Internal Server Error"));
}

TEST_CASE("Attachment TypeMap type isolation, const/mut access, erase, clear, and RAII") {
    CoreTestServer srv;

    struct TraceAttachment {
        std::string trace_id;
        int hop_count{0};
    };

    struct UserAttachment {
        uint64_t user_id;
        std::string username;
    };

    struct UnusedAttachment {
        double dummy{0.0};
    };

    struct DestructionProbe {
        std::shared_ptr<int> token;
    };

    srv->GET("/test_attachment_contract", [](kale::Context &ctx) {
        // 1. Initial empty state
        CHECK_FALSE(ctx.has_attachment<TraceAttachment>());
        CHECK_FALSE(ctx.has_attachment<UserAttachment>());
        CHECK_EQ(ctx.get_attachment<TraceAttachment>(), nullptr);

        // 2. Set attachments and verify type isolation
        ctx.set_attachment(TraceAttachment{"tr-uuid-001", 1});
        ctx.set_attachment(UserAttachment{42, "tester"});

        CHECK(ctx.has_attachment<TraceAttachment>());
        CHECK(ctx.has_attachment<UserAttachment>());
        CHECK_FALSE(ctx.has_attachment<UnusedAttachment>());
        CHECK_EQ(ctx.get_attachment<UnusedAttachment>(), nullptr);

        // 3. Const access
        const kale::Context &const_ctx = ctx;
        const auto *c_trace = const_ctx.get_attachment<TraceAttachment>();
        REQUIRE_NE(c_trace, nullptr);
        CHECK_EQ(c_trace->trace_id, "tr-uuid-001");
        CHECK_EQ(c_trace->hop_count, 1);

        const auto *c_user = const_ctx.get_attachment<UserAttachment>();
        REQUIRE_NE(c_user, nullptr);
        CHECK_EQ(c_user->user_id, 42);
        CHECK_EQ(c_user->username, "tester");

        CHECK_EQ(const_ctx.get_attachment<UnusedAttachment>(), nullptr);

        // 4. Mutable access
        auto *m_trace = ctx.get_attachment<TraceAttachment>();
        REQUIRE_NE(m_trace, nullptr);
        m_trace->hop_count = 5;
        CHECK_EQ(ctx.get_attachment<TraceAttachment>()->hop_count, 5);

        // 5. RAII destruction on erase_attachment
        auto probe_count = std::make_shared<std::atomic<int>>(0);
        {
            auto token = std::shared_ptr<int>(new int(42), [probe_count](int *p) {
                delete p;
                probe_count->fetch_add(1, std::memory_order_relaxed);
            });
            ctx.set_attachment(DestructionProbe{std::move(token)});
        }
        CHECK_EQ(probe_count->load(), 0);
        ctx.erase_attachment<DestructionProbe>();
        CHECK_EQ(probe_count->load(), 1);
        CHECK_FALSE(ctx.has_attachment<DestructionProbe>());
        CHECK_EQ(ctx.get_attachment<DestructionProbe>(), nullptr);

        // User and trace attachments still preserved after erasing probe
        CHECK(ctx.has_attachment<TraceAttachment>());
        CHECK(ctx.has_attachment<UserAttachment>());

        // 6. RAII destruction on clear_attachments
        auto clear_count = std::make_shared<std::atomic<int>>(0);
        {
            auto token = std::shared_ptr<int>(new int(42), [clear_count](int *p) {
                delete p;
                clear_count->fetch_add(1, std::memory_order_relaxed);
            });
            ctx.set_attachment(DestructionProbe{std::move(token)});
        }
        CHECK_EQ(clear_count->load(), 0);
        ctx.clear_attachments();
        CHECK_EQ(clear_count->load(), 1);
        CHECK_FALSE(ctx.has_attachment<TraceAttachment>());
        CHECK_FALSE(ctx.has_attachment<UserAttachment>());
        CHECK_FALSE(ctx.has_attachment<DestructionProbe>());

        ctx.string("attachment_ok");
    });

    auto resp = srv.get("/test_attachment_contract");
    CHECK_EQ(resp.status, 200);
    CHECK_EQ(resp.body, "attachment_ok");
}

TEST_CASE("Native response body replacement, binary safety, and self-alias replacement") {
    CoreTestServer srv;

    srv->GET("/test_body_replacement", [](kale::Context &ctx) {
        // Initial state
        CHECK_EQ(ctx.resp_body_size(), 0);
        CHECK_EQ(get_resp_body(ctx), "");

        // 1. First body assignment: string
        ctx.string("hello world");
        CHECK_EQ(ctx.resp_body_size(), 11);
        CHECK_EQ(get_resp_body(ctx), "hello world");

        // 2. Repeated replacement with different content: html
        ctx.html("<title>Kale</title>");
        CHECK_EQ(ctx.resp_body_size(), 19);
        CHECK_EQ(get_resp_body(ctx), "<title>Kale</title>");
        CHECK_EQ(ctx.resp_header("Content-Type"), "text/html; charset=utf-8");

        // 3. Repeated replacement with json
        ctx.json(kale::Json::Object{{"status", "ok"}});
        CHECK_EQ(get_resp_body(ctx), "{\"status\":\"ok\"}");
        CHECK_EQ(ctx.resp_header("Content-Type"), "application/json; charset=utf-8");

        // 4. Repeated replacement with empty body
        ctx.body("");
        CHECK_EQ(ctx.resp_body_size(), 0);
        CHECK_EQ(get_resp_body(ctx), "");

        // 5. Binary body replacement with embedded null bytes
        std::string binary_data("\x00\x01\x02\x00\xfe\xff\x00\x42", 8);
        ctx.set_header("Content-Type", "application/octet-stream");
        ctx.body(binary_data);

        CHECK_EQ(ctx.resp_body_size(), 8);
        CHECK_EQ(get_resp_body(ctx), binary_data);

        std::string merged;
        CHECK(ctx.get_resp_body_merged(merged));
        CHECK_EQ(merged, binary_data);

        const void *blocks[4];
        size_t block_sizes[4];
        size_t nblocks = ctx.get_resp_body_blocks(blocks, block_sizes, 4);
        CHECK_EQ(nblocks, 1);
        CHECK_EQ(block_sizes[0], 8);
        CHECK_EQ(std::memcmp(blocks[0], binary_data.data(), 8), 0);

        // 6. Self-alias replacement: inspect existing native output block view and pass directly back to body()
        const void *cur_blocks[1];
        size_t cur_sizes[1];
        REQUIRE_EQ(ctx.get_resp_body_blocks(cur_blocks, cur_sizes, 1), 1);
        std::string_view self_view(static_cast<const char *>(cur_blocks[0]), cur_sizes[0]);
        ctx.body(self_view);
        CHECK_EQ(ctx.resp_body_size(), 8);
        CHECK_EQ(get_resp_body(ctx), binary_data);

        // Sub-slice self-alias replacement
        REQUIRE_EQ(ctx.get_resp_body_blocks(cur_blocks, cur_sizes, 1), 1);
        std::string_view sub_view(static_cast<const char *>(cur_blocks[0]) + 1, 4);
        ctx.body(sub_view);
        CHECK_EQ(ctx.resp_body_size(), 4);
        CHECK_EQ(get_resp_body(ctx), binary_data.substr(1, 4));

        // 7. Fragmented native output body with >128 blocks: self-alias replacement
        ctx.raw_resp().clear_output_body();
        for (int i = 0; i < 150; ++i) {
            char ch = static_cast<char>('A' + (i % 26));
            ctx.raw_resp().append_output_body(&ch, 1);
        }
        CHECK_EQ(ctx.resp_body_size(), 150);

        // Inspect a block beyond the 128 boundary (e.g. block 140)
        const void *frag_blocks[160];
        size_t frag_sizes[160];
        size_t total_blocks = ctx.get_resp_body_blocks(frag_blocks, frag_sizes, 160);
        REQUIRE_EQ(total_blocks, 150);

        // Pass a view of block 140 directly back to ctx.body()
        std::string_view block_140_view(static_cast<const char *>(frag_blocks[140]), frag_sizes[140]);
        char expected_char = static_cast<char>('A' + (140 % 26));
        CHECK_EQ(block_140_view, std::string_view(&expected_char, 1));

        ctx.body(block_140_view);
        CHECK_EQ(ctx.resp_body_size(), 1);
        CHECK_EQ(get_resp_body(ctx), std::string(1, expected_char));
    });

    // Wire verification: binary body delivered intact over HTTP
    auto raw = send_raw_http(srv.port(),
        "GET /test_body_replacement HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(raw.status, 200);
    std::string expected_char_str(1, static_cast<char>('A' + (140 % 26)));
    CHECK_EQ(raw.body.size(), 1);
    CHECK_EQ(raw.body, expected_char_str);
    CHECK(raw.headers.find("Content-Length: 1") != std::string::npos);
}

TEST_CASE("Temporary body inputs remain valid across coroutine await points") {
    CoreTestServer srv;

    // Passing temporary std::string and string literals into body setters before co_await
    srv->GET("/test_temp_await", [](kale::Context &ctx) -> coke::Task<void> {
        const std::string expected_payload = std::string("transient-temp-payload-") + std::to_string(98765);
        // Construct a temporary std::string on the fly; native output body copies it
        ctx.string(std::string(expected_payload));

        // Suspend execution across coroutine sleep
        co_await coke::sleep(0.01);

        // After resumption, native body must remain intact and valid
        CHECK_EQ(get_resp_body(ctx), expected_payload);
        CHECK_EQ(ctx.resp_body_size(), expected_payload.size());
        co_return;
    });

    srv->POST("/test_temp_rvalue_body", [](kale::Context &ctx) -> coke::Task<void> {
        std::string generated(2048, 'x');
        ctx.body(std::move(generated)); // rvalue move

        co_await coke::sleep(0.01);

        CHECK_EQ(ctx.resp_body_size(), 2048);
        CHECK_EQ(get_resp_body(ctx), std::string(2048, 'x'));
        co_return;
    });

    auto r1 = srv.get("/test_temp_await");
    CHECK_EQ(r1.status, 200);
    const std::string expected_payload = std::string("transient-temp-payload-") + std::to_string(98765);
    CHECK_EQ(r1.body, expected_payload);

    auto r2 = srv.post("/test_temp_rvalue_body");
    CHECK_EQ(r2.status, 200);
    CHECK_EQ(r2.body.size(), 2048);
    CHECK_EQ(r2.body, std::string(2048, 'x'));
}

TEST_CASE("Repeated Set-Cookie headers propagate across normal, file, and SSE transports") {
    ScopedTempDir tmp("kale_cookie_test_");
    tmp.write_file("download.dat", "file-content-for-cookie-test");

    CoreTestServer srv;

    // 1. Normal response: multiple cookies
    srv->GET("/cookie_normal", [](kale::Context &ctx) {
        ctx.set_cookie(kale::Cookie("sid", "sess_alpha_1").set_path("/").set_http_only(true));
        ctx.set_cookie(kale::Cookie("theme", "nord").set_path("/ui"));
        ctx.set_cookie(kale::Cookie("lang", "en-US"));
        ctx.string("normal_cookie_payload");
    });

    // 2. File transmission response: multiple cookies
    srv->GET("/cookie_file", [&tmp](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_cookie(kale::Cookie("download_token", "dtk_12345").set_path("/"));
        ctx.set_cookie(kale::Cookie("affinity", "node-7").set_http_only(true));
        co_await kale::send_file(ctx, tmp.file_path("download.dat"));
    });

    // 3. SSE streaming response: multiple cookies
    srv->GET("/cookie_sse", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_cookie(kale::Cookie("sse_session", "sse_stream_abc").set_path("/"));
        ctx.set_cookie(kale::Cookie("stream_track", "trk_999").set_http_only(true));
        kale::SseEmitter sse(ctx);
        co_await sse.send("sse_initial_data");
    });

    // Wire verification: 1. Normal response raw HTTP headers
    {
        auto raw = send_raw_http(srv.port(),
            "GET /cookie_normal HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n");
        CHECK_EQ(raw.status, 200);
        CHECK_EQ(raw.body, "normal_cookie_payload");

        // Verify all 3 distinct Set-Cookie headers exist in raw header bytes
        CHECK(raw.headers.find("Set-Cookie: sid=sess_alpha_1") != std::string::npos);
        CHECK(raw.headers.find("HttpOnly") != std::string::npos);
        CHECK(raw.headers.find("Set-Cookie: theme=nord") != std::string::npos);
        CHECK(raw.headers.find("Set-Cookie: lang=en-US") != std::string::npos);
    }

    // Wire verification: 2. File response raw HTTP headers
    {
        auto raw = send_raw_http(srv.port(),
            "GET /cookie_file HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n");
        CHECK_EQ(raw.status, 200);
        CHECK_EQ(raw.body, "file-content-for-cookie-test");

        CHECK(raw.headers.find("Set-Cookie: download_token=dtk_12345") != std::string::npos);
        CHECK(raw.headers.find("Set-Cookie: affinity=node-7") != std::string::npos);
    }

    // Wire verification: 3. SSE response raw stream headers
    {
        RawStreamClient sse_cli(srv.port());
        sse_cli.send_request("/cookie_sse");
        std::string stream_headers = sse_cli.read_headers();

        CHECK(stream_headers.find("Content-Type: text/event-stream") != std::string::npos);
        CHECK(stream_headers.find("Set-Cookie: sse_session=sse_stream_abc") != std::string::npos);
        CHECK(stream_headers.find("Set-Cookie: stream_track=trk_999") != std::string::npos);

        std::string chunk = sse_cli.read_chunk();
        CHECK(chunk.find("sse_initial_data") != std::string::npos);
    }
}

TEST_CASE("Compression replaces native output body and preserves skipped/failed cases") {
    CoreTestServer srv;

    // 1. Below min_length: compression skipped, original body preserved
    srv->GET("/compress_skip_small", [](kale::Context &ctx) {
        ctx.string("short payload");
        kale::CompressConfig cfg;
        cfg.min_length = 256;
        bool ok = kale::compress_response(ctx, cfg); // min_length 256 > 13
        CHECK_FALSE(ok);
        CHECK_FALSE(ctx.has_resp_header("Content-Encoding"));
        CHECK_EQ(ctx.resp_body_size(), 13);
        CHECK_EQ(get_resp_body(ctx), "short payload");
    });

    // 2. Unsupported encoding: compression skipped, original body preserved
    srv->GET("/compress_skip_encoding", [](kale::Context &ctx) {
        std::string payload(512, 'm');
        ctx.string(payload);
        kale::CompressConfig cfg;
        cfg.min_length = 50;
        bool ok = kale::compress_response(ctx, cfg);
        CHECK_FALSE(ok);
        CHECK_FALSE(ctx.has_resp_header("Content-Encoding"));
        CHECK_EQ(ctx.resp_body_size(), 512);
        CHECK_EQ(get_resp_body(ctx), payload);
    });

    // 3. Status 204 No Content: compression skipped
    srv->GET("/compress_skip_status", [](kale::Context &ctx) {
        ctx.set_status(kale::HttpStatus::NoContent);
        ctx.string(std::string(512, 'n'));
        kale::CompressConfig cfg;
        cfg.min_length = 50;
        bool ok = kale::compress_response(ctx, cfg);
        CHECK_FALSE(ok);
        CHECK_FALSE(ctx.has_resp_header("Content-Encoding"));
    });

    // 4. Successful compression: native body replaced with compressed bytes
    const std::string original_data(2048, 'z');
    srv->GET("/compress_success", [&original_data](kale::Context &ctx) {
        ctx.string(original_data);
        kale::CompressConfig cfg;
        cfg.min_length = 100;
        bool ok = kale::compress_response(ctx, cfg);
        CHECK(ok);
        CHECK(ctx.has_resp_header("Content-Encoding"));
        CHECK_EQ(ctx.resp_header("Content-Encoding"), "gzip");
        CHECK(ctx.has_resp_header("Vary"));

        // Native body was replaced by compressed payload
        CHECK_LT(ctx.resp_body_size(), original_data.size());

        // Decompress native body to verify data integrity
        std::string decompressed;
        REQUIRE(kale::Compress::ungzip(get_resp_body(ctx), decompressed, original_data.size()));
        CHECK_EQ(decompressed, original_data);
    });

    auto r1 = srv.get("/compress_skip_small", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(r1.status, 200);
    CHECK_EQ(r1.body, "short payload");
    CHECK_FALSE(r1.has_header("Content-Encoding"));

    auto r2 = srv.get("/compress_skip_encoding", {{"Accept-Encoding", "unknown_encoding, identity;q=0"}});
    CHECK_EQ(r2.status, 200);
    CHECK_EQ(r2.body, std::string(512, 'm'));
    CHECK_FALSE(r2.has_header("Content-Encoding"));

    auto r3 = srv.get("/compress_skip_status", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(r3.status, 204);

    // Wire test for successful compression
    auto raw_gz = send_raw_http(srv.port(),
        "GET /compress_success HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Accept-Encoding: gzip\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(raw_gz.status, 200);
    CHECK(raw_gz.headers.find("Content-Encoding: gzip") != std::string::npos);
    std::string decompressed_wire;
    REQUIRE(kale::Compress::ungzip(raw_gz.body, decompressed_wire, original_data.size()));
    CHECK_EQ(decompressed_wire, original_data);
}
TEST_CASE("Lifecycle and exception handling before and after stream commit") {
    CoreTestServer srv;

    // 1. Exception thrown before stream commit: safe 500 response
    srv->GET("/lifecycle_throw_pre_commit", [](kale::Context &ctx) {
        (void)ctx;
        throw std::runtime_error("pre_commit_failure");
    });

    // 2. JsonInputError thrown before stream commit: safe 400 response with standard reason phrase
    srv->GET("/lifecycle_json_err_pre_commit", [](kale::Context &ctx) {
        (void)ctx;
        throw kale::JsonInputError("simulated input error");
    });

    // 3. Exception thrown after stream commit: stream abort only, no second response
    srv->GET("/lifecycle_throw_post_commit", [](kale::Context &ctx) -> coke::Task<void> {
        bool ok = co_await ctx.send_chunk("stream_started;");
        CHECK(ok);
        CHECK(ctx.is_stream());
        throw std::runtime_error("post_commit_failure");
    });

    // 4. Fixed-length mismatch on stream end: abort stream without clean termination
    srv->GET("/lifecycle_fixed_length_mismatch", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_header("Content-Length", "100");
        bool ok = co_await ctx.send_chunk("short");
        CHECK(ok);
        co_await ctx.end_chunk();
        CHECK_FALSE(ctx.is_connected());
    });

    // 5. Native series completion publishes Context teardown to the test thread.
    // Receiving the HTTP body alone does not wait for the server reply callback
    // to resume process() and destroy its Context/attachments.
    auto attachment_done = std::make_shared<std::promise<void>>();
    auto attachment_finished = attachment_done->get_future();
    srv->GET("/lifecycle_attachment", [attachment_done](kale::Context &ctx) {
        auto *task = ctx.coke_context().get_task();
        series_of(task)->set_callback([attachment_done](const SeriesWork *) {
            attachment_done->set_value();
        });
        ctx.set_attachment(LifecycleTestAttachment{});
        auto *att = ctx.get_attachment<LifecycleTestAttachment>();
        CHECK_NE(att, nullptr);
        CHECK_EQ(LifecycleTestAttachment::alive_count, 1);
        ctx.string("attachment_ok");
    });


    // Verify pre-commit 500
    auto r1 = srv.get("/lifecycle_throw_pre_commit");
    CHECK_EQ(r1.status, 500);
    CHECK_EQ(r1.body, "Internal Server Error");

    // Verify pre-commit 400 produces standard reason phrase and length 11
    auto r2 = srv.get("/lifecycle_json_err_pre_commit");
    CHECK_EQ(r2.status, 400);
    CHECK_EQ(r2.body, "Bad Request");
    CHECK_EQ(r2.body.size(), 11);
    // Verify post-commit exception
    auto raw_post = send_raw_http(srv.port(),
        "GET /lifecycle_throw_post_commit HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(raw_post.status, 200);
    CHECK(raw_post.body.find("stream_started;") != std::string::npos);
    CHECK(raw_post.body.find("Internal Server Error") == std::string::npos);

    // Verify attachment destruction
    auto r3 = srv.get("/lifecycle_attachment");
    CHECK_EQ(r3.status, 200);
    REQUIRE(attachment_finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    attachment_finished.get();
    CHECK_EQ(LifecycleTestAttachment::alive_count, 0);

    // Verify fixed-length mismatch wire behavior
    auto raw_mismatch = send_raw_http(srv.port(),
        "GET /lifecycle_fixed_length_mismatch HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(raw_mismatch.status, 200);
    // Stream aborted prematurely on length mismatch: payload size 5 < expected 100
    CHECK_LT(raw_mismatch.body.size(), 100);

    // Exactly one status line in raw response, no duplicate status line appended
    size_t first_status = raw_post.headers.find("HTTP/1.1");
    CHECK_NE(first_status, std::string::npos);
    CHECK_EQ(raw_post.body.find("HTTP/1.1"), std::string::npos);
}

TEST_CASE("Idle SSE peer-close terminates quiet queue pop and releases stream attachments") {
    CoreTestServer srv;
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto destruction_promise = std::make_shared<std::promise<void>>();
    auto destruction_fut = destruction_promise->get_future();

    auto handler_done_promise = std::make_shared<std::promise<void>>();
    auto handler_done_fut = handler_done_promise->get_future();

    srv->GET("/sse-consume-peer-close", [destroyed, destruction_promise,
                                         handler_done_promise](kale::Context &ctx) -> coke::Task<void> {
        auto witness = std::make_shared<DestructionWitness>(destroyed, destruction_promise);
        ctx.set_attachment(NotifyingAttachment(std::move(witness)));

        kale::SseEmitter sse(ctx);
        auto queue = std::make_shared<coke::Queue<std::string>>(10);

        co_await sse.consume(queue);

        handler_done_promise->set_value();
    });

    {
        RawStreamClient client(srv.port());
        client.send_request("/sse-consume-peer-close");

        std::string headers = client.read_headers();
        CHECK(headers.find("200 OK") != std::string::npos);
        CHECK(headers.find("text/event-stream") != std::string::npos);

        // Abruptly close peer socket while queue pop is waiting quietly
        client.close();

        REQUIRE(handler_done_fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    }

    REQUIRE(destruction_fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK(destroyed->load());

    srv.server().stop();
}

TEST_CASE("Idle SSE server stop terminates quiet queue pop cleanly") {
    CoreTestServer srv;
    auto handler_done_promise = std::make_shared<std::promise<void>>();
    auto handler_done_fut = handler_done_promise->get_future();

    srv->GET("/sse-consume-server-stop", [handler_done_promise](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        auto queue = std::make_shared<coke::Queue<std::string>>(10);

        co_await sse.consume(queue);

        handler_done_promise->set_value();
    });

    RawStreamClient client(srv.port());
    client.send_request("/sse-consume-server-stop");

    std::string headers = client.read_headers();
    CHECK(headers.find("200 OK") != std::string::npos);

    // Stop server while SSE handler is quietly awaiting queue->pop()
    srv.server().stop();

    REQUIRE(handler_done_fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
}

TEST_CASE("SSE queue normal drain emits items and terminal chunk preserving keep-alive") {
    CoreTestServer srv;
    auto handler_done = std::make_shared<std::promise<void>>();
    auto handler_done_fut = handler_done->get_future();

    srv->GET("/sse-consume-drain", [handler_done](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        auto queue = std::make_shared<coke::Queue<std::string>>(10);

        // Queue 2 items and then close the queue from application side
        queue->try_push("first");
        queue->try_push("second");
        queue->close();

        co_await sse.consume(queue);

        handler_done->set_value();
    });

    srv->GET("/after-drain", [](kale::Context &ctx) {
        ctx.string("reused_ok");
    });

    RawStreamClient client(srv.port());
    client.send_request("/sse-consume-drain", false);

    std::string headers = client.read_headers();
    CHECK(headers.find("200 OK") != std::string::npos);

    std::string chunk1 = client.read_chunk();
    CHECK_EQ(chunk1, "data: first\n\n");

    std::string chunk2 = client.read_chunk();
    CHECK_EQ(chunk2, "data: second\n\n");

    // Terminal chunk indicates clean stream completion
    std::string term_chunk = client.read_chunk();
    CHECK_EQ(term_chunk, "");

    REQUIRE(handler_done_fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);

    // Verify keep-alive connection reuse
    client.send_request("/after-drain");
    std::string headers2 = client.read_headers();
    CHECK(headers2.find("200 OK") != std::string::npos);
    std::string body2 = client.read_body_until_close(headers2);
    CHECK_EQ(body2, "reused_ok");

    srv.server().stop();
}

TEST_CASE("SSE bounded queue push unblocks when stream terminates") {
    CoreTestServer srv;
    auto queue = std::make_shared<coke::Queue<std::string>>(1);

    auto push_promise = std::make_shared<std::promise<int>>();
    auto push_fut = push_promise->get_future();

    auto producer_blocked_promise = std::make_shared<std::promise<void>>();
    auto producer_blocked_fut = producer_blocked_promise->get_future();

    auto handler_done = std::make_shared<std::promise<void>>();
    auto handler_done_fut = handler_done->get_future();

    srv->GET("/sse-consume-bounded", [queue, handler_done](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        co_await sse.consume(queue);
        handler_done->set_value();
    });

    RawStreamClient client(srv.port());
    client.send_request("/sse-consume-bounded");
    std::string headers = client.read_headers();
    CHECK(headers.find("200 OK") != std::string::npos);

    // Launch background producer loop pushing into bounded queue until released by close
    auto blocked_producer = [queue, push_promise]() -> coke::Task<void> {
        while (true) {
            int ret = co_await queue->push("payload");
            if (ret != coke::TOP_SUCCESS) {
                push_promise->set_value(ret);
                break;
            }
        }
    };
    coke::detach(blocked_producer());

    // Client reads at least one chunk to ensure streaming is active
    std::string chunk = client.read_chunk();
    CHECK_FALSE(chunk.empty());

    // Peer abruptly closes connection
    client.close();

    // Blocked producer must be released with TOP_CLOSED
    REQUIRE(push_fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK_EQ(push_fut.get(), coke::TOP_CLOSED);

    // Consumer also terminates cleanly
    REQUIRE(handler_done_fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);

    srv.server().stop();
}

TEST_CASE("Multiple SSE subscribers maintain independent dedicated queues") {
    CoreTestServer srv;

    auto queueA = std::make_shared<coke::Queue<std::string>>(10);
    auto queueB = std::make_shared<coke::Queue<std::string>>(10);

    auto doneA = std::make_shared<std::promise<void>>();
    auto doneB = std::make_shared<std::promise<void>>();
    auto futA = doneA->get_future();
    auto futB = doneB->get_future();

    srv->GET("/sse-sub-A", [queueA, doneA](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        co_await sse.consume(queueA);
        doneA->set_value();
    });

    srv->GET("/sse-sub-B", [queueB, doneB](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        co_await sse.consume(queueB);
        doneB->set_value();
    });

    RawStreamClient clientA(srv.port());
    clientA.send_request("/sse-sub-A");
    CHECK(clientA.read_headers().find("200 OK") != std::string::npos);

    RawStreamClient clientB(srv.port());
    clientB.send_request("/sse-sub-B");
    CHECK(clientB.read_headers().find("200 OK") != std::string::npos);

    // Client A closes abruptly
    clientA.close();

    // Sub A terminates
    REQUIRE(futA.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK(queueA->closed());

    // Sub B must remain completely intact and operational
    CHECK_FALSE(queueB->closed());
    queueB->try_push("event_for_B");
    std::string b_chunk = clientB.read_chunk();
    CHECK_EQ(b_chunk, "data: event_for_B\n\n");

    // Close B from application side
    queueB->close();
    REQUIRE(futB.wait_for(std::chrono::seconds(3)) == std::future_status::ready);

    srv.server().stop();
}

TEST_CASE("Native close callback bridge lifetime survives teardown race") {
    CoreTestServer srv;
    auto handler_done = std::make_shared<std::promise<void>>();
    auto handler_done_fut = handler_done->get_future();

    srv->GET("/sse-bridge-lifetime", [handler_done](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        auto queue = std::make_shared<coke::Queue<std::string>>(10);
        co_await sse.consume(queue);
        handler_done->set_value();
    });

    {
        RawStreamClient client(srv.port());
        client.send_request("/sse-bridge-lifetime");
        CHECK(client.read_headers().find("200 OK") != std::string::npos);

        // Abrupt disconnect
        client.close();
        REQUIRE(handler_done_fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    }

    srv.server().stop();
}

TEST_CASE("SSE consume parameter validation and exception safety") {
    CoreTestServer srv;

    srv->GET("/sse-consume-validation", [](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);

        // 1. Null queue must throw std::invalid_argument (both string and SseEvent overloads)
        bool caught_null = false;
        try {
            co_await sse.consume(std::shared_ptr<coke::Queue<std::string>>(nullptr));
        } catch (const std::invalid_argument &) {
            caught_null = true;
        }
        CHECK(caught_null);

        bool caught_null_ev = false;
        try {
            co_await sse.consume(std::shared_ptr<coke::Queue<kale::SseEvent>>(nullptr));
        } catch (const std::invalid_argument &) {
            caught_null_ev = true;
        }
        CHECK(caught_null_ev);

        // 2. Normal close
        co_await sse.close();

        // 3. Calling consume on closed emitter throws DeadStreamLoopException
        auto queue = std::make_shared<coke::Queue<std::string>>(10);
        bool caught_dead = false;
        try {
            co_await sse.consume(queue);
        } catch (const kale::DeadStreamLoopException &) {
            caught_dead = true;
        }
        CHECK(caught_dead);

        // 4. Closed emitter failure must close the passed queue so producers unblock
        CHECK(queue->closed());
    });

    auto res = srv.get("/sse-consume-validation");
    CHECK_EQ(res.status, 200);

    srv.server().stop();
}

TEST_CASE("Structured SseEvent queue consume emits formatted events and terminal chunk") {
    CoreTestServer srv;
    auto handler_done = std::make_shared<std::promise<void>>();
    auto handler_done_fut = handler_done->get_future();

    srv->GET("/sse-consume-events", [handler_done](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        auto queue = std::make_shared<coke::Queue<kale::SseEvent>>(10);

        kale::SseEvent ev1{.id = "1", .event = "delta", .data = "payload1", .retry = -1, .comment = ""};
        kale::SseEvent ev2{.id = "2", .event = "delta", .data = "payload2", .retry = -1, .comment = ""};

        queue->try_push(std::move(ev1));
        queue->try_push(std::move(ev2));
        queue->close();

        // Exercise public SseEvent overload
        co_await sse.consume(queue);

        handler_done->set_value();
    });

    RawStreamClient client(srv.port());
    client.send_request("/sse-consume-events", false);

    std::string headers = client.read_headers();
    CHECK(headers.find("200 OK") != std::string::npos);
    CHECK(headers.find("text/event-stream") != std::string::npos);

    std::string chunk1 = client.read_chunk();
    CHECK_EQ(chunk1, "id: 1\nevent: delta\ndata: payload1\n\n");

    std::string chunk2 = client.read_chunk();
    CHECK_EQ(chunk2, "id: 2\nevent: delta\ndata: payload2\n\n");

    std::string term = client.read_chunk();
    CHECK_EQ(term, "");

    REQUIRE(handler_done_fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);

    srv.server().stop();
}

TEST_CASE("SSE consume after Context end_chunk on Queue<string> completes without hanging") {
    CoreTestServer srv;
    auto handler_done = std::make_shared<std::promise<void>>();
    auto handler_done_fut = handler_done->get_future();
    auto queue = std::make_shared<coke::Queue<std::string>>(10);

    srv->GET("/sse-consume-after-end-string", [handler_done, queue](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        co_await sse.send("pre-end-string");
        co_await ctx.end_chunk();

        // Valid empty dedicated queue consumed after Context::end_chunk
        co_await sse.consume(queue);
        handler_done->set_value();
    });

    RawStreamClient client(srv.port());
    client.send_request("/sse-consume-after-end-string", true);

    std::string headers = client.read_headers();
    CHECK(headers.find("200 OK") != std::string::npos);
    CHECK(headers.find("text/event-stream") != std::string::npos);

    std::string chunk1 = client.read_chunk();
    CHECK_EQ(chunk1, "data: pre-end-string\n\n");

    std::string term = client.read_chunk();
    CHECK_EQ(term, "");

    // 1. Verify handler returns cleanly within timeout, releasing queue on failure to prevent suite deadlock
    auto ready1 = handler_done_fut.wait_for(std::chrono::seconds(3));
    if (ready1 != std::future_status::ready) {
        queue->close();
        (void)handler_done_fut.wait_for(std::chrono::seconds(1));
    }
    REQUIRE(ready1 == std::future_status::ready);
    // 2. Verify dedicated channel was closed by consume guard
    CHECK(queue->closed());

    // 3. Verify terminal frame is not duplicated: connection closes on EOF (0 bytes read)
    char extra[16] = {0};
    ssize_t n = ::recv(client.fd(), extra, sizeof(extra), 0);
    CHECK_EQ(n, 0);

    // 4. Verify external server.stop does not hang
    srv.server().stop();
}

TEST_CASE("SSE consume after Context end_chunk on Queue<SseEvent> completes without hanging") {
    CoreTestServer srv;
    auto handler_done = std::make_shared<std::promise<void>>();
    auto handler_done_fut = handler_done->get_future();
    auto queue = std::make_shared<coke::Queue<kale::SseEvent>>(10);

    srv->GET("/sse-consume-after-end-event", [handler_done, queue](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        kale::SseEvent ev{.id = "42", .event = "delta", .data = "pre-end-event", .retry = -1, .comment = ""};
        co_await sse.send_event(ev);
        co_await ctx.end_chunk();

        // Valid empty dedicated Queue<SseEvent> consumed after Context::end_chunk
        co_await sse.consume(queue);
        handler_done->set_value();
    });

    RawStreamClient client(srv.port());
    client.send_request("/sse-consume-after-end-event", true);

    std::string headers = client.read_headers();
    CHECK(headers.find("200 OK") != std::string::npos);
    CHECK(headers.find("text/event-stream") != std::string::npos);

    std::string chunk1 = client.read_chunk();
    CHECK_EQ(chunk1, "id: 42\nevent: delta\ndata: pre-end-event\n\n");

    std::string term = client.read_chunk();
    CHECK_EQ(term, "");

    auto ready2 = handler_done_fut.wait_for(std::chrono::seconds(3));
    if (ready2 != std::future_status::ready) {
        queue->close();
        (void)handler_done_fut.wait_for(std::chrono::seconds(1));
    }
    REQUIRE(ready2 == std::future_status::ready);
    CHECK(queue->closed());

    char extra[16] = {0};
    ssize_t n = ::recv(client.fd(), extra, sizeof(extra), 0);
    CHECK_EQ(n, 0);

    srv.server().stop();
}

TEST_CASE("SSE consume after Context end_chunk preserves keep-alive and framing") {
    CoreTestServer srv;
    auto handler_done = std::make_shared<std::promise<void>>();
    auto handler_done_fut = handler_done->get_future();
    auto queue = std::make_shared<coke::Queue<std::string>>(10);

    srv->GET("/sse-stream-reuse", [handler_done, queue](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        co_await sse.send("live");
        co_await ctx.end_chunk();
        co_await sse.consume(queue);
        handler_done->set_value();
    });

    srv->GET("/after-stream-reuse-ping", [](kale::Context &ctx) {
        ctx.string("pong");
    });

    RawStreamClient client(srv.port());
    client.send_request("/sse-stream-reuse", false);

    std::string headers = client.read_headers();
    CHECK(headers.find("200 OK") != std::string::npos);

    std::string chunk1 = client.read_chunk();
    CHECK_EQ(chunk1, "data: live\n\n");

    std::string term = client.read_chunk();
    CHECK_EQ(term, "");

    auto ready3 = handler_done_fut.wait_for(std::chrono::seconds(3));
    if (ready3 != std::future_status::ready) {
        queue->close();
        (void)handler_done_fut.wait_for(std::chrono::seconds(1));
    }
    REQUIRE(ready3 == std::future_status::ready);
    CHECK(queue->closed());

    client.send_request("/after-stream-reuse-ping", true);
    std::string headers2 = client.read_headers();
    CHECK(headers2.find("200 OK") != std::string::npos);
    std::string body2 = client.read_body_until_close(headers2);
    CHECK_EQ(body2, "pong");

    srv.server().stop();
}

TEST_CASE("SSE consume after Context abort_stream terminates cleanly without hanging") {
    CoreTestServer srv;
    auto handler_done = std::make_shared<std::promise<void>>();
    auto handler_done_fut = handler_done->get_future();
    auto queue = std::make_shared<coke::Queue<std::string>>(10);

    srv->GET("/sse-consume-after-abort", [handler_done, queue](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        co_await sse.send("pre-abort");
        ctx.abort_stream();

        co_await sse.consume(queue);
        handler_done->set_value();
    });

    RawStreamClient client(srv.port());
    client.send_request("/sse-consume-after-abort", true);

    std::string headers = client.read_headers();
    CHECK(headers.find("200 OK") != std::string::npos);

    std::string chunk1 = client.read_chunk();
    CHECK_EQ(chunk1, "data: pre-abort\n\n");

    auto ready4 = handler_done_fut.wait_for(std::chrono::seconds(3));
    if (ready4 != std::future_status::ready) {
        queue->close();
        (void)handler_done_fut.wait_for(std::chrono::seconds(1));
    }
    REQUIRE(ready4 == std::future_status::ready);
    CHECK(queue->closed());

    srv.server().stop();
}


TEST_CASE("Compression middleware: min_length threshold configuration behavior") {
    kale::CompressConfig config;
    config.set_min_length(10000);

    CoreTestServer server;
    server.server().use(kale::compression(config));

    std::string payload_2k(2000, 'A');
    std::string payload_12k(12000, 'B');

    server->GET("/test_compress_threshold_below", [payload_2k](kale::Context &ctx) {
        ctx.set_content_type("text/plain");
        ctx.string(payload_2k);
    });

    server->GET("/test_compress_threshold_above", [payload_12k](kale::Context &ctx) {
        ctx.set_content_type("text/plain");
        ctx.string(payload_12k);
    });

    auto res_below = server.get("/test_compress_threshold_below", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(res_below.status, 200);
    CHECK_FALSE(res_below.has_header("Content-Encoding"));
    CHECK_EQ(res_below.body, payload_2k);

    auto res_above = server.get("/test_compress_threshold_above", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(res_above.status, 200);
    CHECK_EQ(res_above.header("Content-Encoding"), "gzip");
    std::string decompressed;
    REQUIRE(kale::Compress::ungzip(res_above.body, decompressed));
    CHECK_EQ(decompressed, payload_12k);
}

TEST_CASE("Compression middleware: outermost ordering compresses post-processed body and headers") {
    CoreTestServer server;
    kale::CompressConfig config;
    config.set_min_length(50);
    server.server().use(kale::compression(config));

    server.server().use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        co_await next();
        ctx.set_header("X-Inner-Middleware", "post-processed");
        std::string current;
        ctx.get_resp_body_merged(current);
        ctx.string(current + " [modified by inner middleware]");
    });

    server->GET("/inner_alter", [](kale::Context &ctx) {
        ctx.set_content_type("text/plain");
        std::string base(200, 'A');
        ctx.string(base);
    });

    auto res = server.get("/inner_alter", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.header("X-Inner-Middleware"), "post-processed");
    CHECK_EQ(res.header("Content-Encoding"), "gzip");
    CHECK(res.header("Vary").find("Accept-Encoding") != std::string::npos);

    std::string decompressed;
    REQUIRE(kale::Compress::ungzip(res.body, decompressed));
    std::string expected = std::string(200, 'A') + " [modified by inner middleware]";
    CHECK_EQ(decompressed, expected);
}

TEST_CASE("Compression middleware: 404 and 405 error responses are compressed") {
    CoreTestServer server;
    kale::CompressConfig config;
    config.set_min_length(100);
    server.server().use(kale::compression(config));

    server.server().set_not_found_handler([](kale::Context &ctx) {
        ctx.set_status(kale::HttpStatus::NotFound);
        ctx.set_content_type("text/plain");
        ctx.string(std::string(300, '4'));
    });

    server.server().set_method_not_allowed_handler([](kale::Context &ctx) {
        ctx.set_status(kale::HttpStatus::MethodNotAllowed);
        ctx.set_content_type("text/plain");
        ctx.string(std::string(300, '5'));
    });

    server->POST("/only_post", [](kale::Context &ctx) {
        ctx.string("ok");
    });

    auto res_404 = server.get("/non_existent_route", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(res_404.status, 404);
    CHECK_EQ(res_404.header("Content-Encoding"), "gzip");
    std::string dec_404;
    REQUIRE(kale::Compress::ungzip(res_404.body, dec_404));
    CHECK_EQ(dec_404, std::string(300, '4'));

    auto res_405 = server.get("/only_post", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(res_405.status, 405);
    CHECK_EQ(res_405.header("Content-Encoding"), "gzip");
    std::string dec_405;
    REQUIRE(kale::Compress::ungzip(res_405.body, dec_405));
    CHECK_EQ(dec_405, std::string(300, '5'));
}

TEST_CASE("Compression middleware: committed streams and replied responses remain untouched") {
    CoreTestServer server;
    kale::CompressConfig config;
    config.set_min_length(50);
    server.server().use(kale::compression(config));

    server->GET("/stream_endpoint", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_content_type("text/plain");
        co_await ctx.send_chunk(std::string(200, 'S'));
        co_await ctx.end_chunk();
    });

    server->GET("/replied_endpoint", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_content_type("text/plain");
        ctx.string(std::string(200, 'R'));
        co_await ctx.reply();
    });

    auto res_stream = server.get("/stream_endpoint", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(res_stream.status, 200);
    CHECK_FALSE(res_stream.has_header("Content-Encoding"));
    CHECK_EQ(res_stream.body, std::string(200, 'S'));

    auto res_replied = server.get("/replied_endpoint", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(res_replied.status, 200);
    CHECK_FALSE(res_replied.has_header("Content-Encoding"));
    CHECK_EQ(res_replied.body, std::string(200, 'R'));
}

TEST_CASE("Compression middleware: uncaught handler exceptions produce uncompressed error response") {
    CoreTestServer server;
    kale::CompressConfig config;
    config.set_min_length(50);
    server.server().use(kale::compression(config));

    server->GET("/throw_std_exception", [](kale::Context &ctx) {
        (void)ctx;
        throw std::runtime_error("simulated handler crash with long message: " + std::string(200, 'E'));
    });

    server->GET("/throw_http_error", [](kale::Context &ctx) {
        (void)ctx;
        throw kale::HttpError(kale::HttpStatus::InternalServerError, "server failure: " + std::string(200, 'F'));
    });

    auto res1 = server.get("/throw_std_exception", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(res1.status, 500);
    CHECK_FALSE(res1.has_header("Content-Encoding"));

    auto res2 = server.get("/throw_http_error", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(res2.status, 500);
    CHECK_FALSE(res2.has_header("Content-Encoding"));
}

TEST_CASE("Compression middleware: pre-encoded and Content-Range responses are skipped") {
    CoreTestServer server;
    kale::CompressConfig config;
    config.set_min_length(50);
    server.server().use(kale::compression(config));

    const std::string raw_pre_encoded = "pre-encoded-binary-payload-data-" + std::string(200, 'P');
    server->GET("/pre_encoded", [raw_pre_encoded](kale::Context &ctx) {
        ctx.set_content_type("application/json");
        ctx.set_header("Content-Encoding", "br");
        ctx.string(raw_pre_encoded);
    });

    const std::string partial_content = "bytes 0-199/500 " + std::string(200, 'C');
    server->GET("/partial_range", [partial_content](kale::Context &ctx) {
        ctx.set_status(kale::HttpStatus::PartialContent);
        ctx.set_header("Content-Range", "bytes 0-199/500");
        ctx.set_content_type("text/plain");
        ctx.string(partial_content);
    });

    auto res_enc = server.get("/pre_encoded", {{"Accept-Encoding", "gzip, zstd"}});
    CHECK_EQ(res_enc.status, 200);
    CHECK_EQ(res_enc.header("Content-Encoding"), "br");
    CHECK_EQ(res_enc.body, raw_pre_encoded);

    auto res_range = server.get("/partial_range", {{"Accept-Encoding", "gzip, zstd"}});
    CHECK_EQ(res_range.status, 206);
    CHECK_FALSE(res_range.has_header("Content-Encoding"));
    CHECK_EQ(res_range.header("Content-Range"), "bytes 0-199/500");
    CHECK_EQ(res_range.body, partial_content);
}

TEST_CASE("Compression middleware: Vary case-insensitivity and token merging") {
    CoreTestServer server;
    kale::CompressConfig config;
    config.set_min_length(50);
    server.server().use(kale::compression(config));

    std::string payload(200, 'V');

    server->GET("/vary_none", [payload](kale::Context &ctx) {
        ctx.set_content_type("text/plain");
        ctx.string(payload);
    });

    server->GET("/vary_lower", [payload](kale::Context &ctx) {
        ctx.set_header("Vary", "accept-encoding");
        ctx.set_content_type("text/plain");
        ctx.string(payload);
    });

    server->GET("/vary_other", [payload](kale::Context &ctx) {
        ctx.set_header("Vary", "Origin, User-Agent");
        ctx.set_content_type("text/plain");
        ctx.string(payload);
    });

    server->GET("/vary_mixed", [payload](kale::Context &ctx) {
        ctx.set_header("Vary", "Origin, accept-encoding, X-Custom");
        ctx.set_content_type("text/plain");
        ctx.string(payload);
    });

    server->GET("/vary_wildcard", [payload](kale::Context &ctx) {
        ctx.set_header("Vary", "*");
        ctx.set_content_type("text/plain");
        ctx.string(payload);
    });

    auto r1 = server.get("/vary_none", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(r1.header("Content-Encoding"), "gzip");
    CHECK_EQ(r1.header("Vary"), "Accept-Encoding");

    auto r2 = server.get("/vary_lower", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(r2.header("Content-Encoding"), "gzip");
    CHECK_EQ(r2.header("Vary"), "accept-encoding");

    auto r3 = server.get("/vary_other", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(r3.header("Content-Encoding"), "gzip");
    CHECK_EQ(r3.header("Vary"), "Origin, User-Agent, Accept-Encoding");

    auto r4 = server.get("/vary_mixed", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(r4.header("Content-Encoding"), "gzip");
    CHECK_EQ(r4.header("Vary"), "Origin, accept-encoding, X-Custom");

    auto r5 = server.get("/vary_wildcard", {{"Accept-Encoding", "gzip"}});
    CHECK_EQ(r5.header("Content-Encoding"), "gzip");
    CHECK_EQ(r5.header("Vary"), "*");
}

TEST_CASE("HttpServer without compression: retains direct dispatch and leaves body uncompressed") {
    CoreTestServer server;
    std::atomic<int> handler_invoked{0};

    std::string large_payload(5000, 'U');
    server->GET("/uninstalled_compress", [large_payload, &handler_invoked](kale::Context &ctx) {
        handler_invoked.fetch_add(1, std::memory_order_relaxed);
        ctx.set_content_type("text/plain");
        ctx.string(large_payload);
    });

    auto res = server.get("/uninstalled_compress", {{"Accept-Encoding", "gzip, zstd"}});
    CHECK_EQ(res.status, 200);
    CHECK_EQ(handler_invoked.load(), 1);
    CHECK_FALSE(res.has_header("Content-Encoding"));
    CHECK_EQ(res.body, large_payload);
}

TEST_CASE("HttpServer: chunked body visible to 404 middleware and fallback") {
    CoreTestServer server;
    std::string mw_observed_body;
    std::string fallback_observed_body;

    server.server().use([&](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        mw_observed_body = std::string(ctx.req_body());
        co_await next();
    });

    server.server().set_not_found_handler([&](kale::Context &ctx) {
        fallback_observed_body = std::string(ctx.req_body());
        ctx.set_status(kale::HttpStatus::NotFound);
        ctx.string("custom_404_handled");
    });

    // Send chunked request to unmatched route
    mw_observed_body.clear();
    fallback_observed_body.clear();
    auto chunked_resp = send_raw_http(server.port(),
        "POST /unmatched_chunked_target HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n"
        "f\r\nchunked_404_msg\r\n"
        "0\r\n\r\n");

    CHECK_EQ(chunked_resp.status, 404);
    // If prepare_chunked_body_if_needed() is not called on unmatched/fallback path,
    // mw_observed_body and fallback_observed_body remain empty.
    CHECK_EQ(mw_observed_body, "chunked_404_msg");
    CHECK_EQ(fallback_observed_body, "chunked_404_msg");
}

TEST_CASE("HttpError hides diagnostic details and emits standard reason phrase") {
    CoreTestServer server;

    server->GET("/error_not_found", [](kale::Context &) {
        throw kale::HttpError(404, "confidential internal database ID 48102 not found");
    });

    auto res = server.get("/error_not_found");
    CHECK_EQ(res.status, 404);
    CHECK_EQ(res.body, "Not Found");
    CHECK_EQ(res.body.find("confidential"), std::string::npos);

    auto raw_res = send_raw_http(server.port(),
        "GET /error_not_found HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(raw_res.status, 404);
    CHECK(raw_res.status_line.starts_with("HTTP/1.1 404 Not Found"));
    CHECK_EQ(raw_res.body, "Not Found");
    CHECK_EQ(raw_res.body.find("confidential"), std::string::npos);
}

TEST_CASE("HttpError after stream commitment aborts with no second response") {
    CoreTestServer server;

    server->GET("/stream_error_postcommit", [](kale::Context &ctx) -> coke::Task<void> {
        bool ok = co_await ctx.send_chunk("stream_header_committed;");
        CHECK(ok);
        CHECK(ctx.is_stream());
        throw kale::HttpError(400, "late_input_error_after_commit");
    });

    auto raw = send_raw_http(server.port(),
        "GET /stream_error_postcommit HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");

    CHECK_EQ(raw.status, 200);
    CHECK(raw.body.find("stream_header_committed;") != std::string::npos);
    CHECK(raw.body.find("Bad Request") == std::string::npos);
    CHECK(raw.body.find("late_input_error_after_commit") == std::string::npos);
    CHECK_EQ(raw.body.find("HTTP/1.1"), std::string::npos);
}

TEST_CASE("Malformed Context parse_json maps to standard 400 Bad Request") {
    CoreTestServer server;

    server->POST("/api/parse_json_test", [](kale::Context &ctx) {
        auto val = ctx.parse_json();
        ctx.string("parsed_ok");
    });

    // 1. Malformed JSON payload over client abstraction
    auto res = server.post("/api/parse_json_test", "{not_valid_json: 123", {{"Content-Type", "application/json"}});
    CHECK_EQ(res.status, 400);
    CHECK_EQ(res.body, "Bad Request");
    CHECK_EQ(res.body.size(), 11);

    // 2. Wire check verifying framing and reason phrase
    auto wire_res = send_raw_http(server.port(),
        "POST /api/parse_json_test HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: 20\r\n"
        "Connection: close\r\n\r\n"
        "{not_valid_json: 123");
    CHECK_EQ(wire_res.status, 400);
    CHECK(wire_res.status_line.starts_with("HTTP/1.1 400 Bad Request"));
    CHECK_EQ(wire_res.body, "Bad Request");
    CHECK_EQ(wire_res.body.size(), 11);
    CHECK_NE(wire_res.headers.find("Content-Length: 11"), std::string::npos);
}

TEST_CASE("Generic dump-able sync and async handlers serialize once") {
    struct CountingDumpable {
        int *dump_count{nullptr};
        std::string payload{"{\"message\":\"dumped\"}"};

        std::string dump() const {
            if (dump_count) {
                ++(*dump_count);
            }
            return payload;
        }
    };

    CoreTestServer server;
    int sync_dumps = 0;
    int async_dumps = 0;

    // 1. Synchronous generic dump-able handler with Context&
    server->GET("/generic_sync_ctx", [&sync_dumps](kale::Context &) {
        return CountingDumpable{.dump_count = &sync_dumps, .payload = "{\"type\":\"sync_ctx\"}"};
    });

    // 2. Asynchronous generic dump-able handler with Context&
    server->GET("/generic_async_ctx", [&async_dumps](kale::Context &) -> coke::Task<CountingDumpable> {
        co_return CountingDumpable{.dump_count = &async_dumps, .payload = "{\"type\":\"async_ctx\"}"};
    });

    // Verify sync handler execution and single dump serialization
    auto res_sync = server.get("/generic_sync_ctx");
    CHECK_EQ(res_sync.status, 200);
    CHECK_EQ(res_sync.body, "{\"type\":\"sync_ctx\"}");
    CHECK_EQ(res_sync.header("Content-Type"), "application/json; charset=utf-8");
    CHECK_EQ(sync_dumps, 1);

    // Verify async handler execution and single dump serialization
    auto res_async = server.get("/generic_async_ctx");
    CHECK_EQ(res_async.status, 200);
    CHECK_EQ(res_async.body, "{\"type\":\"async_ctx\"}");
    CHECK_EQ(res_async.header("Content-Type"), "application/json; charset=utf-8");
    CHECK_EQ(async_dumps, 1);
}

TEST_CASE("Deterministic SSE precommit terminal Context and repeated-send exception") {
    struct ProbeSync {
        std::promise<void> completion_promise;
        std::atomic<bool> first_send_false{false};
        std::atomic<bool> uncaught_thrown{false};
        std::atomic<bool> ctx_terminal_replied{false};
        std::atomic<bool> ctx_is_stream{false};
    };

    struct HandlerGuard {
        ProbeSync &sync;
        kale::Context &ctx;

        ~HandlerGuard() {
            sync.ctx_terminal_replied.store(ctx.is_replied());
            sync.ctx_is_stream.store(ctx.is_stream());
            try {
                sync.completion_promise.set_value();
            } catch (...) {}
        }
    };

    auto sync = std::make_shared<ProbeSync>();
    CoreTestServer server;

    server->GET("/probe/precommit_abort", [sync](kale::Context &ctx) -> coke::Task<void> {
        HandlerGuard guard{.sync = *sync, .ctx = ctx};

        ctx.abort_stream();
        kale::SseEmitter sse(ctx);

        bool ok1 = co_await sse.send("event1");
        if (!ok1) {
            sync->first_send_false.store(true);
        }

        sync->uncaught_thrown.store(true);
        co_await sse.send("event2_will_throw");
    });

    auto fut = sync->completion_promise.get_future();
    std::string wire_resp;
    {
        RawResponse raw = send_raw_http(server.port(),
            "GET /probe/precommit_abort HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n");
        wire_resp = raw.headers + "\r\n\r\n" + raw.body;
    }

    REQUIRE_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    CHECK(sync->first_send_false.load());
    CHECK(sync->uncaught_thrown.load());
    CHECK(sync->ctx_terminal_replied.load());
    CHECK_EQ(wire_resp.find("500 Internal Server Error"), std::string::npos);
    CHECK_EQ(wire_resp.find("HTTP/1.1 500"), std::string::npos);
}

TEST_CASE("Deterministic SSE postcommit fixed-length overflow and abort") {
    struct ProbeSync {
        std::promise<void> completion_promise;
        std::atomic<bool> step1_committed{false};
        std::atomic<bool> step2_overflow_false{false};
        std::atomic<bool> uncaught_thrown{false};
        std::atomic<bool> ctx_terminal_replied{false};
    };

    struct HandlerGuard {
        ProbeSync &sync;
        kale::Context &ctx;

        ~HandlerGuard() {
            sync.ctx_terminal_replied.store(ctx.is_replied());
            try {
                sync.completion_promise.set_value();
            } catch (...) {}
        }
    };

    const std::string event1_payload = "fixed_event_1";
    const size_t event1_wire_len = std::string("data: " + event1_payload + "\n\n").size();

    auto sync = std::make_shared<ProbeSync>();
    CoreTestServer server;

    server->GET("/probe/fixed_length_overflow", [sync, event1_payload, event1_wire_len](kale::Context &ctx) -> coke::Task<void> {
        HandlerGuard guard{.sync = *sync, .ctx = ctx};

        ctx.set_header("Content-Length", std::to_string(event1_wire_len));
        kale::SseEmitter sse(ctx);

        bool ok1 = co_await sse.send(event1_payload);
        if (ok1) {
            sync->step1_committed.store(true);
        }

        bool ok2 = co_await sse.send("overflow_payload");
        if (!ok2) {
            sync->step2_overflow_false.store(true);
        }

        sync->uncaught_thrown.store(true);
        co_await sse.send("event3_after_overflow_will_throw");
    });

    auto fut = sync->completion_promise.get_future();
    RawResponse raw = send_raw_http(server.port(),
        "GET /probe/fixed_length_overflow HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");

    REQUIRE_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    CHECK(sync->step1_committed.load());
    CHECK(sync->step2_overflow_false.load());
    CHECK(sync->uncaught_thrown.load());
    CHECK(sync->ctx_terminal_replied.load());

    CHECK_EQ(raw.status, 200);
    CHECK(raw.body.find("data: fixed_event_1\n\n") != std::string::npos);
    CHECK_EQ(raw.body.find("500 Internal Server Error"), std::string::npos);
    CHECK_EQ(raw.body.find("HTTP/1.1 500"), std::string::npos);
}
} // TEST_SUITE("core")
