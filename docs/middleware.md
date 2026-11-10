# Middleware Pipeline

This guide explains Kale's coroutine-native middleware architecture, the onion execution model, request-local context attachment, authorization layering, and the structured `HttpError` error lifecycle.

The complete runnable code for this guide is available at [`examples/middleware.cpp`](../examples/middleware.cpp).

---

## Building and Running

Compile and run the middleware example from the repository root using native `xmake`:

```bash
# Build the middleware binary target
xmake build example_middleware

# Run on default port 8080
xmake run example_middleware

# Or run with an ephemeral port assigned by the operating system
xmake run example_middleware 0
```

When started, the service prints its listening URL:
```
Listening on http://127.0.0.1:8080/
```

To stop the service, press `Ctrl+C` or send `SIGTERM`.

---

## The Onion Execution Model

In Kale, middleware functions follow the classic "onion" pattern implemented using C++20 coroutines:

```cpp
#include "coke/task.h"
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/router.h"

// Middleware signature defined in kale/core/router.h:
using Middleware = std::function<coke::Task<void>(kale::Context &ctx, kale::Next next)>;
```

Each middleware wraps downstream processing:
1. **Pre-processing phase**: Code before `co_await next()` executes on the way in.
2. **Suspension & Dispatch**: Calling `co_await next()` suspends the current middleware and invokes the next middleware in the pipeline (or the final route handler).
3. **Post-processing phase (unwinding)**: Code after `co_await next()` executes on the way out, allowing inspection and modification of response status, headers, and body.

```
Incoming Request
    │
    ▼
┌──────────────────────────────────────────────┐
│ Global Middleware (e.g. Trace & Timing)      │
│   [Pre: Record start time, attach Trace]     │
│   │
│   ▼
│ ┌──────────────────────────────────────────┐
│ │ Route-Local Middleware (e.g. Auth Guard) │
│ │   [Pre: Verify Bearer token, set User]   │
│ │   │
│ │   ▼
│ │ ┌──────────────────────────────────────┐ │
│ │ │ Final Business Route Handler         │ │
│ │ │   [Stateless Quote Calculation]      │ │
│ │ └──────────────────────────────────────┘ │
│ │   │
│ │   ▼
│ │   [Post: Route middleware unwinds]       │
│ └──────────────────────────────────────────┘
│   │
│   ▼
│   [Post: Calculate elapsed time, set headers]
└──────────────────────────────────────────────┘
    │
    ▼
Outgoing HTTP Response
```

---

## Request-Local Context Attachment

Middleware often needs to pass parsed, typed information (such as user credentials, tenant IDs, or tracing identifiers) to downstream handlers without unsafe `void*` casts or global variables.

Kale provides a type-safe heterogeneous context map built directly into `kale::Context`:

```cpp
#include <chrono>
#include <string>
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"

struct RequestTrace {
    std::string request_id;
    std::chrono::steady_clock::time_point start_time;
};

struct AuthenticatedUser {
    std::string username;
    std::string role; // e.g. "admin", "member"
    double discount_rate{0.0};
};

void attach_sample_context(kale::Context &ctx) {
    // Attach typed data to active Context
    ctx.set_attachment<RequestTrace>(RequestTrace{
        .request_id = "req-1001",
        .start_time = std::chrono::steady_clock::now()
    });

    ctx.set_attachment<AuthenticatedUser>(AuthenticatedUser{
        .username = "alice_admin",
        .role = "admin",
        .discount_rate = 0.20
    });
}
```

### Context Lifetime Rules
- `ctx.set_attachment<T>()` stores data within an internal map owned by the active `Context` instance.
- Attached data is safely destructed when the HTTP request transaction completes.
- **Rule**: Never retain pointers or references to `ctx.get_attachment<T>()` across tasks that outlive the request handler.

---

## Global vs. Route-Local Middleware

### 1. Global Middleware
Applied to all incoming requests via `server.use(...)`:

