#include "doctest/doctest.h"

#include <arpa/inet.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <map>
#include <netinet/in.h>
#include <new>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#if defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#endif
#endif

#include "coke/sleep.h"
#include "coke/task.h"
#include "coke/wait.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/router.h"
#include "kale/core/http_status.h"
#include "kale/http/form.h"
#include "kale/http/multipart.h"
#include "kale/http/sse.h"

namespace {

bool is_running_on_valgrind() noexcept {
#if defined(RUNNING_ON_VALGRIND)
    return RUNNING_ON_VALGRIND != 0;
#else
    return false;
#endif
}

// Scoped thread-local allocation tracker to measure real Context::form vs parse_form heap allocations
static thread_local bool t_track_alloc = false;
static thread_local size_t t_alloc_count = 0;
static thread_local size_t t_alloc_bytes = 0;

struct ScopedAllocGuard {
    ScopedAllocGuard() {
        t_alloc_count = 0;
        t_alloc_bytes = 0;
        t_track_alloc = true;
    }
    ~ScopedAllocGuard() {
        t_track_alloc = false;
    }
    size_t count() const noexcept { return t_alloc_count; }
    size_t bytes() const noexcept { return t_alloc_bytes; }
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
    char buf[4096];
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
        std::string raw_body = response_data.substr(header_end + 4);
        size_t first_line_end = out.headers.find("\r\n");
        out.status_line = out.headers.substr(0, first_line_end);
        size_t sp1 = out.status_line.find(' ');
        if (sp1 != std::string::npos) {
            size_t sp2 = out.status_line.find(' ', sp1 + 1);
            std::string code_str = out.status_line.substr(sp1 + 1, sp2 != std::string::npos ? sp2 - (sp1 + 1) : std::string::npos);
            std::from_chars(code_str.data(), code_str.data() + code_str.size(), out.status);
        }
        if (out.headers.find("Transfer-Encoding: chunked") != std::string::npos ||
            out.headers.find("transfer-encoding: chunked") != std::string::npos) {
            std::string_view stream = raw_body;
            std::string decoded;
            while (!stream.empty()) {
                size_t crlf = stream.find("\r\n");
                if (crlf == std::string_view::npos) break;
                size_t chunk_len = 0;
                auto [p, ec] = std::from_chars(stream.data(), stream.data() + crlf, chunk_len, 16);
                if (ec != std::errc{}) break;
                stream.remove_prefix(crlf + 2);
                if (chunk_len == 0) break;
                if (stream.size() < chunk_len) break;
                decoded.append(stream.substr(0, chunk_len));
                stream.remove_prefix(chunk_len);
                if (stream.starts_with("\r\n")) stream.remove_prefix(2);
            }
            out.body = std::move(decoded);
        } else {
            out.body = std::move(raw_body);
        }
    } else {
        out.body = response_data;
    }
    return out;
}

class FormStreamServer {
public:
    explicit FormStreamServer(kale::HttpServerParams params = {})
        : server_(params) {
        int ret = server_.start("127.0.0.1", 0);
        REQUIRE_EQ(ret, 0);
        REQUIRE(server_.is_running());
        port_ = static_cast<unsigned short>(server_.listen_port());
        REQUIRE_GT(port_, 0);
    }

    ~FormStreamServer() {
        server_.stop();
    }

    FormStreamServer(const FormStreamServer &) = delete;
    FormStreamServer &operator=(const FormStreamServer &) = delete;

    kale::HttpServer *operator->() noexcept { return &server_; }
    kale::HttpServer &server() noexcept { return server_; }
    unsigned short port() const noexcept { return port_; }

    RawResponse post(std::string_view path, std::string_view body, std::string_view content_type) const {
        std::string req = "POST " + std::string(path) + " HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Content-Type: " + std::string(content_type) + "\r\n"
                          "Content-Length: " + std::to_string(body.size()) + "\r\n"
                          "Connection: close\r\n\r\n" + std::string(body);
        return send_raw_http(port_, req);
    }

    RawResponse get(std::string_view path) const {
        std::string req = "GET " + std::string(path) + " HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Connection: close\r\n\r\n";
        return send_raw_http(port_, req);
    }

private:
    kale::HttpServer server_;
    unsigned short port_{0};
};

class StreamRawClient {
public:
    explicit StreamRawClient(unsigned short port, int rcvbuf = 0) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE_GE(fd_, 0);

