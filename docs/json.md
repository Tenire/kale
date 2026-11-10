# Native JSON Service & Workflow Node Semantics

This task guide demonstrates building asynchronous JSON microservices in Kale. It covers native JSON document manipulation, request body parsing, coroutine-native response serialization, and the memory and numeric model inherited from the underlying Workflow C JSON parser.

Source code: [`../examples/json_api.cpp`](../examples/json_api.cpp)

---

## 1. Goal

The `example_json_api` service implements a pricing and audit metadata service that demonstrates:
- Asynchronous coroutine request handlers returning `coke::Task<void>` and concluding with `co_return;`.
- Constructing native JSON payloads using `kale::Json::Object` and `kale::Json::Array`.
- Parsing and inspecting request bodies using `ctx.req_body()` and `kale::Json::parse(...)`.
- Workflow number representation (IEEE 754 double precision) and numeric extraction via `get<double>()` and `get<int>()`.
- Dynamic proxy mutations (`doc["key"] = ...`) and creating detached owning copies via copy construction.

---

## 2. Build & Run

Kale uses native `xmake` for building targets.

### Build
From the repository root:
```bash
xmake build example_json_api
```

### Run
Run on the default port (`8080`):
```bash
xmake run example_json_api
```

Run on an explicit port (e.g. `9090`):
```bash
xmake run example_json_api 9090
```

Run on an OS-assigned ephemeral port (`0`):
```bash
xmake run example_json_api 0
```

---

## 3. Workflow JSON Architecture & Lifetime Semantics

Kale's `kale::Json` in `kale/http/json.h` wraps the Workflow C-style parser (`workflow/json_parser.h`). Understanding its internals avoids common memory and precision hazards:

### Numeric Representation: IEEE 754 `double`
All JSON numbers parsed by Workflow are created via `json_value_create(JSON_VALUE_NUMBER, static_cast<double>(val))` and queried with `json_value_number(node)`.
- **Underlying Type**: Numbers are stored as 64-bit IEEE 754 floating-point values (`double`).
- **Integer Conversions**: Calling `node.get<int>()` or `node.get<int64_t>()` validates that the floating-point value is finite, integer-valued, and within the range of type `T`, then casts it.
- **Safe Precision Range**: Standard IEEE 754 doubles provide 53 bits of mantissa precision. Exact integer arithmetic is guaranteed only within $[-2^{53} + 1, 2^{53} - 1]$ (i.e. $\pm 9{,}007{,}199{,}254{,}740{,}991$). 64-bit database keys or cryptographic integers outside this range should be transmitted as strings.

### Node Lifetimes: Owning Trees vs. Proxy Borrowers
`kale::Json` objects operate in two distinct modes:
1. **Owning Node (`allocated_ == true`)**:
   Created by root constructors, `kale::Json::parse(...)`, or `kale::Json::Object{...}`. The instance owns the underlying `json_value_t *` tree and automatically releases it upon destruction.
2. **Borrowed Proxy Watcher (`allocated_ == false`)**:
   Created by indexing into an existing object (`kale::Json meta = root["metadata"];`). The proxy holds borrowed raw pointers into `root`'s memory tree.

```
+-----------------------------------------------------------+
| root (allocated_ = true, owns memory tree)                |
|   ├── "service": "catalog-pricing-engine"                 |
|   └── "metadata": { "env": "prod" } <----------------+    |
+------------------------------------------------------|----+
                                                       | (borrowed pointer)
+------------------------------------------------------|----+
| meta (allocated_ = false, proxy watcher) ------------+    |
+-----------------------------------------------------------+
```

> **Warning (Dangling Proxy Hazard)**: A proxy node borrowed via `operator[]` depends on the parent tree's lifetime. If the parent `Json` is destroyed, reassigned, or modified, borrowed proxies become invalid. To safely persist or mutate a subtree independently, create a deep copy using public lvalue copy construction:

```cpp
// Convenience headers (active):
#include <kale/http.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/http/json.h"

void demonstrate_lvalue_copy() {
    kale::Json root = kale::Json::Object{{"metadata", kale::Json::Object{{"env", "prod"}}}};
    auto proxy = root["metadata"];
    kale::Json detached(proxy); // actual public lvalue copy construction
    detached["env"] = "staging";
}
```
---

