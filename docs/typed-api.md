# Strongly-Typed DTO Binding, Validation & OpenAPI 3.0

This task guide demonstrates building type-safe, validated HTTP web services using Kale's declarative Data Transfer Object (DTO) system, compile-time schema reflection, and application error boundaries.

Source code: [`../examples/typed_api.cpp`](../examples/typed_api.cpp)

---

## 1. Goal

The `example_typed_api` service implements a Product Validation and Order Quotation service that demonstrates:
- Declarative DTO field descriptors using `KALE_BIND` and `KALE_FIELD`.
- Strong-typed JSON body binding via `ctx.bind_json<DTO>()`.
- Compile-time constraint validation using `kale::rules::*`.
- Application exception mapping via `kale::HttpError` and `kale::reply_error`.
- Real OpenAPI 3.0.3 specification generation (`kale::generate_openapi`) from registered route graphs and DTO schemas.
- Serving OpenAPI JSON and interactive Swagger UI.

---

## 2. Build & Run

Kale uses native `xmake` for building targets.

### Build
From the repository root:
```bash
xmake build example_typed_api
```

### Run
Run on the default port (`8080`):
```bash
xmake run example_typed_api
```

Run on an explicit port (e.g. `9090`):
```bash
xmake run example_typed_api 9090
```

Run on an OS-assigned ephemeral port (`0`):
```bash
xmake run example_typed_api 0
```

---

## 3. Declarative DTO Architecture & `FieldDesc`

Kale decouples DTO definitions from underlying serialization formats. A single declarative macro definition provides both:
1. **Runtime Ingestion & Validation**: Direct extraction from JSON without intermediate AST reflection.
2. **OpenAPI Schema Reflection**: Compile-time mapping of types and validation rules into OpenAPI 3.0.3 JsonSchema.

### DTO Definition
```cpp
#include <optional>
#include <string>

// Convenience headers (active):
#include <kale/api.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/http/field_desc.h"
// #include "kale/http/validator.h"

using namespace kale::rules;

struct ProductValidationDTO {
    std::string sku;
    std::string name;
    std::string category;
    double price{0.0};
    int stock{0};
    std::optional<std::string> description;

    KALE_BIND(
        KALE_FIELD(sku, is_alphanumeric(), len_range(4, 12)),
        KALE_FIELD(name, min_len(3), max_len(60)),
        KALE_FIELD(category, one_of<std::string>({"peripherals", "displays", "accessories", "audio"})),
        KALE_FIELD(price, gt(0.0), lte(50000.0)),
        KALE_FIELD(stock, gte(0), lte(10000)),
        KALE_FIELD(description, max_len(250))
    )
};
```

### How `KALE_BIND` Works Under the Hood
`KALE_FIELD(field, ...)` expands to a `kale::FieldDesc` instance:
```cpp
#include <optional>
#include <string>

// Convenience headers (active):
#include <kale/api.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/http/field_desc.h"
// #include "kale/http/validator.h"

using namespace kale::rules;

struct ProductValidationDTO {
    std::string sku;
    std::string name;
    std::string category;
    double price{0.0};
    int stock{0};
    std::optional<std::string> description;

    KALE_BIND(
        KALE_FIELD(sku, is_alphanumeric(), len_range(4, 12)),
        KALE_FIELD(name, min_len(3), max_len(60)),
        KALE_FIELD(category, one_of<std::string>({"peripherals", "displays", "accessories", "audio"})),
        KALE_FIELD(price, gt(0.0), lte(50000.0)),
        KALE_FIELD(stock, gte(0), lte(10000)),
        KALE_FIELD(description, max_len(250))
    )
};

// Static assertion verifying DTO reflection traits:
static_assert(kale::HasKaleFields<ProductValidationDTO>);
```
- `key`: The JSON property name (`"sku"`).
- `member`: A pointer-to-member (`&ProductValidationDTO::sku`).
- `rules`: A compile-time tuple of constraint functors (`is_alphanumeric()`, `len_range(4, 12)`).

