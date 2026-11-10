# Routing & Handlers

This guide explores Kale's high-performance Radix Tree routing engine, hierarchical Blueprint composition, path parameters, query string extraction, route metadata, dynamic route enumeration, and RFC-compliant 404/405 error lifecycle handling.

The complete runnable code for this guide is available at [`examples/routing.cpp`](../examples/routing.cpp).

---

## Building and Running

Compile and launch the routing example from the repository root using native `xmake`:

```bash
# Build the routing binary target
xmake build example_routing

# Run on default port 8080 (127.0.0.1)
xmake run example_routing

# Or run with an ephemeral port assigned by the operating system
xmake run example_routing 0
```

When started, the service prints its listening address:
```
Listening on http://127.0.0.1:8080/
```

To gracefully stop the server, press `Ctrl+C` or send `SIGTERM`.

---

## Route Path Syntax

Kale's routing engine uses a multi-branch Radix Tree supporting three segment types:

| Syntax | Segment Type | Example Route | Matched URL | Captured Parameter |
|---|---|---|---|---|
| `/literal` | Static segment | `/api/catalog/items` | `/api/catalog/items` | None |
| `:param` | Colon named parameter | `/items/:id` | `/items/101` | `id="101"` |
| `{param}` | Brace named parameter | `/posts/{year}/{slug}` | `/posts/2026/intro` | `year="2026"`, `slug="intro"` |
| `*wildcard` | Multi-segment wildcard | `/files/*filepath` | `/files/assets/img.png` | `filepath="assets/img.png"` |

### Matching Rules & Precedence

1. **Static Precedence**: Exact static segments take precedence over parameter segments at the same tree depth. For example, `/users/me` matches a dedicated route before falling back to `/users/:id`.
2. **Backtracking**: If a deeper static segment fails to match, the router backtracks to candidate parameter or wildcard branches.
3. **Multi-Segment Wildcard**: Wildcards (`*filepath`) must appear as the final segment pattern. They match all remaining path segments including slashes `/`.

---

## Route Handlers: Synchronous & Asynchronous

Handlers in Kale operate on `kale::Context &ctx` and can be either synchronous functions or C++20 coroutines returning `coke::Task<void>`:

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

void register_sample_routes(kale::HttpServer &server) {
    // 1. Synchronous Handler (immediate return, zero coroutine overhead)
    server.GET("/ping", [](kale::Context &ctx) {
        ctx.set_status(kale::HttpStatus::OK);
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string("pong\n");
    });

    // 2. Coroutine-Native Handler (non-blocking async operations via co_await)
    server.GET("/async-data", [](kale::Context &ctx) -> coke::Task<void> {
        co_await coke::sleep(std::chrono::milliseconds(20));
        ctx.set_status(kale::HttpStatus::OK);
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string("ready\n");
        co_return;
    });
}
```

---

## Extracting Parameters and Queries

### Path Parameters and Query Strings

Path parameters are retrieved using `ctx.param(key)`, and query strings are retrieved with `ctx.query(key)` or `ctx.default_query(key, fallback)`:

```cpp
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_status.h"

inline std::optional<int> parse_int_param(std::string_view sv) {
    if (sv.empty()) return std::nullopt;
    int val = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec == std::errc{} && ptr == sv.data() + sv.size()) return val;
    return std::nullopt;
}

void handle_catalog_item(kale::Context &ctx) {
    // Extract colon path parameter (:id)
    auto id = parse_int_param(ctx.param("id"));
    if (!id || *id <= 0) {
        ctx.set_status(kale::HttpStatus::BadRequest);
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string("Error: Item ID must be a positive integer\n");
        return;
    }

    // Extract query parameters (?category=hardware&page=1&limit=10)
    std::string category = ctx.default_query("category", "all");
    std::string page = ctx.default_query("page", "1");
    std::string limit = ctx.default_query("limit", "10");

    std::string out = "Item: " + std::to_string(*id) + "\nCategory: " + category +
                      "\nPage: " + page + ", Limit: " + limit + "\n";
    ctx.set_content_type("text/plain; charset=utf-8");
    ctx.string(out);
}
```

### Lifetime & Memory Boundaries
- `ctx.param()` returns `std::string_view` pointing to buffers owned by the active request transaction.
- **Rule**: If you need to store or reference parameter data across asynchronous coroutine suspension points or beyond the lifetime of the handler, copy it into an owned `std::string`.

---

## Modular Architecture with Blueprints

Kale provides `kale::Blueprint` to group related routes into isolated, modular domains before mounting them onto the central server.

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/routing.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/blueprint.h"
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"

void configure_application_routes(kale::HttpServer &server) {
    // 1. Define child catalog blueprint
    kale::Blueprint catalog_bp;
    catalog_bp.GET("/items", [](kale::Context &ctx) {
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string("Catalog listing\n");
    }).name("catalog.items.list").tag("module", "catalog");

    // 2. Define child articles blueprint
    kale::Blueprint articles_bp;
    articles_bp.GET("/posts/{year}/{slug}", [](kale::Context &ctx) {
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string("Article lookup\n");
    }).name("articles.posts.get").tag("module", "articles");

    // 3. Compose children into a parent API blueprint
    kale::Blueprint api_bp;
    api_bp.mount("/catalog", catalog_bp);
    api_bp.mount("/articles", articles_bp);

    // 4. Mount the parent blueprint onto the HttpServer
    server.mount("/api", api_bp);
}
```