## 4. Curl Recipes & Expected Responses

All endpoints produce `Content-Type: application/json; charset=utf-8`.

### 4.1. Query Service Information (GET /api/service-info)
Demonstrates structured native `kale::Json::Object` and `kale::Json::Array` construction:
```bash
curl -s http://127.0.0.1:8080/api/service-info | jq .
```
Expected Response (`200 OK`):
```json
{
  "service": "catalog-pricing-engine",
  "version": "1.0.0",
  "engine": "kale-native-workflow-json",
  "features": [
    "dynamic-proxy-mutations",
    "double-precision-arithmetic",
    "coroutine-native"
  ]
}
```

---

### 4.2. Calculate Order Quotation (POST /api/quote/calculate)
Demonstrates JSON parsing, numeric extraction, and IEEE 754 arithmetic:
```bash
curl -s -X POST http://127.0.0.1:8080/api/quote/calculate \
  -H "Content-Type: application/json" \
  -d '{
    "item_name": "Mechanical Keyboard",
    "unit_price": 129.99,
    "quantity": 3,
    "discount_code": "SUMMER10"
  }' | jq .
```
Expected Response (`200 OK`):
```json
{
  "item_name": "Mechanical Keyboard",
  "unit_price": 129.99,
  "quantity": 3,
  "subtotal": 389.97,
  "discount_applied": 38.997,
  "total": 350.973,
  "currency": "USD"
}
```

Attempting to submit non-positive quantities returns a validation error:
```bash
curl -s -X POST http://127.0.0.1:8080/api/quote/calculate \
  -H "Content-Type: application/json" \
  -d '{"item_name": "Mouse", "unit_price": 25.0, "quantity": 0}' | jq .
```
Expected Response (`400 Bad Request`):
```json
{
  "error": "OUT_OF_BOUNDS",
  "message": "unit_price and quantity must be positive numbers"
}
```

---

### 4.3. Audit & Metadata Enrichment (POST /api/audit/enrich)
Demonstrates borrowing a proxy node and detaching it via copy construction before mutating:
```bash
curl -s -X POST http://127.0.0.1:8080/api/audit/enrich \
  -H "Content-Type: application/json" \
  -d '{
    "trace_id": "req-98765",
    "metadata": {
      "region": "us-east-1",
      "cluster_id": 42
    }
  }' | jq .
```
Expected Response (`200 OK`):
```json
{
  "status": "enriched",
  "original_metadata_present": true,
  "enriched_metadata": {
    "region": "us-east-1",
    "cluster_id": 42,
    "inspected_by": "kale-json-audit",
    "workflow_number_format": "IEEE-754-double",
    "is_detached": true
  }
}
```

---

## 5. Generic Customization & Third-Party JSON Libraries

Kale's `Context::json` concept-constrains against any type supporting `.dump() -> std::convertible_to<std::string_view>`. Custom JSON or third-party libraries (e.g. `nlohmann::json`, `nlohmann::ordered_json`) serialize directly through `ctx.json(doc)` without wrapper boilerplate:

```cpp
#include <string>
#include "coke/task.h"

// Convenience headers (active):
#include <kale/core.h>
#include <kale/http.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"
// #include "kale/http/json.h"

struct CustomDumpableJson {
    std::string dump() const {
        return R"({"framework":"kale","custom_json":true})";
    }
};

void register_custom_json_route(kale::HttpServer &server) {
    server.GET("/api/custom-json", [](kale::Context &ctx) -> coke::Task<void> {
        CustomDumpableJson doc;
        ctx.json(doc);
        co_return;
    });
}
```
---

## 6. Real Lifetime & Protocol Limits

1. **Borrowing vs. Owning**: Always distinguish between owning `kale::Json` instances and borrowed proxy nodes. Do not store proxy nodes in long-lived state or capture them across coroutine tasks.
2. **Double Precision**: Remember that numeric values are parsed as IEEE 754 `double`. If your domain requires exact 64-bit unsigned integers (such as Snowflake IDs) or arbitrary-precision currency, pass them as strings.
3. **Framing & Content-Type**: `ctx.json(...)` automatically sets `Content-Type: application/json; charset=utf-8`. Incoming requests should include `Content-Type: application/json` checked via `ctx.is_json()`.
