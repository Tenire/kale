#include "kale/client/reverse_proxy.h"
#include "kale/core/error_response.h"
#include "kale/core/http_error.h"

#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "core/stream_access.h"
#include "kale/core/context.h"
#include "kale/core/http_status.h"
#include "workflow/URIParser.h"
namespace kale {
class ProxyStreamBridge {
public:
    static std::function<void()> make_cancel_hook(const HttpClientResponse &resp) {
        return resp.make_cancel_hook();
    }
};
} // namespace kale

namespace {
bool is_standard_hop_by_hop(std::string_view name) noexcept {
    static constexpr std::string_view standard_hops[] = {
        "connection",
        "keep-alive",
        "proxy-authenticate",
        "proxy-authorization",
        "te",
        "trailer",
        "trailers",
        "transfer-encoding",
        "upgrade"
    };
    for (const auto &h : standard_hops) {
        if (h.size() == name.size() && strncasecmp(h.data(), name.data(), h.size()) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> parse_connection_tokens(std::string_view connection_header) {
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start < connection_header.size()) {
        size_t comma = connection_header.find(',', start);
        size_t end = (comma == std::string_view::npos) ? connection_header.size() : comma;
        while (start < end && std::isspace(static_cast<unsigned char>(connection_header[start]))) {
            ++start;
        }
        size_t token_end = end;
        while (token_end > start && std::isspace(static_cast<unsigned char>(connection_header[token_end - 1]))) {
            --token_end;
        }
        if (token_end > start) {
            tokens.emplace_back(connection_header.substr(start, token_end - start));
        }
        start = (comma == std::string_view::npos) ? connection_header.size() : comma + 1;
    }
    return tokens;
}

bool is_hop_by_hop_header(std::string_view name, const std::vector<std::string> &custom_tokens) noexcept {
    if (is_standard_hop_by_hop(name)) {
        return true;
    }
    for (const auto &tok : custom_tokens) {
        if (tok.size() == name.size() && strncasecmp(tok.data(), name.data(), tok.size()) == 0) {
            return true;
        }
    }
    return false;
}
class ProxyStreamCloseGuard {
public:
    explicit ProxyStreamCloseGuard(kale::Context &ctx) noexcept : ctx_(ctx) {}
    ~ProxyStreamCloseGuard() {
        if (!cleared_) {
            kale::StreamAccess::clear_close_hook(ctx_);
        }
    }
    void clear() noexcept {
        if (!cleared_) {
            cleared_ = true;
            kale::StreamAccess::clear_close_hook(ctx_);
        }
    }
    ProxyStreamCloseGuard(const ProxyStreamCloseGuard &) = delete;
    ProxyStreamCloseGuard &operator=(const ProxyStreamCloseGuard &) = delete;
private:
    kale::Context &ctx_;
    bool cleared_{false};
};



void reset_stale_representation_headers(kale::Context &ctx) noexcept {
    ctx.erase_resp_header("Content-Length");
    ctx.erase_resp_header("Transfer-Encoding");
    ctx.erase_resp_header("Content-Encoding");
    ctx.erase_resp_header("Content-Range");
    ctx.erase_resp_header("ETag");
    ctx.erase_resp_header("Trailer");
    ctx.erase_resp_header("Content-MD5");
    ctx.erase_resp_header("Content-Type");
}

} // namespace

namespace kale {

Handler proxy(HttpClient &upstream, ProxyOptions options) {
    ParsedURI target_uri;
    if (URIParser::parse(options.target_base_url, target_uri) != 0 || !target_uri.scheme || !target_uri.host ||
        (strcasecmp(target_uri.scheme, "http") != 0 && strcasecmp(target_uri.scheme, "https") != 0)) {
        throw std::invalid_argument("Invalid target_base_url for reverse proxy: " + options.target_base_url);
    }
    if (target_uri.port && target_uri.port[0]) {
        char *end = nullptr;
        long port_val = std::strtol(target_uri.port, &end, 10);
        if (*end != '\0' || port_val <= 0 || port_val > 65535) {
            throw std::invalid_argument("Invalid port in target_base_url for reverse proxy: " + options.target_base_url);
        }
    }

    std::string upstream_authority = target_uri.host;
    if (target_uri.port && target_uri.port[0]) {
        upstream_authority += ":";
        upstream_authority += target_uri.port;
    }

    std::string scheme = target_uri.scheme;
    std::string base_path = target_uri.path ? target_uri.path : "";
    while (base_path.size() > 1 && base_path.back() == '/') {
        base_path.pop_back();
    }
    if (base_path == "/") {
        base_path.clear();
    }

    std::string base_query = target_uri.query ? target_uri.query : "";
    std::string userinfo = target_uri.userinfo ? (std::string(target_uri.userinfo) + "@") : "";

    auto handler_fn = [upstream_ptr = &upstream,
                       opt = std::move(options),
                       scheme = std::move(scheme),
                       userinfo = std::move(userinfo),
                       upstream_authority = std::move(upstream_authority),
                       base_path = std::move(base_path),
                       base_query = std::move(base_query)](Context &ctx) -> coke::Task<void> {
        std::string_view req_path = ctx.path();
        std::string subpath;
        if (!opt.strip_prefix.empty() && req_path.starts_with(opt.strip_prefix)) {
            subpath = std::string(req_path.substr(opt.strip_prefix.size()));
        } else {
            std::string_view pattern = ctx.route_pattern();
            size_t star_pos = pattern.find("/*");
            if (star_pos != std::string_view::npos && star_pos > 0 &&
                req_path.starts_with(pattern.substr(0, star_pos))) {
                subpath = std::string(req_path.substr(star_pos));
            } else {
                subpath = std::string(req_path);
            }
        }
        if (subpath.empty() || subpath.front() != '/') {
            subpath = "/" + subpath;
        }
        std::string full_path = base_path + subpath;

        std::string target_url = scheme + "://" + userinfo + upstream_authority + full_path;

        size_t qpos = ctx.raw_uri().find('?');
        std::string incoming_query;
        if (qpos != std::string_view::npos) {
            incoming_query = std::string(ctx.raw_uri().substr(qpos + 1));
        }

        if (!base_query.empty() && !incoming_query.empty()) {
            target_url += "?" + base_query + "&" + incoming_query;
        } else if (!base_query.empty()) {
            target_url += "?" + base_query;
        } else if (!incoming_query.empty()) {
            target_url += "?" + incoming_query;
        }

        HttpClientRequest req = upstream_ptr->request(std::string(ctx.method_string()), target_url);

        std::string conn_hdr = ctx.header("Connection");
        std::vector<std::string> req_hop_tokens = parse_connection_tokens(conn_hdr);

        for (const auto &[key, val] : ctx.headers()) {
            if (is_hop_by_hop_header(key, req_hop_tokens)) {
                continue;
            }
            if (strcasecmp(key.c_str(), "Host") == 0) {
                continue;
            }
            if (strcasecmp(key.c_str(), "Content-Length") == 0) {
                continue;
            }
            if (strncasecmp(key.c_str(), "X-Forwarded-", 12) == 0) {
                continue;
            }
            req.header(key, val);
        }

        if (opt.preserve_host && ctx.has_header("Host")) {
            req.header("Host", ctx.header("Host"));
        } else {
            req.header("Host", upstream_authority);
        }

        std::string client_ip = ctx.peer_addr();
        if (client_ip.empty() || client_ip == "Unknown") {
            client_ip = "127.0.0.1";
        }

        if (opt.trust_downstream_forwarded) {
            if (ctx.has_header("X-Forwarded-For")) {
                req.header("X-Forwarded-For", ctx.header("X-Forwarded-For") + ", " + client_ip);
            } else {
                req.header("X-Forwarded-For", client_ip);
            }
            if (ctx.has_header("X-Forwarded-Proto")) {
                req.header("X-Forwarded-Proto", ctx.header("X-Forwarded-Proto"));
            } else {
                req.header("X-Forwarded-Proto", "http");
            }
            if (ctx.has_header("X-Forwarded-Host")) {
                req.header("X-Forwarded-Host", ctx.header("X-Forwarded-Host"));
            } else if (ctx.has_header("Host")) {
                req.header("X-Forwarded-Host", ctx.header("Host"));
            }
        } else {
            req.header("X-Forwarded-For", client_ip);
            req.header("X-Forwarded-Proto", "http");
            if (ctx.has_header("Host")) {
                req.header("X-Forwarded-Host", ctx.header("Host"));
            }
        }

        std::string_view body_view = ctx.req_body();
        if (!body_view.empty()) {
            req.body(std::string(body_view));
        }
        if (opt.timeout.count() > 0) {
            req.timeout(opt.timeout);
        }
        req.max_body_bytes(opt.max_body_bytes);

        if (opt.rewrite_request) {
            opt.rewrite_request(ctx, req);
        }

        HttpClientResponse up_resp;
        try {
            up_resp = co_await req.send();
        } catch (const HttpTimeoutError &) {
            if (!ctx.is_replied() && !ctx.is_stream()) {
                reply_error(ctx, HttpError(HttpStatus::GatewayTimeout, "Gateway Timeout", "GATEWAY_TIMEOUT"));
            }
            co_return;
        } catch (const HttpClientError &) {
            if (!ctx.is_replied() && !ctx.is_stream()) {
                reply_error(ctx, HttpError(HttpStatus::BadGateway, "Bad Gateway", "BAD_GATEWAY"));
            }
            co_return;
        } catch (...) {
            if (!ctx.is_replied() && !ctx.is_stream()) {
                reply_error(ctx, HttpError(HttpStatus::BadGateway, "Bad Gateway", "BAD_GATEWAY"));
            }
            co_return;
        }

        ctx.set_status(up_resp.status());

        std::string resp_conn_hdr = up_resp.header("Connection");
        std::vector<std::string> resp_hop_tokens = parse_connection_tokens(resp_conn_hdr);

        for (const auto &[k, v] : up_resp.headers()) {
            if (is_hop_by_hop_header(k, resp_hop_tokens)) {
                continue;
            }
            if (strcasecmp(k.c_str(), "Content-Length") == 0) {
                continue;
            }
            if (strcasecmp(k.c_str(), "Set-Cookie") == 0) {
                ctx.raw_resp().add_header_pair(k, v);
            } else {
                ctx.set_header(k, v);
            }
        }

        if (opt.modify_response) {
            opt.modify_response(ctx, up_resp);
        }

        if (!opt.stream_response) {
            try {
                std::string body = co_await up_resp.read_body(opt.max_body_bytes);
                ctx.body(body);
            } catch (const HttpTimeoutError &) {
                if (!ctx.is_replied() && !ctx.is_stream()) {
                    reply_error(ctx, HttpError(HttpStatus::GatewayTimeout, "Gateway Timeout", "GATEWAY_TIMEOUT"));
                }
            } catch (const HttpClientError &) {
                if (!ctx.is_replied() && !ctx.is_stream()) {
                    reply_error(ctx, HttpError(HttpStatus::BadGateway, "Bad Gateway", "BAD_GATEWAY"));
                }
            } catch (...) {
                if (!ctx.is_replied() && !ctx.is_stream()) {
                    reply_error(ctx, HttpError(HttpStatus::BadGateway, "Bad Gateway", "BAD_GATEWAY"));
                }
            }
        } else {
            if (!co_await ctx.send_chunk({})) {
                up_resp.cancel();
                co_return;
            }
            StreamAccess::set_close_hook(ctx, ProxyStreamBridge::make_cancel_hook(up_resp));
            ProxyStreamCloseGuard close_guard(ctx);
            try {
                while (true) {
                    auto chunk = co_await up_resp.read_chunk(opt.max_body_bytes);
                    if (!chunk.has_value()) {
                        close_guard.clear();
                        co_await ctx.end_chunk();
                        break;
                    }
                    bool ok = co_await ctx.send_chunk(std::move(*chunk));
                    if (!ok) {
                        up_resp.cancel();
                        close_guard.clear();
                        break;
                    }
                }
            } catch (const HttpTimeoutError &) {
                close_guard.clear();
                if (!ctx.is_stream() && !ctx.is_replied()) {
                    reset_stale_representation_headers(ctx);
                    ctx.set_status(HttpStatus::GatewayTimeout);
                    ctx.body("Gateway Timeout");
                } else {
                    ctx.abort_stream();
                }
            } catch (const HttpClientError &) {
                close_guard.clear();
                if (!ctx.is_stream() && !ctx.is_replied()) {
                    reset_stale_representation_headers(ctx);
                    ctx.set_status(HttpStatus::BadGateway);
                    ctx.body("Bad Gateway");
                } else {
                    ctx.abort_stream();
                }
            } catch (...) {
                close_guard.clear();
                if (!ctx.is_stream() && !ctx.is_replied()) {
                    reset_stale_representation_headers(ctx);
                    ctx.set_status(HttpStatus::BadGateway);
                    ctx.body("Bad Gateway");
                } else {
                    ctx.abort_stream();
                }
            }
        }
    };

    return Handler(std::move(handler_fn));
}

Handler proxy(HttpClient &upstream, std::string target_base_url) {
    ProxyOptions opts(std::move(target_base_url));
    return proxy(upstream, std::move(opts));
}

} // namespace kale