This creates clean route paths:
- `/api/catalog/items`
- `/api/articles/posts/{year}/{slug}`

---

## Route Metadata and Dynamic Enumeration

Routes can carry metadata for documentation, auditing, and APM tracing:

```cpp
// Convenience headers (active):
#include <kale/routing.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/blueprint.h"
// #include "kale/core/context.h"

void add_tagged_route(kale::Blueprint &bp) {
    bp.GET("/items/:id", [](kale::Context &ctx) {
        ctx.string("Item details\n");
    })
    .name("catalog.items.get")
    .tag("module", "catalog")
    .tag("access", "public");
}
```

Inside any handler or middleware, access the matched route metadata:
```cpp
#include <string_view>
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"

void inspect_route_metadata(const kale::Context &ctx) {
    std::string_view name = ctx.route_name();        // "catalog.items.get"
    std::string_view pattern = ctx.route_pattern();  // "/api/catalog/items/:id"
    std::string_view module = ctx.route_tag("module"); // "catalog"
    (void)name;
    (void)pattern;
    (void)module;
}
```

### Dynamic Route Enumeration & Aligned Formatting

Route introspection is available programmatically via `server.router().routes()`, returning snapshots of `kale::RouteInfo`:

```cpp
#include <cstddef>
#include <string_view>
// Convenience headers (active):
#include <kale/routing.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_method.h"
// #include "kale/core/router.h"

void inspect_route_info(const kale::RouteInfo &info) {
    std::string_view method = kale::method_to_string(info.method);
    std::string_view pattern = info.pattern;
    std::string_view name = info.name;
    size_t middleware_count = info.middleware_count; // Total accumulated middlewares on route pipeline
    size_t body_limit = info.max_body_size;          // Endpoint-specific request body size limit
    (void)method;
    (void)pattern;
    (void)name;
    (void)middleware_count;
    (void)body_limit;
}
```

#### Self-Adaptive Aligned Table Formatting

Rather than formatting columns manually, use `server.format_routes()` (or `server.list_routes()`):

```cpp
#include <iostream>
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"
// #include "kale/core/router.h"

void configure_route_introspection(kale::HttpServer &server) {
    // 1. Print formatted topology table to stdout before serving
    server.list_routes(std::cout);

    // 2. Expose dynamic formatted route table over HTTP
    server.GET("/routes", [&server](kale::Context &ctx) {
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string(server.format_routes());
    }).name("server.routes.enumerate").tag("module", "system");
}
```

The table automatically scans all routes to compute column widths, sorting primarily by path in lexicographical order, and secondarily by standard HTTP method precedence (`GET -> POST -> PUT -> DELETE -> PATCH -> HEAD -> OPTIONS`).

---

## Error Lifecycle: Custom 404 and 405 Handlers

Kale handles route resolution failures with distinct, standards-compliant hooks:

```cpp
#include <string>
// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"
// #include "kale/core/http_status.h"

void register_error_fallbacks(kale::HttpServer &server) {
    // Custom 404 (Not Found) handler: invoked when no path matches
    server.set_not_found_handler([](kale::Context &ctx) {
        ctx.set_status(kale::HttpStatus::NotFound);
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string("404 Not Found: Path '" + std::string(ctx.path()) + "' does not match any route.\n");
    });

    // Custom 405 (Method Not Allowed) handler: router populates the RFC 'Allow' header automatically
    server.set_method_not_allowed_handler([](kale::Context &ctx) {
        ctx.set_status(kale::HttpStatus::MethodNotAllowed);
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string("405 Method Not Allowed: " + std::string(ctx.method_string()) +
                   " is not supported for '" + std::string(ctx.path()) + "'.\nAllowed: " +
                   std::string(ctx.resp_header("Allow")) + "\n");
    });
}
```

