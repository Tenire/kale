#include "kale/core/http_server.h"
#include "kale/core/http_util.h"
#include "kale/core/http_error.h"
#include "kale/core/error_response.h"
#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "workflow/WFHttpServer.h"

namespace kale {

HttpServer::HttpServer()
    : HttpServer(HttpServerParams{}) {
}

HttpServer::HttpServer(const HttpServerParams &params)
    : params_(params) {
    if (!params_.explicit_error_policy) {
        params_.error_policy.logger = params_.logger;
    }
    coke::ServerParams server_params = HTTP_SERVER_PARAMS_DEFAULT;
    if (params_.max_connections > 0) {
        server_params.max_connections = params_.max_connections;
    }
    server_params.peer_response_timeout = params_.peer_response_timeout;
    server_params.receive_timeout = params_.receive_timeout;
    server_params.keep_alive_timeout = params_.keep_alive_timeout;
    server_params.request_size_limit = params_.request_size_limit;
    server_params.ssl_accept_timeout = params_.ssl_accept_timeout;

    server_ = std::make_unique<coke::BasicServer<protocol::HttpRequest, protocol::HttpResponse>>(
        server_params,
        [this](ServerContext ctx) -> coke::Task<void> {
            return this->process(std::move(ctx));
        }
    );
}

HttpServer::~HttpServer() {
    stop();
}

HttpServer &HttpServer::use(Middleware mw) {
    middlewares_.push_back(std::move(mw));
    return *this;
}

int HttpServer::start(unsigned short port) {
    return start(AF_INET, nullptr, port, nullptr, nullptr);
}

int HttpServer::start(const char *host, unsigned short port) {
    return start(AF_UNSPEC, host, port, nullptr, nullptr);
}

int HttpServer::start(int family, const char *host, unsigned short port, const char *cert_file, const char *key_file) {
    if (!server_) {
        return -1;
    }
    int ret = 0;
    if (cert_file && key_file) {
        ret = server_->start(family, host, port, cert_file, key_file);
    } else if (cert_file || key_file) {
        return -1; // Must provide both cert_file and key_file for SSL/TLS
    } else {
        ret = server_->start(family, host, port);
    }
    if (!params_.explicit_error_policy) {
        params_.error_policy.logger = params_.logger;
    }
    if (ret == 0) {
        is_running_ = true;
        is_ssl_ = (cert_file != nullptr && key_file != nullptr);
        start_time_ = std::chrono::steady_clock::now();
        if (params_.logger.enabled(LogLevel::Info)) {
            params_.logger.log(LogLevel::Info, "http_server",
                "Server started on " + listen_host() + ":" + std::to_string(listen_port()));
        }
        print_banner();
    }
    return ret;
}

void HttpServer::stop() {
    if (is_running_ && server_) {
        if (params_.logger.enabled(LogLevel::Info)) {
            params_.logger.log(LogLevel::Info, "http_server", "Server stopped");
        }
        is_running_ = false;
        server_->stop();
    }
}

void HttpServer::print_banner() const {
    if (!params_.show_banner) {
        return;
    }
    if (params_.banner_printer) {
        try {
            params_.banner_printer(*this);
        } catch (...) {}
        return;
    }
    if (!params_.banner_text.empty()) {
        std::cout << params_.banner_text << "\n";
    }
    const std::string scheme = is_ssl_ ? "https://" : "http://";
    std::cout << "Kale HTTP Server listening on " << scheme << listen_host() << ":" << listen_port() << "\n";
    std::string routes = format_routes();
    if (!routes.empty()) {
        std::cout << routes << "\n";
    }
}

int HttpServer::listen_port() const noexcept {
    if (!server_) return -1;
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (server_->get_listen_addr(reinterpret_cast<struct sockaddr *>(&ss), &len) != 0) {
        return -1;
    }
    if (ss.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<struct sockaddr_in *>(&ss)->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<struct sockaddr_in6 *>(&ss)->sin6_port);
    }
    return -1;
}

std::string HttpServer::listen_host() const {
    if (!server_) return "";
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (server_->get_listen_addr(reinterpret_cast<struct sockaddr *>(&ss), &len) != 0) {
        return "";
    }
    char buf[INET6_ADDRSTRLEN];
    if (ss.ss_family == AF_INET) {
        if (inet_ntop(AF_INET, &(reinterpret_cast<struct sockaddr_in *>(&ss)->sin_addr), buf, sizeof(buf))) {
            return buf;
        }
    } else if (ss.ss_family == AF_INET6) {
        if (inet_ntop(AF_INET6, &(reinterpret_cast<struct sockaddr_in6 *>(&ss)->sin6_addr), buf, sizeof(buf))) {
            return buf;
        }
    }
    return "";
}

void HttpServer::wait_finish() {
    if (server_) {
        server_->wait_finish();
        is_running_ = false;
    }
}

size_t HttpServer::conn_count() const noexcept {
    return server_ ? server_->get_conn_count() : 0;
}

long long HttpServer::uptime_seconds() const noexcept {
    if (!is_running_) return 0;
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
}

void HttpServer::apply_conn_governance(Context &ctx) {
    if (!params_.connection_governance) return;
    bool should_close = false;
    auto *task = ctx.coke_context().get_task();
    if (!task) return;

    if (params_.max_requests_per_connection > 0 &&
        task->get_task_seq() >= static_cast<long long>(params_.max_requests_per_connection - 1)) {
        should_close = true;
    }
    if (server_ && params_.high_watermark_ratio > 0.0 && params_.high_watermark_ratio <= 1.0) {
        size_t max_conns = params_.max_connections > 0 ? params_.max_connections : 2000;
        size_t threshold = static_cast<size_t>(max_conns * params_.high_watermark_ratio);
        if (server_->get_conn_count() >= threshold) {
            should_close = true;
        }
    }

    if (should_close) {
        task->set_keep_alive(0);
        if (!ctx.has_resp_header("Connection")) {
            ctx.set_header("Connection", "close");
        }
    }
}

