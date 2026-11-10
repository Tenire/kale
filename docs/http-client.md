# HTTP Client & Upstream Aggregation

This guide covers Kale's coroutine-native C++20 `HttpClient`, request composition, structured response parsing, retry configuration, and timeout/error handling.

The complete runnable example program is located at [`../examples/http_client.cpp`](../examples/http_client.cpp).

---

## Capabilities & Architecture

`kale::HttpClient` provides non-blocking, coroutine-native HTTP/1.1 client capabilities. Network I/O operations suspend the executing coroutine (`coke::Task<void>`), yielding control back to the event loop until socket events fire.

### Key Characteristics
- **Request Composition**: Method helpers (`get`, `post`, `put`, `del`, `patch`, `head`, `options`, `request`) returning an owning `HttpClientRequest` builder.
- **Response Inspection**: Read status code, inspect headers, and read body content asynchronously (`read_body`, `read_chunk`).
- **Timeout Management**: Configure send, receive, and total timeouts using `std::chrono::duration` types.
- **Local Service Aggregation**: Designed to communicate with local services without third-party network dependencies.

---

## Building and Running

Compile the client example target with `xmake`:

```bash
xmake build example_http_client
```

### Two-Terminal Launch Recipe

Pair the client example with `example_hello`:

#### Terminal 1: Launch Upstream Service
```bash
xmake build example_hello
xmake run example_hello 8080
```
Expected output:
```
Listening on http://127.0.0.1:8080/
```

#### Terminal 2: Launch Client Aggregator
```bash
KALE_EXAMPLE_UPSTREAM=http://127.0.0.1:8080 xmake run example_http_client 8081
```
Expected output:
```
Listening on http://127.0.0.1:8081/
```

To stop either service, send `SIGINT` (`Ctrl+C`) or `SIGTERM`.

---

## Request Builder Reference

`kale::HttpClient` provides builder composition on `HttpClientRequest`:

```cpp
#include <chrono>
#include "coke/task.h"

// Convenience headers (active):
#include <kale/client.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/client/http_client.h"

coke::Task<void> fetch_example(kale::HttpClient &client) {
    auto response = co_await client.get("http://127.0.0.1:8080/async?name=Tester")
        .header("Accept", "text/plain")
        .timeout(std::chrono::seconds(5))
        .send();
    (void)response;
}
```
### Source-Confirmed Methods (`HttpClientRequest`)

| Method | Argument | Description |
|---|---|---|
| `.header(key, val)` | `std::string, std::string` | Appends a single header pair. |
| `.headers(list)` | `std::vector<std::pair<std::string, std::string>>` | Appends a collection of header pairs. |
| `.body(data)` | `std::string` | Sets request payload string. |
| `.body(data, content_type)`| `std::string, std::string` | Sets payload and `Content-Type` header. |
| `.json(j)` | `const kale::Json &` | Sets serialized JSON body and sets `Content-Type: application/json`. |
| `.json(raw_json)` | `std::string_view` | Sets raw JSON string and sets `Content-Type: application/json`. |
| `.timeout(d)` | `std::chrono::duration` | Sets connect, send, receive, and total timeouts uniformly. |
| `.connect_timeout(d)` | `std::chrono::duration` | Configures client connection duration field. |
| `.send_timeout(d)` | `std::chrono::duration` | TCP write deadline for sending request headers and body. |
| `.receive_timeout(d)` | `std::chrono::duration` | Read deadline for waiting on upstream headers and response body. |
| `.total_timeout(d)` | `std::chrono::duration` | End-to-end deadline from `send()` start through response body completion. |
| `.max_body_bytes(bytes)` | `std::size_t` | Maximum allowed response body bytes (throws `HttpBodyTooLargeError` if exceeded). |
| `.follow_redirects(max)` | `int` | Number of redirects to follow (default: 0). |
| `.retry(max_retries)` | `int` | Retry attempts on connection error. |
| `.send()` | *None* | Dispatches request and returns `coke::Task<HttpClientResponse>`. |

---

## Retained Protocol Limits & Edge Boundaries

Per project guidance and backend implementation:

