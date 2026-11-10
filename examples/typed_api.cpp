#include "example_server.h"

#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include "kale/core/context.h"
#include "kale/core/error_response.h"
#include "kale/core/http_error.h"
#include "kale/middleware/error_handler.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/http/binding.h"
#include "kale/http/field_desc.h"
#include "kale/http/json.h"
#include "kale/http/json_reader.h"
#include "kale/http/openapi.h"
#include "kale/http/validator.h"

using namespace kale::rules;

namespace {

// ============================================================================
// Declarative DTOs with Field Descriptors (FieldDesc) & Validation Rules
// ============================================================================

struct ProductValidationDTO {
    std::string sku;
    std::string name;
    std::string category;
    double price{0.0};
    int stock{0};
    std::optional<std::string> description;

    // Declarative compile-time binding descriptors
    KALE_BIND(
        KALE_FIELD(sku, is_alphanumeric(), len_range(4, 12)),
        KALE_FIELD(name, min_len(3), max_len(60)),
        KALE_FIELD(category, one_of<std::string>({"peripherals", "displays", "accessories", "audio"})),
        KALE_FIELD(price, gt(0.0), lte(50000.0)),
        KALE_FIELD(stock, gte(0), lte(10000)),
        KALE_FIELD(description, max_len(250))
    )
};

struct OrderQuoteRequestDTO {
    std::string sku;
    int quantity{1};
    std::string customer_email;
    std::optional<std::string> promo_code;

    KALE_BIND(
        KALE_FIELD(sku, is_alphanumeric(), len_range(4, 12)),
        KALE_FIELD(quantity, range(1, 100)),
        KALE_FIELD(customer_email, is_email()),
        KALE_FIELD(promo_code, max_len(20))
    )
};

struct OrderQuoteResponseDTO {
    std::string sku;
    int quantity{0};
    double unit_price{0.0};
    double subtotal{0.0};
    double discount{0.0};
    double total{0.0};
    std::string customer_email;

    KALE_BIND(
        KALE_FIELD(sku),
        KALE_FIELD(quantity),
        KALE_FIELD(unit_price),
        KALE_FIELD(subtotal),
        KALE_FIELD(discount),
        KALE_FIELD(total),
        KALE_FIELD(customer_email)
    )
};
// ============================================================================
// Domain Exceptions (Derived from HttpError)
// ============================================================================

class ProductNotFoundException : public kale::HttpError {
public:
    explicit ProductNotFoundException(std::string sku)
        : kale::HttpError(404, "Product SKU '" + sku + "' not found in catalog", "PRODUCT_NOT_FOUND"),
          sku_(std::move(sku)) {
        with_detail("sku", sku_);
    }

    [[nodiscard]] const std::string &sku() const noexcept { return sku_; }

private:
    std::string sku_;
};

class InsufficientStockException : public kale::HttpError {
public:
    InsufficientStockException(std::string sku, int requested, int available)
        : kale::HttpError(409, "Requested quantity (" + std::to_string(requested) +
                               ") exceeds available stock (" + std::to_string(available) + ")",
                          "INSUFFICIENT_STOCK"),
          sku_(std::move(sku)), requested_(requested), available_(available) {
        with_detail("sku", sku_);
        with_detail("requested_quantity", std::to_string(requested_));
        with_detail("available_stock", std::to_string(available_));
    }

    [[nodiscard]] const std::string &sku() const noexcept { return sku_; }
    [[nodiscard]] int requested() const noexcept { return requested_; }
    [[nodiscard]] int available() const noexcept { return available_; }

private:
    std::string sku_;
    int requested_{0};
    int available_{0};
};

} // namespace