`KALE_BIND(...)` defines `const auto &__kale_fields() const`, returning a `std::tuple` of `FieldDesc` instances. Because everything resolves at compile time:
- No heap allocations occur for metadata dispatch.
- No dynamic type introspection (RTTI) is required for binding.
- Fields declared as `std::optional<T>` are treated as optional properties; non-optional fields are automatically marked as `required`.

---

## 4. Built-in Validation Rules (`kale::rules`)

The `kale/http/validator.h` header provides composable constraint rules:

| Rule Functor | Description | OpenAPI Annotation |
| :--- | :--- | :--- |
| `min_len(n)` | Minimum string or array length | `minLength: n` |
| `max_len(n)` | Maximum string or array length | `maxLength: n` |
| `len_range(min, max)` | Length bounded within $[min, max]$ | `x-kale-min-byte-length`, `x-kale-max-byte-length` |
| `gt(boundary)` | Value must be strictly $> boundary$ | `minimum: boundary, exclusiveMinimum: true` |
| `gte(boundary)` | Value must be $\ge boundary$ | `minimum: boundary` |
| `lt(boundary)` | Value must be strictly $< boundary$ | `maximum: boundary, exclusiveMaximum: true` |
| `lte(boundary)` | Value must be $\le boundary$ | `maximum: boundary` |
| `range(min, max)` | Value bounded within $[min, max]$ | `minimum: min, maximum: max` |
| `one_of<T>({...})` | Value must be in allowed choices | `enum: [...]` |
| `is_email()` | RFC 5321 / RFC 5322 compliant email check | `format: "email"` |
| `is_uuid_v7()` | RFC 9562 UUIDv7 format validation | `format: "uuid", x-kale-uuid-version: 7` |
| `is_alphanumeric()` | Letters and digits only | `x-kale-rule: "is_alphanumeric"` |
| `is_hex()` | Hexadecimal characters only | `x-kale-rule: "is_hex"` |
| `starts_with(str)` | Must start with prefix | `x-kale-starts-with: str` |
| `ends_with(str)` | Must end with suffix | `x-kale-ends-with: str` |
| `contains(str)` | Must contain substring | `x-kale-contains: str` |
| `custom(lambda)` | Custom predicate lambda | `x-kale-custom-validation: true` |

---

## 5. Application Exception Mapping & Error Handling

Kale uses `kale::HttpError`, `kale::reply_error`, and `kale::error_handler` for structured HTTP error recovery.

```cpp
#include <string>
#include <utility>

// Convenience headers (active):
#include <kale/core.h>
#include <kale/http.h>
#include <kale/middlewares.h>
#include <kale/api.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/error_response.h"
// #include "kale/core/http_error.h"
// #include "kale/core/http_server.h"
// #include "kale/core/http_status.h"
// #include "kale/middleware/error_handler.h"
// #include "kale/http/json.h"
// #include "kale/http/json_reader.h"

class ProductNotFoundException : public kale::HttpError {
public:
    explicit ProductNotFoundException(std::string sku)
        : kale::HttpError(404, "Product SKU '" + sku + "' not found in catalog", "PRODUCT_NOT_FOUND"),
          sku_(std::move(sku)) {
        with_detail("sku", sku_);
    }
    const std::string &sku() const noexcept { return sku_; }
private:
    std::string sku_;
};

void configure_error_handling(kale::HttpServer &server) {
    // Centralized standard_json error formatting
    server.use(kale::error_handler(kale::ErrorFormatters::standard_json()));

    // Application exception mapping middleware feeding reply_error
    server.use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        try {
            co_await next();
        } catch (const ProductNotFoundException &e) {
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
}
```

---

## 6. Real OpenAPI 3.0 Specification Generation

Kale generates OpenAPI specifications directly from registered routes and DTO schema reflection. No static or hardcoded JSON files are required.