```cpp
#include <chrono>
#include <string>
#include <string_view>
#include "coke/task.h"
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_error.h"
// #include "kale/core/http_server.h"
// #include "kale/core/http_status.h"
// #include "kale/core/router.h"

struct RequestTrace {
    std::string request_id;
    std::chrono::steady_clock::time_point start_time;
};

struct AuthenticatedUser {
    std::string username;
    std::string role;
    double discount_rate{0.0};
};

void configure_middlewares(kale::HttpServer &server) {
    // 1. Global Middleware: Correlation ID, Execution Timing & Error Unwinding
    server.use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        auto start = std::chrono::steady_clock::now();

        std::string req_id = ctx.header("X-Request-Id");
        if (req_id.empty()) {
            req_id = "req-trace-auto";
        }

        ctx.set_attachment<RequestTrace>(RequestTrace{req_id, start});

        try {
            co_await next();
        } catch (const kale::HttpError &e) {
            ctx.set_status(e.status_code());
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("HTTP Error " + std::to_string(e.status_code()) + ": " + e.what() + "\n");
        }

        const auto *trace = ctx.get_attachment<RequestTrace>();
        if (trace) {
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - trace->start_time
            ).count();
            ctx.set_header("X-Request-Id", trace->request_id);
            ctx.set_header("X-Response-Time-Us", std::to_string(elapsed));
        }
    });

    // 2. Route-Local Middleware: Bearer Token Authorization Guard
    auto require_auth = [](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        std::string_view auth_hdr = ctx.header_view("Authorization");
        if (auth_hdr.empty() || !auth_hdr.starts_with("Bearer ")) {
            ctx.set_header("WWW-Authenticate", "Bearer realm=\"api\"");
            throw kale::HttpError(kale::HttpStatus::Unauthorized, "Missing or malformed Authorization header");
        }
        ctx.set_attachment<AuthenticatedUser>(AuthenticatedUser{"alice_admin", "admin", 0.20});
        co_await next();
    };

    // Attach route-local guard before the final handler
    server.POST("/quote/discounted", require_auth, [](kale::Context &ctx) {
        ctx.string("Authorized quote\n");
    });
}
```

---

## Error Handling & Exception Lifecycle

In Kale, middlewares and handlers communicate protocol/business failures by throwing structured `kale::HttpError` exceptions:

```cpp
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_error.h"
// #include "kale/core/http_status.h"

void validate_base_price(double price) {
    if (price <= 0.0) {
        throw kale::HttpError::bad_request("Request body must contain a positive numeric base price")
            .with_detail("base_price", "must be greater than zero");
    }
}
```

When an exception is thrown in downstream code:
1. Downstream handlers or middlewares throw structured `HttpError` or domain exceptions.
2. The `error_handler` middleware (or outer onion middleware) catches exceptions.
3. Errors are formatted using the configured policy and formatters (`plain_text`, `standard_json`, or RFC 9457 `problem_details`).
4. Protocol-safe headers (`WWW-Authenticate` on 401, `Retry-After` on 429/503, `Allow` on 405) are preserved while framing headers are sanitized.
5. In production mode (`expose_internal_errors = false`), 500 error messages are blurred to clients while detailed diagnostic messages are written to `Logger`.
---

## Verification & curl Recipes

Test the running `example_middleware` service with curl:

### 1. Public Endpoint (Global Middleware Only)
```bash
curl -s -i http://127.0.0.1:8080/public/status
```
**Response (200 OK)**:
```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
X-Request-Id: req-1000
X-Response-Time-Us: 14

Service Status: Operational
Correlation Trace ID: req-1000
```
Notice the global middleware injected `X-Request-Id` and `X-Response-Time-Us`.

### 2. Client-Supplied Correlation ID
```bash
curl -s -i -H "X-Request-Id: client-trace-888" http://127.0.0.1:8080/public/status
```
**Response (200 OK)**:
```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
X-Request-Id: client-trace-888
X-Response-Time-Us: 12

Service Status: Operational
Correlation Trace ID: client-trace-888
```
The server preserved and echoed the client-supplied correlation ID.

### 3. Protected Route: Missing Authorization Header
```bash
curl -s -i -X POST http://127.0.0.1:8080/quote/discounted
```
**Response (401 Unauthorized)**:
```http
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Bearer realm="api"
Content-Type: text/plain; charset=utf-8
X-Request-Id: req-1001
X-Response-Time-Us: 18

HTTP Error 401: Missing or malformed Authorization header; expected 'Bearer <token>'
```
`HttpError(HttpStatus::Unauthorized, ...)` caused the middleware to return status 401, retain the `WWW-Authenticate` header, and format the error response.

### 4. Protected Route: Invalid Token
```bash
curl -s -i -X POST http://127.0.0.1:8080/quote/discounted \
  -H "Authorization: Bearer invalid-token-123"
```
**Response (403 Forbidden)**:
```http
HTTP/1.1 403 Forbidden
Content-Type: text/plain; charset=utf-8
X-Request-Id: req-1002
X-Response-Time-Us: 15

HTTP Error 403: Invalid or expired bearer token
```

### 5. Protected Route: Empty or Invalid Body
```bash
curl -s -i -X POST http://127.0.0.1:8080/quote/discounted \
  -H "Authorization: Bearer admin-token-secret"
```
**Response (400 Bad Request)**:
```http
HTTP/1.1 400 Bad Request
Content-Type: text/plain; charset=utf-8
X-Request-Id: req-1003
X-Response-Time-Us: 20

HTTP Error 400: Request body must contain a positive numeric base price
```

