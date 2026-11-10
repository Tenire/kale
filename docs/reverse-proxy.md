# Reverse Proxy Gateway

This guide covers Kale's coroutine-native reverse proxy engine (`kale::proxy`), request forwarding, header sanitation, streaming responses, and protocol boundaries.

The complete runnable example program is located at [`../examples/reverse_proxy.cpp`](../examples/reverse_proxy.cpp).

---

## Gateway Architecture

`kale::proxy` generates a `kale::Handler` that forwards incoming HTTP requests to an upstream HTTP authority.

```
+--------------------+        +-----------------------+        +--------------------+
|  Downstream Client | -----> |  Kale Reverse Proxy   | -----> |  Upstream Backend  |
|   (curl / browser) |        |      (Port 8081)      |        |    (Port 8080)     |
|                    | <----- |                       | <----- |                    |
+--------------------+        +-----------------------+        +--------------------+
```

Forwarding yields the executing coroutine during upstream network I/O, maintaining zero thread blocking.

---

## Building and Running

Compile the reverse proxy binary target using `xmake`:

```bash
xmake build example_reverse_proxy
```

### Two-Terminal Launch Recipe

Pair the reverse proxy gateway with `example_hello`:

#### Terminal 1: Launch Upstream Service
```bash
xmake build example_hello
xmake run example_hello 8080
```
Expected output:
```
Listening on http://127.0.0.1:8080/
```

#### Terminal 2: Launch Reverse Proxy Gateway
```bash
KALE_EXAMPLE_UPSTREAM=http://127.0.0.1:8080 xmake run example_reverse_proxy 8081
```
Expected output:
```
Listening on http://127.0.0.1:8081/
```

To stop either service, send `SIGINT` (`Ctrl+C`) or `SIGTERM`.

---

## Configuration (`ProxyOptions`)

Kale configures reverse proxy routes via `kale::ProxyOptions`:

```cpp
#include <chrono>

// Convenience headers (active):
#include <kale/core.h>
#include <kale/client.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/client/http_client.h"
// #include "kale/client/reverse_proxy.h"
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"

void configure_gateway(kale::HttpServer &server, kale::HttpClient &client) {
    kale::ProxyOptions opts("http://127.0.0.1:8080");
    opts.strip_prefix = "/proxy";
    opts.preserve_host = false;
    opts.trust_downstream_forwarded = false;
    opts.timeout = std::chrono::seconds(10);
    opts.max_body_bytes = 16 * 1024 * 1024;
    opts.stream_response = false;

    opts.rewrite_request = [](const kale::Context &, kale::HttpClientRequest &req) {
        req.header("X-Forwarded-Gateway", "Kale-ReverseProxy");
    };

    opts.modify_response = [](kale::Context &ctx, const kale::HttpClientResponse &) {
        ctx.set_header("X-Proxy-Gateway", "Kale-Edge");
    };

    server.any("/proxy/*path", kale::proxy(client, opts));
    server.any("/proxy", kale::proxy(client, opts));
}
```

### Source-Confirmed `ProxyOptions` Reference

| Option | Type | Default | Description |
|---|---|---|---|
| `target_base_url` | `std::string` | *Required* | Upstream target base URL (e.g. `http://127.0.0.1:8080`). |
| `strip_prefix` | `std::string` | `""` | Leading path prefix to strip before forwarding request. |
| `preserve_host` | `bool` | `false` | When `false`, rewrites `Host` to upstream authority; when `true`, retains downstream `Host`. |
| `trust_downstream_forwarded` | `bool` | `false` | When `false`, discards untrusted client `X-Forwarded-*` headers. |
| `max_body_bytes` | `std::size_t` | `16 MB` | Maximum allowed upstream response body size before rejecting. |
| `timeout` | `std::chrono::milliseconds` | `0` (none) | Round-trip deadline for upstream dispatch and response reception. |
| `stream_response` | `bool` | `false` | When `true`, streams chunks incrementally using chunked transfer encoding. |
| `rewrite_request` | `std::function` | `nullptr` | Hook to inspect context and mutate upstream request headers or body before send. |
| `modify_response` | `std::function` | `nullptr` | Hook to inspect upstream response and set additional downstream headers. |