```cpp
#include <optional>
#include <string>
#include <utility>

// Convenience headers (active):
#include <kale/core.h>
#include <kale/api.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_method.h"
// #include "kale/core/http_server.h"
// #include "kale/http/field_desc.h"
// #include "kale/http/openapi.h"
// #include "kale/http/validator.h"

using namespace kale::rules;

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
    double total{0.0};

    KALE_BIND(
        KALE_FIELD(sku),
        KALE_FIELD(quantity),
        KALE_FIELD(unit_price),
        KALE_FIELD(total)
    )
};

void setup_openapi(kale::HttpServer &server, kale::OpenApiRegistry &openapi_registry) {
    openapi_registry.associate(kale::HttpMethod::POST, "/api/v1/orders/quote")
        .set_summary("Calculate order price quotation")
        .set_description("Validates order DTO and generates pricing quote")
        .add_tag("Orders")
        .body<OrderQuoteRequestDTO>("Order quotation parameters")
        .response<OrderQuoteResponseDTO>(200, "Quotation calculated successfully")
        .response(404, "Product SKU not found in catalog")
        .response(409, "Requested quantity exceeds available stock")
        .response(422, "DTO field validation constraint failure");

    auto openapi_doc = kale::generate_openapi(
        kale::OpenApiConfig{
            .title = "Storefront & Order Service API",
            .version = "1.0.0",
            .description = "Auto-generated OpenAPI 3.0.3 specification"
        },
        server.router().routes(),
        openapi_registry
    );

    server.GET("/openapi.json", kale::make_openapi_handler(std::move(openapi_doc)));
}
```

---

## 7. Curl Recipes & Expected Responses

All endpoints produce `Content-Type: application/json; charset=utf-8`.

### 7.1. Validate Product DTO (POST /api/v1/products/validate)
Submit a valid product payload:
```bash
curl -s -X POST http://127.0.0.1:8080/api/v1/products/validate \
  -H "Content-Type: application/json" \
  -d '{
    "sku": "KBPRO87",
    "name": "Tenkeyless Mechanical Keyboard",
    "category": "peripherals",
    "price": 149.00,
    "stock": 30,
    "description": "RGB Backlit"
  }' | jq .
```
Expected Response (`200 OK`):
```json
{
  "status": "valid",
  "sku": "KBPRO87",
  "name": "Tenkeyless Mechanical Keyboard",
  "category": "peripherals",
  "price": 149,
  "stock": 30,
  "description": "RGB Backlit"
}
```
Submit a payload violating alphanumeric constraints (e.g. SKU containing hyphens):
```bash
curl -s -X POST http://127.0.0.1:8080/api/v1/products/validate \
  -H "Content-Type: application/json" \
  -d '{
    "sku": "KB-PRO-87",
    "name": "Keyboard",
    "category": "peripherals",
    "price": 149.0,
    "stock": 10
  }' | jq .
```
Expected Response (`422 Unprocessable Entity`):
```json
{
  "error": "VALIDATION_FAILED",
  "field": "sku",
  "expected": "alphanumeric string",
  "reason": "must contain only letters and digits"
}
```

---

### 7.2. Calculate Order Quotation (POST /api/v1/orders/quote)
Submit a valid order quotation request:
```bash
curl -s -X POST http://127.0.0.1:8080/api/v1/orders/quote \
  -H "Content-Type: application/json" \
  -d '{
    "sku": "KBPRO87",
    "quantity": 2,
    "customer_email": "alice@example.com",
    "promo_code": "KALE15"
  }' | jq .
```
Expected Response (`200 OK`):
```json
{
  "sku": "KBPRO87",
  "quantity": 2,
  "unit_price": 149,
  "subtotal": 298,
  "discount": 44.7,
  "total": 253.3,
  "customer_email": "alice@example.com"
}
```

Triggering Domain Exception: Unknown SKU (handled via reply_error):
```bash
curl -s -X POST http://127.0.0.1:8080/api/v1/orders/quote \
  -H "Content-Type: application/json" \
  -d '{
    "sku": "NOTFOUND01",
    "quantity": 1,
    "customer_email": "bob@example.com"
  }' | jq .
```
Expected Response (`404 Not Found`):
```json
{
  "code": 404,
  "error_code": "PRODUCT_NOT_FOUND",
  "message": "Product SKU 'NOTFOUND01' not found in catalog",
  "details": {
    "sku": "NOTFOUND01"
  }
}
```

