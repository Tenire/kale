#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <string_view>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coke/future.h"
#include "coke/sleep.h"
#include "coke/wait.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/http/file_response.h"
#include "workflow/HttpUtil.h"

namespace {
using namespace std::chrono_literals;

// All tests use actual HTTP sockets. A small receive window plus a multi-MiB
// chunk forces the native writer to retain its buffer through backpressure.
class StreamClient {
public:
    explicit StreamClient(int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE_GE(fd_, 0);
        timeval timeout{10, 0};
        REQUIRE_EQ(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
        int receive_buffer = 32 * 1024;
        REQUIRE_EQ(::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)), 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(::connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    }
    ~StreamClient() { if (fd_ >= 0) ::close(fd_); }
    StreamClient(const StreamClient &) = delete;
    StreamClient &operator=(const StreamClient &) = delete;

    void request(std::string_view path, std::string_view extra_headers = {}) {
        const std::string request = "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + std::string(extra_headers) + "\r\n";
        size_t offset = 0;
        while (offset < request.size()) {
            auto sent = ::send(fd_, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
            REQUIRE_GT(sent, 0);
            offset += static_cast<size_t>(sent);
        }
    }
    std::string exact(size_t count) {
        std::string result(count, '\0');
        size_t offset = 0;
        while (offset < count) {
            auto received = ::recv(fd_, result.data() + offset, count - offset, 0);
            if (received <= 0) break;
            offset += static_cast<size_t>(received);
        }
        result.resize(offset);
        return result;
    }
    std::string line() {
        std::string result;
        while (!result.ends_with("\r\n")) {
            auto c = exact(1);
            if (c.empty()) break;
            result += c;
        }
        return result;
    }
    std::string headers() {
        std::string result;
        for (;;) {
            auto part = line();
            result += part;
            if (part.empty() || part == "\r\n") return result;
        }
    }
    std::string chunk() {
        const auto length = line();
        size_t count = 0;
        auto parsed = std::from_chars(length.data(), length.data() + length.size(), count, 16);
        REQUIRE(parsed.ec == std::errc{});
        auto result = exact(count);
        REQUIRE_EQ(result.size(), count);
        REQUIRE_EQ(exact(2), "\r\n");
        return result;
    }
    void reset() {
        linger rst{1, 0};
        REQUIRE_EQ(::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &rst, sizeof(rst)), 0);
        ::close(fd_);
        fd_ = -1;
    }
private:
    int fd_{-1};
};

struct RunningStreamServer {
    kale::HttpServer server;
    int port() const { return server.listen_port(); }
    void start() { REQUIRE_EQ(server.start("127.0.0.1", 0), 0); }
    ~RunningStreamServer() { server.stop(); }
};

std::string binary_pattern(size_t size) {
    std::string result(size, '\0');
    for (size_t i = 0; i < size; ++i) result[i] = static_cast<char>(i % 251);
    return result;
}

// Child task completion includes immediate storage release after false. Native
// writes must already have retired before any of these buffers are overwritten.
coke::Task<bool> borrowed_send(kale::Context &ctx, unsigned char fill) {
    std::string data(8 * 1024 * 1024, static_cast<char>(fill));
    bool result = co_await ctx.send_chunk(std::string_view(data));
    std::fill(data.begin(), data.end(), static_cast<char>(0xa5));
    co_return result;
}

coke::Task<bool> abort_after_gate(kale::Context &ctx, coke::Future<void> &gate) {
    co_await gate.wait();
    ctx.abort_stream();
    co_return true;
}
} // namespace

TEST_SUITE("stream_optimization") {
TEST_CASE("borrowed binary buffer may be reused after completed send and owning Task may await later") {
    std::promise<void> done;
    auto completion = done.get_future();
    RunningStreamServer fixture;
    fixture.server.GET("/binary", [&done](kale::Context &ctx) -> coke::Task<void> {
        std::string buffer = binary_pattern(128 * 1024);
        auto delayed_borrow = ctx.send_chunk(std::string_view(buffer));
        co_await coke::sleep(1ms);
        CHECK(co_await std::move(delayed_borrow));
        std::fill(buffer.begin(), buffer.end(), '\x7f');
        CHECK(co_await ctx.send_chunk(std::string_view(buffer)));
        auto delayed_owner = ctx.send_chunk(binary_pattern(96 * 1024));
        co_await coke::sleep(1ms);
        CHECK(co_await std::move(delayed_owner));
        CHECK(co_await ctx.end_chunk());
        done.set_value();
    });
    fixture.start();
    StreamClient client(fixture.port());
    client.request("/binary");
    REQUIRE(client.headers().find("200 OK") != std::string::npos);
    CHECK_EQ(client.chunk(), binary_pattern(128 * 1024));
    CHECK_EQ(client.chunk(), std::string(128 * 1024, '\x7f'));
    CHECK_EQ(client.chunk(), binary_pattern(96 * 1024));
    CHECK(client.chunk().empty());
    REQUIRE(completion.wait_for(10s) == std::future_status::ready);
    completion.get();
}

TEST_CASE("borrowed large binary chunk survives native partial writes before immediate reuse") {
    std::promise<void> entered;
    auto started = entered.get_future();
    std::promise<void> done;
    auto completion = done.get_future();
    std::atomic<bool> write_finished{false};
    RunningStreamServer fixture;
    fixture.server.GET("/partial", [&](kale::Context &ctx) -> coke::Task<void> {
        ctx.coke_context().get_task()->set_send_timeout(15000);
        std::string data = binary_pattern(8 * 1024 * 1024);
        entered.set_value();
        CHECK(co_await ctx.send_chunk(std::string_view(data)));
        write_finished.store(true);
        std::fill(data.begin(), data.end(), '\x55');
        CHECK(co_await ctx.send_chunk(std::string_view(data.data(), 1024)));
        CHECK(co_await ctx.end_chunk());
        done.set_value();
    });
    fixture.start();
    StreamClient client(fixture.port());
    client.request("/partial");
    REQUIRE(client.headers().find("200 OK") != std::string::npos);
    REQUIRE(started.wait_for(10s) == std::future_status::ready);
    // Receiving the headers proves native startup has happened. Withholding the
    // body and a 32KiB receive buffer cannot accept the entire 8MiB write.
    CHECK_FALSE(write_finished.load());
    CHECK_EQ(client.chunk(), binary_pattern(8 * 1024 * 1024));
    CHECK_EQ(client.chunk(), std::string(1024, '\x55'));
    CHECK(client.chunk().empty());
    REQUIRE(completion.wait_for(10s) == std::future_status::ready);
    completion.get();
}

TEST_CASE("disconnect retires current and queued borrowed writes before their owners release storage") {
    std::promise<std::vector<bool>> done;
    auto completion = done.get_future();
    RunningStreamServer fixture;
    fixture.server.GET("/disconnect", [&done](kale::Context &ctx) -> coke::Task<void> {
        ctx.coke_context().get_task()->set_send_timeout(5000);
        std::vector<coke::Task<bool>> tasks;
        tasks.emplace_back(borrowed_send(ctx, 0));
        tasks.emplace_back(borrowed_send(ctx, 1));
        tasks.emplace_back(borrowed_send(ctx, 2));
        auto results = co_await coke::async_wait(std::move(tasks));
        done.set_value(std::move(results));
    });
    fixture.start();
    StreamClient client(fixture.port());
    client.request("/disconnect");
    REQUIRE(client.headers().find("200 OK") != std::string::npos);
    client.reset();
    REQUIRE(completion.wait_for(10s) == std::future_status::ready);
    auto results = completion.get();
    REQUIRE_EQ(results.size(), 3);
    CHECK(std::none_of(results.begin(), results.end(), [](bool value) { return value; }));
}

TEST_CASE("explicit abort retires active write and releases never-submitted queue before owners exit") {
    coke::Promise<void> abort_signal;
    auto abort_gate = abort_signal.get_future();
    std::promise<std::vector<bool>> done;
    auto completion = done.get_future();
    RunningStreamServer fixture;
    fixture.server.GET("/abort", [&](kale::Context &ctx) -> coke::Task<void> {
        std::vector<coke::Task<bool>> tasks;
        tasks.emplace_back(borrowed_send(ctx, 0));
        tasks.emplace_back(borrowed_send(ctx, 1));
        tasks.emplace_back(borrowed_send(ctx, 2));
        tasks.emplace_back(abort_after_gate(ctx, abort_gate));
        auto results = co_await coke::async_wait(std::move(tasks));
        done.set_value(std::move(results));
    });
    fixture.start();
    StreamClient client(fixture.port());
    client.request("/abort");
    REQUIRE(client.headers().find("200 OK") != std::string::npos);
    abort_signal.set_value();
    REQUIRE(completion.wait_for(10s) == std::future_status::ready);
    auto results = completion.get();
    REQUIRE_EQ(results.size(), 4);
    CHECK_FALSE(results[0]);
    CHECK_FALSE(results[1]);
    CHECK_FALSE(results[2]);
    CHECK(results[3]);
}

TEST_CASE("file sender reuses its binary pread buffer across full and trailing partial blocks") {
    const std::string expected = binary_pattern(3 * 64 * 1024 + 317);
    char filename[] = "/tmp/kale-stream-optimization-XXXXXX";
    int fd = ::mkstemp(filename);
    REQUIRE_GE(fd, 0);
    struct TempFile {
        const char *name;
        ~TempFile() { ::unlink(name); }
    } temporary{filename};
    size_t written = 0;
    while (written < expected.size()) {
        auto count = ::write(fd, expected.data() + written, expected.size() - written);
        REQUIRE_GT(count, 0);
        written += static_cast<size_t>(count);
    }
    REQUIRE_EQ(::close(fd), 0);
    std::promise<int> done;
    auto completion = done.get_future();
    RunningStreamServer fixture;
    fixture.server.GET("/file", [&](kale::Context &ctx) -> coke::Task<void> {
        done.set_value(co_await kale::send_file(ctx, filename));
    });
    fixture.start();
    StreamClient client(fixture.port());
    client.request("/file");
    REQUIRE(client.headers().find("200 OK") != std::string::npos);
    CHECK_EQ(client.exact(expected.size()), expected);
    REQUIRE(completion.wait_for(10s) == std::future_status::ready);
    CHECK_EQ(completion.get(), 0);
}

TEST_CASE("header_view borrows first native value without changing owning duplicate and Cookie joins") {
    std::promise<void> done;
    auto completion = done.get_future();
    RunningStreamServer fixture;
    fixture.server.GET("/headers", [&done](kale::Context &ctx) {
        protocol::HttpHeaderCursor cursor(&ctx.raw_req());
        protocol::HttpMessageHeader native;
        native.name = "X-Long";
        native.name_len = 6;
        REQUIRE(cursor.find(&native));
        const auto view = ctx.header_view("x-LONG");
        CHECK_EQ(view.data(), native.value);
        CHECK_EQ(view, std::string(128, 'a'));
        CHECK_EQ(ctx.header("X-Long"), std::string(128, 'a') + ", second");
        CHECK_EQ(ctx.header_view("Cookie"), "first=1");
        CHECK_EQ(ctx.header("cookie"), "first=1; second=2");
        CHECK(ctx.header_view("").empty());
        CHECK(ctx.header_view("missing").empty());
        CHECK(ctx.has_header("X-Empty"));
        CHECK(ctx.header_view("X-Empty").empty());
        kale::Context moved(std::move(ctx));
        CHECK_EQ(moved.header_view("X-Long").data(), view.data());
        CHECK_EQ(view, std::string(128, 'a'));
        ctx = std::move(moved);
        const std::string owned = ctx.header("X-Long");
        CHECK(ctx.raw_req().set_header_pair("X-Long", "replacement"));
        CHECK_EQ(ctx.header_view("X-Long"), "replacement");
        CHECK_EQ(owned, std::string(128, 'a') + ", second");
        ctx.string("ok");
        done.set_value();
    });
    fixture.start();
    StreamClient client(fixture.port());
    client.request("/headers", "X-Long: " + std::string(128, 'a') + "\r\nX-Long: second\r\nCookie: first=1\r\nCookie: second=2\r\nX-Empty:\r\n");
    REQUIRE(client.headers().find("200 OK") != std::string::npos);
    CHECK_EQ(client.exact(2), "ok");
    REQUIRE(completion.wait_for(10s) == std::future_status::ready);
    completion.get();
}
} // TEST_SUITE
