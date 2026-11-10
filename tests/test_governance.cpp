#include "doctest/doctest.h"

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "coke/http/http_client.h"
#include "coke/sleep.h"
#include <future>
#include "coke/wait.h"
#include "kale/core/http_server.h"
#include "kale/core/logger.h"
#include "kale/http/health.h"
#include "kale/core/http_error.h"
#include "kale/middleware/prometheus.h"
#include "kale/middleware/rate_limiter.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

namespace {
using namespace std::chrono_literals;
using Clock = kale::RateLimitClock;
struct Reply {
    int state{-1};
    int status{0};
    std::string body;
    std::map<std::string, std::string, std::less<>> headers;
};
coke::Task<Reply> request(std::string url, std::string method = "GET", std::string forwarded = {}) {
    auto *task = WFTaskFactory::create_http_task(url, 0, 0, nullptr);
    task->get_req()->set_method(method);
    task->get_req()->add_header_pair("Connection", "close");
    if (!forwarded.empty()) task->get_req()->add_header_pair("X-Forwarded-For", forwarded);
    auto result = co_await coke::HttpAwaiter(task);
    Reply reply;
    reply.state = result.state;
    if (result.state != 0) co_return reply;
    if (const char *status = result.resp.get_status_code()) {
        const std::string_view code(status);
        std::from_chars(code.data(), code.data() + code.size(), reply.status);
    }
    if (result.resp.is_chunked()) reply.body = protocol::HttpUtil::decode_chunked_body(&result.resp);
    else {
        const void *body = nullptr;
        std::size_t size = 0;
        result.resp.get_parsed_body(&body, &size);
        if (body && size) reply.body.assign(static_cast<const char *>(body), size);
    }
    protocol::HttpHeaderCursor cursor(&result.resp);
    std::string name, value;
    while (cursor.next(name, value)) reply.headers[name] = value;
    co_return reply;
}
class Server {
public:
    ~Server() { server.stop(); }
    void start() {
        server.set_logger(kale::Logger::off());
        REQUIRE_EQ(server.start("127.0.0.1", 0), 0);
    }
    Reply get(std::string path, std::string method = "GET", std::string forwarded = {}) {
        auto response = coke::sync_wait(request(
            "http://127.0.0.1:" + std::to_string(server.listen_port()) + path, method, forwarded));
        REQUIRE_EQ(response.state, 0);
        return response;
    }
    kale::HttpServer server;
};
std::uint64_t total(const kale::PrometheusRegistry &registry) {
    std::uint64_t count = 0;
    for (const auto &sample : registry.snapshot()) count += sample.requests;
    return count;
}
const kale::HttpMetricSample *find_sample(const std::vector<kale::HttpMetricSample> &samples,
                                        std::string_view route, int status) {
    for (const auto &sample : samples) if (sample.route == route && sample.status == status) return &sample;
    return nullptr;
}
} // namespace

