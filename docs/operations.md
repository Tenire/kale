# Operations, Observability & Compression

This guide explains how to configure an operable HTTP service using Kale's middleware pipeline. It demonstrates response compression negotiation (Zstandard and Gzip), request correlation (`X-Request-Id`), structured access logging, CORS preflight handling, HTTP security headers, in-memory fixed-window rate limiting, Prometheus telemetry exposition, and health/readiness probes.

The complete runnable example program is located at [`../examples/operations.cpp`](../examples/operations.cpp).

---

## Architecture & Middleware Ordering

Kale structures middleware concerns into a deterministic onion-model pipeline:
- **Request Ingress**: Middlewares run in the exact order they are registered via `server.use()`.
- **Response Egress**: As downstream handlers complete, execution unwinds in reverse order.

```
Incoming HTTP Request
       |
       v
+-------------------------------------------------------------+
| 1. kale::compression()              [Outermost: Body Compr] |
|   +---------------------------------------------------------+
|   | 2. kale::access_log()           [Timing & Status Logger]|
|   |   +-----------------------------------------------------+
|   |   | 3. kale::request_id()       [X-Request-Id Injector] |
|   |   |   +-------------------------------------------------+
|   |   |   | 4. kale::cors()         [CORS & Preflight Guard]|
|   |   |   |   +---------------------------------------------+
|   |   |   |   | 5. kale::security_headers() [Nosniff/Frame] |
|   |   |   |   |   +-----------------------------------------+
|   |   |   |   |   | 6. kale::rate_limiter() [20 req/s Limit]|
|   |   |   |   |   |   +-------------------------------------+
|   |   |   |   |   |   | 7. kale::prometheus_metrics()       |
|   |   |   |   |   |   |   +---------------------------------+
|   |   |   |   |   |   |   | Final Route Handler             |
|   |   |   |   |   |   |   | (/health, /metrics, /api/...)   |
+---+---+---+---+---+---+---+---------------------------------+
```

### Why Compression Is Outermost
`kale::compression` modifies response bodies during stack unwinding. Placing compression outermost ensures:
1. Downstream handlers generate standard uncompressed text or JSON bodies.
2. Intermediary middlewares (such as security headers, CORS, or access loggers) inspect standard HTTP payloads and status codes before compression occurs.
3. Compression evaluates client `Accept-Encoding` preferences (`zstd` > `gzip`), compresses the body buffer in-place if body size meets `min_length`, and sets `Content-Encoding` and the updated `Content-Length`.

*Note: Compression reduces payload bytes transmitted across the network; it introduces CPU encoding overhead and does not improve local raw request throughput.*

---

## Building and Running

Compile the operations example using `xmake`:

```bash
xmake build example_operations
```

Run on the default port `8080`:

```bash
xmake run example_operations 8080
```
Expected output:
```
Listening on http://127.0.0.1:8080/
```

To stop the service, send `SIGINT` (`Ctrl+C`) or `SIGTERM`.

---

## Application-Owned Sinks & Stores

Middleware stores and sinks must outlive the server and all active requests. In `examples/operations.cpp`, stores and sinks are instantiated in `main()` before `examples::serve(server, options)`:

```cpp
#include "example_server.h"
#include <chrono>
#include <iostream>
#include <mutex>
// Convenience headers (active):
#include <kale/core.h>
#include <kale/http.h>
#include <kale/middlewares.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/middleware/access_log.h"
// #include "kale/http/health.h"
// #include "kale/middleware/prometheus.h"
// #include "kale/middleware/rate_limiter.h"

class ConsoleAccessLogger {
public:
    void log(const kale::AccessLogRecord &rec) {
        std::lock_guard<std::mutex> lock(mu_);
        std::cout << rec.method << " " << rec.target << "\n";
    }
private:
    std::mutex mu_;
};

int main(int argc, char *argv[]) {
    examples::ServerOptions options(argc, argv);
    kale::HttpServer server;

    // 1. Thread-safe log sink
    ConsoleAccessLogger log_sink;

    // 2. Fixed-window in-memory rate limit store
    kale::RateLimitPolicy rate_policy;
    rate_policy.limit = 20;                        // 20 requests
    rate_policy.window = std::chrono::seconds(1);  // per 1-second window
    rate_policy.max_keys = 10000;                  // max 10k concurrent keys
    kale::MemoryRateLimitStore rate_store(rate_policy);

    // 3. Prometheus metrics registry
    kale::PrometheusRegistry metrics_registry;

    // 4. Thread-safe atomic readiness flag for health probes
    kale::ReadinessState readiness(true);

    // Register middlewares referencing above stores...

    return examples::serve(server, options);
}
```

