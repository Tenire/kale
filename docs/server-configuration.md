# Server Configuration & Architecture

This guide covers configuring `kale::HttpServer`, tuning connection limits and timeouts, setting up TLS encryption with OpenSSL, understanding graceful shutdown semantics, and working within memory buffering vs streaming boundaries.

---

## Configuring HttpServerParams

`kale::HttpServer` accepts a `kale::HttpServerParams` structure at construction to configure server-wide networking, resource limits, and path normalization.

```cpp
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"

void configure_server() {
    kale::HttpServerParams params;
    params.set_max_connections(5000)
          .set_peer_response_timeout(5000)       // 5 seconds
          .set_receive_timeout(10000)            // 10 seconds
          .set_keep_alive_timeout(30000)         // 30 seconds
          .set_request_size_limit(10 * 1024 * 1024) // 10 MiB limit
          .set_ssl_accept_timeout(5000)          // 5 seconds
          .set_clean_path(true);

    kale::HttpServer server(params);
    (void)server;
}
```

### Parameter Reference

| Parameter | Type | Default | Description |
|---|---|---|---|
| `max_connections` | `size_t` | `2000` | Maximum number of concurrent client TCP connections accepted by the server. |
| `peer_response_timeout` | `int` (ms) | `10000` | Maximum time to wait for peer socket response before closing connection. |
| `receive_timeout` | `int` (ms) | `-1` | Maximum time to wait while reading request data (`-1` indicates no timeout). |
| `keep_alive_timeout` | `int` (ms) | `60000` | Timeout for idle keep-alive connections before termination. |
| `request_size_limit` | `size_t` | `unlimited` | Maximum allowable request body size in bytes. Requests exceeding this receive `413 Payload Too Large`. |
| `ssl_accept_timeout` | `int` (ms) | `10000` | Maximum time allowed to complete the TLS/SSL handshake. |
| `clean_path` | `bool` | `true` | Automatically normalize URL paths, removing duplicate slashes (`//`) and dot segments (`.` / `..`). |
| `connection_governance` | `bool` | `true` | Enables connection throttling and load-shedding when connection count reaches high watermark. |
| `max_requests_per_connection` | `size_t` | `10000` | Maximum number of HTTP requests serviced on a single persistent connection before issuing `Connection: close`. |
| `high_watermark_ratio` | `double` | `0.95` | Threshold ratio of `max_connections` (95%) at which connection governance begins shedding connections. |
| `logger` | `Logger` | `Warn` | Internal kernel logger for lifecycle events and unhandled exceptions. Use `Logger::off()` to silence completely. |
| `show_banner` | `bool` | `false` | Enables startup banner and runtime dashboard upon successful server bind. Default off for silent CI and containers. |
| `banner_text` | `std::string` | `""` | Optional custom ASCII banner text printed when `show_banner` is enabled. |
| `banner_printer` | `BannerPrinter` | `nullptr` | Optional custom callback `std::function<void(const HttpServer &)>` to render the startup dashboard. |
| `error_formatter` | `ErrorFormatter` | `nullptr` | Centralized formatter function for direct error replies and unhandled server fallback responses. |
| `error_policy` | `ErrorPolicy` | `{}` | Centralized error recovery policy governing 500 message blurring and RPC-over-HTTP 200 status modes. |

---

## TLS / HTTPS Configuration

Kale supports native TLS encryption via OpenSSL. The server family overload allows binding with certificate and private key PEM files:

```cpp
#include <sys/socket.h>
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"

int start_with_tls(kale::HttpServer &server) {
    return server.start(AF_INET, "127.0.0.1", 8443, "server.crt", "server.key");
}
```

### Generating Local Self-Signed Certificates
To test TLS locally, generate a self-signed certificate and RSA private key using OpenSSL:

```bash
openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
  -keyout server.key \
  -out server.crt \
  -subj "/CN=127.0.0.1"
```

### Running Examples with TLS
All 16 example services use the common lifecycle helper `examples::serve()`. You can enable TLS on any example by setting the environment variables `KALE_EXAMPLE_CERT` and `KALE_EXAMPLE_KEY`:

```bash
# Build the example
xmake build example_hello

# Run with TLS enabled
KALE_EXAMPLE_CERT=server.crt KALE_EXAMPLE_KEY=server.key xmake run example_hello 8443
```

Console output:
```
Listening on https://127.0.0.1:8443/
```

Verify with `curl`:
```bash
curl --cacert server.crt https://127.0.0.1:8443/
```

> **Note**: Both `KALE_EXAMPLE_CERT` and `KALE_EXAMPLE_KEY` must be set simultaneously. If only one is provided, `examples::serve` will reject startup with an error.

---