Triggering Domain Exception: Exceeding Available Stock:
```bash
curl -s -X POST http://127.0.0.1:8080/api/v1/orders/quote \
  -H "Content-Type: application/json" \
  -d '{
    "sku": "KBPRO87",
    "quantity": 99,
    "customer_email": "carol@example.com"
  }' | jq .
```
Expected Response (`409 Conflict`):
```json
{
  "code": 409,
  "error_code": "INSUFFICIENT_STOCK",
  "message": "Requested quantity (99) exceeds available stock (50)",
  "details": {
    "available_stock": "50",
    "requested_quantity": "99",
    "sku": "KBPRO87"
  }
}
```
---

### 7.3. Retrieve OpenAPI Specification (GET /openapi.json)
```bash
curl -s http://127.0.0.1:8080/openapi.json | jq .
```
Expected Response (`200 OK`):
```json
{
  "openapi": "3.0.3",
  "info": {
    "title": "Storefront & Order Service API",
    "version": "1.0.0",
    "description": "Auto-generated OpenAPI 3.0.3 specification from declarative DTO schemas and routes"
  },
  "paths": {
    "/api/v1/products/validate": {
      "post": {
        "summary": "Validate product specification DTO",
        "description": "Checks SKU, name, price, stock, and category constraints without side effects",
        "tags": ["Products"],
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "type": "object",
                "properties": {
                  "sku": {
                    "type": "string",
                    "x-kale-rule": "is_alphanumeric"
                  },
                  "name": {
                    "type": "string",
                    "minLength": 3,
                    "maxLength": 60
                  },
                  "category": {
                    "type": "string",
                    "enum": ["peripherals", "displays", "accessories", "audio"]
                  },
                  "price": {
                    "type": "number",
                    "minimum": 0,
                    "maximum": 50000
                  },
                  "stock": {
                    "type": "integer",
                    "minimum": 0,
                    "maximum": 10000
                  },
                  "description": {
                    "type": "string",
                    "maxLength": 250,
                    "nullable": true
                  }
                },
                "required": ["sku", "name", "category", "price", "stock"]
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "DTO passed all constraint rules"
          },
          "422": {
            "description": "Validation constraint failure (HTTP 422)"
          }
        }
      }
    }
  }
}
```

---

## 8. Generic Reader & Third-Party JSON Customization

The DTO binding engine in `kale/http/json_reader.h` is generic across JSON parsers and can be called directly as a free function or via `ctx.bind_json<DTO, J>()`:

```cpp
#include <optional>
#include <string>
#include <string_view>
#include "coke/task.h"

// Convenience headers (active):
#include <kale/core.h>
#include <kale/http.h>
#include <kale/api.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"
// #include "kale/core/http_status.h"
// #include "kale/http/binding.h"
// #include "kale/http/field_desc.h"
// #include "kale/http/json.h"
// #include "kale/http/json_reader.h"
// #include "kale/http/validator.h"

using namespace kale::rules;

struct ProductValidationDTO {
    std::string sku;
    std::string name;
    KALE_BIND(
        KALE_FIELD(sku, is_alphanumeric(), len_range(4, 12)),
        KALE_FIELD(name, min_len(3), max_len(60))
    )
};

template <typename DTO, typename J = kale::Json>
DTO parse_dto_from_raw_json(std::string_view raw_json) {
    return kale::bind_json_body<DTO, J>(raw_json);
}

void register_generic_bind_route(kale::HttpServer &server) {
    server.POST("/api/custom-bind", [](kale::Context &ctx) -> coke::Task<void> {
        auto dto = ctx.bind_json<ProductValidationDTO, kale::Json>();
        ctx.set_status(kale::HttpStatus::OK);
        ctx.json(kale::Json::Object{{"status", "bound_successfully"}});
        co_return;
    });
}
```
`JsonReader<J>` uses C++20 concepts to detect methods (`.contains()`, `.is_string()`, `.get<T>()`, `.is_number()`) on custom JSON types (such as `nlohmann::json` or `nlohmann::ordered_json`) without requiring extra adapter wrappers.