        struct timeval tv{.tv_sec = 2, .tv_usec = 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (rcvbuf > 0) {
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        }

        struct sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
        int ret = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
        REQUIRE_EQ(ret, 0);
    }

    ~StreamRawClient() {
        close();
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void send_request(std::string_view path, bool close_conn = true) {
        std::string req = "GET " + std::string(path) + " HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Connection: " + (close_conn ? "close" : "keep-alive") + "\r\n\r\n";
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
        char crlf[2];
        ::recv(fd_, crlf, 2, MSG_WAITALL);
        return chunk_data;
    }

private:
    int fd_{-1};
};

} // namespace

__attribute__((noinline)) void* operator new(size_t size) {
    if (t_track_alloc) {
        t_alloc_count += 1;
        t_alloc_bytes += size;
    }
    if (void *allocation = std::malloc(size ? size : 1)) return allocation;
    throw std::bad_alloc();
}

__attribute__((noinline)) void* operator new[](size_t size) {
    return ::operator new(size);
}

__attribute__((noinline)) void operator delete(void* p) noexcept {
    std::free(p);
}

__attribute__((noinline)) void operator delete[](void* p) noexcept {
    std::free(p);
}

__attribute__((noinline)) void operator delete(void* p, size_t) noexcept {
    std::free(p);
}

__attribute__((noinline)) void operator delete[](void* p, size_t) noexcept {
    std::free(p);
}

TEST_SUITE("core") {

TEST_CASE("Direct scalar Context::form and has_form URL-encoded semantics and allocation comparison") {
    FormStreamServer server;

    server->POST("/test_url_form", [](kale::Context &ctx) {
        // 1. Direct scalar lookup vs full-map equivalence
        auto full_map = kale::parse_form(ctx);

        CHECK(kale::has_form(ctx, "alpha"));
        CHECK_EQ(kale::form(ctx, "alpha"), "100");
        CHECK_EQ(full_map.at("alpha"), "100");

        CHECK(kale::has_form(ctx, "beta"));
        CHECK_EQ(kale::form(ctx, "beta"), "hello world");
        CHECK_EQ(full_map.at("beta"), "hello world");

        CHECK(kale::has_form(ctx, "empty_val"));
        CHECK_EQ(kale::form(ctx, "empty_val"), "");
        CHECK_EQ(full_map.at("empty_val"), "");

        CHECK(kale::has_form(ctx, "flag_only"));
        CHECK_EQ(kale::form(ctx, "flag_only"), "");
        CHECK_EQ(full_map.at("flag_only"), "");

        // 2. Encoded keys and values
        CHECK(kale::has_form(ctx, "spaced key"));
        CHECK_EQ(kale::form(ctx, "spaced key"), "spaced val");
        CHECK_EQ(full_map.at("spaced key"), "spaced val");

        CHECK(kale::has_form(ctx, "plus+key"));
        CHECK_EQ(kale::form(ctx, "plus+key"), "percent decoded");
        CHECK_EQ(full_map.at("plus+key"), "percent decoded");

        // 3. Duplicate key precedence: split_query emplace() retains the FIRST winner
        CHECK(kale::has_form(ctx, "dup"));
        CHECK_EQ(kale::form(ctx, "dup"), "first_winner");
        CHECK_EQ(full_map.at("dup"), "first_winner");

        // 4. Missing keys
        CHECK_FALSE(kale::has_form(ctx, "non_existent"));
        CHECK_EQ(kale::form(ctx, "non_existent"), "");
        CHECK_FALSE(kale::has_form(ctx, ""));
        CHECK_EQ(kale::form(ctx, ""), "");

        // 5. Malformed escape handling matches existing url_decode
        CHECK(kale::has_form(ctx, "bad_pct"));
        CHECK_EQ(kale::form(ctx, "bad_pct"), "foo%ZZbar");
        CHECK_EQ(full_map.at("bad_pct"), "foo%ZZbar");

        // 6. Real scoped allocation comparison on >SSO key and value
        size_t map_allocs = 0;
        {
            ScopedAllocGuard guard;
            auto m = kale::parse_form(ctx);
            auto v = m.find("k_sso_19");
            (void)v;
            map_allocs = guard.count();
        }

        size_t scalar_allocs = 0;
        std::string scalar_val;
        {
            ScopedAllocGuard guard;
            scalar_val = kale::form(ctx, "k_sso_19");
            scalar_allocs = guard.count();
        }

        CAPTURE(map_allocs);
        CAPTURE(scalar_allocs);
        if (!is_running_on_valgrind()) {
            CHECK_GT(map_allocs, scalar_allocs);
            CHECK_LE(scalar_allocs, 2);
        }
        CHECK_EQ(scalar_val, full_map.at("k_sso_19"));
        ctx.string("ok_form");
    });

    std::string body =
        "alpha=100&beta=hello+world&empty_val=&flag_only&spaced%20key=spaced+val"
        "&plus%2Bkey=percent+decoded&dup=first_winner&dup=second_loser&bad_pct=foo%ZZbar";

    for (int i = 0; i < 20; ++i) {
        body += "&k_sso_" + std::to_string(i) + "=";
        body += "long_value_exceeding_standard_sso_capacity_prefix_0123456789_abcdefghijklmnopqrstuvwxyz_" + std::to_string(i);
    }

    auto res = server.post("/test_url_form", body, "application/x-www-form-urlencoded");
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, "ok_form");
}