## Signal Handling and Shutdown Semantics

Kale applications operate across multiple asynchronous worker threads managed by Workflow and Coke. Proper signal handling requires coordinating signal delivery so that shutdown signals are processed reliably.

### Signal Masking Pattern
In `examples/example_server.h`, the helper implements the following sequence:

1. **Pre-Thread Signal Masking**:
```cpp
#include "example_server.h"

void setup_server_options(int argc, char *argv[]) {
    examples::ServerOptions options(argc, argv);
    (void)options;
}
```
   Before constructing `HttpServer` or launching worker threads, `ServerOptions` calls:
```cpp
#include <csignal>
#include <pthread.h>

void block_server_signals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}
```
   This ensures that all worker threads created subsequently inherit a signal mask where `SIGINT` and `SIGTERM` are blocked.

2. **Synchronous Signal Awaiting**:
   Inside `examples::serve(server, options)`, the main thread waits synchronously for signals:
```cpp
#include <csignal>

int wait_for_shutdown_signal() {
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGINT);
    sigaddset(&wait_set, SIGTERM);
    int sig = 0;
    sigwait(&wait_set, &sig);
    return sig;
}
```

3. **Server Stop**:
   When a signal is caught, the main thread invokes:
```cpp
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"

void stop_server(kale::HttpServer &server) {
    server.stop();
}
```
   - Stops the listening socket from accepting new TCP connections.
   - Closes idle keep-alive connections immediately.
   - Permits in-flight requests to complete their current processing.

4. **Why `wait_finish()` is Omitted After `stop()`**:
   The example server helper purposely executes `server.stop()` and exits `main()` rather than calling `wait_finish()`. Calling `wait_finish()` blocks until all background threads and internal queues terminate; if background operations or long-polling streams remain open, `wait_finish()` can delay process termination. `stop()` ensures clean socket closure and resource reclamation on process exit.

---

## Buffering vs. Streaming Boundaries

Understanding data boundaries is critical when designing network services. Kale distinguishes between in-memory buffering and streaming workflows:

### 1. In-Memory Request Buffering
By default, standard handlers receive the complete HTTP request body buffered in memory:
```cpp
#include <string_view>
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"

void register_data_handler(kale::HttpServer &server) {
    server.POST("/api/data", [](kale::Context &ctx) {
        std::string_view payload = ctx.req_body();
        (void)payload;
        ctx.string("Received\n");
    });
}
```
- **Boundary**: The entire request is read into memory before the handler executes.
- **Protection**: Consider setting `params.set_request_size_limit(...)` to prevent excessive memory consumption from large payloads.

### 2. Static File Delivery & Caching
Static files served through `kale::FileCache` provide in-memory caching with automatic cache invalidation and RFC 7233 byte-range support:
- Small to medium static assets are served directly from cache without redundant disk reads.
- Range requests (`Range: bytes=0-1023`) allow clients to resume downloads or stream media without loading entire files into user-space buffers.
- For complete file serving details, see [Static File Serving](files.md).

### 3. Multipart Uploads & Chunked Ingress
- For file uploads, multipart parsing inspects form parts and boundaries.
- To handle large files without consuming heap memory, use asynchronous file persistence to stream data directly to disk. See [Multipart File Uploads](uploads.md) and [Resumable Uploads](resumable-upload.md).

### 4. Response Streaming (Chunked & SSE)
For dynamic or long-lived output:
- **Server-Sent Events (SSE)**: Use `kale::SseEmitter` alongside dedicated subscriber queues (`coke::Queue`) to push realtime notifications without lock contention. See [Server-Sent Events & Streaming](streaming.md).

### 5. Async Ownership and Coroutine Lifetimes
`kale::Context` is an ephemeral object bound strictly to the HTTP transaction lifetime:
```cpp
#include <chrono>
#include <string>
#include "coke/sleep.h"
#include "coke/task.h"
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"

coke::Task<void> process_async(std::string path_data) {
    co_await coke::sleep(std::chrono::milliseconds(50));
    (void)path_data;
    co_return;
}

void register_safe_coroutine(kale::HttpServer &server) {
    server.GET("/safe", [](kale::Context &ctx) -> coke::Task<void> {
        // Copy borrowed request path into owned value before async suspension
        std::string owned_path(ctx.path());
        co_await process_async(std::move(owned_path));
        ctx.string("Accepted\n");
        co_return;
    });
}
```
Always copy path parameters, headers, or body slices if they must survive beyond the handler's coroutine frame.

---

## Internal Diagnostic Logger (`kale::Logger`)

Kale includes an instance-owned, lock-free internal diagnostic logger for framework lifecycle events (server listen address, shutdown) and unhandled request exceptions (500 errors).

