# Session Management

Kale provides built-in, asynchronous session management via the `kale::session` middleware. Sessions decouple sensitive user state from client-side cookies by maintaining state in an application-controlled store while exchanging an opaque, cryptographically random session identifier with the client.

The complete working source code for this guide is located at [`../examples/sessions.cpp`](../examples/sessions.cpp).

---

## Architecture & Security Mechanics

Session security relies on coordinated client and server responsibilities:

1. **Opaque Session Identifiers**:
   - The client cookie holds only a cryptographically random 256-bit identifier (a 64-character hexadecimal token).
   - Session identifiers are unguessable and carry no payload or sensitive state on the client.
2. **Application-Owned Session Store**:
   - State is stored in a backend implementing `kale::SessionStore` (such as `kale::MemorySessionStore`).
   - In `examples/sessions.cpp`, `MemorySessionStore` is constructed on `main()`'s stack and owned across the server's lifecycle, guaranteeing no background thread leaks or invalid memory access.
3. **Session Fixation Prevention**:
   - On successful login, the application calls `sess.regenerate()` if a session already exists (or `sess.create()` if starting anonymous), issuing a fresh identifier and invalidating the old record to prevent session fixation attacks.
4. **Cookie Attributes**:
   - `HttpOnly`: Set to `true` to block JavaScript access (`document.cookie`), mitigating Cross-Site Scripting (XSS) session theft.
   - `SameSite=Lax`: Mitigates cross-site request leakage on external top-level navigations.
   - `Secure`: Dynamically set to `true` when TLS certificates are configured (`KALE_EXAMPLE_CERT` / `KALE_EXAMPLE_KEY`), ensuring cookies are transmitted only over HTTPS.
5. **Security Headers**:
   - Hardens browser processing using `kale::security_headers`, applying `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: strict-origin-when-cross-origin`, and HSTS under TLS.

---

## C++ API Usage

The following excerpt from [`../examples/sessions.cpp`](../examples/sessions.cpp) illustrates middleware registration and handler integration:

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/security.h>
#include <kale/http.h>
#include <kale/middlewares.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/core/context.h"
// #include "kale/core/http_status.h"
// #include "kale/security/session.h"
// #include "kale/middleware/security_headers.h"
// #include "kale/http/json.h"

#include <chrono>
#include <string>

void configure_sessions(kale::HttpServer &server, kale::MemorySessionStore &store, bool is_tls) {
    kale::SessionOptions options;
    options.cookie_name = "kale_session";
    options.cookie_path = "/";
    options.http_only = true;
    options.same_site = kale::SessionSameSite::Lax;
    options.secure = is_tls;
    options.ttl = std::chrono::hours(12);

    server.use(kale::session(store, std::move(options)));

    server.POST("/login", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        if (sess.exists()) {
            co_await sess.regenerate({{"username", "alice"}, {"role", "admin"}});
        } else {
            co_await sess.create({{"username", "alice"}, {"role", "admin"}});
        }
        ctx.json(kale::Json::Object{{"status", "authenticated"}});
        co_return;
    });

    server.GET("/profile", [](kale::Context &ctx) {
        auto &sess = kale::get_session(ctx);
        if (!sess.exists()) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "Unauthorized"}
            });
            return;
        }
        ctx.json(kale::Json::Object{
            {"username", sess.get("username").value_or("")}
        });
    });

    server.POST("/logout", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        if (sess.exists()) {
            co_await sess.destroy();
        }
        ctx.json(kale::Json::Object{{"status", "logged_out"}});
        co_return;
    });
}
```

---

## Building and Running

Compile the binary target using native `xmake`:

```bash
xmake build example_sessions
```

Run the server on the default port (`8080`):

```bash
xmake run example_sessions
```

Specify a custom port or ephemeral port (`0`):

```bash
xmake run example_sessions 9090
xmake run example_sessions 0
```

To run with TLS termination (HTTP/1.1 over TLS):

```bash
KALE_EXAMPLE_CERT=/path/to/cert.pem KALE_EXAMPLE_KEY=/path/to/key.pem xmake run example_sessions 8443
```

---

## Session Workflow & Verification (curl)

The example provides three core endpoints:
- `POST /login`: Validates credentials and initializes session data.
- `GET /profile`: Reads protected user information from session context.
- `POST /logout`: Terminates the session in storage and expires the client cookie.

### 1. Attempt Unauthenticated Access

Query the protected profile endpoint without a session cookie:

```bash
curl -i http://127.0.0.1:8080/profile
```

**Expected Response**:
```http
HTTP/1.1 401 Unauthorized
Content-Type: application/json; charset=utf-8
X-Content-Type-Options: nosniff
X-Frame-Options: DENY
Referrer-Policy: strict-origin-when-cross-origin