int main(int argc, char *argv[]) {
    try {
        // Shared lifecycle initialization must be the first statement in main
        examples::ServerOptions options(argc, argv);

        kale::HttpServer server;
        kale::OpenApiRegistry openapi_registry;

        // ====================================================================
        // Centralized standard_json error formatting
        server.use(kale::error_handler(kale::ErrorFormatters::standard_json()));

        // Application exception mapping middleware feeding reply_error
        server.use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            try {
                co_await next();
            } catch (const ProductNotFoundException &e) {
                kale::reply_error(ctx, e);
            } catch (const InsufficientStockException &e) {
                kale::reply_error(ctx, e);
            } catch (const kale::JsonBindError &err) {
                auto http_err = kale::HttpError::unprocessable_entity("Validation failed", "VALIDATION_FAILED")
                                    .with_detail("field", err.field())
                                    .with_detail("expected", err.expected())
                                    .with_detail("reason", err.reason());
                kale::reply_error(ctx, http_err);
            } catch (const kale::JsonInputError &err) {
                auto http_err = kale::HttpError::bad_request(err.what(), "MALFORMED_JSON");
                kale::reply_error(ctx, http_err);
            }
        });
        // ====================================================================
        // Route Declarations & Business Logic
        // ====================================================================

        // 1. POST /api/v1/products/validate - Declarative DTO Field Validation
        server.POST("/api/v1/products/validate", [](kale::Context &ctx) -> coke::Task<void> {
            // Type-safe DTO binding against native Kale JSON; throws JsonBindError on constraint failure
            auto dto = ctx.bind_json<ProductValidationDTO>();

            kale::Json::Object receipt{
                {"status", "valid"},
                {"sku", dto.sku},
                {"name", dto.name},
                {"category", dto.category},
                {"price", dto.price},
                {"stock", dto.stock}
            };
            if (dto.description) {
                receipt.push_back("description", *dto.description);
            }

            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(receipt);
            co_return;
        });

        openapi_registry.associate(kale::HttpMethod::POST, "/api/v1/products/validate")
            .set_summary("Validate product specification DTO")
            .set_description("Checks SKU, name, price, stock, and category constraints without side effects")
            .add_tag("Products")
            .body<ProductValidationDTO>("Product specification payload")
            .response(200, "DTO passed all constraint rules")
            .response(422, "Validation constraint failure (HTTP 422)");

        // 2. POST /api/v1/orders/quote - Order Pricing & Domain Boundary Check
        server.POST("/api/v1/orders/quote", [](kale::Context &ctx) -> coke::Task<void> {
            auto req_dto = ctx.bind_json<OrderQuoteRequestDTO>();

            // Business rule checks demonstrating Domain Error Boundaries:
            if (req_dto.sku == "NOTFOUND01" || req_dto.sku == "NONE0000") {
                throw ProductNotFoundException(req_dto.sku);
            }
            if (req_dto.quantity > 50) {
                throw InsufficientStockException(req_dto.sku, req_dto.quantity, 50);
            }

            double unit_price = 149.00;
            if (req_dto.sku == "AUDANC01") {
                unit_price = 189.50;
            } else if (req_dto.sku == "DSP4K27") {
                unit_price = 399.00;
            }
            double subtotal = unit_price * static_cast<double>(req_dto.quantity);
            double discount = 0.0;
            if (req_dto.promo_code && *req_dto.promo_code == "KALE15") {
                discount = subtotal * 0.15;
            }

            double total = subtotal - discount;

            kale::Json::Object quote{
                {"sku", req_dto.sku},
                {"quantity", req_dto.quantity},
                {"unit_price", unit_price},
                {"subtotal", subtotal},
                {"discount", discount},
                {"total", total},
                {"customer_email", req_dto.customer_email}
            };

            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(quote);
            co_return;
        });

        openapi_registry.associate(kale::HttpMethod::POST, "/api/v1/orders/quote")
            .set_summary("Calculate order price quotation")
            .set_description("Validates order DTO and generates pricing quote, triggering domain exceptions on missing items or stock limits")
            .add_tag("Orders")
            .body<OrderQuoteRequestDTO>("Order quotation parameters")
            .response<OrderQuoteResponseDTO>(200, "Quotation calculated successfully")
            .response(404, "Product SKU not found in catalog")
            .response(409, "Requested quantity exceeds available stock")
            .response(422, "DTO field validation constraint failure");

        // ====================================================================
        // OpenAPI 3.0.3 Specification Endpoint & Swagger UI
        // ====================================================================

        auto openapi_doc = kale::generate_openapi(
            kale::OpenApiConfig{
                .title = "Storefront & Order Service API",
                .version = "1.0.0",
                .description = "Auto-generated OpenAPI 3.0.3 specification from declarative DTO schemas and routes"
            },
            server.router().routes(),
            openapi_registry
        );

        server.GET("/openapi.json", kale::make_openapi_handler(std::move(openapi_doc)));

        server.GET("/swagger", [](kale::Context &ctx) -> coke::Task<void> {
            ctx.set_content_type("text/html; charset=utf-8");
            ctx.html(R"(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <title>Storefront API - Swagger UI</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css">
  <style>body { margin: 0; background: #fafafa; }</style>
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
  <script>
    window.onload = function() {
      SwaggerUIBundle({
        url: '/openapi.json',
        dom_id: '#swagger-ui',
        deepLinking: true
      });
    };
  </script>
</body>
</html>)");
            co_return;
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