Because `examples::serve` blocks on signal handling until termination, all stores and sinks remain valid throughout the service lifecycle.

---

## Middleware Component Reference

### 1. Compression (`kale::compression`)
Negotiates encoding (`zstd` > `gzip`) based on client `Accept-Encoding`:

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/middlewares.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/middleware/compression.h"
// #include "kale/core/http_server.h"

void setup_compression(kale::HttpServer &server) {
    kale::CompressConfig comp_cfg;
    comp_cfg.min_length = 512; // Minimum body size in bytes to trigger compression
    comp_cfg.set_zstd_level(3).set_gzip_level(6);
    server.use(kale::compression(comp_cfg));
}
```

### 2. Access Logging (`kale::access_log`)
Invokes a thread-safe sink upon downstream completion:

```cpp
#include <iostream>

// Convenience headers (active):
#include <kale/core.h>
#include <kale/middlewares.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/middleware/access_log.h"

void setup_access_log(kale::HttpServer &server) {
    server.use(kale::access_log([](kale::AccessLogRecord rec) {
        std::cout << rec.method << " " << rec.target
                  << " -> " << rec.status_code
                  << " (" << rec.duration.count() << " us)"
                  << " id=" << rec.request_id << "\n";
    }));
}
```

### 3. Request Correlation (`kale::request_id`)
Checks for existing `X-Request-Id` headers or generates a 128-bit hex UUID. Handlers retrieve it via `kale::get_request_id(ctx)`:

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/middlewares.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/middleware/request_id.h"

void setup_request_id(kale::HttpServer &server) {
    server.use(kale::request_id());
}
```

### 4. Cross-Origin Resource Sharing (`kale::cors`)
Handles preflight `OPTIONS` requests (returning 204 when allowed) and attaches `Access-Control-*` headers:

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/middlewares.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/middleware/cors.h"

void setup_cors(kale::HttpServer &server) {
    kale::CorsConfig cors_cfg;
    cors_cfg.allow_origins = {"*"};
    cors_cfg.allow_methods = {"GET", "POST", "PUT", "DELETE", "OPTIONS"};
    cors_cfg.allow_headers = {"Content-Type", "Authorization", "X-Request-Id"};
    cors_cfg.max_age = 3600;
    server.use(kale::cors(cors_cfg));
}
```

### 5. HTTP Security Headers (`kale::security_headers`)
Injects defensive headers:
- `X-Content-Type-Options: nosniff`
- `X-Frame-Options: DENY`
- `Referrer-Policy: strict-origin-when-cross-origin`

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/middlewares.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/middleware/security_headers.h"

void setup_security_headers(kale::HttpServer &server) {
    server.use(kale::security_headers());
}
```

### 6. Rate Limiting (`kale::rate_limiter`)
Enforces fixed-window limits using `RateLimitStore`. Denied requests receive `429 Too Many Requests` with a `Retry-After: N` header:

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/middlewares.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/middleware/rate_limiter.h"

void setup_rate_limiter(kale::HttpServer &server, kale::RateLimitStore &rate_store) {
    kale::RateLimiterConfig limiter_cfg; // Defaults to client peer IP address
    server.use(kale::rate_limiter(rate_store, limiter_cfg));
}
```

### 7. Prometheus Metrics (`kale::prometheus_metrics`)
Records counts and execution durations into `PrometheusRegistry`. Exposes metrics via `kale::prometheus_exporter`:

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/middlewares.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/middleware/prometheus.h"

void setup_prometheus(kale::HttpServer &server, kale::PrometheusRegistry &metrics_registry) {
    kale::PrometheusConfig prom_cfg;
    prom_cfg.skip_route_tag = "metrics.skip";
    server.use(kale::prometheus_metrics(metrics_registry, prom_cfg));

    server.GET("/metrics", kale::prometheus_exporter(metrics_registry));
}
```

---

## curl Interactions

### 1. Health & Readiness Probes

#### Liveness Probe
```bash
curl -i -s http://127.0.0.1:8080/health
```
Expected output:
```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Content-Length: 2
Connection: keep-alive

OK
```