{
  "error": "Unauthorized",
  "message": "No active session found. Please authenticate via POST /login"
}
```

### 2. Authenticate and Store Cookie Jar

Submit demo credentials (`alice` / `secret123`) and save the returned cookie into `cookies.txt`:

```bash
curl -i -c cookies.txt \
  -X POST http://127.0.0.1:8080/login \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'
```

**Expected Response**:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Set-Cookie: kale_session=2a8e71b5c409fa...64hex...; Path=/; Max-Age=43200; HttpOnly; SameSite=Lax
X-Content-Type-Options: nosniff
X-Frame-Options: DENY
Referrer-Policy: strict-origin-when-cross-origin

{
  "status": "authenticated",
  "message": "Session established successfully",
  "user": {
    "id": 101,
    "username": "alice",
    "role": "admin",
    "display_name": "Alice Wonderland"
  }
}
```

Inspect `cookies.txt` to observe the RFC 6265 cookie parameters:
```text
# Netscape HTTP Cookie File
127.0.0.1	FALSE	/	FALSE	1731110400	kale_session	2a8e71b5c409fa...64hex...
```

### 3. Access Protected Profile with Session Cookie

Present the cookie jar (`-b cookies.txt`) to the `/profile` route:

```bash
curl -i -b cookies.txt http://127.0.0.1:8080/profile
```

**Expected Response**:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
X-Content-Type-Options: nosniff
X-Frame-Options: DENY
Referrer-Policy: strict-origin-when-cross-origin

{
  "user_id": "101",
  "username": "alice",
  "role": "admin",
  "display_name": "Alice Wonderland"
}
```

### 4. Logout and Cookie Invalidation

Terminate the session:

```bash
curl -i -b cookies.txt -c cookies.txt \
  -X POST http://127.0.0.1:8080/logout
```

**Expected Response**:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Set-Cookie: kale_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax
X-Content-Type-Options: nosniff
X-Frame-Options: DENY

{
  "status": "logged_out",
  "message": "Session terminated and cookie invalidated"
}
```

The response includes `Max-Age=0`, directing the HTTP client to delete `kale_session`. The corresponding record in `MemorySessionStore` is purged simultaneously.

### 5. Verify Session Invalidation

Re-request `/profile` with the updated cookie jar:

```bash
curl -i -b cookies.txt http://127.0.0.1:8080/profile
```

**Expected Response**:
```http
HTTP/1.1 401 Unauthorized
Content-Type: application/json; charset=utf-8

{
  "error": "Unauthorized",
  "message": "No active session found. Please authenticate via POST /login"
}
```

---

## Production Security Contract & Responsibilities

While the example demonstrates complete session lifecycle patterns, production deployments must observe key distinctions:

| Layer | Example Implementation | Production Responsibility |
| :--- | :--- | :--- |
| **Password Storage** | Plaintext matching in `UserDatabase` | Use salted, memory-hard cryptographic password hashes (e.g. Argon2id, scrypt, or bcrypt). Never store or compare plaintext passwords. |
| **Session Backend** | In-memory `MemorySessionStore` (single-node) | Single-process memory stores do not share state across horizontally scaled cluster instances. Use a distributed backend (e.g. Redis) or shared persistent storage. |
| **Transport Layer** | HTTP allowed on loopback (`127.0.0.1`) | Enforce HTTPS/TLS everywhere. Set `session_opts.secure = true` and ensure HSTS header is dispatched (`Strict-Transport-Security: max-age=31536000`). |
| **Session Expiration** | Fixed TTL (12 hours) | Implement idle timeouts, absolute timeouts, and explicit session rotation upon privilege elevation to prevent session fixation attacks. |
