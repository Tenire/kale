#ifndef KALE_CORE_HTTP_SERVER_H
#define KALE_CORE_HTTP_SERVER_H

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "coke/net/basic_server.h"
#include "coke/task.h"
#include "kale/core/context.h"
#include "kale/core/router.h"
#include "kale/core/error_response.h"
#include "kale/core/logger.h"

namespace kale {
class Blueprint;

class HttpServer;
using BannerPrinter = std::function<void(const HttpServer &)>;


struct HttpServerParams {
    size_t max_connections{2000};
    int peer_response_timeout{10 * 1000}; // 10s default matching Workflow
    int receive_timeout{-1};
    int keep_alive_timeout{60 * 1000};
    size_t request_size_limit{static_cast<size_t>(-1)};
    int ssl_accept_timeout{10 * 1000};
    bool connection_governance{true};
    size_t max_requests_per_connection{10000};
    double high_watermark_ratio{0.95};

    bool clean_path{true};

    Logger logger{LogLevel::Warn};
    bool show_banner{false};
    std::string banner_text;
    BannerPrinter banner_printer{nullptr};

    ErrorFormatter error_formatter{nullptr};
    ErrorPolicy error_policy{};
    bool explicit_error_policy{false};
    HttpServerParams &set_max_connections(size_t max_conn) noexcept { max_connections = max_conn; return *this; }
    HttpServerParams &set_peer_response_timeout(int ms) noexcept { peer_response_timeout = ms; return *this; }
    HttpServerParams &set_receive_timeout(int ms) noexcept { receive_timeout = ms; return *this; }
    HttpServerParams &set_keep_alive_timeout(int ms) noexcept { keep_alive_timeout = ms; return *this; }
    HttpServerParams &set_request_size_limit(size_t bytes) noexcept { request_size_limit = bytes; return *this; }
    HttpServerParams &set_ssl_accept_timeout(int ms) noexcept { ssl_accept_timeout = ms; return *this; }
    HttpServerParams &set_clean_path(bool enable) noexcept { clean_path = enable; return *this; }
    HttpServerParams &set_logger(Logger l) noexcept {
        logger = std::move(l);
        if (!explicit_error_policy) {
            error_policy.logger = logger;
        }
        return *this;
    }
    HttpServerParams &set_show_banner(bool enable) noexcept { show_banner = enable; return *this; }
    HttpServerParams &set_banner_text(std::string text) { banner_text = std::move(text); return *this; }
    HttpServerParams &set_banner_printer(BannerPrinter printer) { banner_printer = std::move(printer); return *this; }
    HttpServerParams &set_error_formatter(ErrorFormatter f) { error_formatter = std::move(f); return *this; }
    HttpServerParams &set_error_policy(ErrorPolicy p) {
        error_policy = std::move(p);
        explicit_error_policy = true;
        return *this;
    }
};

class HttpServer {
public:
    HttpServer();
    explicit HttpServer(const HttpServerParams &params);
    ~HttpServer();

    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;
    HttpServer(HttpServer &&) = delete;
    HttpServer &operator=(HttpServer &&) = delete;

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder GET(std::string path, Args&&... args) {
        return router_.GET(std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder POST(std::string path, Args&&... args) {
        return router_.POST(std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder PUT(std::string path, Args&&... args) {
        return router_.PUT(std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder DELETE(std::string path, Args&&... args) {
        return router_.DELETE(std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder PATCH(std::string path, Args&&... args) {
        return router_.PATCH(std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder HEAD(std::string path, Args&&... args) {
        return router_.HEAD(std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder OPTIONS(std::string path, Args&&... args) {
        return router_.OPTIONS(std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder route(HttpMethod method, std::string path, Args&&... args) {
        return router_.route(method, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    HttpServer &any(std::string path, Args&&... args) {
        router_.any(std::move(path), std::forward<Args>(args)...);
        return *this;
    }

    HttpServer &mount(std::string_view prefix, Blueprint blueprint);
    HttpServer &mount(Blueprint blueprint);

    HttpServer &use(Middleware mw);

    template <typename M>
        requires MiddlewareCallable<M>
    HttpServer &use(M &&m) {
        return use(make_middleware(std::forward<M>(m)));
    }

    int listen_port() const noexcept;
    std::string listen_host() const;

    template <HandlerCallable H>
    HttpServer &set_not_found_handler(H &&handler) {
        not_found_handler_ = make_handler(std::forward<H>(handler));
        return *this;
    }

    template <HandlerCallable H>
    HttpServer &set_method_not_allowed_handler(H &&handler) {
        method_not_allowed_handler_ = make_handler(std::forward<H>(handler));
        return *this;
    }

    HttpServer &set_logger(Logger logger) noexcept {
        params_.logger = std::move(logger);
        if (!params_.explicit_error_policy) {
            params_.error_policy.logger = params_.logger;
        }
        return *this;
    }
    const Logger &logger() const noexcept { return params_.logger; }

    HttpServer &set_show_banner(bool enable) noexcept {
        params_.show_banner = enable;
        return *this;
    }

    HttpServer &set_banner_text(std::string text) {
        params_.banner_text = std::move(text);
        return *this;
    }

    HttpServer &set_banner_printer(BannerPrinter printer) {
        params_.banner_printer = std::move(printer);
        return *this;
    }

    HttpServer &set_error_formatter(ErrorFormatter f) {
        params_.error_formatter = std::move(f);
        return *this;
    }

    HttpServer &set_error_policy(ErrorPolicy p) {
        params_.error_policy = std::move(p);
        params_.explicit_error_policy = true;
        return *this;
    }

    HttpServer &set_error_handler(ErrorFormatter formatter, ErrorPolicy policy = {}) {
        params_.error_formatter = std::move(formatter);
        params_.error_policy = std::move(policy);
        params_.explicit_error_policy = true;
        return *this;
    }

    [[nodiscard]] std::string format_routes() const {
        return router_.format_routes();
    }

    void list_routes(std::ostream &os = std::cout) const {
        router_.list_routes(os);
    }

    int start(unsigned short port);
    int start(const char *host, unsigned short port);
    int start(int family, const char *host, unsigned short port, const char *cert_file = nullptr, const char *key_file = nullptr);
    void stop();
    void wait_finish();

    bool is_running() const noexcept { return is_running_; }
    bool is_ssl() const noexcept { return is_ssl_; }
    size_t conn_count() const noexcept;
    long long uptime_seconds() const noexcept;

    Router &router() noexcept { return router_; }
    const Router &router() const noexcept { return router_; }

private:
    coke::Task<void> process(ServerContext ctx);
    void apply_conn_governance(Context &ctx);
    void print_banner() const;

    HttpServerParams params_;
    Router router_;
    std::vector<Middleware> middlewares_;
    Handler not_found_handler_;
    Handler method_not_allowed_handler_;

    std::unique_ptr<coke::BasicServer<protocol::HttpRequest, protocol::HttpResponse>> server_;
    std::chrono::steady_clock::time_point start_time_;
    bool is_running_{false};
    bool is_ssl_{false};
};

} // namespace kale

#endif // KALE_CORE_HTTP_SERVER_H