TEST_SUITE("governance") {
TEST_CASE("fixed windows have exact boundaries, ceiling and backward clock policy") {
    kale::MemoryRateLimitStore store({2, 1500ms, 2, 16});
    const Clock::time_point start{10s};
    auto first = store.consume("a", start);
    CHECK(first.allowed);
    CHECK_EQ(first.remaining, 1);
    CHECK(store.consume("a", start).allowed);
    CHECK_EQ(store.consume("a", start).retry_after, 2s);
    CHECK_EQ(store.consume("a", start + 500ms).retry_after, 1s);
    CHECK_EQ(store.consume("a", start + 1499ms).retry_after, 1s);
    CHECK(store.consume("a", start + 1500ms).allowed);
    CHECK(store.consume("a", start - 1s).allowed);
    CHECK_FALSE(store.consume("a", start - 1s).allowed);
    CHECK_EQ(store.size(), 1);
    CHECK(store.consume("b", start + 1500ms).allowed);
    CHECK_FALSE(store.consume("c", start + 1500ms).allowed);
    CHECK_EQ(store.consume("c", start + 1500ms).retry_after, 2s);
    CHECK_EQ(store.size(), 2);
    CHECK(store.consume("c", start + 3s).allowed);
    CHECK_EQ(store.size(), 1);
}

TEST_CASE("fixed window capacity maintenance is bounded and expiry index is per key") {
    kale::MemoryRateLimitStore store({10000, 1s, 200, 16});
    const Clock::time_point start{};
    for (int i = 0; i < 200; ++i) CHECK(store.consume(std::to_string(i), start).allowed);
    for (int i = 0; i < 1000; ++i) CHECK(store.consume("0", start).allowed);
    CHECK_EQ(store.size(), 200);
    CHECK_FALSE(store.consume("new", start).allowed);
    CHECK(store.consume("new", start + 1s).allowed);
    CHECK_EQ(store.size(), 137);
    CHECK(store.consume("new", start + 1s).allowed);
    CHECK_EQ(store.size(), 73);
    CHECK(store.consume("new", start + 1s).allowed);
    CHECK_EQ(store.size(), 9);
    CHECK(store.consume("new", start + 1s).allowed);
    CHECK_EQ(store.size(), 1);
    CHECK_FALSE(store.consume(std::string(17, char(120)), start + 1s).allowed);
    CHECK_EQ(store.size(), 1);
}

TEST_CASE("fixed window validates immutable policy and handles clock arithmetic limits") {
    CHECK_THROWS_AS(kale::MemoryRateLimitStore({0, 1s, 1, 1}), std::invalid_argument);
    CHECK_THROWS_AS(kale::MemoryRateLimitStore({1, 0s, 1, 1}), std::invalid_argument);
    CHECK_THROWS_AS(kale::MemoryRateLimitStore({1, -1s, 1, 1}), std::invalid_argument);
    CHECK_THROWS_AS(kale::MemoryRateLimitStore({1, 1s, 0, 1}), std::invalid_argument);
    CHECK_THROWS_AS(kale::MemoryRateLimitStore({1, 1s, 1, 0}), std::invalid_argument);
    kale::MemoryRateLimitStore store({1, 1500ms, 2, 16});
    CHECK_FALSE(store.consume("a", Clock::time_point::max()).allowed);
    CHECK_EQ(store.consume("a", Clock::time_point::max()).retry_after, 2s);
    CHECK_EQ(store.size(), 0);
    kale::MemoryRateLimitStore edge({1, 1s, 2, 16});
    CHECK(edge.consume("a", Clock::time_point::max() - 1s).allowed);
    CHECK_EQ(edge.consume("a", Clock::time_point::max() - 1ns).retry_after, 1s);
    CHECK_FALSE(edge.consume("a", Clock::time_point::max()).allowed);
    CHECK_EQ(edge.size(), 0);
    kale::MemoryRateLimitStore negative({1, 1s, 2, 16});
    CHECK(negative.consume("a", Clock::time_point::min()).allowed);
    CHECK(negative.consume("a", Clock::time_point{}).allowed);
}

TEST_CASE("concurrent fixed-window debit and transition are atomic") {
    kale::MemoryRateLimitStore store({37, 1s, 16, 16});
    for (int window = 0; window != 2; ++window) {
        std::atomic<int> accepted{0};
        std::barrier start(8);
        std::vector<std::thread> workers;
        for (int i = 0; i != 8; ++i) workers.emplace_back([&] {
            start.arrive_and_wait();
            for (int request = 0; request != 20; ++request)
                if (store.consume("same", Clock::time_point{std::chrono::seconds(window)}).allowed) ++accepted;
        });
        for (auto &worker : workers) worker.join();
        CHECK_EQ(accepted.load(), 37);
        CHECK_EQ(store.size(), 1);
    }
}

TEST_CASE("limiter HTTP uses actual peer, fixed-window Retry-After and route tag partitions") {
    kale::MemoryRateLimitStore store({1, 1500ms, 16, 128});
    std::atomic<long long> ticks{0};
    kale::RateLimiterConfig config;
    config.now = [&] { return Clock::time_point{std::chrono::nanoseconds(ticks.load())}; };
    Server server;
    server.server.use(kale::rate_limiter(store, config));
    server.server.GET("/a", [] { return "a"; });
    server.server.GET("/b", [] { return "b"; });
    server.start();
    CHECK_EQ(server.get("/a", "GET", "1.2.3.4").status, 200);
    auto denied = server.get("/b", "GET", "5.6.7.8");
    CHECK_EQ(denied.status, 429);
    CHECK_EQ(denied.headers["Retry-After"], "2");
    CHECK_EQ(denied.body, "Too Many Requests");
    ticks = 1499999999;
    CHECK_EQ(server.get("/a").headers["Retry-After"], "1");
    ticks = 1500000000;
    CHECK_EQ(server.get("/b").status, 200);

    kale::MemoryRateLimitStore tagged_store({1, 1s, 16, 128});
    kale::RateLimiterConfig tagged;
    tagged.route_tag = "quota";
    tagged.key_extractor = [](const kale::Context &) { return "app-key"; };
    Server partitions;
    partitions.server.use(kale::rate_limiter(tagged_store, std::move(tagged)));
    partitions.server.GET("/x/:id", [] { return "x"; }).tag("quota", "x");
    partitions.server.GET("/y", [] { return "y"; }).tag("quota", "y");
    partitions.start();
    CHECK_EQ(partitions.get("/x/1").status, 200);
    CHECK_EQ(partitions.get("/x/2").status, 429);
    CHECK_EQ(partitions.get("/y").status, 200);
}

TEST_CASE("application custom store receives stable keys and injected time over HTTP") {
    class CustomStore final : public kale::RateLimitStore {
    public:
        kale::RateLimitVerdict consume(std::string_view key, Clock::time_point now) override {
            CHECK_EQ(key, "tenant");
            CHECK_EQ(now, Clock::time_point{42s});
            const auto call = calls.fetch_add(1);
            return {call == 0, 0, 0s};
        }
        std::atomic<int> calls{0};
    } store;
    kale::RateLimiterConfig config;
    config.key_extractor = [](const kale::Context &ctx) { return std::string(ctx.route_tag("tenant")); };
    config.now = [] { return Clock::time_point{42s}; };
    Server server;
    std::atomic<int> downstream{0};
    auto limiter = kale::rate_limiter(store, config);
    server.server.use(limiter);
    server.server.GET("/custom", [&] { ++downstream; return "ok"; }).tag("tenant", "tenant");
    server.start();
    CHECK_EQ(server.get("/custom").status, 200);
    auto denied = server.get("/custom");
    CHECK_EQ(denied.status, 429);
    CHECK_EQ(denied.headers["Retry-After"], "1");
    CHECK_EQ(downstream.load(), 1);
    CHECK_EQ(store.calls.load(), 2);
}

TEST_CASE("metrics snapshots are coherent, concurrent, bounded and escaped") {
    kale::PrometheusRegistry registry(3);
    std::atomic<bool> done{false};
    std::atomic<bool> reader_consistent{true};
    std::thread reader([&] {
        while (!done.load()) {
            for (const auto &sample : registry.snapshot()) {
                if (sample.duration_seconds != static_cast<double>(sample.requests) * 0.25) {
                    reader_consistent.store(false, std::memory_order_relaxed);
                }
            }
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> workers;
    for (int i = 0; i != 8; ++i) workers.emplace_back([&] {
        for (int j = 0; j != 1000; ++j) registry.observe("GET", "/items/:id", 200, 0.25);
    });
    for (auto &worker : workers) worker.join();
    done = true;
    reader.join();
    CHECK(reader_consistent.load());
    auto samples = registry.snapshot();
    REQUIRE_EQ(samples.size(), 1);
    CHECK_EQ(samples[0].requests, 8000);
    const std::string escaped = std::string("/quote") + char(34) + char(92) + char(10);
    registry.observe("BAD\nMETHOD", escaped, 999, 0);
    registry.observe("GET", "/extra", 200, 0);
    registry.observe("POST", std::string(1025, char(120)), 201, 0);
    samples = registry.snapshot();
    REQUIRE_EQ(samples.size(), 3);
    const auto *overflow = find_sample(samples, "<overflow>", 500);
    REQUIRE(overflow);
    CHECK_EQ(overflow->requests, 2);
    const std::string label = std::string("/quote") + char(92) + char(34) + char(92) + char(92) + char(92) + char(110);
    const auto exposition = registry.serialize();
    CHECK_NE(exposition.find(label), std::string::npos);
    CHECK_EQ(exposition.find("BAD"), std::string::npos);
    CHECK_NE(exposition.find("# TYPE http_request_duration_seconds summary"), std::string::npos);
    CHECK_NE(exposition.find("http_request_duration_seconds_count"), std::string::npos);
    CHECK_THROWS_AS(registry.observe("GET", "/", 200, -1), std::invalid_argument);
    CHECK_THROWS_AS(registry.observe("GET", "/", 200, std::numeric_limits<double>::infinity()), std::invalid_argument);
    CHECK_THROWS_AS(registry.observe("GET", "/", 200, std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    CHECK_THROWS_AS(kale::PrometheusRegistry(0), std::invalid_argument);
    kale::PrometheusRegistry only_overflow(1);
    only_overflow.observe("GET", "/a", 200, 1);
    CHECK_EQ(only_overflow.snapshot().size(), 1);
    CHECK_EQ(only_overflow.snapshot()[0].route, "<overflow>");
}

TEST_CASE("metrics record real HTTP route patterns, statuses and exceptions with explicit scrape policy") {
    kale::PrometheusRegistry registry;
    Server server;
    server.server.use(kale::prometheus_metrics(registry));
    server.server.GET("/items/:id", [](kale::Context &ctx) { ctx.set_status(201).string("created"); });
    server.server.GET("/throw", []() -> void { throw std::runtime_error("secret"); });
    server.server.GET("/missing", []() -> void { throw kale::HttpError(404, "missing"); });
    server.server.GET("/committed", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_status(202).string("accepted");
        co_await ctx.reply();
        throw kale::HttpError(409, "after reply");
    });
    server.server.GET("/metrics", kale::prometheus_exporter(registry));
    server.server.GET("/counted", [](kale::Context &ctx) { ctx.set_status(200).string("counted"); });
    server.start();
    CHECK_EQ(server.get("/items/123?secret=one").status, 201);
    CHECK_EQ(server.get("/items/456").status, 201);
    CHECK_EQ(server.get("/throw").status, 500);
    CHECK_EQ(server.get("/missing").status, 404);
    CHECK_EQ(server.get("/committed").status, 202);
    CHECK_EQ(server.get("/unmatched-one").status, 404);
    CHECK_EQ(server.get("/unmatched-two").status, 404);
    auto scrape = server.get("/metrics");
    CHECK_EQ(scrape.status, 200);
    CHECK_EQ(scrape.headers["Content-Type"], "text/plain; version=0.0.4; charset=utf-8");
    CHECK_EQ(scrape.body.find("/items/123"), std::string::npos);
    CHECK_EQ(scrape.body.find("secret"), std::string::npos);
    auto samples = registry.snapshot();
    const auto *items = find_sample(samples, "/items/:id", 201);
    REQUIRE(items);
    CHECK_EQ(items->requests, 2);
    REQUIRE(find_sample(samples, "/throw", 500));
    REQUIRE(find_sample(samples, "/missing", 404));
    REQUIRE(find_sample(samples, "/committed", 202));
    const auto *unmatched = find_sample(samples, "<unmatched>", 404);
    REQUIRE(unmatched);
    CHECK_EQ(unmatched->requests, 2);
    CHECK_FALSE(find_sample(samples, "/metrics", 200));
    const auto before = total(registry);
    CHECK_EQ(server.get("/counted").status, 200);
    CHECK_EQ(server.get("/metrics").status, 200);
    CHECK_EQ(total(registry), before + 1);
    for (const auto &sample : registry.snapshot()) {
        CHECK(std::isfinite(sample.duration_seconds));
        CHECK_GE(sample.duration_seconds, 0);
    }
}

TEST_CASE("readiness is application-owned, can flip and hides callback exceptions") {
    kale::ReadinessState ready;
    Server server;
    server.server.GET("/health", kale::health_handler());
    server.server.HEAD("/health", kale::health_handler());
    server.server.GET("/ready", kale::readiness_handler(ready));
    server.server.HEAD("/ready", kale::readiness_handler(ready));
    server.server.GET("/throw-ready", kale::readiness_handler([]() -> bool { throw std::runtime_error("password-secret"); }));
    server.start();
    CHECK_EQ(server.get("/health").status, 200);
    CHECK_EQ(server.get("/health").body, "OK");
    CHECK_EQ(server.get("/health", "HEAD").status, 200);
    auto unavailable = server.get("/ready");
    CHECK_EQ(unavailable.status, 503);
    CHECK_EQ(unavailable.body, "Not Ready");
    CHECK_EQ(server.get("/ready", "HEAD").status, 503);
    ready.set_ready(true);
    CHECK_EQ(server.get("/ready").status, 200);
    CHECK_EQ(server.get("/ready").body, "OK");
    ready.set_ready(false);
    CHECK_EQ(server.get("/ready").status, 503);
    auto failure = server.get("/throw-ready");
    CHECK_EQ(failure.status, 503);
    CHECK_EQ(failure.body, "Not Ready");
    CHECK_EQ(failure.body.find("secret"), std::string::npos);
    CHECK_THROWS_AS(kale::readiness_handler(std::function<bool()>{}), std::invalid_argument);
}

TEST_CASE("readiness callback completes after a peer disconnect and state remains usable") {
    std::promise<void> entered;
    auto entered_fut = entered.get_future();
    std::atomic<bool> release{false};
    std::promise<void> finished;
    auto finished_fut = finished.get_future();
    std::atomic<bool> ready{false};
    Server server;
    server.server.use([&](kale::Context &, kale::Next next) -> coke::Task<void> {
        entered.set_value();
        while (!release.load()) {
            co_await coke::sleep(1ms);
        }
        co_await next();
        finished.set_value();
    });
    server.server.GET("/ready", kale::readiness_handler([&] { return ready.load(); }));
    server.start();
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE_GE(fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(server.server.listen_port()));
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    REQUIRE_EQ(::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    const std::string raw = "GET /ready HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    REQUIRE_EQ(::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL), static_cast<ssize_t>(raw.size()));
    entered_fut.wait();
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    ready = true;
    release = true;
    finished_fut.wait();
    CHECK(ready.load());
}
} // TEST_SUITE