---

## Header Sanitation & Protocol Rules

### 1. Hop-by-Hop Header Stripping (RFC 7230 §6.1)
`kale::proxy` removes all hop-by-hop headers from both the forwarded request and the returned response:
- `Connection`
- `Keep-Alive`
- `Proxy-Authenticate`
- `Proxy-Authorization`
- `TE`
- `Trailer` / `Trailers`
- `Transfer-Encoding`
- `Upgrade`

Custom headers named inside incoming `Connection:` token lists are parsed and stripped automatically.

### 2. Forwarded Headers
- `X-Forwarded-For`: Appends or sets the client's peer IP address (`peer_addr()`).
- `X-Forwarded-Proto`: Set to `http` or `https`.
- `X-Forwarded-Host`: Set to the incoming `Host` header.

### 3. Multiple Set-Cookie Propagation
Multiple upstream `Set-Cookie` headers are preserved and emitted as distinct header lines rather than merged into a single line.

---

## Retained Protocol Limits & Edge Boundaries

1. **Logical Stream Cancellation**: In streaming mode (`stream_response = true`), client disconnect wakes the chunk reader and triggers logical cancellation. It does not guarantee immediate socket termination if bytes are already queued in kernel TCP buffers.
2. **Native Timeout Granularity**: The timeout option uses `std::chrono::milliseconds`. When upstream exceeds the configured deadline, the proxy returns `504 Gateway Timeout`.
3. **Connect Deadlines**: Per project limits #2079, the incremental backend has no per-request TCP connect timeout, and total native DNS/connect deadlines are not guaranteed by the socket layer.
4. **HTTP/1.1 Framing**: `kale::proxy` operates over HTTP/1.1 and does not perform HTTP/2 multiplexing or raw TCP stream tunneling.

---

## curl Interactions

With `example_hello` running on `127.0.0.1:8080` and `example_reverse_proxy` on `127.0.0.1:8081`:

### 1. Gateway Status
```bash
curl -s http://127.0.0.1:8081/ | jq .
```
Expected output:
```json
{
  "behavior": [
    "Hop-by-hop headers stripped per RFC 7230",
    "X-Forwarded-For populated from client peer IP",
    "Host header rewritten to upstream authority",
    "Upstream Set-Cookie headers preserved"
  ],
  "gateway": "reverse_proxy",
  "routes": [
    "ALL /proxy/*path  - Buffered proxy (strips /proxy prefix)",
    "ALL /stream/*path - Streaming proxy (chunked transfer)"
  ],
  "upstream": "http://127.0.0.1:8080"
}
```

### 2. Forwarding Requests (`/proxy/` -> `/`)
```bash
curl -i -s http://127.0.0.1:8081/proxy/
```
Expected output:
```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Content-Length: 13
X-Proxy-Gateway: Kale-Edge
Connection: keep-alive

Hello, Kale!
```

### 3. Query Parameter Forwarding
```bash
curl -s "http://127.0.0.1:8081/proxy/async?name=GatewayUser"
```
Expected output:
```
Hello, GatewayUser (from coroutine)!
```

### 4. Streaming Forwarding Route
```bash
curl -i -s http://127.0.0.1:8081/stream/
```
Expected output:
```http
HTTP/1.1 200 OK
Transfer-Encoding: chunked
Connection: keep-alive

Hello, Kale!
```

### 5. Upstream Outage Error Handling
If `example_hello` on port 8080 is stopped:
```bash
curl -i -s http://127.0.0.1:8081/proxy/
```
Expected output:
```http
HTTP/1.1 502 Bad Gateway
Content-Length: 11
Connection: keep-alive

Bad Gateway
```

---

## Library Dependencies

To use `kale::proxy`, link against `kale_client`:

```lua
target("my_gateway_app")
    set_kind("binary")
    add_deps("kale_client")
    add_files("main.cpp")
```
`kale_client` includes `kale::HttpClient` and `kale::proxy`.
