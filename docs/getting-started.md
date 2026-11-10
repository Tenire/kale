# Getting Started with Kale

This guide walks you through building your first HTTP service with Kale, understanding its lifecycle and coroutine model, and running the `hello` example.

---

## Prerequisites

Kale requires a modern C++20 toolchain and Linux environment:
- **C++ Compiler**: GCC 11+ or Clang 13+ with full C++20 coroutine support (`-std=c++20`).
- **Build System**: [xmake](https://xmake.io) (v2.5.0 or later).
- **Libraries**:
  - OpenSSL (system library)
  - zlib & zstd (compression support)
  - Sogou Workflow & Coke coroutine library (managed automatically by xmake via repository dependencies)

All external dependencies are resolved automatically by `xmake` using the configuration declared in `xmake.lua`.

---

## Project Structure & Dependencies

Kale is organized into 4 modular static libraries (`kale_core`, `kale_http`, `kale_security`, `kale_client`) and an umbrella target (`kale`):

```
kale-core/
├── include/kale/         # Public headers partitioned by module (core/, middleware/, http/, security/, client/)
├── src/                  # Library implementation
├── examples/             # Practical runnable example programs
│   ├── example_server.h  # Common server lifecycle helper
│   ├── hello.cpp         # Getting started service
│   └── xmake.lua         # Example build targets
└── docs/                 # Task-oriented documentation guides
```

When writing your own application, link against the specific minimal library (e.g. `kale_core` for basic routing, or `kale` for all modules) and include the appropriate `<kale/...>` headers.

---

## The Hello World Service

Below is the complete implementation of `examples/hello.cpp`. It demonstrates both a synchronous handler and an asynchronous, coroutine-native handler:

```cpp
#include "example_server.h"

#include <chrono>
#include <iostream>
#include <string>

#include "coke/sleep.h"
#include "coke/task.h"
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"
// #include "kale/core/http_status.h"

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);
        kale::HttpServer server;

        // Minimal synchronous hello route: void(Context &ctx)
        server.GET("/", [](kale::Context &ctx) {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Hello, Kale!\n");
        });

        // Coroutine-native route: coke::Task<void>(Context &ctx)
        // Demonstrates non-blocking coroutine suspension without thread blocking
        server.GET("/async", [](kale::Context &ctx) -> coke::Task<void> {
            co_await coke::sleep(std::chrono::milliseconds(10));

            std::string name = ctx.default_query("name", "World");
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Hello, " + name + " (from coroutine)!\n");
            co_return;
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
```

---

## Building and Running

### 1. Configure and Build
Ensure you are at the repository root:

```bash
# Configure the build in release mode
xmake f -m release

# Build the hello example target
xmake build example_hello
```

To build all 16 example targets in one command:
```bash
xmake build examples
```

### 2. Run the Service
Run `example_hello` using `xmake run`. By default, the server binds to port `8080` on loopback (`127.0.0.1`):

```bash
xmake run example_hello
```

Expected output:
```
Listening on http://127.0.0.1:8080/
```

You can pass a custom port as a positional command-line argument:
```bash
xmake run example_hello 9000
```

### Ephemeral Port Binding
To let the operating system automatically allocate an unused ephemeral port, pass `0`:
```bash
xmake run example_hello 0
# Example output: Listening on http://127.0.0.1:43917/
```
The helper inspects the bound socket and immediately prints the assigned URL to stdout.

### 3. Test with curl
Open a separate terminal to send requests:

```bash
# Synchronous endpoint
curl -i http://127.0.0.1:8080/
```

Expected response:
```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Content-Length: 13
Connection: Keep-Alive

Hello, Kale!

```bash
# Asynchronous coroutine endpoint
curl -i "http://127.0.0.1:8080/async?name=User"
```

Expected response:
```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Connection: Keep-Alive

Hello, User (from coroutine)!

### 4. Stopping the Service
Press `Ctrl+C` in the terminal running the server, or send `SIGINT`/`SIGTERM` from another process:
```bash
kill -TERM $(pgrep example_hello)
```
The server catches the signal, ceases accepting new connections, drains existing work, and terminates with exit code 0.

---

## Key Architectural Concepts

### 1. Signal Blocking and ServerOptions
In multi-threaded server environments, asynchronous signal handling across worker threads can cause race conditions or missed signals.
Kale examples enforce a strict lifecycle contract via `examples::ServerOptions`:

```cpp
#include "example_server.h"

void init_options(int argc, char *argv[]) {
    examples::ServerOptions options(argc, argv);
    (void)options;
}
```

This **must be the very first statement** in `main()`. Its constructor calls `pthread_sigmask(SIG_BLOCK, ...)` to mask `SIGINT` and `SIGTERM` on the main thread before any background worker threads or event loops are spawned. Subsequent threads inherit this blocked signal mask.

When `examples::serve(server, options)` executes, the main thread calls `sigwait()` to synchronously wait for termination signals. Once a signal is caught, it invokes `server.stop()`, cleanly stopping the server without deadlocks.

### 2. Synchronous vs Coroutine Handlers
Kale supports two handler signatures:
- **Synchronous**: `void(kale::Context &)`
  Suitable for in-memory operations, route dispatch, simple JSON construction, or immediate text replies. The response is committed as soon as the function returns.
- **Coroutine-native**: `coke::Task<void>(kale::Context &)`
  Required whenever you perform asynchronous I/O, such as outbound HTTP requests (`coke::HttpClient`), database queries, asynchronous timers (`coke::sleep`), or queue operations (`coke::Queue`). Execution suspends with `co_await` without blocking the underlying thread pool.

### 3. Request-Local Context Lifetimes
Every incoming HTTP request receives a `kale::Context` instance providing direct methods for inspecting the request and assembling the response.

```cpp
#include <string>
#include <string_view>
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_status.h"

void handle_request(kale::Context &ctx) {
    // Reading request properties
    std::string_view path = ctx.path();
    std::string_view method = ctx.method_string();
    std::string user_agent = ctx.header("User-Agent");
    std::string query_val = ctx.query("token");
    (void)path;
    (void)method;
    (void)user_agent;
    (void)query_val;

    // Writing response properties
    ctx.set_status(kale::HttpStatus::OK);
    ctx.set_header("X-Custom-Header", "KaleService");
    ctx.string("Response text\n");
}
```

**Lifetime Boundary Rule**: The `Context` and the memory holding request headers and body are owned by the request transaction and are deallocated after the handler coroutine finishes.
- **Do not** store references or pointers to `ctx` or header string views in objects that outlive the handler.
- If data must persist beyond the request (e.g. across detached tasks or background queues), copy it into an owned type such as `std::string`.

---

## Next Steps

Explore the task guides to build more complete services:
- **[Routing & Handlers](routing.md)**: Dynamic path parameters (`/users/:id`), wildcards, and modular Blueprints.
- **[Middleware](middleware.md)**: Global and route-level middlewares, access logging, and execution ordering.
- **[JSON Processing](json.md)**: Handling JSON payloads with `kale::Json`.
- **[Server Configuration](server-configuration.md)**: Tuning timeouts, connection limits, and configuring TLS.