1. **Per-Request TCP Connect Timeout**: The underlying incremental network backend has no per-request TCP connect-timeout control.
2. **DNS & Connect Deadlines**: Total native DNS resolution and connection deadlines are not guaranteed by the underlying socket layer.
3. **Logical Stream Cancellation**: Calling `response.cancel()` marks the response stream as cancelled and wakes any pending `read_chunk()` readers with `HttpClientError::Code::Aborted`. It is a logical cancellation and does not guarantee immediate physical TCP socket RST at the kernel level.
4. **Idempotent Retries**: `client.get()` and `client.head()` requests are idempotent. When using `.retry(N)`, non-idempotent methods (`POST`, `PUT`, `DELETE`) do not retry on connection drops to avoid duplicate mutations.

---

## Response Handling & Exceptions

Awaiting `.send()` yields `kale::HttpClientResponse`:

```cpp
#include <iostream>
#include <string>
#include "coke/task.h"

// Convenience headers (active):
#include <kale/client.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/client/http_client.h"

coke::Task<void> inspect_example(kale::HttpClientResponse &response) {
    int status = response.status();
    std::string server_header = response.header("Server");
    std::string body = co_await response.read_body();

    std::cout << "Status: " << status << ", Server: " << server_header << ", Body: " << body << "\n";
}
```

When network failures, unreachable hosts, or deadlines occur, operations throw exceptions derived from `kale::HttpClientError`:
- `kale::HttpTimeoutError`: Thrown when receive or total timeout expires (`.is_timeout()` returns `true`).
- `kale::HttpBodyTooLargeError`: Thrown when response body exceeds `max_body_bytes`.
- `kale::HttpClientError`: Base exception containing `.code()`, `.sys_error()`, and `.what()`.

---

## curl Interactions

With `example_hello` running on `127.0.0.1:8080` and `example_http_client` running on `127.0.0.1:8081`:

### 1. Root Service Index
```bash
curl -s http://127.0.0.1:8081/ | jq .
```
Expected output:
```json
{
  "routes": [
    "GET  /aggregate   - Calls upstream /",
    "GET  /greet       - Calls upstream /async?name=...",
    "POST /api/echo    - Local echo endpoint for testing POST",
    "POST /relay       - Relays payload via HttpClient to local /api/echo"
  ],
  "service": "http_client_aggregator",
  "upstream": "http://127.0.0.1:8080"
}
```

### 2. Upstream Aggregation (`GET /`)
```bash
curl -s http://127.0.0.1:8081/aggregate | jq .
```
Expected output:
```json
{
  "status": "ok",
  "target_url": "http://127.0.0.1:8080/",
  "upstream_body": "Hello, Kale!\n",
  "upstream_status": 200
}
```

### 3. Parametrized Upstream Greeting (`GET /async?name=...`)
```bash
curl -s "http://127.0.0.1:8081/greet?name=Alice" | jq .
```
Expected output:
```json
{
  "greeting": "Hello, Alice (from coroutine)!\n",
  "queried_name": "Alice",
  "status": "ok",
  "upstream_status": 200
}
```

### 4. Payload Relay via POST
Send a JSON payload to `/relay`, which relays it via `kale::HttpClient` to the service's own `/api/echo` endpoint (or to an optional `?upstream=...` URL):
```bash
curl -s -X POST http://127.0.0.1:8081/relay \
  -H "Content-Type: application/json" \
  -d '{"message":"hello_from_client","count":5}' | jq .
```
Expected output:
```json
{
  "relayed": true,
  "reply": "{\"message\":\"hello_from_client\",\"count\":5}",
  "status": 200,
  "target_url": "http://127.0.0.1:8081/api/echo"
}
```

### 5. Upstream Outage Error Handling
If you terminate `example_hello` on port 8080 and make a request:
```bash
curl -s http://127.0.0.1:8081/aggregate | jq .
```
Expected output:
```json
{
  "error": "bad_gateway",
  "message": "Connection refused",
  "target_url": "http://127.0.0.1:8080/"
}
```

---

## Library Dependencies

To use `kale::HttpClient`, link against `kale_client`:

```lua
target("my_client_app")
    set_kind("binary")
    add_deps("kale_client")
    add_files("main.cpp")
```