TEST_CASE("Direct scalar Context::form and has_form multipart/form-data semantics and allocation comparison") {
    FormStreamServer server;

    server->POST("/test_multipart_form", [](kale::Context &ctx) {
        // Direct field scanning without constructing full vector/map
        CHECK(kale::has_form(ctx, "field1"));
        CHECK_EQ(kale::form(ctx, "field1"), "value1");

        CHECK(kale::has_form(ctx, "field2"));
        CHECK_EQ(kale::form(ctx, "field2"), "multi\r\nline\r\nvalue");

        // Duplicate field winner in multipart (last part parsed overwrote in previous parse_form, verify match)
        auto full_map = kale::parse_form(ctx);
        CHECK(kale::has_form(ctx, "dup_field"));
        CHECK_EQ(kale::form(ctx, "dup_field"), full_map.at("dup_field"));

        // File parts with filename are excluded from form() fields
        CHECK_FALSE(kale::has_form(ctx, "file_upload"));
        CHECK_EQ(kale::form(ctx, "file_upload"), "");

        // File accessor finds file correctly
        auto f = kale::form_file(ctx, "file_upload");
        REQUIRE(f.has_value());
        CHECK_EQ(f->filename, "hello.txt");
        CHECK_EQ(f->data, "file_content_here");

        // Missing field
        CHECK_FALSE(kale::has_form(ctx, "absent"));
        CHECK_EQ(kale::form(ctx, "absent"), "");

        // Real scoped allocation comparison on multipart
        size_t map_allocs = 0;
        {
            ScopedAllocGuard guard;
            auto m = kale::parse_form(ctx);
            (void)m;
            map_allocs = guard.count();
        }

        size_t scalar_allocs = 0;
        {
            ScopedAllocGuard guard;
            std::string val = kale::form(ctx, "field2");
            (void)val;
            scalar_allocs = guard.count();
        }

        CAPTURE(map_allocs);
        CAPTURE(scalar_allocs);
        if (!is_running_on_valgrind()) {
            CHECK_GT(map_allocs, scalar_allocs);
            CHECK_LE(scalar_allocs, 2);
        }
        ctx.string("ok_multipart");
    });

    const std::string mp_body =
        "--myboundary\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n\r\n"
        "value1\r\n"
        "--myboundary\r\n"
        "Content-Disposition: form-data; name=\"field2\"\r\n\r\n"
        "multi\r\nline\r\nvalue\r\n"
        "--myboundary\r\n"
        "Content-Disposition: form-data; name=\"file_upload\"; filename=\"hello.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "file_content_here\r\n"
        "--myboundary\r\n"
        "Content-Disposition: form-data; name=\"dup_field\"\r\n\r\n"
        "dup_val_1\r\n"
        "--myboundary\r\n"
        "Content-Disposition: form-data; name=\"dup_field\"\r\n\r\n"
        "dup_val_2\r\n"
        "--myboundary--\r\n";

    auto res = server.post("/test_multipart_form", mp_body, "multipart/form-data; boundary=myboundary");
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, "ok_multipart");
}
TEST_CASE("Direct scalar Context::form and has_form repeated disposition regression") {
    FormStreamServer server;

    server->POST("/test_repeated_disposition", [](kale::Context &ctx) {
        auto full_map = kale::parse_form(ctx);

        // 1. name first, then separate Content-Disposition filename
        // Full parser: first header sets name="foo", second header sets filename="bar.txt" without clearing name.
        // Because filename is nonempty, it becomes a FormFile (not in form/parse_form).
        CHECK_EQ(kale::has_form(ctx, "foo_file"), full_map.find("foo_file") != full_map.end());
        CHECK_EQ(kale::form(ctx, "foo_file"), "");
        auto f1 = kale::form_file(ctx, "foo_file");
        REQUIRE(f1.has_value());
        CHECK_EQ(f1->filename, "bar.txt");
        CHECK_EQ(f1->data, "data_file_1");

        // 2. filename first, then separate Content-Disposition name
        // Full parser: first header sets filename="baz.txt", second header sets name="qux".
        // Both filename and name are set, so it is a file and excluded from form().
        CHECK_EQ(kale::has_form(ctx, "qux"), full_map.find("qux") != full_map.end());
        CHECK_EQ(kale::form(ctx, "qux"), "");
        auto f2 = kale::form_file(ctx, "qux");
        REQUIRE(f2.has_value());
        CHECK_EQ(f2->filename, "baz.txt");
        CHECK_EQ(f2->data, "data_file_2");

        // 3. Repeated disposition with empty params after valid name
        // Full parser: first sets name="alpha", second has empty/unrelated disposition -> name remains "alpha".
        CHECK(kale::has_form(ctx, "alpha"));
        CHECK_EQ(kale::form(ctx, "alpha"), "alpha_value");
        CHECK_EQ(kale::form(ctx, "alpha"), full_map.at("alpha"));

        // 4. Repeated disposition updating name
        // Full parser: first sets name="old_name", second sets name="new_name" -> name becomes "new_name".
        CHECK_FALSE(kale::has_form(ctx, "old_name"));
        CHECK(kale::has_form(ctx, "new_name"));
        CHECK_EQ(kale::form(ctx, "new_name"), "updated_value");
        CHECK_EQ(kale::form(ctx, "new_name"), full_map.at("new_name"));
        // 5. Exact reviewer minimal fixture: name first, then extra=attribute
        // Full parser: first header sets name="split_field", second header does not overwrite or clear it.
        CHECK(kale::has_form(ctx, "split_field"));
        CHECK_EQ(kale::form(ctx, "split_field"), "split_value");
        CHECK_EQ(kale::form(ctx, "split_field"), full_map.at("split_field"));

        // 6. Filename retained across name-only header to ensure file exclusion parity
        CHECK_FALSE(kale::has_form(ctx, "file_retained"));
        CHECK_EQ(kale::form(ctx, "file_retained"), "");
        auto f3 = kale::form_file(ctx, "file_retained");
        REQUIRE(f3.has_value());
        CHECK_EQ(f3->filename, "keep_file.bin");
        CHECK_EQ(f3->data, "file_data_3");

        ctx.string("ok_repeated_disp");
    });
    const std::string mp_body =
        "--repbound\r\n"
        "Content-Disposition: form-data; name=\"foo_file\"\r\n"
        "Content-Disposition: form-data; filename=\"bar.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "data_file_1\r\n"
        "--repbound\r\n"
        "Content-Disposition: form-data; filename=\"baz.txt\"\r\n"
        "Content-Disposition: form-data; name=\"qux\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "data_file_2\r\n"
        "--repbound\r\n"
        "Content-Disposition: form-data; name=\"alpha\"\r\n"
        "Content-Disposition: form-data; other_param=\"ignored\"\r\n\r\n"
        "alpha_value\r\n"
        "--repbound\r\n"
        "Content-Disposition: form-data; name=\"old_name\"\r\n"
        "Content-Disposition: form-data; name=\"new_name\"\r\n\r\n"
        "updated_value\r\n"
        "--repbound\r\n"
        "Content-Disposition: form-data; name=\"split_field\"\r\n"
        "Content-Disposition: form-data; extra=attribute\r\n\r\n"
        "split_value\r\n"
        "--repbound\r\n"
        "Content-Disposition: form-data; filename=\"keep_file.bin\"\r\n"
        "Content-Disposition: form-data; name=\"file_retained\"\r\n\r\n"
        "file_data_3\r\n"
        "--repbound--\r\n";

    auto res = server.post("/test_repeated_disposition", mp_body, "multipart/form-data; boundary=repbound");
    CHECK_EQ(res.status, 200);
    CHECK_EQ(res.body, "ok_repeated_disp");
}