coke::Task<void> HttpServer::process(ServerContext server_ctx) {
    Context ctx(std::move(server_ctx));
    ctx.set_error_config(&params_.error_formatter, &params_.error_policy);

    std::string_view req_path = ctx.path();
    if (req_path.find("/../") != std::string_view::npos || req_path.ends_with("/..")) {
        reply_error(ctx, HttpError::forbidden("Forbidden: Path traversal rejected", "FORBIDDEN"));
        apply_conn_governance(ctx);
        (void)co_await ctx.reply();
        co_return;
    }

    std::string cleaned_path_storage;
    if (params_.clean_path && PathUtil::needs_normalization(req_path, true)) {
        cleaned_path_storage = PathUtil::clean_path(req_path);
        if (req_path.size() > 1 && req_path.back() == '/' && cleaned_path_storage != "/") {
            cleaned_path_storage.push_back('/');
        }
        req_path = cleaned_path_storage;
    }

    RouteResult result = router_.route(ctx.method(), req_path);
    for (const auto &[k, v] : result.params) {
        if (v == ".." || v.find("/../") != std::string_view::npos || v.starts_with("../") || v.ends_with("/..")) {
            reply_error(ctx, HttpError::forbidden("Forbidden: Path traversal rejected in parameters", "FORBIDDEN"));
            apply_conn_governance(ctx);
            (void)co_await ctx.reply();
            co_return;
        }
    }
    ctx.matched_endpoint_ = result.endpoint;
    ctx.route_params_ = std::move(result.params);
    std::exception_ptr ep;

    try {
        if (result.matched && result.endpoint && result.endpoint->handler) {
            ctx.prepare_chunked_body_if_needed();
            if (result.endpoint->max_body_size > 0 && ctx.req_body().size() > result.endpoint->max_body_size) {
                reply_error(ctx, HttpError::payload_too_large("Payload Too Large", "PAYLOAD_TOO_LARGE"));
                apply_conn_governance(ctx);
                (void)co_await ctx.reply();
                co_return;
            }

            if (middlewares_.empty() && result.endpoint->middlewares.empty()) {
                if (result.endpoint->handler.is_sync()) {
                    result.endpoint->handler.invoke_sync(ctx);
                } else {
                    co_await result.endpoint->handler.invoke_async(ctx);
                }
            } else {
                co_await compose_and_run(middlewares_, result.endpoint->middlewares, ctx, result.endpoint->handler);
            }
        } else {
            const bool has_consumer = !middlewares_.empty() ||
                (result.method_not_allowed ? static_cast<bool>(method_not_allowed_handler_)
                                           : static_cast<bool>(not_found_handler_));
            if (has_consumer) {
                ctx.prepare_chunked_body_if_needed();
            }
            auto fallback_handler = [this, method_not_allowed = result.method_not_allowed, allow_hdr = result.allow_header()](
                                        Context &fallback_ctx) -> coke::Task<void> {
                if (method_not_allowed) {
                    fallback_ctx.set_header("Allow", allow_hdr);
                    if (method_not_allowed_handler_) {
                        if (method_not_allowed_handler_.is_sync()) {
                            method_not_allowed_handler_.invoke_sync(fallback_ctx);
                        } else {
                            co_await method_not_allowed_handler_.invoke_async(fallback_ctx);
                        }
                    } else {
                        reply_error(fallback_ctx, HttpError::method_not_allowed(allow_hdr));
                    }
                } else {
                    if (not_found_handler_) {
                        if (not_found_handler_.is_sync()) {
                            not_found_handler_.invoke_sync(fallback_ctx);
                        } else {
                            co_await not_found_handler_.invoke_async(fallback_ctx);
                        }
                    } else {
                        reply_error(fallback_ctx, HttpError::not_found());
                    }
                }
            };

            if (middlewares_.empty()) {
                bool is_sync = false;
                if (result.method_not_allowed) {
                    is_sync = !method_not_allowed_handler_ || method_not_allowed_handler_.is_sync();
                } else {
                    is_sync = !not_found_handler_ || not_found_handler_.is_sync();
                }

                if (is_sync) {
                    if (result.method_not_allowed) {
                        ctx.set_header("Allow", result.allow_header());
                        if (method_not_allowed_handler_) {
                            method_not_allowed_handler_.invoke_sync(ctx);
                        } else {
                            reply_error(ctx, HttpError::method_not_allowed(result.allow_header()));
                        }
                    } else {
                        if (not_found_handler_) {
                            not_found_handler_.invoke_sync(ctx);
                        } else {
                            reply_error(ctx, HttpError::not_found());
                        }
                    }
                } else {
                    co_await fallback_handler(ctx);
                }
            } else {
                co_await compose_and_run(middlewares_, {}, ctx, Handler::from_async(fallback_handler));
            }
        }

        if (ctx.is_stream()) {
            co_await ctx.end_chunk();
        }
        if (!ctx.is_replied()) {
            apply_conn_governance(ctx);
            (void)co_await ctx.reply();
        }
    } catch (...) {
        ep = std::current_exception();
    }

    if (ep) {
        if (ctx.is_stream()) {
            ctx.abort_stream();
        } else if (!ctx.is_replied()) {
            try {
                reply_error(ctx, ep);
                apply_conn_governance(ctx);
                (void)co_await ctx.reply();
            } catch (...) {
                if (auto *task = ctx.coke_context().get_task()) {
                    task->noreply();
                }
                ctx.abort_stream();
            }
        }
    }
}

} // namespace kale