---

## Verification & curl Recipes

Run the following commands in another terminal while `example_routing` is running:

### 1. Query Parameters
```bash
curl -s "http://127.0.0.1:8080/api/catalog/items?category=hardware&page=1&limit=5"
```
**Response (200 OK)**:
```
Catalog Listing
Filter Category: hardware
Page: 1, Limit: 5
Items:
  [101] Mechanical Keyboard (hardware)
  [102] USB-C Dock (accessories)
```

### 2. Colon Named Parameter (`:id`)
```bash
curl -s http://127.0.0.1:8080/api/catalog/items/101
```
**Response (200 OK)**:
```
Item ID: 101
Title: Sample Catalog Item
In Stock: yes
```

```bash
# Out of range item ID
curl -s -i http://127.0.0.1:8080/api/catalog/items/9999
```
**Response (404 Not Found)**:
```
HTTP/1.1 404 Not Found
Content-Type: text/plain; charset=utf-8

Item 9999 not found
```

### 3. Stateless Quote Calculation (POST)
```bash
curl -s -X POST http://127.0.0.1:8080/api/catalog/quote -d "5"
```
**Response (200 OK)**:
```
Quote Calculation:
Quantity: 5
Unit Price: $49.99
Total Estimated Cost: $249.95
```

### 4. Stateless String Transformation (PUT)
```bash
curl -s -X PUT http://127.0.0.1:8080/api/catalog/slugify -d "Modern C++ Routing in Kale!"
```
**Response (200 OK)**:
```
Input: Modern C++ Routing in Kale!
Generated Slug: modern-c-routing-in-kale
```

### 5. Brace Parameters (`{year}/{slug}`)
```bash
curl -s http://127.0.0.1:8080/api/articles/posts/2026/modern-cpp-routing
```
**Response (200 OK)**:
```
Article Lookup
Year: 2026
Slug: modern-cpp-routing
Headline: Modern C++ Routing with Kale
```

### 6. Wildcard Parameter (`*filepath`)
```bash
curl -s http://127.0.0.1:8080/api/articles/files/docs/api/v1/spec.pdf
```
**Response (200 OK)**:
```
Requested Archive Path: docs/api/v1/spec.pdf
Status: Available
```

### 7. Plain Text Route Enumeration
```bash
curl -s http://127.0.0.1:8080/routes
```
**Response (200 OK)**:
```
Registered Server Routes:
METHOD   PATTERN                                 NAME                    TAGS
------   -------                                 ----                    ----
GET      /api/articles/files/*filepath           articles.files.get      access=public, module=articles
GET      /api/articles/posts/{year}/{slug}       articles.posts.get      access=public, module=articles
GET      /api/catalog/items                      catalog.items.list      access=public, module=catalog
GET      /api/catalog/items/:id                  catalog.items.get       access=public, module=catalog
POST     /api/catalog/quote                      catalog.quote           access=public, module=catalog
PUT      /api/catalog/slugify                    catalog.slugify         access=public, module=catalog
GET      /routes                                 server.routes.enumerate module=system
```

### 8. Custom 404 Trigger
```bash
curl -s -i http://127.0.0.1:8080/nonexistent/path
```
**Response (404 Not Found)**:
```http
HTTP/1.1 404 Not Found
Content-Type: text/plain; charset=utf-8

404 Not Found: Path '/nonexistent/path' does not match any route.
```

### 9. Custom 405 Trigger with RFC `Allow` Header
```bash
# Send DELETE to a path that only accepts GET
curl -s -i -X DELETE http://127.0.0.1:8080/api/articles/posts/2026/modern-cpp-routing
```
**Response (405 Method Not Allowed)**:
```http
HTTP/1.1 405 Method Not Allowed
Allow: GET
Content-Type: text/plain; charset=utf-8

405 Method Not Allowed: DELETE is not supported for '/api/articles/posts/2026/modern-cpp-routing'.
Allowed: GET
```
Notice that Kale automatically populates `Allow: GET` on the response.
