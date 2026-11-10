# Kale Documentation

Kale is an asynchronous, coroutine-native C++20 HTTP framework built atop [Coke](https://github.com/zkshao/coke) and Sogou [Workflow](https://github.com/sogou/workflow). It provides asynchronous HTTP services with structured routing, middleware onion pipelines, static file caching, streaming multipart uploads, Server-Sent Events (SSE), OpenAPI generation, session/JWT authentication, reverse proxying, and Prometheus operational metrics.

This documentation serves as a practical, task-oriented manual organized by capabilities, guides, and runnable example programs.

---

## Documentation Map

### Getting Started & Configuration
- **[Getting Started](getting-started.md)**: Prerequisites, dependency closure, project setup, building with `xmake`, writing your first synchronous and coroutine-native endpoints, lifecycle management, and signal handling.
- **[Server Configuration](server-configuration.md)**: Server parameters (`HttpServerParams`), connection governance, timeouts, TLS configuration with OpenSSL, graceful shutdown semantics, and memory vs streaming buffer boundaries.

### Core Routing & Middleware
- **[Routing & Handlers](routing.md)**: Route registration (`GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD`, `OPTIONS`, `any`), path parameters (`:id`), wildcards (`*path`), query string parsing, status codes, header manipulation, redirects, synchronous vs coroutine handlers, and modular `Blueprint` routing.
- **[Middleware Pipeline](middleware.md)**: Onion-model middleware architecture, global and route-local middleware, request context data attachments, execution ordering, request timing, access logging, and error handling.

### Data Handling & APIs
- **[JSON Processing](json.md)**: Parsing JSON request payloads, constructing structured JSON responses with native `kale::Json`, serialization, error responses, and coroutine-safe data handling.
- **[Typed API & OpenAPI](typed-api.md)**: Declarative DTO binding (`FieldDesc`), automatic input validation, OpenAPI 3.0 schema and route generation, and application error boundaries.
- **[Form Handling](forms.md)**: Processing `application/x-www-form-urlencoded` submissions and inspecting `multipart/form-data` fields and file metadata.

### Files & Streaming
- **[Static File Serving](files.md)**: Static asset delivery, in-memory `FileCache`, MIME type resolution, and HTTP Range requests (RFC 7233 partial content).
- **[Multipart File Uploads](uploads.md)**: Handling single and multi-file multipart uploads, asynchronous disk writes, destination directory controls, and request payload limits.
- **[Resumable Uploads](resumable-upload.md)**: Chunked upload coordinator, session state tracking (initialize, append offset chunks, query status, complete, abort), and EVP SHA-256 integrity verification.
- **[Server-Sent Events & Streaming](streaming.md)**: Coroutine-driven event feeds (SSE) via `kale::SseEmitter` using dedicated per-subscriber queues, client disconnect teardown, and HTTP chunked transfer encoding.

### Security & Authentication
- **[Session Management](sessions.md)**: State management with encrypted/signed cookies, cookie configuration (HttpOnly, SameSite, Secure), session lifetime, and login/logout workflows.
- **[Authentication & Security](authentication.md)**: JWT token generation and verification, claim validation (expiry, audience, issuer), Bearer authorization guards, CSRF protection middleware, and secure form submissions.

### Client & Operational Services
- **[HTTP Client](http-client.md)**: Asynchronous outbound HTTP requests using `coke::HttpClient`, upstream service aggregation, timeouts, and error handling.
- **[Reverse Proxy](reverse-proxy.md)**: Upstream request forwarding, header filtering (hop-by-hop headers), body pass-through, and proxy timeouts.
- **[Operations & Observability](operations.md)**: Service status endpoints (`/health`, `/ready`), Prometheus metrics exposition (`/metrics`), Gzip/Zstandard response compression, rate limiting, and security headers.

---

## Runnable Example Programs

All examples reside in the `examples/` directory. Each program is a standalone, runnable service or client demonstrating a complete user workflow.

| Basename | Target Name | Module & Minimal Deps | Guide | Description |
|---|---|---|---|---|
| `hello.cpp` | `example_hello` | `kale_core` | [Getting Started](getting-started.md) | Synchronous and coroutine Hello World with signal handling |
| `routing.cpp` | `example_routing` | `kale_core` | [Routing](routing.md) | Parameters, queries, wildcards, 404/405 handlers, and Blueprints |
| `middleware.cpp` | `example_middleware` | `kale_core` | [Middleware](middleware.md) | Global/route middleware, context attachments, onion execution |
| `json_api.cpp` | `example_json_api` | `kale_http` | [JSON](json.md) | Structured JSON request/response processing and coroutines |
| `typed_api.cpp` | `example_typed_api` | `kale_http` | [Typed API](typed-api.md) | DTO binding, validation, and OpenAPI documentation |
| `forms.cpp` | `example_forms` | `kale_http` | [Forms](forms.md) | URL-encoded and multipart form input inspection |
| `file_service.cpp` | `example_file_service` | `kale_http` | [Files](files.md) | FileCache, static asset delivery, and RFC 7233 range requests |
| `uploads.cpp` | `example_uploads` | `kale_http` | [Uploads](uploads.md) | Multipart file uploads with asynchronous disk persistence |
| `resumable_upload.cpp` | `example_resumable_upload` | `kale_http`, `kale_security` | [Resumable Upload](resumable-upload.md) | Chunked resumable upload session with EVP SHA-256 validation |
| `event_stream.cpp` | `example_event_stream` | `kale_http` | [Streaming](streaming.md) | Server-Sent Events (SSE) publisher/subscriber channel |
| `sessions.cpp` | `example_sessions` | `kale_security` | [Sessions](sessions.md) | Cookie-based session authentication, login, profile, and logout |
| `jwt_auth.cpp` | `example_jwt_auth` | `kale_security` | [Authentication](authentication.md) | JWT token creation, Bearer header authorization, and verification |
| `secure_forms.cpp` | `example_secure_forms` | `kale_security` | [Authentication](authentication.md) | CSRF token validation and protected state-changing form actions |
| `http_client.cpp` | `example_http_client` | `kale_client` | [HTTP Client](http-client.md) | Asynchronous client calling an upstream HTTP service |
| `reverse_proxy.cpp` | `example_reverse_proxy` | `kale_client` | [Reverse Proxy](reverse-proxy.md) | Upstream gateway proxying requests, headers, and bodies |
| `operations.cpp` | `example_operations` | `kale_http` | [Operations](operations.md) | Compression (gzip/zstd), rate limiting, metrics, and health check |
| `access_log.cpp` | `example_access_log` | `kale_http`, `kale_core` | [Middleware](middleware.md) | Access log pipeline with request ID, duration tracking, and internal Logger diagnostics |

---

## Building and Running Examples

The repository uses [xmake](https://xmake.io) as its build system. Targets declare minimal library dependencies and run with project root as their working directory.

### Build All Examples
```bash
xmake build examples
```

### Build and Run a Specific Example
```bash
xmake build example_hello
xmake run example_hello [PORT]
```

By default, example servers bind to `127.0.0.1:8080`. Passing `0` binds an ephemeral port allocated by the operating system:
```bash
xmake run example_hello 0
# Outputs: Listening on http://127.0.0.1:41235/
```

To stop any running example service, send `SIGINT` (Ctrl+C) or `SIGTERM`.

### Enabling TLS
Examples support optional TLS via environment variables pointing to PEM certificate and private key files:
```bash
KALE_EXAMPLE_CERT=server.crt KALE_EXAMPLE_KEY=server.key xmake run example_hello 8443
```
For local self-signed certificate generation instructions, see [Server Configuration](server-configuration.md).

---

## Framework Architecture & Conventions

### Modular Library Layout
Kale is divided into 4 focused static libraries, allowing targets to link only what they need:
- **`kale_core`**: Minimal core runtime containing `HttpServer`, `Router`, `Blueprint`, `Context`, `Request`, `Response`, `Cookie`, `Logger`, core error models, raw Gzip/Zstd codecs, and CRC32 checksums. Links `workflow`, `coke`, `zlib`, and `zstd`.
- **`kale_http`**: High-level HTTP abstractions including `kale::Json`, `FileCache`, multipart forms, streaming uploads, SSE (`ServerSentEventStream`), OpenAPI, schema validation, and the standard middleware suite (`middleware/`). Depends on `kale_core`.
- **`kale_security`**: Cryptographic and authentication services including sessions, signed cookies, JWT token issuance/verification, CSRF protection, and OpenSSL EVP checksums. Depends on `kale_core` and `kale_http`.
- **`kale_client`**: Outbound asynchronous HTTP client (`coke::HttpClient` integration) and reverse proxy gateway. Depends on `kale_core` and `kale_http`.
- **`kale`**: Umbrella phony target aggregating all four libraries for applications wanting the full framework surface.

### Header Organization
Kale supports both precise headers for granular dependencies and bounded umbrella convenience headers for streamlined development.

#### Header Usage Policy
- **Documentation snippets**: May present convenience umbrella headers (active) alongside commented precise include groups to illustrate both approaches without duplicating code blocks.
- **Examples, internal headers, library sources, and test suites**: Strictly retain precise, minimal includes.
- **Linking policy**: Header inclusion alone does not force linking unnecessary libraries. Targets continue to follow modular library linking (`kale_core`, `kale_http`, `kale_security`, `kale_client`, or the umbrella `kale`). Note that `<kale/middlewares.h>` includes all standard middleware headers (`<kale/middleware/*.h>`), with compression and codecs backed by `kale_core` and `kale_http`.

#### Umbrella Convenience Headers
Eight umbrella headers aggregate public interfaces across functional domains:
1. **`<kale/core.h>`**: Core server, context, router, cookies, methods, errors, logger, codecs, and status codes (`checksum.h`, `compress.h`, `context.h`, `cookie.h`, `crc32_checksum.h`, `error_response.h`, `http_error.h`, `http_method.h`, `http_server.h`, `http_status.h`, `http_util.h`, `logger.h`, `router.h`). Note: Does not include `blueprint.h`.
2. **`<kale/routing.h>`**: Modular routing and composition (`blueprint.h`, `router.h`).
3. **`<kale/http.h>`**: HTTP payloads, caching, forms, multipart uploads, SSE, and health endpoints (`file_cache.h`, `file_response.h`, `form.h`, `health.h`, `json.h`, `multipart.h`, `resumable_upload.h`, `sse.h`, `static_files.h`, `upload.h`).
4. **`<kale/api.h>`**: DTO binding, validation, and OpenAPI 3.0 generation (`binding.h`, `field_desc.h`, `json_reader.h`, `openapi.h`, `validator.h`).
5. **`<kale/stream.h>`**: Streaming context, file response, and Server-Sent Events (`context.h`, `file_response.h`, `sse.h`).
6. **`<kale/middlewares.h>`**: Built-in HTTP middlewares (`access_log.h`, `compression.h`, `cors.h`, `error_handler.h`, `prometheus.h`, `rate_limiter.h`, `request_id.h`, `security_headers.h`).
7. **`<kale/security.h>`**: Authentication, JWT tokens, sessions, CSRF protection, and cryptographic checksums (`csrf.h`, `evp_checksum.h`, `jwt.h`, `session.h`, `signed_cookie.h`).
8. **`<kale/client.h>`**: Outbound HTTP client and reverse proxy gateway (`http_client.h`, `reverse_proxy.h`).

There are no `kale.h` or `compress.h` umbrella headers; non-aggregated components (such as standalone compressors or precise utilities) use explicit subsystem headers. For details on individual subsystems, see the respective documentation guides linked in the [Documentation Map](#documentation-map).

#### Precise Headers Reference
For minimal compile boundaries and fine-grained dependencies, include exact headers directly:
- Core server and routing: `#include "kale/core/http_server.h"`, `#include "kale/core/router.h"`, `#include "kale/core/blueprint.h"`, `#include "kale/core/context.h"`, `#include "kale/core/http_status.h"`, `#include "kale/core/logger.h"`
- HTTP, JSON & files: `#include "kale/http/json.h"`, `#include "kale/http/binding.h"`, `#include "kale/http/file_cache.h"`, `#include "kale/http/multipart.h"`, `#include "kale/http/sse.h"`
- Middlewares: `#include "kale/middleware/access_log.h"`, `#include "kale/middleware/compression.h"`, `#include "kale/middleware/cors.h"`, `#include "kale/middleware/error_handler.h"`, `#include "kale/middleware/prometheus.h"`, `#include "kale/middleware/rate_limiter.h"`, `#include "kale/middleware/request_id.h"`, `#include "kale/middleware/security_headers.h"`
- Security & authentication: `#include "kale/security/session.h"`, `#include "kale/security/jwt.h"`, `#include "kale/security/csrf.h"`
- Client & proxy: `#include "kale/client/http_client.h"`, `#include "kale/client/reverse_proxy.h"`
- Codecs & checksums: `#include "kale/core/compress.h"`, `#include "kale/core/crc32_checksum.h"`
### Asynchronous Coroutine Model
Kale handlers are native C++20 coroutines returning `coke::Task<void>` (or synchronous functions taking `kale::Context &`). In coroutine handlers, use `co_await` with Coke tasks (such as `coke::sleep`, `coke::HttpClient`, or asynchronous file operations) without blocking operating system threads:

```cpp
#include <chrono>
#include "coke/sleep.h"
#include "coke/task.h"
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"
// #include "kale/core/http_status.h"

void register_async_route(kale::HttpServer &server) {
    server.GET("/async", [](kale::Context &ctx) -> coke::Task<void> {
        co_await coke::sleep(std::chrono::milliseconds(50));
        ctx.set_status(kale::HttpStatus::OK);
        ctx.string("Async complete\n");
        co_return;
    });
}
```

### Request-Local Lifetimes
The `kale::Context` reference is valid only during the execution of that specific HTTP request handler. Never capture references to `Context` or request buffer slices across asynchronous tasks that outlive the handler's coroutine frame. If data must outlive the handler, copy it into owned `std::string` or value types.
