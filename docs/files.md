# Static File Serving & Byte-Range Downloads

This guide explains how to serve static assets, configure in-memory LRU caching, and implement asynchronous file streaming with RFC 7233 Range support in Kale.

The complete runnable code for this guide is located at [`examples/file_service.cpp`](../examples/file_service.cpp).

---

## Building and Running

Compile and launch the file service example using native `xmake`:

```bash
# Build the binary target
xmake build example_file_service

# Run on default port 8080 from repository root
xmake run example_file_service

# Or run on an OS-assigned ephemeral port
xmake run example_file_service 0
```

### Environment Variables & Working Directory

By default, the service resolves assets from `examples/assets` relative to the current working directory. You can point the service to an alternative directory by setting `KALE_EXAMPLE_FILES`:

```bash
KALE_EXAMPLE_FILES=/var/www/static xmake run example_file_service 8080
```

When started, the service prints its listening URL:
```
Listening on http://127.0.0.1:8080/
```

Press `Ctrl+C` or send `SIGTERM` to gracefully stop the service.

---

## Architecture & Minimal Libraries

File serving in Kale is split across two core layers:

| Component | Header | Library | Description |
|---|---|---|---|
| `static_files` | `#include "kale/http/static_files.h"` | `kale_http` | Router handler for filesystem trees with ETag and caching |
| `FileCache` | `#include "kale/http/file_cache.h"` | `kale_http` | Coroutine-safe LRU cache avoiding cache stampedes |
| `send_file` / `send_file_range` | `#include "kale/http/file_response.h"` | `kale_http` | Direct coroutine file streaming using non-blocking `coke::pread` |
| `Context` | `#include "kale/core/context.h"` | `kale_core` | Core request/response context |

---

## Static File Serving (`static_files`)

The `static_files` helper creates a `kale::Handler` that serves files under a specified root directory. It automatically matches against a Radix Tree wildcard parameter (`*filepath`).

```cpp
#include <string>

// Convenience headers (active):
#include <kale/core.h>
#include <kale/http.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/http/static_files.h"

void configure_static_routes(kale::HttpServer &server, const std::string &files_dir) {
    server.GET("/static/*filepath", kale::static_files(files_dir));
    server.HEAD("/static/*filepath", kale::static_files(files_dir));
}
```

### Features Built into `static_files`

1. **Automatic MIME Types**: Infers `Content-Type` based on file extension (e.g., `.html` -> `text/html; charset=utf-8`, `.txt` -> `text/plain; charset=utf-8`, `.css` -> `text/css; charset=utf-8`).
2. **HTTP Caching Headers**: Generates strong `ETag` and `Last-Modified` timestamps.
3. **Conditional Requests**: Evaluates `If-None-Match` and `If-Modified-Since` headers and returns `304 Not Modified` without transmitting file bodies.
4. **Path Traversal Protection**: Inspects each path segment, rejects `..` traversal with `403 Forbidden`, and prevents escaping the configured root directory.
5. **Symlink Boundary Enforcement**: Rejects symlinks by default (`allow_symlinks = false`) to avoid leaking files outside the intended web root.

---

## In-Memory LRU File Caching

For high-throughput endpoints serving frequent small assets, configure an in-memory LRU cache using `StaticFilesConfig`:

```cpp
#include <string>

// Convenience headers (active):
#include <kale/core.h>
#include <kale/http.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/http/file_cache.h"
// #include "kale/http/static_files.h"

void configure_cached_routes(kale::HttpServer &server, const std::string &files_dir) {
    kale::StaticFilesConfig cache_cfg;
    cache_cfg.root_dir = files_dir;
    cache_cfg.url_prefix = "/cached";
    cache_cfg.enable_cache = true;
    cache_cfg.cache_config.capacity = 512;
    cache_cfg.cache_config.max_file_size = 2 * 1024 * 1024;
    cache_cfg.cache_config.check_mtime = true;

    server.GET("/cached/*filepath", kale::static_files(cache_cfg));
    server.HEAD("/cached/*filepath", kale::static_files(cache_cfg));
}
```

Cached files are loaded asynchronously using `coke::LruCache`'s `get_or_create` coroutine mechanism, preventing the "thundering herd" problem when many concurrent requests access an uncached file simultaneously.

