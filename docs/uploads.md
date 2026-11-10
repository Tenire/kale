# Multipart File Uploads & Asynchronous Persistence

This guide explains how to accept and process `multipart/form-data` uploads in Kale, sanitize filenames, persist files asynchronously without blocking worker threads, and return structured metadata.

The complete runnable code for this guide is located at [`examples/uploads.cpp`](../examples/uploads.cpp).

---

## Building and Running

Compile and launch the uploads example with native `xmake`:

```bash
# Build the binary target
xmake build example_uploads

# Run on default port 8080 from repository root
xmake run example_uploads

# Or run with an OS-assigned ephemeral port
xmake run example_uploads 0
```

### Destination Storage Directory

By default, files are saved to `build/example-uploads` relative to the current working directory. You can customize the storage directory via `KALE_EXAMPLE_UPLOAD_DIR`:

```bash
KALE_EXAMPLE_UPLOAD_DIR=/tmp/kale-storage xmake run example_uploads 8080
```

When started, the service prints:
```
Listening on http://127.0.0.1:8080/
```

Press `Ctrl+C` or send `SIGTERM` to gracefully stop the service.

---

## Request Lifecycle & Buffering Semantics

Kale enforces bounded buffered request handling rather than single-request ingress streaming:

1. **Bounded Buffered Ingress**: In Kale's HTTP server, incoming request bodies (including `multipart/form-data`) are bounded and buffered in memory by the HTTP protocol parser before handler coroutines run. The maximum payload per request is strictly governed by `HttpServerParams::request_size_limit` (e.g. 16 MB).
2. **Asynchronous Non-Blocking Persistence**: When the handler coroutine calls `kale::save_form_file` or `kale::save_upload`, Kale writes the buffered payload to disk using non-blocking asynchronous file I/O (`coke::pwrite`). The worker thread never blocks on synchronous disk I/O.
3. **Partitioned Ingress Across Requests**: For files larger than a single request's buffer budget, payloads are partitioned by the client into finite bounded parts and uploaded across multiple distinct requests to direct disk offsets via [Resumable Uploads](resumable-upload.md).

---

## Architecture & Minimal Libraries

| Component | Header | Library | Description |
|---|---|---|---|
| `parse_multipart` / `FormFile` | `#include "kale/http/multipart.h"` | `kale_http` | Parses multipart MIME boundaries into `FormFile` views |
| `save_form_file` / `save_upload` | `#include "kale/http/upload.h"` | `kale_http` | Asynchronously persists files to disk via `coke::pwrite` |
| `send_file_range` / `send_file` | `#include "kale/http/file_response.h"` | `kale_http` | Asynchronously streams uploaded files with RFC 7233 Range |
| `Context` | `#include "kale/core/context.h"` | `kale_core` | Core request/response context |
| `Json` | `#include "kale/http/json.h"` | `kale_http` | JSON parsing and serialization |

---

## Multipart Parsing & Storage Workflow

### 1. Extracting Uploaded Files

Use `kale::parse_multipart(ctx)` to parse all multipart parts from the request body:

```cpp
#include <string>
#include <vector>

#include "coke/task.h"

// Convenience headers (active):
#include <kale/core.h>
#include <kale/http.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"
// #include "kale/http/multipart.h"

void inspect_multipart_files(kale::HttpServer &server) {
    server.POST("/api/upload/inspect", [](kale::Context &ctx) -> coke::Task<void> {
        std::vector<kale::FormFile> files = kale::parse_multipart(ctx);
        if (files.empty()) {
            ctx.set_status(kale::HttpStatus::BadRequest).string("No multipart files received\n");
            co_return;
        }

        for (const auto &file : files) {
            // file.name: form field name (e.g. "file" or "avatar")
            // file.filename: client-provided filename (e.g. "photo.jpg")
            // file.content_type: MIME type (e.g. "image/jpeg")
            // file.data: non-owning string_view referencing the parsed body
            (void)file;
        }
        ctx.string("Parsed files successfully\n");
    });
}
```

### 2. Sanitizing Filenames & Defending Traversal

Never trust `file.filename` directly. Clients can send arbitrary strings including `../../etc/passwd` or null bytes. Always strip directory components before constructing disk paths:

