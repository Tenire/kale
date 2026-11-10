#include "example_server.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "kale/client/http_client.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
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

std::string percent_encode(std::string_view input) {
    static constexpr char hex_chars[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char c : input) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex_chars[(c >> 4) & 0x0F]);
            out.push_back(hex_chars[c & 0x0F]);
        }
    }
    return out;
}
} // namespace

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);
        kale::HttpServer server;

        const std::string upstream_base = get_upstream_url();

        // Asynchronous HTTP client instance with shared default timeouts
        kale::HttpClientOptions client_opts;
        client_opts.send_timeout = std::chrono::milliseconds(5000);
        client_opts.receive_timeout = std::chrono::milliseconds(10000);
        client_opts.keep_alive_timeout = std::chrono::milliseconds(60000);

        kale::HttpClient client(client_opts);

        // Service index
        server.GET("/", [upstream_base](kale::Context &ctx) {
            ctx.json(kale::Json::Object{
                {"service", "http_client_aggregator"},
                {"upstream", upstream_base},
                {"routes", kale::Json::Array{
                    "GET  /aggregate   - Calls upstream /",
                    "GET  /greet       - Calls upstream /async?name=...",
                    "POST /api/echo    - Local echo endpoint for testing POST",
                    "POST /relay       - Relays payload via HttpClient to local /api/echo"
                }}
            });
        });

        // 1. Fetch root of upstream service (pairs with hello's GET /)
        server.GET("/aggregate", [&client, upstream_base](kale::Context &ctx) -> coke::Task<void> {
            std::string target_url = upstream_base + "/";

            try {
                auto response = co_await client.get(target_url)
                    .header("Accept", "text/plain")
                    .timeout(std::chrono::seconds(5))
                    .send();

                int upstream_status = response.status();
                std::string body = co_await response.read_body();

                ctx.set_status(kale::HttpStatus::OK);
                ctx.json(kale::Json::Object{
                    {"status", "ok"},
                    {"target_url", target_url},
                    {"upstream_status", upstream_status},
                    {"upstream_body", body}
                });
            } catch (const kale::HttpTimeoutError &e) {
                ctx.set_status(kale::HttpStatus::GatewayTimeout);
                ctx.json(kale::Json::Object{
                    {"error", "gateway_timeout"},
                    {"message", e.what()},
                    {"target_url", target_url}
                });
            } catch (const kale::HttpClientError &e) {
                ctx.set_status(kale::HttpStatus::BadGateway);
                ctx.json(kale::Json::Object{
                    {"error", "bad_gateway"},
                    {"message", e.what()},
                    {"target_url", target_url}
                });
            }
        });

        // 2. Fetch parametrized coroutine endpoint (pairs with hello's GET /async?name=...)
        server.GET("/greet", [&client, upstream_base](kale::Context &ctx) -> coke::Task<void> {
            std::string name = ctx.default_query("name", "World");
            std::string target_url = upstream_base + "/async?name=" + percent_encode(name);

            try {
                auto response = co_await client.get(target_url)
                    .header("Accept", "text/plain")
                    .timeout(std::chrono::seconds(5))
                    .send();

                int upstream_status = response.status();
                std::string body = co_await response.read_body();

                ctx.set_status(kale::HttpStatus::OK);
                ctx.json(kale::Json::Object{
                    {"status", "ok"},
                    {"queried_name", name},
                    {"upstream_status", upstream_status},
                    {"greeting", body}
                });
            } catch (const kale::HttpTimeoutError &e) {
                ctx.set_status(kale::HttpStatus::GatewayTimeout);
                ctx.json(kale::Json::Object{
                    {"error", "gateway_timeout"},
                    {"message", e.what()}
                });
            } catch (const kale::HttpClientError &e) {
                ctx.set_status(kale::HttpStatus::BadGateway);
                ctx.json(kale::Json::Object{
                    {"error", "bad_gateway"},
                    {"message", e.what()}
                });
            }
        });

        // 3. Local echo endpoint for verifying POST interactions
        server.POST("/api/echo", [](kale::Context &ctx) {
            std::string_view req_data = ctx.req_body();
            ctx.set_content_type("application/json; charset=utf-8");
            if (req_data.empty()) {
                ctx.string("{\"echo\":\"empty\"}");
            } else {
                ctx.string(std::string(req_data));
            }
        });

        // 4. Relay POST payload via HttpClient to local /api/echo (or optional ?upstream= URL)
        server.POST("/relay", [&client, &server](kale::Context &ctx) -> coke::Task<void> {
            std::string target_url;
            if (ctx.has_query("upstream")) {
                target_url = ctx.query("upstream");
            } else {
                int port = server.listen_port();
                if (port <= 0) {
                    port = 8081;
                }
                target_url = "http://127.0.0.1:" + std::to_string(port) + "/api/echo";
            }
            std::string payload = std::string(ctx.req_body());

            try {
                auto response = co_await client.post(target_url)
                    .header("Content-Type", "application/json")
                    .body(std::move(payload))
                    .timeout(std::chrono::seconds(5))
                    .send();

                int status = response.status();
                std::string upstream_body = co_await response.read_body();

                ctx.set_status(status);
                ctx.json(kale::Json::Object{
                    {"relayed", true},
                    {"target_url", target_url},
                    {"status", status},
                    {"reply", upstream_body}
                });
            } catch (const kale::HttpTimeoutError &e) {
                ctx.set_status(kale::HttpStatus::GatewayTimeout);
                ctx.json(kale::Json::Object{
                    {"error", "gateway_timeout"},
                    {"message", e.what()}
                });
            } catch (const kale::HttpClientError &e) {
                ctx.set_status(kale::HttpStatus::BadGateway);
                ctx.json(kale::Json::Object{
                    {"error", "bad_gateway"},
                    {"message", e.what()}
                });
            }
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