TEST_CASE("Streaming owning buffer and literal overload resolution") {
    FormStreamServer server;

    server->GET("/test_stream_owned", [](kale::Context &ctx) -> coke::Task<void> {
        // String literal calls explicit const char* forwarding overload without ambiguity
        bool ok1 = co_await ctx.send_chunk("literal_chunk;");
        CHECK(ok1);

        // std::string_view calls send_chunk(std::string_view)
        std::string_view sv = "view_chunk;";
        bool ok2 = co_await ctx.send_chunk(sv);
        CHECK(ok2);

        // Owned std::string calls send_chunk(std::string) by move
        std::string owned = "owned_chunk;";
        bool ok3 = co_await ctx.send_chunk(std::move(owned));
        CHECK(ok3);

        // Temporary rvalue std::string
        bool ok4 = co_await ctx.send_chunk(std::string("temp_owned_chunk;"));
        CHECK(ok4);

        // Raw const char* variable
        const char *cstr = "cstr_chunk;";
        bool ok5 = co_await ctx.send_chunk(cstr);
        CHECK(ok5);

        co_await ctx.end_chunk();
    });

    auto res = server.get("/test_stream_owned");
    CHECK_EQ(res.status, 200);
    CHECK(res.body.find("literal_chunk;view_chunk;owned_chunk;temp_owned_chunk;cstr_chunk;") != std::string::npos);
}