```cpp
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

std::filesystem::path resolve_safe_upload_path(const std::string &upload_dir, std::string_view raw_name) {
    size_t last_sep = raw_name.find_last_of("/\\");
    std::string_view base_name = (last_sep == std::string_view::npos) ? raw_name : raw_name.substr(last_sep + 1);

    std::string safe_name;
    if (base_name.empty() || base_name == "." || base_name == "..") {
        safe_name = "upload_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".bin";
    } else {
        safe_name = std::string(base_name);
    }

    return std::filesystem::path(upload_dir) / safe_name;
}
```

### 3. Asynchronous Persistence

Call `kale::save_form_file` to asynchronously write the file to the target filesystem location:

```cpp
#include <string>

#include "coke/task.h"

// Convenience headers (active):
#include <kale/core.h>
#include <kale/http.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/http/multipart.h"
// #include "kale/http/upload.h"

coke::Task<void> handle_file_persistence(kale::Context &ctx, const kale::FormFile &file, const std::string &dest_path) {
    int ret = co_await kale::save_form_file(file, dest_path);
    if (ret != 0) {
        ctx.set_status(kale::HttpStatus::InternalServerError).string("Failed to save file\n");
        co_return;
    }
    ctx.string("File saved successfully\n");
}
```

The `save_form_file` helper automatically opens the destination file, iterates through the buffer in non-blocking slices via `coke::pwrite`, and closes the file descriptor upon completion.

---

## Practical `curl` Interactions

### 1. Uploading a File via Multipart Form

```bash
curl -i -F "file=@examples/assets/hello.txt" http://127.0.0.1:8080/api/upload
```

Response:
```http
HTTP/1.1 201 Created
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "status": "success",
  "files": [
    {
      "field": "file",
      "filename": "hello.txt",
      "content_type": "text/plain",
      "size_bytes": 357,
      "saved_path": "build/example-uploads/hello.txt"
    }
  ]
}
```

### 2. Multi-File Upload in a Single Request

```bash
curl -i -F "doc=@examples/assets/hello.txt" \
        -F "webpage=@examples/assets/index.html" \
        http://127.0.0.1:8080/api/upload
```

Response:
```http
HTTP/1.1 201 Created
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "status": "success",
  "files": [
    {
      "field": "doc",
      "filename": "hello.txt",
      "content_type": "text/plain",
      "size_bytes": 357,
      "saved_path": "build/example-uploads/hello.txt"
    },
    {
      "field": "webpage",
      "filename": "index.html",
      "content_type": "text/html",
      "size_bytes": 1588,
      "saved_path": "build/example-uploads/index.html"
    }
  ]
}
```

### 3. Listing Uploaded Files

```bash
curl -i http://127.0.0.1:8080/api/uploads
```

Response:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "directory": "build/example-uploads",
  "files": [
    { "filename": "hello.txt", "size_bytes": 357 },
    { "filename": "index.html", "size_bytes": 1588 }
  ]
}
```

### 4. Downloading Uploaded File with Byte Range

```bash
curl -i -H "Range: bytes=0-20" http://127.0.0.1:8080/api/uploads/hello.txt
```

Response:
```http
HTTP/1.1 206 Partial Content
Accept-Ranges: bytes
Content-Length: 21
Content-Range: bytes 0-20/357
Content-Type: text/plain; charset=utf-8
Server: Kale

Hello from Kale C++20
```

### 5. Deleting an Uploaded File

```bash
curl -i -X DELETE http://127.0.0.1:8080/api/uploads/hello.txt
```

Response:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "status": "deleted",
  "filename": "hello.txt"
}
```

---

## Security & Lifetime Rules

1. **Buffer Validity**: `FormFile::data` is a `std::string_view` referencing the request body in memory. It remains valid for the duration of the coroutine handler. Do not store `FormFile::data` across coroutine boundaries without copying into a `std::string`.
2. **Payload Size Limits**: Always configure `HttpServerParams::set_request_size_limit(bytes)` to prevent clients from overflowing process memory with oversized bodies.
3. **Path Traversal Defense**: Always sanitize `filename` before creating filesystem paths. Rejection of `/` and `\` prevents subfolder escapes.
