#include "doctest/doctest.h"

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "kale/core/http_server.h"
#include "kale/core/logger.h"
#include "kale/core/router.h"

TEST_CASE("Logger level filtering, sinks, and exception safety") {
    SUBCASE("Logger level threshold filtering") {
        kale::Logger logger(kale::LogLevel::Warn);
        CHECK_FALSE(logger.enabled(kale::LogLevel::Trace));
        CHECK_FALSE(logger.enabled(kale::LogLevel::Debug));
        CHECK_FALSE(logger.enabled(kale::LogLevel::Info));
        CHECK(logger.enabled(kale::LogLevel::Warn));
        CHECK(logger.enabled(kale::LogLevel::Error));
        CHECK_FALSE(logger.enabled(kale::LogLevel::Off)); // Off must never compare as enabled
    }

    SUBCASE("Logger::off() is disabled for all levels") {
        auto logger = kale::Logger::off();
        CHECK_FALSE(logger.enabled(kale::LogLevel::Trace));
        CHECK_FALSE(logger.enabled(kale::LogLevel::Debug));
        CHECK_FALSE(logger.enabled(kale::LogLevel::Info));
        CHECK_FALSE(logger.enabled(kale::LogLevel::Warn));
        CHECK_FALSE(logger.enabled(kale::LogLevel::Error));
        CHECK_FALSE(logger.enabled(kale::LogLevel::Off));
    }

    SUBCASE("Custom Sink receives LogContext and message") {
        struct Entry {
            kale::LogLevel level;
            std::string component;
            std::string method;
            std::string path;
            int status_code{0};
            std::string message;
        };

        std::vector<Entry> entries;
        kale::Logger logger(kale::LogLevel::Info, [&](kale::LogLevel lvl, const kale::LogContext &ctx, std::string_view msg) {
            entries.push_back(Entry{
                lvl,
                std::string(ctx.component),
                std::string(ctx.method),
                std::string(ctx.path),
                ctx.status_code,
                std::string(msg)
            });
        });

        // Debug filtered out
        logger.log(kale::LogLevel::Debug, "test", "should not appear");
        CHECK(entries.empty());

        // Info with LogContext recorded
        kale::LogContext ctx{"http_server", "POST", "/api/items", 201};
        logger.log(kale::LogLevel::Info, ctx, "created item");
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].level == kale::LogLevel::Info);
        CHECK(entries[0].component == "http_server");
        CHECK(entries[0].method == "POST");
        CHECK(entries[0].path == "/api/items");
        CHECK(entries[0].status_code == 201);
        CHECK(entries[0].message == "created item");
    }

    SUBCASE("Sink throwing exception is isolated safely") {
        kale::Logger logger(kale::LogLevel::Error, [](kale::LogLevel, const kale::LogContext &, std::string_view) {
            throw std::runtime_error("broken sink");
        });

        CHECK_NOTHROW(logger.log(kale::LogLevel::Error, "core", "test message"));
    }
}

TEST_CASE("Router format_routes row order and metadata parsing") {
    kale::Router router;

    auto dummy_handler = [](kale::Context &ctx) { ctx.string("ok"); };

    router.POST("/users", dummy_handler);
    router.GET("/users", dummy_handler);
    router.DELETE("/users", dummy_handler);
    router.GET("/api/v1/health", dummy_handler);
    router.GET("/", dummy_handler);

    std::string formatted = router.format_routes();
    CHECK_FALSE(formatted.empty());

    // Parse lines from formatted table
    std::istringstream iss(formatted);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    struct ParsedRoute {
        std::string method;
        std::string path;
    };
    std::vector<ParsedRoute> routes;
    for (const auto &l : lines) {
        std::istringstream row(l);
        std::string m, p;
        if (row >> m >> p) {
            if (m == "GET" || m == "POST" || m == "DELETE" || m == "PUT" || m == "PATCH" || m == "HEAD" || m == "OPTIONS") {
                routes.push_back({m, p});
            }
        }
    }

    REQUIRE_EQ(routes.size(), 5);

    // Primary sort: path ascending ("/" -> "/api/v1/health" -> "/users")
    CHECK(routes[0].path == "/");
    CHECK(routes[1].path == "/api/v1/health");
    CHECK(routes[2].path == "/users");
    CHECK(routes[3].path == "/users");
    CHECK(routes[4].path == "/users");

    // Secondary sort: standard method order on same path (GET -> POST -> DELETE)
    CHECK(routes[2].method == "GET");
    CHECK(routes[3].method == "POST");
    CHECK(routes[4].method == "DELETE");
}

TEST_CASE("HttpServer banner execution, port observation, and printer exception isolation") {
    SUBCASE("Default server with show_banner=false emits no banner output on start") {
        kale::HttpServerParams params;
        params.set_show_banner(false);
        params.set_logger(kale::Logger::off());

        kale::HttpServer server(params);
        CHECK_EQ(server.start("127.0.0.1", 0), 0);
        CHECK(server.is_running());
        CHECK_GT(server.listen_port(), 0);
        server.stop();
        CHECK_FALSE(server.is_running());
    }

    SUBCASE("Custom banner printer is invoked after successful bind with running server and effective port") {
        kale::HttpServerParams params;
        params.set_show_banner(true);
        params.set_logger(kale::Logger::off());

        bool callback_ran = false;
        int observed_port = 0;
        bool observed_running = false;

        params.set_banner_printer([&](const kale::HttpServer &srv) {
            callback_ran = true;
            observed_running = srv.is_running();
            observed_port = srv.listen_port();
        });

        kale::HttpServer server(params);
        CHECK_EQ(server.start("127.0.0.1", 0), 0);
        CHECK(callback_ran);
        CHECK(observed_running);
        CHECK_GT(observed_port, 0);
        CHECK_EQ(observed_port, server.listen_port());

        server.stop();
        CHECK_FALSE(server.is_running());
    }

    SUBCASE("Throwing banner printer is safely isolated without failing server startup") {
        kale::HttpServerParams params;
        params.set_show_banner(true);
        params.set_logger(kale::Logger::off());
        params.set_banner_printer([](const kale::HttpServer &) {
            throw std::runtime_error("printer failure simulation");
        });

        kale::HttpServer server(params);
        CHECK_EQ(server.start("127.0.0.1", 0), 0);
        CHECK(server.is_running());
        CHECK_GT(server.listen_port(), 0);

        server.stop();
        CHECK_FALSE(server.is_running());
    }
}