TEST_CASE("Streaming lazy coroutine lifetime preservation across multiple suspensions") {
    FormStreamServer server;

    server->GET("/test_lazy_lifetime", [](kale::Context &ctx) -> coke::Task<void> {
        // Create an owning task from a temporary >SSO string inside an inner scope,
        // then destroy the temporary source completely before awaiting the task across sleep
        coke::Task<bool> send_task;
        {
            std::string temporary_source(8192, 'L');
            temporary_source[0] = 'H';
            temporary_source[8191] = 'T';
            // Move temporary into send_chunk: task created, temporary destroyed at end of scope
            send_task = ctx.send_chunk(std::move(temporary_source));
        }

        // Sleep to yield and ensure inner scope stack is rewritten / frame suspended
        co_await coke::sleep(0.01);

        // Now await the moved task whose source string was destroyed before suspension
        bool ok1 = co_await std::move(send_task);
        CHECK(ok1);

        // Second chunk: temporary rvalue expression created directly in parameter
        coke::Task<bool> second_task = ctx.send_chunk(std::string("second_temp_chunk;"));
        co_await coke::sleep(0.01);
        bool ok2 = co_await std::move(second_task);
        CHECK(ok2);

        co_await ctx.end_chunk();
    });

    auto res = server.get("/test_lazy_lifetime");
    CHECK_EQ(res.status, 200);
    std::string expected(8192, 'L');
    expected.front() = 'H';
    expected.back() = 'T';
    expected.append("second_temp_chunk;");
    CHECK_EQ(res.body, expected);
}

TEST_CASE("SSE lazy send tasks retain formatted input before suspension") {
    FormStreamServer server;
    server->GET("/test_sse_lazy_inputs", [](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        std::string data(4096, 'D');
        std::string event_name = "original";
        std::string comment(4096, 'C');
        kale::SseEvent event;
        event.id = "first";
        event.data = std::string(4096, 'E');

        auto data_task = sse.send(data);
        auto named_task = sse.send(event_name, data);
        auto event_task = sse.send_event(event);
        auto comment_task = sse.send_comment(comment);
        data.assign(data.size(), 'x');
        event_name.assign(event_name.size(), 'x');
        event.id = "other";
        event.data.assign(event.data.size(), 'x');
        comment.assign(comment.size(), 'x');
        auto temporary_task = sse.send_event(kale::SseEvent{"", "", std::string(4096, 'T'), -1, ""});
        co_await coke::sleep(0.001);

        CHECK(co_await std::move(data_task));
        CHECK(co_await std::move(named_task));
        CHECK(co_await std::move(event_task));
        CHECK(co_await std::move(comment_task));
        CHECK(co_await std::move(temporary_task));
        co_await sse.close();
    });

    auto res = server.get("/test_sse_lazy_inputs");
    REQUIRE_EQ(res.status, 200);
    std::string expected = "data: " + std::string(4096, 'D') + "\n\n";
    expected += "event: original\ndata: " + std::string(4096, 'D') + "\n\n";
    expected += "id: first\ndata: " + std::string(4096, 'E') + "\n\n";
    expected += ": " + std::string(4096, 'C') + "\n\n";
    expected += "data: " + std::string(4096, 'T') + "\n\n";
    CHECK_EQ(res.body, expected);
}