```cpp
#include <iostream>
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/core/logger.h"

void configure_logging() {
    kale::HttpServerParams params;

    // 1. Set minimum log level (Trace, Debug, Info, Warn, Error, Off)
    params.set_logger(kale::Logger(kale::LogLevel::Info));

    // 2. Or install a custom sink (e.g. forward to spdlog or custom file appender)
    kale::Logger custom_logger(kale::LogLevel::Debug, [](kale::LogLevel lvl,
                                                         const kale::LogContext &ctx,
                                                         std::string_view msg) {
        std::cout << "[" << kale::level_to_string(lvl) << "] "
                  << ctx.component << " " << ctx.method << " " << ctx.path
                  << " (status " << ctx.status_code << "): " << msg << "\n";
    });
    params.set_logger(std::move(custom_logger));

    // 3. For automated unit test suites, silence completely:
    params.set_logger(kale::Logger::off());

    kale::HttpServer server(params);

    // 4. Alternatively, configure directly on an existing HttpServer instance:
    server.set_logger(kale::Logger(kale::LogLevel::Info));
    // Or silence:
    server.set_logger(kale::Logger::off());
}
```

Calling `server.set_logger(...)` or `params.set_logger(...)` synchronizes both `params.logger` and `params.error_policy.logger`, ensuring that server lifecycle logging and centralized error reporting share the identical configured logger.

### Concurrency and Reliability Guarantees
- **No Global Locks**: The default console logger uses a stack-allocated buffer (1024 bytes) and issues a single best-effort POSIX `::write(STDERR_FILENO, ...)`, avoiding interleaving across threads without requiring global mutexes.
- **Exception Isolation**: Custom log sinks are wrapped in `try ... catch` blocks inside `Logger::log()`; any exception raised by third-party logging sinks is caught to prevent breaking the request coroutine pipeline.
- **Set Before Serving**: Mutating the logger level or sink must be performed before starting the server.

---

## Startup Banner & Configuration Dashboard

To prevent log pollution in automated test suites and containerized environments, startup banners are **disabled by default** (`show_banner = false`).

When explicitly enabled, `HttpServer` renders its startup banner and configuration summary:

```cpp
#include <iostream>
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"

void configure_banner(kale::HttpServer &server) {
    // Enable built-in dashboard
    server.set_show_banner(true);

    // Or provide custom ASCII art / text:
    server.set_banner_text(
        "  _  __     _      \n"
        " | |/ /__ _| | ___ \n"
        " | ' </ _` | |/ -_)\n"
        " |_|\\_\\__,_|_|\\___|\n"
    );

    // Dynamic port resolution: When binding to port 0 (ephemeral),
    // the banner displays the actual operating-system assigned port after bind.
}
```

---

## Centralized Server Error Policy

`HttpServerParams` allows setting a centralized `ErrorFormatter` and `ErrorPolicy` that govern how direct errors (`reply_error`) and unhandled server fallback errors are formatted:

```cpp
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/error_response.h"
// #include "kale/core/http_server.h"

void configure_error_handling(kale::HttpServerParams &params) {
    // 1. Choose standard formatter: plain_text, standard_json, problem_details (RFC 9457)
    params.set_error_formatter(kale::default_problem_details_formatter());

    // 2. Configure production redaction policy:
    kale::ErrorPolicy policy;
    policy.expose_internal_errors = false; // Blurs 500 error messages to clients
    policy.default_blurred_message = "Internal Server Error";
    policy.logger = params.logger;         // Uncaught exception details sent to Logger only

    // 3. Optional RPC mode: always return HTTP 200 on wire
    // policy.always_http_200 = true;

    params.set_error_policy(policy);
}
```

### Default Unconfigured Wire Output vs. Explicit Formatters

- **Default (Unconfigured Fallback)**: When no error formatter is configured, `HttpServer` preserves legacy-safe wire behavior by outputting the HTTP standard status reason phrase only (e.g. `"Not Found"`, `"Method Not Allowed"`, `"Internal Server Error"`). This guarantees backwards compatibility with clients expecting plain HTTP reason strings.
- **Explicit Plain Text (`plain_text_error_formatter`)**: When explicitly chosen, formats structured plain text with status, error code, and detailed message (e.g. `400 [BAD_REQUEST] Invalid query parameter\n`).
- **Explicit Standard JSON (`standard_json_error_formatter`)**: Formats a structured JSON envelope containing `"code"`, `"error_code"`, and `"message"`.
- **Explicit Problem Details (`problem_details_error_formatter`)**: Formats an RFC 9457 Problem Details JSON payload containing `"type"`, `"title"`, `"status"`, `"detail"`, and any field-level `"invalid_params"`.
