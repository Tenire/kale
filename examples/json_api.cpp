#include "example_server.h"

#include <iostream>
#include <string>
#include <string_view>

#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/http/json.h"

int main(int argc, char *argv[]) {
    try {
        // Shared lifecycle initialization must be the first statement in main
        examples::ServerOptions options(argc, argv);

        kale::HttpServer server;

        // 1. GET /api/service-info - Constructing native JSON responses
        server.GET("/api/service-info", [](kale::Context &ctx) -> coke::Task<void> {
            // Build response using native kale::Json::Object and kale::Json::Array
            kale::Json::Object info{
                {"service", "catalog-pricing-engine"},
                {"version", "1.0.0"},
                {"engine", "kale-native-workflow-json"},
                {"features", kale::Json::Array{
                    "dynamic-proxy-mutations",
                    "double-precision-arithmetic",
                    "coroutine-native"
                }}
            };

            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(info);
            co_return;
        });

        // 2. POST /api/quote/calculate - JSON input parsing, Workflow double numbers & arithmetic
        server.POST("/api/quote/calculate", [](kale::Context &ctx) -> coke::Task<void> {
            if (!ctx.is_json() || ctx.req_body().empty()) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "BAD_REQUEST"},
                    {"message", "Expected application/json payload"}
                });
                co_return;
            }

            kale::Json input;
            try {
                input = kale::Json::parse(ctx.req_body());
            } catch (...) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "INVALID_JSON"},
                    {"message", "Payload is not valid JSON"}
                });
                co_return;
            }

            if (!input.is_valid() || !input.is_object()) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "INVALID_OBJECT"},
                    {"message", "Root element must be a JSON object"}
                });
                co_return;
            }

            // Inspect required fields
            if (!input.has("item_name") || !input["item_name"].is_string() ||
                !input.has("unit_price") || !input["unit_price"].is_number() ||
                !input.has("quantity") || !input["quantity"].is_number()) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "MISSING_REQUIRED_FIELDS"},
                    {"message", "Fields 'item_name' (string), 'unit_price' (number), and 'quantity' (number) are required"}
                });
                co_return;
            }

            std::string item_name = input["item_name"].get<std::string>();

            // Truthful Workflow numbers: numbers are stored internally as IEEE 754 double
            double unit_price = input["unit_price"].get<double>();
            int quantity = input["quantity"].get<int>();

            if (unit_price <= 0.0 || quantity <= 0) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "OUT_OF_BOUNDS"},
                    {"message", "unit_price and quantity must be positive numbers"}
                });
                co_return;
            }

            double discount_ratio = 0.0;
            if (input.has("discount_code") && input["discount_code"].is_string()) {
                std::string code = input["discount_code"].get<std::string>();
                if (code == "SUMMER10") {
                    discount_ratio = 0.10;
                } else if (code == "VIP20") {
                    discount_ratio = 0.20;
                }
            }

            double subtotal = unit_price * static_cast<double>(quantity);
            double discount_amount = subtotal * discount_ratio;
            double total = subtotal - discount_amount;

            kale::Json::Object quote{
                {"item_name", std::move(item_name)},
                {"unit_price", unit_price},
                {"quantity", quantity},
                {"subtotal", subtotal},
                {"discount_applied", discount_amount},
                {"total", total},
                {"currency", "USD"}
            };

            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(quote);
            co_return;
        });

        // 3. POST /api/audit/enrich - Dynamic proxy mutation and deep copy (.copy()) semantics
        server.POST("/api/audit/enrich", [](kale::Context &ctx) -> coke::Task<void> {
            if (!ctx.is_json() || ctx.req_body().empty()) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "BAD_REQUEST"},
                    {"message", "Expected application/json payload"}
                });
                co_return;
            }

            kale::Json tree;
            try {
                tree = kale::Json::parse(ctx.req_body());
            } catch (...) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "INVALID_JSON"},
                    {"message", "Payload is not valid JSON"}
                });
                co_return;
            }

            if (!tree.is_valid() || !tree.is_object()) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "INVALID_OBJECT"},
                    {"message", "Input must be a JSON object"}
                });
                co_return;
            }

            // Demonstrate proxy node borrowing: tree["metadata"] returns a borrowed proxy (allocated_ == false).
            kale::Json meta_proxy = tree["metadata"];

            // Detach borrowed node into an independent owning tree via copy construction:
            kale::Json meta_detached = meta_proxy.is_valid() ? meta_proxy : kale::Json(kale::Json::Object{});

            // Dynamic proxy mutation: mutate fields directly on the detached object
            meta_detached["inspected_by"] = "kale-json-audit";
            meta_detached["workflow_number_format"] = "IEEE-754-double";
            meta_detached["is_detached"] = true;

            kale::Json::Object reply{
                {"status", "enriched"},
                {"original_metadata_present", meta_proxy.is_valid()},
                {"enriched_metadata", std::move(meta_detached)}
            };

            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(reply);
            co_return;
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
