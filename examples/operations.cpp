#include "example_server.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/http/health.h"
#include "kale/http/json.h"
#include "kale/middleware/access_log.h"
#include "kale/middleware/compression.h"
#include "kale/middleware/cors.h"
#include "kale/middleware/prometheus.h"
#include "kale/middleware/rate_limiter.h"
#include "kale/middleware/request_id.h"
#include "kale/middleware/security_headers.h"

namespace {

class ConsoleAccessLogger {
public:
    void log(const kale::AccessLogRecord &rec) {
        std::lock_guard<std::mutex> lock(mu_);
        std::cout << "[ACCESS] "
                  << rec.method << " " << rec.target
                  << " -> " << rec.status_code
                  << " (" << rec.duration.count() << " us)"
                  << (rec.request_id.empty() ? "" : " id=" + rec.request_id)
                  << (rec.downstream_exception ? " [ERR]" : "")
                  << "\n";
    }

private:
    std::mutex mu_;
};

} // namespace

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);

        // Application-owned stores and sinks declared BEFORE server to outlive server lifecycle
        ConsoleAccessLogger log_sink;

        kale::RateLimitPolicy rate_policy;
        rate_policy.limit = 20;
        rate_policy.window = std::chrono::seconds(1);
        rate_policy.max_keys = 10000;
        kale::MemoryRateLimitStore rate_store(rate_policy);

        kale::PrometheusRegistry metrics_registry;
        kale::ReadinessState readiness(true);

        kale::HttpServer server;
        server.set_show_banner(true);
        server.set_banner_text("=== Kale Operations Showcase Service ===");

        // 1. Compression (outermost: compresses uncommitted body during stack unwind)
        kale::CompressConfig comp_cfg;
        comp_cfg.min_length = 512;
        comp_cfg.set_zstd_level(3).set_gzip_level(6);
        server.use(kale::compression(comp_cfg));

        // 2. Access logger
        server.use(kale::access_log([&log_sink](kale::AccessLogRecord rec) {
            log_sink.log(std::move(rec));
        }));

        // 3. Request-ID injection & correlation
        kale::RequestIdConfig req_id_cfg;
        req_id_cfg.header_name = "X-Request-Id";
        server.use(kale::request_id(req_id_cfg));

        // 4. CORS preflight and response headers
        kale::CorsConfig cors_cfg;
        cors_cfg.allow_origins = {"*"};
        cors_cfg.allow_methods = {"GET", "POST", "PUT", "DELETE", "OPTIONS"};
        cors_cfg.allow_headers = {"Content-Type", "Authorization", "X-Request-Id"};
        cors_cfg.max_age = 3600;
        server.use(kale::cors(cors_cfg));

        // 5. Security response headers
        kale::SecurityHeadersConfig sec_cfg;
        sec_cfg.content_type_nosniff = true;
        sec_cfg.frame_options = "DENY";
        sec_cfg.referrer_policy = "strict-origin-when-cross-origin";
        server.use(kale::security_headers(sec_cfg));

        // 6. Rate limiter (per-IP in-memory window)
        kale::RateLimiterConfig limiter_cfg;
        server.use(kale::rate_limiter(rate_store, limiter_cfg));

        // 7. Prometheus latency and request metrics
        kale::PrometheusConfig prom_cfg;
        prom_cfg.skip_route_tag = "metrics.skip";
        server.use(kale::prometheus_metrics(metrics_registry, prom_cfg));

        // Operational endpoints
        server.GET("/health", kale::health_handler()).tag("metrics.skip", "true");
        server.GET("/ready", kale::readiness_handler(readiness)).tag("metrics.skip", "true");

        server.POST("/ready/toggle", [&readiness](kale::Context &ctx) {
            bool current = readiness.is_ready();
            readiness.set_ready(!current);
            ctx.json(kale::Json::Object{
                {"action", "toggle_readiness"},
                {"ready", !current}
            });
        }).tag("metrics.skip", "true");

        server.GET("/metrics", kale::prometheus_exporter(metrics_registry));
        // Dynamic route introspection table
        server.GET("/routes", [&server](kale::Context &ctx) {
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string(server.format_routes());
        }).tag("metrics.skip", "true");


        // Service index
        server.GET("/", [](kale::Context &ctx) {
            ctx.json(kale::Json::Object{
                {"service", "operations_showcase"},
                {"pipeline", kale::Json::Array{
                    "compression", "access_log", "request_id",
                    "cors", "security_headers", "rate_limiter", "prometheus"
                }},
                {"endpoints", kale::Json::Array{
                    "/health        - Liveness check",
                    "/ready         - Readiness check",
                    "/ready/toggle  - POST toggles readiness state",
                    "/metrics       - Prometheus metrics export",
                    "/routes        - Dynamic route table via format_routes()",
                    "/api/catalog   - Large JSON payload for compression test",
                    "/api/quote     - POST stateless quote calculator"
                }}
            });
        });

        // Large JSON response (>1KB) to demonstrate Zstd/Gzip compression negotiation
        server.GET("/api/catalog", [](kale::Context &ctx) {
            kale::Json::Array items;
            for (int i = 1; i <= 20; ++i) {
                items.push_back(kale::Json::Object{
                    {"id", i},
                    {"sku", "SKU-" + std::to_string(100 + i)},
                    {"name", "Server Component Item " + std::to_string(i)},
                    {"category", (i % 2 == 0) ? "Hardware" : "Software"},
                    {"price_usd", 49.99 + (i * 5.0)}
                });
            }

            ctx.json(kale::Json::Object{
                {"catalog_size", 20},
                {"items", std::move(items)}
            });
        });

        // Stateless quote calculator: computes total from incoming JSON or query
        server.POST("/api/quote", [](kale::Context &ctx) {
            std::string req_id(kale::get_request_id(ctx));
            std::string_view body = ctx.req_body();

            double unit_price = 25.0;
            int quantity = 1;

            if (!body.empty()) {
                try {
                    auto doc = kale::Json::parse(body);
                    if (doc.has("quantity") && doc["quantity"].is_number()) {
                        quantity = doc["quantity"].get<int>();
                    }
                    if (doc.has("unit_price") && doc["unit_price"].is_number()) {
                        unit_price = doc["unit_price"].get<double>();
                    }
                } catch (const std::exception &) {
                    ctx.set_status(kale::HttpStatus::BadRequest);
                    ctx.json(kale::Json::Object{
                        {"error", "invalid_json"},
                        {"message", "Expected JSON object with quantity and unit_price"}
                    });
                    return;
                }
            }

            double subtotal = quantity * unit_price;
            double tax = subtotal * 0.08;
            double total = subtotal + tax;

            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(kale::Json::Object{
                {"request_id", req_id},
                {"quantity", quantity},
                {"unit_price", unit_price},
                {"subtotal", subtotal},
                {"tax", tax},
                {"total", total}
            });
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
