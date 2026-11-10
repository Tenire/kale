#include "doctest/doctest.h"

#include <atomic>
#include <charconv>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "coke/http/http_client.h"
#include "coke/task.h"
#include "coke/wait.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/logger.h"
#include "kale/core/router.h"
#include "kale/core/http_status.h"
#include "kale/http/json.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

namespace {

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
    std::string url,
    std::string method = "GET",
    HeaderList headers = {},
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
    co_return TestResponse::from_coke(res);
}

class FastPathTestServer {
public:
    explicit FastPathTestServer(kale::HttpServerParams params = {})
        : server_(params) {
        server_.set_logger(kale::Logger::off());
        int ret = server_.start("127.0.0.1", 0);
        REQUIRE_EQ(ret, 0);
        REQUIRE(server_.is_running());
        port_ = static_cast<unsigned short>(server_.listen_port());
        REQUIRE_GT(port_, 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(port_);
    }

    ~FastPathTestServer() {
        server_.stop();
    }

    FastPathTestServer(const FastPathTestServer &) = delete;
    FastPathTestServer &operator=(const FastPathTestServer &) = delete;

    kale::HttpServer *operator->() noexcept { return &server_; }
    kale::HttpServer &server() noexcept { return server_; }
    unsigned short port() const noexcept { return port_; }

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
        return coke::sync_wait(
            do_request(url(path), std::move(method), std::move(headers), std::move(body))
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

struct CustomDumpable {
    int code{0};
    std::string msg;

    std::string dump() const {
        return "{\"code\":" + std::to_string(code) + ",\"msg\":\"" + msg + "\"}";
    }
};

struct RawWireResponse {
    int status{0};
    std::string status_line;
    std::string headers;
    std::string body;

    std::string header(std::string_view name) const {
        std::string search = "\r\n" + std::string(name) + ":";
        auto pos = headers.find(search);
        if (pos == std::string::npos) {
            if (headers.starts_with(std::string(name) + ":")) {
                pos = 0;
            } else {
                return "";
            }
        } else {
            pos += 2;
        }
        auto colon = headers.find(':', pos);
        if (colon == std::string::npos) return "";
        auto val_start = headers.find_first_not_of(" \t", colon + 1);
        auto val_end = headers.find("\r\n", val_start);
        return headers.substr(val_start, val_end != std::string::npos ? val_end - val_start : std::string::npos);
    }
};

struct FdGuard {
    int fd{-1};
    FdGuard() = default;
    explicit FdGuard(int f) noexcept : fd(f) {}
    ~FdGuard() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
    FdGuard(FdGuard &&o) noexcept : fd(o.fd) { o.fd = -1; }
    FdGuard &operator=(FdGuard &&o) noexcept {
        if (this != &o) {
            if (fd >= 0) ::close(fd);
            fd = o.fd;
            o.fd = -1;
        }
        return *this;
    }
    int get() const noexcept { return fd; }
    operator int() const noexcept { return fd; }
};

inline RawWireResponse send_raw_wire_http(unsigned short port, std::string_view request) {
    int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE_GE(raw_fd, 0);
    FdGuard fd(raw_fd);
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

inline void verify_keep_alive_recovery(unsigned short port, std::string_view req1, std::string_view req2,
                                       std::string &res1_body, std::string &res2_body,
                                       int &res1_status, int &res2_status) {
    int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE_GE(raw_fd, 0);
    FdGuard fd(raw_fd);

    struct timeval tv{.tv_sec = 3, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
    REQUIRE_EQ(ret, 0);

    size_t sent1 = 0;
    while (sent1 < req1.size()) {
        ssize_t n = ::send(fd, req1.data() + sent1, req1.size() - sent1, 0);
        REQUIRE_GT(n, 0);
        sent1 += static_cast<size_t>(n);
    }

    std::string resp1_data;
    char c;
    while (resp1_data.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(fd, &c, 1, 0);
        REQUIRE_GT(n, 0);
        resp1_data.push_back(c);
    }
    size_t h1_end = resp1_data.find("\r\n\r\n");
    std::string h1 = resp1_data.substr(0, h1_end);

    size_t sp1 = h1.find(' ');
    size_t sp2 = (sp1 != std::string::npos) ? h1.find(' ', sp1 + 1) : std::string::npos;
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
        std::string s_str = h1.substr(sp1 + 1, sp2 - (sp1 + 1));
        std::from_chars(s_str.data(), s_str.data() + s_str.size(), res1_status);
    }

    size_t cl1 = 0;
    std::string cl_search = "\r\nContent-Length:";
    auto cl_pos = h1.find(cl_search);
    if (cl_pos == std::string::npos && h1.starts_with("Content-Length:")) {
        cl_pos = 0;
    } else if (cl_pos != std::string::npos) {
        cl_pos += 2;
    }
    REQUIRE_NE(cl_pos, std::string::npos);
    auto colon1 = h1.find(':', cl_pos);
    auto v_start1 = h1.find_first_not_of(" \t", colon1 + 1);
    auto v_end1 = h1.find("\r\n", v_start1);
    std::string cl_str = h1.substr(v_start1, v_end1 - v_start1);
    std::from_chars(cl_str.data(), cl_str.data() + cl_str.size(), cl1);

    res1_body.resize(cl1);
    size_t b_read = 0;
    while (b_read < cl1) {
        ssize_t n = ::recv(fd, res1_body.data() + b_read, cl1 - b_read, 0);
        REQUIRE_GT(n, 0);
        b_read += static_cast<size_t>(n);
    }

    size_t sent2 = 0;
    while (sent2 < req2.size()) {
        ssize_t n = ::send(fd, req2.data() + sent2, req2.size() - sent2, 0);
        REQUIRE_GT(n, 0);
        sent2 += static_cast<size_t>(n);
    }

    std::string resp2_data;
    while (resp2_data.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(fd, &c, 1, 0);
        REQUIRE_GT(n, 0);
        resp2_data.push_back(c);
    }
    size_t h2_end = resp2_data.find("\r\n\r\n");
    std::string h2 = resp2_data.substr(0, h2_end);

    size_t sp2_1 = h2.find(' ');
    size_t sp2_2 = (sp2_1 != std::string::npos) ? h2.find(' ', sp2_1 + 1) : std::string::npos;
    if (sp2_1 != std::string::npos && sp2_2 != std::string::npos) {
        std::string s2_str = h2.substr(sp2_1 + 1, sp2_2 - (sp2_1 + 1));
        std::from_chars(s2_str.data(), s2_str.data() + s2_str.size(), res2_status);
    }

    size_t cl2 = 0;
    auto cl2_pos = h2.find(cl_search);
    if (cl2_pos == std::string::npos && h2.starts_with("Content-Length:")) {
        cl2_pos = 0;
    } else if (cl2_pos != std::string::npos) {
        cl2_pos += 2;
    }
    if (cl2_pos != std::string::npos) {
        auto colon2 = h2.find(':', cl2_pos);
        auto v_start2 = h2.find_first_not_of(" \t", colon2 + 1);
        auto v_end2 = h2.find("\r\n", v_start2);
        std::string cl2_str = h2.substr(v_start2, v_end2 - v_start2);
        std::from_chars(cl2_str.data(), cl2_str.data() + cl2_str.size(), cl2);
    }

    res2_body.resize(cl2);
    size_t b2_read = 0;
    while (b2_read < cl2) {
        ssize_t n = ::recv(fd, res2_body.data() + b2_read, cl2 - b2_read, 0);
        REQUIRE_GT(n, 0);
        b2_read += static_cast<size_t>(n);
    }

}

} // namespace

TEST_SUITE("core") {

TEST_CASE("Dispatch Fast Path: Handler empty/null semantics and invocation safety") {
    // Default construction represents an empty handler
    kale::Handler h_default;
    CHECK_FALSE(bool(h_default));
    CHECK_FALSE(h_default.is_sync());
    CHECK_FALSE(h_default.is_async());

    // nullptr constructor
    kale::Handler h_null = nullptr;
    CHECK_FALSE(bool(h_null));

    // Empty std::function construction normalizes to empty
    kale::Handler::SyncFn empty_sync_fn;
    kale::Handler h_empty_sync(empty_sync_fn);
    CHECK_FALSE(bool(h_empty_sync));
    CHECK_FALSE(h_empty_sync.is_sync());

    kale::Handler::AsyncFn empty_async_fn;
    kale::Handler h_empty_async(empty_async_fn);
    CHECK_FALSE(bool(h_empty_async));
    CHECK_FALSE(h_empty_async.is_async());

    // Factories normalize empty targets
    CHECK_FALSE(bool(kale::Handler::from_sync({})));
    CHECK_FALSE(bool(kale::Handler::from_async({})));

    // Non-empty sync handler
    auto h_sync = kale::make_handler([](kale::Context &ctx) { ctx.string("ok"); });
    CHECK(bool(h_sync));
    CHECK(h_sync.is_sync());
    CHECK_FALSE(h_sync.is_async());

    // Non-empty async handler
    auto h_async = kale::make_handler([](kale::Context &) -> coke::Task<void> { co_return; });
    CHECK(bool(h_async));
    CHECK(h_async.is_async());
    CHECK_FALSE(h_async.is_sync());

    // Copy and move preserve validity and classification
    kale::Handler h_copy = h_sync;
    CHECK(bool(h_copy));
    CHECK(h_copy.is_sync());
    kale::Handler h_move = std::move(h_copy);
    CHECK(bool(h_move));
    CHECK(h_move.is_sync());

    // Invocation on empty handler throws std::bad_function_call with a live Context
    FastPathTestServer server;
    server->GET("/test_empty_handler", [](kale::Context &ctx) {
        kale::Handler empty_h;
        CHECK_THROWS_AS(empty_h.invoke_sync(ctx), std::bad_function_call);
        CHECK_THROWS_AS(empty_h.invoke_async(ctx), std::bad_function_call);
        ctx.string("empty_checked");
    });
    auto res_empty = server.get("/test_empty_handler");
    CHECK_EQ(res_empty.status, 200);
    CHECK_EQ(res_empty.body, "empty_checked");

    server->GET("/test_empty_async_bridge", [](kale::Context &ctx) -> coke::Task<void> {
        kale::Handler empty_h;
        bool caught = false;
        try {
            co_await empty_h(ctx);
        } catch (const std::bad_function_call &) {
            caught = true;
        }
        CHECK(caught);
        ctx.string("bridge_checked");
    });
    auto res_bridge = server.get("/test_empty_async_bridge");
    CHECK_EQ(res_bridge.status, 200);
    CHECK_EQ(res_bridge.body, "bridge_checked");
}

TEST_CASE("Dispatch Fast Path: genuine synchronous invocation without coroutine wrapper") {
    FastPathTestServer server;
    std::atomic<int> sync_invocations{0};

    server->GET("/fast_sync", [&sync_invocations](kale::Context &ctx) {
        sync_invocations.fetch_add(1, std::memory_order_relaxed);
        ctx.string("fast_sync_ok");
    });

    auto res = server.get("/fast_sync");
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, "fast_sync_ok");
    CHECK_EQ(sync_invocations.load(), 1);

    // Call again to verify exact single invocation per request
    auto res2 = server.get("/fast_sync");
    CHECK_EQ(res2.status, 200);
    CHECK_EQ(res2.body, "fast_sync_ok");
    CHECK_EQ(sync_invocations.load(), 2);
}

TEST_CASE("Dispatch Fast Path: synchronous return value adaptation without copies") {
    FastPathTestServer server;

    // 1. void return with Context&
    server->GET("/ret_void_ctx", [](kale::Context &ctx) {
        ctx.set_header("X-Custom", "present");
        ctx.string("void_ctx");
    });
    auto res_void_ctx = server.get("/ret_void_ctx");
    CHECK_EQ(res_void_ctx.status, 200);
    CHECK_EQ(res_void_ctx.header("X-Custom"), "present");
    CHECK_EQ(res_void_ctx.body, "void_ctx");

    // 2. void return with no args
    server->GET("/ret_void_noarg", []() {});
    auto res_void_noarg = server.get("/ret_void_noarg");
    CHECK_EQ(res_void_noarg.status, 200);

    // 3. std::string return with Context&
    server->GET("/ret_str_ctx/:param", [](kale::Context &ctx) -> std::string {
        return "hello_" + std::string(ctx.param("param"));
    });
    auto res_str_ctx = server.get("/ret_str_ctx/world");
    CHECK_EQ(res_str_ctx.status, 200);
    CHECK_EQ(res_str_ctx.body, "hello_world");

    // 4. std::string return with no args
    server->GET("/ret_str_noarg", []() -> std::string {
        return "plain_string_noarg";
    });
    auto res_str_noarg = server.get("/ret_str_noarg");
    CHECK_EQ(res_str_noarg.status, 200);
    CHECK_EQ(res_str_noarg.body, "plain_string_noarg");

    // 5. const char* return with Context& and no args
    server->GET("/ret_cstr_ctx", [](kale::Context &) -> const char* {
        return "const_char_ctx";
    });
    auto res_cstr_ctx = server.get("/ret_cstr_ctx");
    CHECK_EQ(res_cstr_ctx.status, 200);
    CHECK_EQ(res_cstr_ctx.body, "const_char_ctx");

    server->GET("/ret_cstr_noarg", []() -> const char* {
        return "const_char_noarg";
    });
    auto res_cstr_noarg = server.get("/ret_cstr_noarg");
    CHECK_EQ(res_cstr_noarg.status, 200);
    CHECK_EQ(res_cstr_noarg.body, "const_char_noarg");

    // 6. std::string_view return without std::string copy
    static constexpr std::string_view STATIC_VIEW = "string_view_zero_copy_payload";
    server->GET("/ret_view_ctx", [](kale::Context &) -> std::string_view {
        return STATIC_VIEW;
    });
    auto res_view_ctx = server.get("/ret_view_ctx");
    CHECK_EQ(res_view_ctx.status, 200);
    CHECK_EQ(res_view_ctx.body, STATIC_VIEW);

    server->GET("/ret_view_noarg", []() -> std::string_view {
        return STATIC_VIEW;
    });
    auto res_view_noarg = server.get("/ret_view_noarg");
    CHECK_EQ(res_view_noarg.status, 200);
    CHECK_EQ(res_view_noarg.body, STATIC_VIEW);

    // 7. Json return with Context& and no args
    server->GET("/ret_json_ctx", [](kale::Context &) -> kale::Json {
        return kale::Json::parse("{\"status\":\"ok\",\"count\":100}");
    });
    auto res_json_ctx = server.get("/ret_json_ctx");
    CHECK_EQ(res_json_ctx.status, 200);
    CHECK_EQ(res_json_ctx.json()["status"].get<std::string>(), "ok");
    CHECK_EQ(res_json_ctx.json()["count"].get<int>(), 100);

    server->GET("/ret_json_noarg", []() -> kale::Json {
        return kale::Json::parse("{\"noarg\":true}");
    });
    auto res_json_noarg = server.get("/ret_json_noarg");
    CHECK_EQ(res_json_noarg.status, 200);
    CHECK(res_json_noarg.json()["noarg"].get<bool>());

    // 8. Generic dumpable object
    server->GET("/ret_dump_ctx", [](kale::Context &) -> CustomDumpable {
        return CustomDumpable{42, "custom_ctx"};
    });
    auto res_dump_ctx = server.get("/ret_dump_ctx");
    CHECK_EQ(res_dump_ctx.status, 200);
    CHECK_EQ(res_dump_ctx.json()["code"].get<int>(), 42);
    CHECK_EQ(res_dump_ctx.json()["msg"].get<std::string>(), "custom_ctx");

    server->GET("/ret_dump_noarg", []() -> CustomDumpable {
        return CustomDumpable{99, "custom_noarg"};
    });
    auto res_dump_noarg = server.get("/ret_dump_noarg");
    CHECK_EQ(res_dump_noarg.status, 200);
    CHECK_EQ(res_dump_noarg.json()["code"].get<int>(), 99);
    CHECK_EQ(res_dump_noarg.json()["msg"].get<std::string>(), "custom_noarg");
}

TEST_CASE("Dispatch Fast Path: asynchronous coroutine task return values") {
    FastPathTestServer server;

    // Task<void>
    server->GET("/async_void_ctx", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.string("async_void_done");
        co_return;
    });
    auto res_void_ctx = server.get("/async_void_ctx");
    CHECK_EQ(res_void_ctx.status, 200);
    CHECK_EQ(res_void_ctx.body, "async_void_done");

    server->GET("/async_void_noarg", []() -> coke::Task<void> {
        co_return;
    });
    auto res_void_noarg = server.get("/async_void_noarg");
    CHECK_EQ(res_void_noarg.status, 200);

    // Task<std::string>
    server->GET("/async_str_ctx", [](kale::Context &) -> coke::Task<std::string> {
        co_return "async_string_ctx";
    });
    auto res_str_ctx = server.get("/async_str_ctx");
    CHECK_EQ(res_str_ctx.status, 200);
    CHECK_EQ(res_str_ctx.body, "async_string_ctx");

    server->GET("/async_str_noarg", []() -> coke::Task<std::string> {
        co_return "async_string_noarg";
    });
    auto res_str_noarg = server.get("/async_str_noarg");
    CHECK_EQ(res_str_noarg.status, 200);
    CHECK_EQ(res_str_noarg.body, "async_string_noarg");

    // Task<std::string_view>
    server->GET("/async_view", []() -> coke::Task<std::string_view> {
        co_return "async_view_result";
    });
    auto res_view = server.get("/async_view");
    CHECK_EQ(res_view.status, 200);
    CHECK_EQ(res_view.body, "async_view_result");

    // Task<const char*>
    server->GET("/async_cstr", []() -> coke::Task<const char*> {
        co_return "async_cstr_result";
    });
    auto res_cstr = server.get("/async_cstr");
    CHECK_EQ(res_cstr.status, 200);
    CHECK_EQ(res_cstr.body, "async_cstr_result");

    // Task<Json>
    server->GET("/async_json_ctx", [](kale::Context &) -> coke::Task<kale::Json> {
        co_return kale::Json::parse("{\"async_key\":\"async_val\"}");
    });
    auto res_json_ctx = server.get("/async_json_ctx");
    CHECK_EQ(res_json_ctx.status, 200);
    CHECK_EQ(res_json_ctx.json()["async_key"].get<std::string>(), "async_val");

    server->GET("/async_json_noarg", []() -> coke::Task<kale::Json> {
        co_return kale::Json::parse("{\"async_noarg_key\":777}");
    });
    auto res_json_noarg = server.get("/async_json_noarg");
    CHECK_EQ(res_json_noarg.status, 200);
    CHECK_EQ(res_json_noarg.json()["async_noarg_key"].get<int>(), 777);

    // Task<CustomDumpable>
    server->GET("/async_dump", []() -> coke::Task<CustomDumpable> {
        co_return CustomDumpable{555, "async_dump_payload"};
    });
    auto res_dump = server.get("/async_dump");
    CHECK_EQ(res_dump.status, 200);
    CHECK_EQ(res_dump.json()["code"].get<int>(), 555);
    CHECK_EQ(res_dump.json()["msg"].get<std::string>(), "async_dump_payload");
}

TEST_CASE("Dispatch Fast Path: onion middleware execution order with sync and async handlers") {
    FastPathTestServer server;
    std::vector<std::string> trace;

    // Global Middleware
    server.server().use([&trace](kale::Context &, kale::Next next) -> coke::Task<void> {
        trace.push_back("G_in");
        co_await next();
        trace.push_back("G_out");
    });

    // Route with route-level middleware + synchronous handler
    server->GET(
        "/onion_sync",
        [&trace](kale::Context &, kale::Next next) -> coke::Task<void> {
            trace.push_back("R_in");
            co_await next();
            trace.push_back("R_out");
        },
        [&trace](kale::Context &ctx) {
            trace.push_back("HAND_SYNC");
            ctx.string("onion_sync_res");
        }
    );

    trace.clear();
    auto res_sync = server.get("/onion_sync");
    CHECK_EQ(res_sync.status, 200);
    CHECK_EQ(res_sync.body, "onion_sync_res");
    REQUIRE_EQ(trace.size(), 5);
    CHECK_EQ(trace[0], "G_in");
    CHECK_EQ(trace[1], "R_in");
    CHECK_EQ(trace[2], "HAND_SYNC");
    CHECK_EQ(trace[3], "R_out");
    CHECK_EQ(trace[4], "G_out");

    // Route with route-level middleware + asynchronous handler
    server->GET(
        "/onion_async",
        [&trace](kale::Context &, kale::Next next) -> coke::Task<void> {
            trace.push_back("R_async_in");
            co_await next();
            trace.push_back("R_async_out");
        },
        [&trace](kale::Context &ctx) -> coke::Task<void> {
            trace.push_back("HAND_ASYNC");
            ctx.string("onion_async_res");
            co_return;
        }
    );

    trace.clear();
    auto res_async = server.get("/onion_async");
    CHECK_EQ(res_async.status, 200);
    CHECK_EQ(res_async.body, "onion_async_res");
    REQUIRE_EQ(trace.size(), 5);
    CHECK_EQ(trace[0], "G_in");
    CHECK_EQ(trace[1], "R_async_in");
    CHECK_EQ(trace[2], "HAND_ASYNC");
    CHECK_EQ(trace[3], "R_async_out");
    CHECK_EQ(trace[4], "G_out");

    // Next-once rule: calling next() a second time inside middleware throws std::logic_error
    server->GET(
        "/next_twice",
        [](kale::Context &, kale::Next next) -> coke::Task<void> {
            co_await next();
            co_await next(); // Must throw std::logic_error
        },
        [](kale::Context &ctx) {
            ctx.string("inner_ok");
        }
    );

    auto res_twice = server.get("/next_twice");
    CHECK_EQ(res_twice.status, 500);

    // Early reply boundary: middleware sets response and does NOT call next()
    std::atomic<int> uncalled_count{0};
    server->GET(
        "/early_reply",
        [](kale::Context &ctx, kale::Next) -> coke::Task<void> {
            ctx.set_status(kale::HttpStatus::Unauthorized);
            ctx.string("early_blocked");
            co_return;
        },
        [&uncalled_count](kale::Context &ctx) {
            uncalled_count.fetch_add(1);
            ctx.string("should_never_reach");
        }
    );

    auto res_early = server.get("/early_reply");
    CHECK_EQ(res_early.status, 401);
    CHECK_EQ(res_early.body, "early_blocked");
    CHECK_EQ(uncalled_count.load(), 0);
}

TEST_CASE("Dispatch Fast Path: exception boundaries and single invocation guarantee") {
    FastPathTestServer server;
    std::atomic<int> sync_err_count{0};
    std::atomic<int> async_err_count{0};
    std::atomic<int> json_err_count{0};

    // 1. Sync handler throws std::runtime_error
    server->GET("/throw_sync", [&sync_err_count](kale::Context &) {
        sync_err_count.fetch_add(1);
        throw std::runtime_error("sync handler crash");
    });

    auto res_sync = server.get("/throw_sync");
    CHECK_EQ(res_sync.status, 500);
    CHECK_EQ(res_sync.body, "Internal Server Error");
    CHECK_EQ(sync_err_count.load(), 1);

    // 2. Async handler throws std::runtime_error
    server->GET("/throw_async", [&async_err_count](kale::Context &) -> coke::Task<void> {
        async_err_count.fetch_add(1);
        throw std::runtime_error("async handler crash");
        co_return;
    });

    auto res_async = server.get("/throw_async");
    CHECK_EQ(res_async.status, 500);
    CHECK_EQ(res_async.body, "Internal Server Error");
    CHECK_EQ(async_err_count.load(), 1);

    // 3. Handler throws JsonInputError via ctx.parse_json()
    server->POST("/throw_json", [&json_err_count](kale::Context &ctx) {
        json_err_count.fetch_add(1);
        (void)ctx.parse_json();
    });

    auto res_json = server.post("/throw_json", "{malformed_json");
    CHECK_EQ(res_json.status, 400);
    CHECK_EQ(res_json.body, "Bad Request");
    CHECK_EQ(res_json.body.size(), 11);
    CHECK_EQ(json_err_count.load(), 1);

    // 4. Middleware throws before calling next()
    std::atomic<int> downstream_count{0};
    server->GET(
        "/mw_throw",
        [](kale::Context &, kale::Next) -> coke::Task<void> {
            throw std::runtime_error("middleware fault");
            co_return;
        },
        [&downstream_count](kale::Context &ctx) {
            downstream_count.fetch_add(1);
            ctx.string("ok");
        }
    );

    auto res_mw_throw = server.get("/mw_throw");
    CHECK_EQ(res_mw_throw.status, 500);
    CHECK_EQ(downstream_count.load(), 0);
}

TEST_CASE("Dispatch Fast Path: 404 and 405 with synchronous and asynchronous handlers") {
    // 1. Default 404 and 405 fast path without middlewares
    FastPathTestServer default_server;
    default_server->GET("/existing_route", [](kale::Context &ctx) {
        ctx.string("found");
    });

    auto res_404 = default_server.get("/non_existent_path");
    CHECK_EQ(res_404.status, 404);
    CHECK_EQ(res_404.body, "Not Found");

    auto res_405 = default_server.post("/existing_route");
    CHECK_EQ(res_405.status, 405);
    CHECK_EQ(res_405.body, "Method Not Allowed");
    CHECK(res_405.has_header("Allow"));
    CHECK(res_405.header("Allow").find("GET") != std::string::npos);

    // 2. Custom synchronous 404 and 405 handlers
    FastPathTestServer custom_sync_server;
    custom_sync_server.server().set_not_found_handler([](kale::Context &ctx) {
        ctx.set_status(kale::HttpStatus::NotFound);
        ctx.string("custom_sync_404_body");
    });
    custom_sync_server.server().set_method_not_allowed_handler([](kale::Context &ctx) {
        ctx.set_status(kale::HttpStatus::MethodNotAllowed);
        ctx.string("custom_sync_405_body");
    });
    custom_sync_server->GET("/custom_endpoint", [](kale::Context &ctx) { ctx.string("endpoint"); });

    auto res_custom_sync_404 = custom_sync_server.get("/nowhere");
    CHECK_EQ(res_custom_sync_404.status, 404);
    CHECK_EQ(res_custom_sync_404.body, "custom_sync_404_body");

    auto res_custom_sync_405 = custom_sync_server.post("/custom_endpoint");
    CHECK_EQ(res_custom_sync_405.status, 405);
    CHECK_EQ(res_custom_sync_405.body, "custom_sync_405_body");

    // 3. Custom asynchronous 404 and 405 handlers
    FastPathTestServer custom_async_server;
    custom_async_server.server().set_not_found_handler([](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_status(kale::HttpStatus::NotFound);
        ctx.string("custom_async_404_body");
        co_return;
    });
    custom_async_server.server().set_method_not_allowed_handler([](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_status(kale::HttpStatus::MethodNotAllowed);
        ctx.string("custom_async_405_body");
        co_return;
    });
    custom_async_server->GET("/custom_endpoint", [](kale::Context &ctx) { ctx.string("endpoint"); });

    auto res_custom_async_404 = custom_async_server.get("/nowhere");
    CHECK_EQ(res_custom_async_404.status, 404);
    CHECK_EQ(res_custom_async_404.body, "custom_async_404_body");

    auto res_custom_async_405 = custom_async_server.post("/custom_endpoint");
    CHECK_EQ(res_custom_async_405.status, 405);
    CHECK_EQ(res_custom_async_405.body, "custom_async_405_body");
}

TEST_CASE("Dispatch Fast Path: max body size limit, traversal rejection, and path clean predicate") {
    kale::HttpServerParams params;
    params.clean_path = true;
    FastPathTestServer server(params);

    std::atomic<int> payload_handler_invoked{0};
    server->POST("/bounded_body", [&payload_handler_invoked](kale::Context &ctx) {
        payload_handler_invoked.fetch_add(1);
        ctx.string("body_accepted");
    }).max_body_size(10);

    // Payload under limit: accepted
    auto res_small = server.post("/bounded_body", "12345");
    CHECK_EQ(res_small.status, 200);
    CHECK_EQ(res_small.body, "body_accepted");
    CHECK_EQ(payload_handler_invoked.load(), 1);

    // Payload over limit: 413 Payload Too Large, handler not invoked
    auto res_large = server.post("/bounded_body", "12345678901234567890");
    CHECK_EQ(res_large.status, 413);
    CHECK_EQ(res_large.body, "Payload Too Large");
    CHECK_EQ(payload_handler_invoked.load(), 1);

    // Path traversal in path rejected
    auto res_traversal = server.get("/../forbidden");
    CHECK_EQ(res_traversal.status, 403);

    // Path traversal in route parameter rejected
    server->GET("/user/:id", [](kale::Context &ctx) {
        ctx.string(ctx.param("id"));
    });
    auto res_param_traversal = server.get("/user/..%2fetc");
    CHECK_EQ(res_param_traversal.status, 403);

    // Path normalization: canonical path matches directly
    server->GET("/canonical/path", [](kale::Context &ctx) {
        ctx.string("canonical_ok");
    });
    auto res_canonical = server.get("/canonical/path");
    CHECK_EQ(res_canonical.status, 200);
    CHECK_EQ(res_canonical.body, "canonical_ok");

    // Path normalization: redundant slashes collapsed by clean_path
    auto res_redundant = server.get("//canonical///path");
    CHECK_EQ(res_redundant.status, 200);
    CHECK_EQ(res_redundant.body, "canonical_ok");
}


TEST_CASE("Dispatch Fast Path: precommit 500 resets stale Content-Length=2 and preserves custom headers and cookies") {
    FastPathTestServer server;

    server->GET("/error_length2", [](kale::Context &ctx) {
        ctx.set_header("Content-Length", "2");
        ctx.set_header("X-Preserved-Trace", "trace-id-abc");
        ctx.set_cookie("session_token", "tok999");
        throw std::runtime_error("handler deliberate failure");
    });

    // 1. Client via WFTaskFactory (do_request)
    auto res = server.get("/error_length2");
    CHECK_EQ(res.status, 500);
    CHECK_EQ(res.body, "Internal Server Error");
    CHECK_EQ(res.header("Content-Length"), "21");
    CHECK_EQ(res.header("X-Preserved-Trace"), "trace-id-abc");
    CHECK(res.has_header("Set-Cookie"));
    CHECK(res.header("Set-Cookie").find("session_token=tok999") != std::string::npos);

    // 2. Raw wire socket verification
    auto wire_res = send_raw_wire_http(server.port(),
        "GET /error_length2 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(wire_res.status, 500);
    CHECK_EQ(wire_res.body, "Internal Server Error");
    CHECK_EQ(wire_res.header("Content-Length"), "21");
    CHECK_EQ(wire_res.header("X-Preserved-Trace"), "trace-id-abc");
    CHECK(wire_res.header("Set-Cookie").find("session_token=tok999") != std::string::npos);
}

TEST_CASE("Dispatch Fast Path: precommit 500 resets bogus Content-Length on send_chunk precommit failure") {
    FastPathTestServer server;

    server->GET("/error_prestream_bogus", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_header("Content-Length", "bogus");
        ctx.set_header("X-Trace", "prestream-trace");
        co_await ctx.send_chunk("stream payload");
    });

    // 1. Client via WFTaskFactory
    auto res = server.get("/error_prestream_bogus");
    CHECK_EQ(res.status, 500);
    CHECK_EQ(res.body, "Internal Server Error");
    CHECK_EQ(res.header("Content-Length"), "21");
    CHECK_EQ(res.header("X-Trace"), "prestream-trace");

    // 2. Raw wire socket verification
    auto wire_res = send_raw_wire_http(server.port(),
        "GET /error_prestream_bogus HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(wire_res.status, 500);
    CHECK_EQ(wire_res.body, "Internal Server Error");
    CHECK_EQ(wire_res.header("Content-Length"), "21");
    CHECK_EQ(wire_res.header("X-Trace"), "prestream-trace");
}

TEST_CASE("Dispatch Fast Path: precommit 400 JsonInputError resets stale Content-Length and sets correct Content-Type") {
    FastPathTestServer server;

    server->POST("/api/json_endpoint", [](kale::Context &ctx) {
        ctx.set_header("Content-Length", "2");
        ctx.set_header("Content-Type", "application/json");
        ctx.set_header("X-Custom-Req", "preserved-val");
        auto j = ctx.parse_json();
        ctx.string("ok");
    });

    // Malformed JSON payload
    auto res = server.post("/api/json_endpoint", "{invalid_json_payload");
    CHECK_EQ(res.status, 400);
    CHECK_EQ(res.body, "Bad Request");
    CHECK_EQ(res.header("Content-Length"), "11");
    CHECK_EQ(res.header("Content-Type"), "text/plain; charset=utf-8");
    CHECK_EQ(res.header("X-Custom-Req"), "preserved-val");

    // Raw wire socket verification
    auto wire_res = send_raw_wire_http(server.port(),
        "POST /api/json_endpoint HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: 21\r\n"
        "Connection: close\r\n\r\n"
        "{invalid_json_payload");
    CHECK_EQ(wire_res.status, 400);
    CHECK_EQ(wire_res.body, "Bad Request");
    CHECK_EQ(wire_res.header("Content-Length"), "11");
    CHECK_EQ(wire_res.header("Content-Type"), "text/plain; charset=utf-8");
    CHECK_EQ(wire_res.header("X-Custom-Req"), "preserved-val");
}

TEST_CASE("Dispatch Fast Path: precommit 500 erases incompatible encoding and framing headers") {
    FastPathTestServer server;

    server->GET("/error_incompatible_framing", [](kale::Context &ctx) {
        ctx.set_header("Content-Encoding", "gzip");
        ctx.set_header("Transfer-Encoding", "chunked");
        ctx.set_header("Content-Range", "bytes 0-10/100");
        ctx.set_header("Trailer", "X-Checksum");
        ctx.set_header("X-App-Id", "app-123");
        throw std::runtime_error("failure with incompatible framing");
    });

    auto wire_res = send_raw_wire_http(server.port(),
        "GET /error_incompatible_framing HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    CHECK_EQ(wire_res.status, 500);
    CHECK_EQ(wire_res.body, "Internal Server Error");
    CHECK_EQ(wire_res.header("Content-Length"), "21");
    CHECK_EQ(wire_res.header("Content-Encoding"), "");
    CHECK_EQ(wire_res.header("Transfer-Encoding"), "");
    CHECK_EQ(wire_res.header("Content-Range"), "");
    CHECK_EQ(wire_res.header("Trailer"), "");
    CHECK_EQ(wire_res.header("X-App-Id"), "app-123");
}

TEST_CASE("Dispatch Fast Path: fallback not_found_handler failure resets framing to 500") {
    FastPathTestServer server;
    server.server().set_not_found_handler([](kale::Context &ctx) {
        ctx.set_header("Content-Length", "4");
        ctx.set_header("X-Fallback", "true");
        throw std::runtime_error("fallback handler failure");
    });

    auto res = server.get("/unmatched_fallback_test");
    CHECK_EQ(res.status, 500);
    CHECK_EQ(res.body, "Internal Server Error");
    CHECK_EQ(res.header("Content-Length"), "21");
    CHECK_EQ(res.header("X-Fallback"), "true");
}

TEST_CASE("Dispatch Fast Path: sequential keep-alive boundary preserved after 500 error with length2") {
    FastPathTestServer server;

    server->GET("/error_keepalive", [](kale::Context &ctx) {
        ctx.set_header("Content-Length", "2");
        throw std::runtime_error("first request failed");
    });

    server->GET("/subsequent_keepalive", [](kale::Context &ctx) {
        ctx.string("second_request_success");
    });

    std::string req1 =
        "GET /error_keepalive HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: keep-alive\r\n\r\n";

    std::string req2 =
        "GET /subsequent_keepalive HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n";

    std::string res1_body, res2_body;
    int res1_status = 0, res2_status = 0;

    verify_keep_alive_recovery(server.port(), req1, req2, res1_body, res2_body, res1_status, res2_status);

    CHECK_EQ(res1_status, 500);
    CHECK_EQ(res1_body, "Internal Server Error");
    CHECK_EQ(res2_status, 200);
    CHECK_EQ(res2_body, "second_request_success");
}

TEST_CASE("Pipeline: three-argument compose_and_run with empty route middleware lifetime") {
    FastPathTestServer server;

    // Subcase 1: compose_and_run 3-arg with non-empty global middleware list
    server->GET("/test_compose_3arg_mws", [](kale::Context &ctx) -> coke::Task<void> {
        std::vector<kale::Middleware> mws;
        bool mw_executed = false;
        mws.push_back([&mw_executed](kale::Context &c, kale::Next next) -> coke::Task<void> {
            mw_executed = true;
            c.set_header("X-Compose-MW", "1");
            co_await next();
        });
        bool handler_executed = false;
        auto final_h = kale::Handler::from_sync([&handler_executed](kale::Context &c) {
            handler_executed = true;
            c.string("3arg_mws_ok");
        });

        // Invoking 3-arg compose_and_run where second argument route_mws is defaulted to temporary {}
        co_await kale::compose_and_run(mws, ctx, final_h);
        CHECK(mw_executed);
        CHECK(handler_executed);
    });

    // Subcase 2: compose_and_run 3-arg with empty global middleware list
    server->GET("/test_compose_3arg_empty", [](kale::Context &ctx) -> coke::Task<void> {
        std::vector<kale::Middleware> empty_mws;
        bool handler_executed = false;
        auto final_h = kale::Handler::from_sync([&handler_executed](kale::Context &c) {
            handler_executed = true;
            c.string("3arg_empty_ok");
        });

        co_await kale::compose_and_run(empty_mws, ctx, final_h);
        CHECK(handler_executed);
    });

    auto res1 = server.get("/test_compose_3arg_mws");
    CHECK_EQ(res1.status, 200);
    CHECK_EQ(res1.body, "3arg_mws_ok");
    CHECK_EQ(res1.header("X-Compose-MW"), "1");

    auto res2 = server.get("/test_compose_3arg_empty");
    CHECK_EQ(res2.status, 200);
    CHECK_EQ(res2.body, "3arg_empty_ok");
}

TEST_CASE("Dispatch Fast Path: handler invocation qualification and stateful service pattern") {
    // 1. Static qualification contract: HandlerCallable requires const-invocable callables
    auto mutable_lambda = [count = 0](kale::Context &ctx) mutable {
        ++count;
        ctx.string(std::to_string(count));
    };

    // HandlerCallable rejects mutable callables that cannot be invoked as const
    CHECK_FALSE(kale::HandlerCallable<decltype(mutable_lambda)>);
    CHECK_FALSE(std::is_invocable_v<const decltype(mutable_lambda)&, kale::Context&>);

    // 2. Direct Router registration and route matching verification
    kale::Router router;

    // Register direct const async lambda using thread-safe external state
    auto atomic_async_count = std::make_shared<std::atomic<int>>(0);
    router.GET("/router_direct_async", [atomic_async_count](kale::Context &ctx) -> coke::Task<void> {
        int val = ++(*atomic_async_count);
        ctx.string(std::to_string(val));
        co_return;
    });

    // Explicit erased std::function path via Handler::from_sync
    auto sync_count = std::make_shared<std::atomic<int>>(0);
    router.GET("/router_sync", kale::Handler::from_sync([sync_count](kale::Context &ctx) {
        int val = ++(*sync_count);
        ctx.string(std::to_string(val));
    }));

    // Explicit erased std::function path via Handler::from_async
    auto async_count = std::make_shared<std::atomic<int>>(10);
    router.GET("/router_async", kale::Handler::from_async([async_count](kale::Context &ctx) -> coke::Task<void> {
        int val = ++(*async_count);
        ctx.string(std::to_string(val));
        co_return;
    }));

    auto r1 = router.route(kale::HttpMethod::GET, "/router_direct_async");
    CHECK(r1.matched);
    REQUIRE(r1.endpoint != nullptr);
    CHECK(r1.endpoint->handler.is_async());

    auto r2 = router.route(kale::HttpMethod::GET, "/router_sync");
    CHECK(r2.matched);
    REQUIRE(r2.endpoint != nullptr);
    CHECK(r2.endpoint->handler.is_sync());

    auto r3 = router.route(kale::HttpMethod::GET, "/router_async");
    CHECK(r3.matched);
    REQUIRE(r3.endpoint != nullptr);
    CHECK(r3.endpoint->handler.is_async());

    // 3. HttpServer registration and full HTTP invocation across multiple requests
    FastPathTestServer server;

    // Direct async handler on HttpServer
    auto s_async_count = std::make_shared<std::atomic<int>>(100);
    server->GET("/direct_async", [s_async_count](kale::Context &ctx) -> coke::Task<void> {
        int val = ++(*s_async_count);
        ctx.string(std::to_string(val));
        co_return;
    });

    // Invocation of handler registered via Handler::from_sync on HttpServer
    auto s_sync_count = std::make_shared<std::atomic<int>>(0);
    server->GET("/sync_endpoint", kale::Handler::from_sync([s_sync_count](kale::Context &ctx) {
        int val = ++(*s_sync_count);
        ctx.string(std::to_string(val));
    }));

    // Invocation of handler registered via Handler::from_async on HttpServer
    auto s_from_async_count = std::make_shared<std::atomic<int>>(10);
    server->GET("/async_endpoint", kale::Handler::from_async([s_from_async_count](kale::Context &ctx) -> coke::Task<void> {
        int val = ++(*s_from_async_count);
        ctx.string(std::to_string(val));
        co_return;
    }));

    // Verify direct async invocation persists state safely across requests
    auto res_direct1 = server.get("/direct_async");
    CHECK_EQ(res_direct1.status, 200);
    CHECK_EQ(res_direct1.body, "101");

    auto res_direct2 = server.get("/direct_async");
    CHECK_EQ(res_direct2.status, 200);
    CHECK_EQ(res_direct2.body, "102");

    // Verify sync invocation persists state safely across requests
    auto res1 = server.get("/sync_endpoint");
    CHECK_EQ(res1.status, 200);
    CHECK_EQ(res1.body, "1");

    auto res2 = server.get("/sync_endpoint");
    CHECK_EQ(res2.status, 200);
    CHECK_EQ(res2.body, "2");

    // Verify async invocation persists state safely across requests
    auto res_async1 = server.get("/async_endpoint");
    CHECK_EQ(res_async1.status, 200);
    CHECK_EQ(res_async1.body, "11");

    auto res_async2 = server.get("/async_endpoint");
    CHECK_EQ(res_async2.status, 200);
    CHECK_EQ(res_async2.body, "12");
}

TEST_CASE("Router: backtracking match success clears allowed_methods residue") {
    kale::Router router;

    // Static route at /api/data with only POST method registered
    router.POST("/api/data", [](kale::Context &ctx) {
        ctx.string("post_data");
    });

    // Param route at /api/:resource with GET method registered
    router.GET("/api/:resource", [](kale::Context &ctx) {
        ctx.string("get_resource:" + std::string(ctx.param("resource")));
    });

    // 1. Dispatch GET /api/data:
    // First, static child matches path "api/data" but fails method (only POST registered),
    // collecting POST into allowed_methods.
    // Next, backtracking explores param child "/api/:resource", which matches both path and method GET.
    // The successful match must clear allowed_methods residue and mark matched = true.
    auto res_get = router.route(kale::HttpMethod::GET, "/api/data");
    CHECK(res_get.matched);
    CHECK_FALSE(res_get.method_not_allowed);
    CHECK(res_get.allowed_methods.empty());
    CHECK_EQ(res_get.param("resource"), "data");

    // 2. Dispatch DELETE /api/data:
    // Neither static nor param branch matches DELETE. Both branches fail method, so allowed_methods
    // must report 405 Method Not Allowed with both POST and GET.
    auto res_del = router.route(kale::HttpMethod::DELETE, "/api/data");
    CHECK_FALSE(res_del.matched);
    CHECK(res_del.method_not_allowed);
    CHECK_EQ(res_del.allowed_methods.size(), 2);
}
} // TEST_SUITE("core")