---

## Direct Asynchronous File & Range Streaming

When you need custom routing logic, access control, or custom headers (such as `Content-Disposition: attachment`), use `kale::send_file` and `kale::send_file_range`:

```cpp
#include <filesystem>
#include <string>
#include <string_view>

#include "coke/task.h"

// Convenience headers (active):
#include <kale/core.h>
#include <kale/stream.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"
// #include "kale/http/file_response.h"

void register_download_route(kale::HttpServer &server, const std::string &files_dir) {
    server.GET("/download/:filename", [files_dir](kale::Context &ctx) -> coke::Task<void> {
        std::string_view filename = ctx.param("filename");

        if (filename.empty() || filename.find('/') != std::string_view::npos ||
            filename.find('\\') != std::string_view::npos || filename == ".." || filename == ".") {
            ctx.set_status(kale::HttpStatus::Forbidden).string("403 Forbidden: Invalid filename\n");
            co_return;
        }

        std::filesystem::path target = std::filesystem::path(files_dir) / filename;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(target, ec)) {
            ctx.set_status(kale::HttpStatus::NotFound).string("404 Not Found: File not found\n");
            co_return;
        }

        ctx.set_header("Content-Disposition", "attachment; filename=\"" + std::string(filename) + "\"");

        if (ctx.has_header("Range")) {
            co_await kale::send_file_range(ctx, target.string(), ctx.header("Range"));
        } else {
            co_await kale::send_file(ctx, target.string());
        }
    });
}
```

### Protocol Details & Status Codes

- **Full File Request (`send_file`)**: Returns `200 OK` with `Content-Type`, `Content-Length`, and streamed body blocks.
- **Valid Byte Range (`send_file_range`)**: When `Range: bytes=0-20` is sent, returns `206 Partial Content`, `Content-Range: bytes 0-20/357`, and transmits exactly 21 bytes.
- **Unsatisfiable Range**: If the requested offset exceeds the file length (e.g. `bytes=5000-6000`), returns `416 Range Not Satisfiable` with `Content-Range: bytes */357`.

---

## Practical `curl` Interactions

### 1. Fetching a Static File

```bash
curl -i http://127.0.0.1:8080/static/hello.txt
```

Response:
```http
HTTP/1.1 200 OK
Accept-Ranges: bytes
Content-Length: 357
Content-Type: text/plain; charset=utf-8
ETag: "165-..."
Last-Modified: Sat, 08 Nov 2026 15:01:00 GMT
Server: Kale

Hello from Kale C++20 Framework!
This is a sample text asset served by Kale's asynchronous file service.
...
```

### 2. Validating 304 Not Modified Caching

```bash
curl -i -H 'If-None-Match: "165-..."' http://127.0.0.1:8080/static/hello.txt
```

Response:
```http
HTTP/1.1 304 Not Modified
ETag: "165-..."
Server: Kale
```

### 3. Requesting a Byte Range (RFC 7233)

```bash
curl -i -H "Range: bytes=0-25" http://127.0.0.1:8080/download/hello.txt
```

Response:
```http
HTTP/1.1 206 Partial Content
Accept-Ranges: bytes
Content-Disposition: attachment; filename="hello.txt"
Content-Length: 26
Content-Range: bytes 0-25/357
Content-Type: text/plain; charset=utf-8
Server: Kale

Hello from Kale C++20 Fram
```

### 4. Unsatisfiable Range Handling

```bash
curl -i -H "Range: bytes=9000-9500" http://127.0.0.1:8080/download/hello.txt
```

Response:
```http
HTTP/1.1 416 Range Not Satisfiable
Content-Range: bytes */357
Server: Kale
```

---

## Security & Concurrency Considerations

1. **Non-Blocking File I/O**: File reads are executed via `coke::pread` on worker threads, preventing physical worker threads from stalling while reading slow storage.
2. **Finite Request Bounds**: Global body size limits configured on `HttpServerParams` protect against memory exhaustion attacks.
3. **No Root Escapes**: Both `static_files` and custom download endpoints strictly prohibit traversal (`..`) and absolute path injection.
