#include "example_server.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "kale/client/http_client.h"
#include "kale/client/reverse_proxy.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/http/json.h"

namespace {

std::string get_upstream_url() {
    const char *env = std::getenv("KALE_EXAMPLE_UPSTREAM");
    if (env != nullptr && *env != '\0') {
        std::string s(env);
        while (s.size() > 1 && s.back() == '/') {
            s.pop_back();
        }
        return s;
    }
    return "http://127.0.0.1:8080";
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);
        kale::HttpServer server;

        const std::string upstream_base = get_upstream_url();

        kale::HttpClientOptions client_opts;
        client_opts.send_timeout = std::chrono::milliseconds(5000);
        client_opts.receive_timeout = std::chrono::milliseconds(10000);
        client_opts.keep_alive_timeout = std::chrono::milliseconds(60000);

        kale::HttpClient client(client_opts);

        // Gateway index
        server.GET("/", [upstream_base](kale::Context &ctx) {
            ctx.json(kale::Json::Object{
                {"gateway", "reverse_proxy"},
                {"upstream", upstream_base},
                {"routes", kale::Json::Array{
                    "ALL /proxy/*path  - Buffered proxy (strips /proxy prefix)",
                    "ALL /stream/*path - Streaming proxy (chunked transfer)"
                }},
                {"behavior", kale::Json::Array{
                    "Hop-by-hop headers stripped per RFC 7230",
                    "X-Forwarded-For populated from client peer IP",
                    "Host header rewritten to upstream authority",
                    "Upstream Set-Cookie headers preserved"
                }}
            });
        });

        // Buffered reverse proxy configuration
        kale::ProxyOptions proxy_opts(upstream_base);
        proxy_opts.strip_prefix = "/proxy";
        proxy_opts.preserve_host = false;
        proxy_opts.trust_downstream_forwarded = false;
        proxy_opts.timeout = std::chrono::seconds(10);
        proxy_opts.max_body_bytes = 16 * 1024 * 1024;
        proxy_opts.stream_response = false;

        proxy_opts.rewrite_request = [](const kale::Context &, kale::HttpClientRequest &req) {
            req.header("X-Forwarded-Gateway", "Kale-ReverseProxy");
        };

        proxy_opts.modify_response = [](kale::Context &ctx, const kale::HttpClientResponse &) {
            ctx.set_header("X-Proxy-Gateway", "Kale-Edge");
        };

        server.any("/proxy/*path", kale::proxy(client, proxy_opts));
        server.any("/proxy", kale::proxy(client, proxy_opts));

        // Streaming reverse proxy configuration
        kale::ProxyOptions stream_opts(upstream_base);
        stream_opts.strip_prefix = "/stream";
        stream_opts.preserve_host = false;
        stream_opts.timeout = std::chrono::seconds(30);
        stream_opts.stream_response = true;

        server.any("/stream/*path", kale::proxy(client, stream_opts));
        server.any("/stream", kale::proxy(client, stream_opts));

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