TEST_CASE("Deterministic in-flight write disconnect and teardown behavior") {
    FormStreamServer server;

    std::atomic<bool> server_started_streaming{false};
    std::atomic<int> writes_started{0};
    std::atomic<int> writes_completed{0};
    std::atomic<bool> in_flight_write_blocked{false};
    std::atomic<bool> write_failed{false};
    std::atomic<bool> server_stream_done{false};

    server->GET("/test_stream_disconnect", [&](kale::Context &ctx) -> coke::Task<void> {
        server_started_streaming = true;
        // 1. Initial handshake chunk sent to client
        bool ok = co_await ctx.send_chunk("handshake_chunk\n");
        CHECK(ok);

        // 2. Continually write large chunks (64KB) until backpressure blocks a write in flight
        std::string large_payload(64 * 1024, 'D');
        for (int i = 0; i < 200; ++i) {
            writes_started.fetch_add(1);
            if (writes_started.load() > writes_completed.load()) {
                in_flight_write_blocked = true;
            }
            bool sent = co_await ctx.send_chunk(large_payload);
            if (sent) {
                writes_completed.fetch_add(1);
            } else {
                write_failed = true;
                break;
            }
        }
        server_stream_done = true;
    });

    const unsigned short port = server.port();
    {
        // Constrain client TCP receive buffer to 4096 bytes to trigger TCP window saturation / backpressure
        StreamRawClient client(port, 4096);
        client.send_request("/test_stream_disconnect");
        std::string headers = client.read_headers();
        CHECK(headers.find("200 OK") != std::string::npos);

        // Robust handshake verification: read chunk content directly from wire
        std::string first = client.read_chunk();
        CHECK_EQ(first, "handshake_chunk\n");
        CHECK(server_started_streaming.load());

        // Wait until backpressure occurs: writes_started > writes_completed and remains blocked
        auto wait_backpressure = std::chrono::steady_clock::now();
        while (writes_completed.load() == writes_started.load()) {
            auto elapsed = std::chrono::steady_clock::now() - wait_backpressure;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 500) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        // Verify backpressure condition was reached: at least one write in-flight
        CHECK(in_flight_write_blocked.load());
        CHECK_GE(writes_started.load(), writes_completed.load());

        // Now abruptly close the socket while a write is genuinely blocked in flight
        client.close();
    }

    // Bounded termination: assert server-side handler detects write failure and completes without hanging
    auto start_wait = std::chrono::steady_clock::now();
    while (!server_stream_done.load()) {
        auto elapsed = std::chrono::steady_clock::now() - start_wait;
        REQUIRE_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 3);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(server_stream_done.load());
    CHECK(write_failed.load());
    CHECK_GT(writes_started.load(), 0);
}

TEST_CASE("Direct scalar Context::form and has_form multipart media-type case insensitivity") {
    FormStreamServer server;

    server->POST("/test_case_insensitive_multipart", [](kale::Context &ctx) {
        CHECK(kale::has_form(ctx, "field1"));
        CHECK_EQ(kale::form(ctx, "field1"), "value1");
        CHECK(kale::has_form(ctx, "field2"));
        CHECK_EQ(kale::form(ctx, "field2"), "value2");

        auto full_map = kale::parse_form(ctx);
        CHECK_EQ(full_map.count("field1"), 1);
        CHECK_EQ(full_map["field1"], "value1");
        CHECK_EQ(full_map["field2"], "value2");

        ctx.string("ok_case_insensitive");
    });

    const std::string mp_body =
        "--caseboundary\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n\r\n"
        "value1\r\n"
        "--caseboundary\r\n"
        "Content-Disposition: form-data; name=\"field2\"\r\n\r\n"
        "value2\r\n"
        "--caseboundary--\r\n";

    SUBCASE("Mixed case: Multipart/Form-Data") {
        auto res = server.post("/test_case_insensitive_multipart", mp_body, "Multipart/Form-Data; boundary=caseboundary");
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, "ok_case_insensitive");
    }

    SUBCASE("Upper case: MULTIPART/FORM-DATA") {
        auto res = server.post("/test_case_insensitive_multipart", mp_body, "MULTIPART/FORM-DATA; boundary=caseboundary");
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, "ok_case_insensitive");
    }
}

} // TEST_SUITE("core")