### 6. Successful Protected Calculation (Admin Token - 20% Discount)
```bash
curl -s -i -X POST http://127.0.0.1:8080/quote/discounted \
  -H "Authorization: Bearer admin-token-secret" \
  -d "250.00"
```
**Response (200 OK)**:
```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
X-Request-Id: req-1004
X-Response-Time-Us: 32

Authorized Discount Quote
User: alice_admin (Role: admin)
Base Price: $250.00
Discount Rate: 20%
Discount Savings: -$50.00
Final Quoted Price: $200.00
```

### 7. Successful Protected Calculation (Member Token - 5% Discount)
```bash
curl -s -i -X POST http://127.0.0.1:8080/quote/discounted \
  -H "Authorization: Bearer member-token-secret" \
  -d "250.00"
```
**Response (200 OK)**:
```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
X-Request-Id: req-1005
X-Response-Time-Us: 30

Authorized Discount Quote
User: bob_member (Role: member)
Base Price: $250.00
Discount Rate: 5%
Discount Savings: -$12.50
Final Quoted Price: $237.50
```
The request successfully passed through:
1. Global trace middleware (recorded start time and generated `req-1005`)
2. Route-local authentication middleware (verified token and attached `AuthenticatedUser` with 5% discount)
3. Quote business handler (extracted user attachment, parsed base price, calculated discount savings and final quoted price)
4. Global trace middleware unwinding (calculated execution time and populated response headers `X-Request-Id` and `X-Response-Time-Us`)

---

## Built-in Standard Middlewares (`include/kale/middleware/`)

Kale ships with 8 standard HTTP middlewares, located under `include/kale/middleware/` and compiled into `kale_http`. Consuming code can include the convenience umbrella header `<kale/middlewares.h>` or precise component headers:

```cpp
// Convenience header for all middleware:
#include <kale/middlewares.h>

// Precise headers:
// #include "kale/middleware/access_log.h"
// #include "kale/middleware/compression.h"
// #include "kale/middleware/cors.h"
// #include "kale/middleware/error_handler.h"
// #include "kale/middleware/prometheus.h"
// #include "kale/middleware/rate_limiter.h"
// #include "kale/middleware/request_id.h"
// #include "kale/middleware/security_headers.h"
```

### 1. Access Logging (`kale::access_log`)
Records external HTTP access logs including HTTP method, path, status code, microsecond execution duration, and correlated `request_id`. Default output uses a single best-effort POSIX `::write` to `STDERR_FILENO` without requiring global locks.

### 2. Error Handler (`kale::error_handler`)
Centralized exception recovery and response formatting middleware. Catches `HttpError`, `std::exception`, and unhandled exceptions. Unconfigured server fallbacks preserve legacy-safe HTTP status reason strings (e.g. `"Not Found"`), whereas explicit formatters enable structured formats:
- `ErrorFormatters::plain_text()`: structured `"<status> [<code>] <message>\n"`
- `ErrorFormatters::standard_json()`: business JSON envelope with code, error code, and message
- `ErrorFormatters::problem_details()`: RFC 9457 Problem Details JSON payload

### 3. Response Compression (`kale::compression`)
Negotiates Zstandard (`zstd`) and Gzip (`gzip`) body compression against client `Accept-Encoding`, respecting body length thresholds and streaming boundaries. Codecs and CRC-32 checksums are powered by `kale_core`.

### 4. CORS (`kale::cors`)
Handles preflight `OPTIONS` requests, credentials, allowed origins, methods, and headers. Intercepted origins receive `403 Forbidden` (`CORS_FORBIDDEN`).
### 5. Rate Limiting (`kale::rate_limiter`)
Handles preflight `OPTIONS` requests, credentials, allowed origins, methods, and headers. Intercepted origins receive `403 Forbidden` (`CORS_FORBIDDEN`).

### 6. Request ID (`kale::request_id`)
Enforces request rate limits using in-memory or custom storage. Denied requests receive `429 Too Many Requests` (`RATE_LIMITED`) with standard `Retry-After` headers.

### 7. Security Headers (`kale::security_headers`)
Checks for incoming `X-Request-Id` headers or generates cryptographically safe 128-bit hex UUID identifiers attached to the active `Context`.

### 8. Prometheus Telemetry (`kale::prometheus_metrics`)
Injects defensive HTTP headers including `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, and `Referrer-Policy: strict-origin-when-cross-origin`.

### 9. Prometheus Telemetry (`kale::prometheus_metrics`)
Collects HTTP request counts and latency histograms into `kale::PrometheusRegistry` for scraping via `kale::prometheus_exporter`.