#### Readiness Probe
```bash
curl -i -s http://127.0.0.1:8080/ready
```
Expected output:
```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Content-Length: 5
Connection: keep-alive

READY
```

#### Readiness Toggle (Simulate Node Drain)
```bash
curl -s -X POST http://127.0.0.1:8080/ready/toggle | jq .
```
Expected output:
```json
{
  "action": "toggle_readiness",
  "ready": false
}
```

Querying `/ready` now returns `503 Service Unavailable`:
```bash
curl -i -s http://127.0.0.1:8080/ready
```
Expected output:
```http
HTTP/1.1 503 Service Unavailable
Content-Type: text/plain; charset=utf-8
Content-Length: 7
Connection: keep-alive

UNREADY
```

Toggle back to ready:
```bash
curl -s -X POST http://127.0.0.1:8080/ready/toggle | jq .
```

---

### 2. Response Compression (Zstandard & Gzip)

#### Uncompressed Request
```bash
curl -i -s http://127.0.0.1:8080/api/catalog
```
Expected headers:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Content-Length: 2287
X-Request-Id: 5e2b...
X-Content-Type-Options: nosniff
X-Frame-Options: DENY
```

#### Zstandard Request (`Accept-Encoding: zstd`)
```bash
curl -i -s http://127.0.0.1:8080/api/catalog -H "Accept-Encoding: zstd"
```
Expected headers:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Content-Encoding: zstd
Vary: Accept-Encoding
Content-Length: 326
```

#### Gzip Request (`Accept-Encoding: gzip`)
```bash
curl -i -s http://127.0.0.1:8080/api/catalog -H "Accept-Encoding: gzip"
```
Expected headers:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Content-Encoding: gzip
Vary: Accept-Encoding
Content-Length: 489
```

---

### 3. Stateless Quote Calculation & Request-ID Correlation

Send a JSON payload with quantity and unit price:
```bash
curl -s -X POST http://127.0.0.1:8080/api/quote \
  -H "Content-Type: application/json" \
  -H "X-Request-Id: client-trace-42" \
  -d '{"quantity":4,"unit_price":20.0}' | jq .
```
Expected output:
```json
{
  "quantity": 4,
  "request_id": "client-trace-42",
  "subtotal": 80,
  "tax": 6.4,
  "total": 86.4,
  "unit_price": 20
}
```
Notice:
- `request_id` echoes the incoming client trace ID from `kale::get_request_id(ctx)`.
- The calculation is computed statelessly from the input values.
- Access logger prints duration and status to stdout.

---

### 4. Rate Limiting Enforced

Send a burst exceeding 20 requests within one second:
```bash
for i in {1..25}; do curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/api/catalog; done
```
Output transitions to `429`:
```
200
200
...
429
429
```

Inspecting a rate-limited response:
```bash
curl -i -s http://127.0.0.1:8080/api/catalog
```
Expected output:
```http
HTTP/1.1 429 Too Many Requests
Content-Type: text/plain; charset=utf-8
Retry-After: 1
Content-Length: 21

Too Many Requests
```

---

### 5. Prometheus Telemetry Exposition

```bash
curl -s http://127.0.0.1:8080/metrics
```
Expected output:
```prometheus
# HELP http_requests_total Total number of HTTP requests processed
# TYPE http_requests_total counter
http_requests_total{method="GET",route="/api/catalog",status="200"} 20
http_requests_total{method="GET",route="/api/catalog",status="429"} 5
http_requests_total{method="POST",route="/api/quote",status="200"} 1

# HELP http_request_duration_seconds HTTP request execution duration in seconds
# TYPE http_request_duration_seconds histogram
http_request_duration_seconds_sum{method="GET",route="/api/catalog",status="200"} 0.00351
http_request_duration_seconds_count{method="GET",route="/api/catalog",status="200"} 20
```

---

## Library Dependencies

The operational components require linking against `kale_http`:

```lua
target("my_operable_service")
    set_kind("binary")
    add_deps("kale_http")
    add_files("main.cpp")
```

- `kale_http`: Provides access logger, CORS, request-id, security headers, rate limiter, Prometheus metrics, health handlers, and the compression middleware (`kale::compression`).
- `kale_core`: Transitive base dependency providing `HttpServer`, `Router`, `Context`, `Logger`, and raw Gzip/Zstd/CRC32 codecs (linking `zlib` and `zstd`).
