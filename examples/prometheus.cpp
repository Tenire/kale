#include "example_server.h"

#include <chrono>
#include <iostream>
#include <string>

#include "coke/sleep.h"
#include "coke/task.h"
#include "kale/core/context.h"
#include "kale/core/http_error.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/http/health.h"
#include "kale/http/json.h"
#include "kale/middleware/prometheus.h"


int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);
        kale::HttpServer server;

        // Shared Prometheus metrics registry
        kale::PrometheusRegistry registry;

        // Install Prometheus metrics middleware
        server.use(kale::prometheus_metrics(registry));

        // POST /api/orders: creates an order, returns 201 Created
        server.POST("/api/orders", [](kale::Context &ctx) -> coke::Task<void> {
            std::string item = "Widget";
            int quantity = 1;

            if (ctx.is_json() && !ctx.req_body().empty()) {
                try {
                    auto doc = kale::Json::parse(ctx.req_body());
                    if (doc.is_valid() && doc.is_object()) {
                        if (doc.has("item") && doc["item"].is_string()) {
                            item = doc["item"].get<std::string>();
                        }
                        if (doc.has("quantity") && doc["quantity"].is_number()) {
                            quantity = doc["quantity"].get<int>();
                        }
                    }
                } catch (...) {
                    // Fall back to defaults
                }
            }

            ctx.set_status(kale::HttpStatus::Created);
            ctx.json(kale::Json::Object{
                {"order_id", "1001"},
                {"item", item},
                {"quantity", quantity},
                {"status", "created"}
            });
            co_return;
        });

        // GET /api/orders/:id: retrieves order by ID
        server.GET("/api/orders/:id", [](kale::Context &ctx) {
            std::string id = std::string(ctx.param("id"));

            if (id == "999") {
                ctx.set_status(kale::HttpStatus::NotFound);
                ctx.json(kale::Json::Object{
                    {"error", "Order not found"},
                    {"order_id", id}
                });
                return;
            }

            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(kale::Json::Object{
                {"order_id", id},
                {"item", "Widget"},
                {"quantity", 1}
            });
        });

        // GET /api/slow: simulated async delay via coke::sleep
        server.GET("/api/slow", [](kale::Context &ctx) -> coke::Task<void> {
            co_await coke::sleep(std::chrono::milliseconds(50));
            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(kale::Json::Object{
                {"message", "Processed after 50ms async delay"}
            });
            co_return;
        });

        // GET /api/fail: throws HttpError 500
        server.GET("/api/fail", []() -> void {
            throw kale::HttpError(kale::HttpStatus::InternalServerError, "Simulated server failure");
        });

        // GET /metrics: Prometheus text exposition (automatically skipped from metric recording)
        server.GET("/metrics", kale::prometheus_exporter(registry));

        // GET /health: Health status endpoint, skipped from metric recording
        server.GET("/health", kale::health_handler()).tag("metrics.skip", "true");

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
