# Authentication & Request Security

Kale provides authentication and transport security primitives designed for microservices and web applications. This guide covers two complementary security models:
1. **Stateless JWT Bearer Authentication** ([`../examples/jwt_auth.cpp`](../examples/jwt_auth.cpp)): RFC 7519 JSON Web Tokens with RFC 6750 Bearer authentication and cryptographic claim verification.
2. **Stateful Secure Forms & CSRF Defense** ([`../examples/secure_forms.cpp`](../examples/secure_forms.cpp)): Double-Submit Cookie CSRF protection integrated with user session context.

---

## Part 1: Stateless JWT Bearer Authentication

### Architecture & Standards

- **RFC 7519 / RFC 7518**: Tokens are compact, URL-safe JSON Web Signatures signed using HMAC-SHA256 (`HS256`).
- **Algorithm Whitelist (Anti-Confusion)**: `kale::JwtValidator` enforces an explicit algorithm whitelist via `.allow_algorithm(JwtAlgorithm::HS256)`, preventing algorithm confusion attacks (such as `alg: "none"` or public-key-as-HMAC substitution).
- **Claim Enforcement**: Strict validation for registered claims:
  - `iss` (Issuer): Verified against expected identity authority.
  - `aud` (Audience): Verified against recipient service identifier.
  - `sub` (Subject): Authenticated principal (username/ID).
  - `exp` (Expiration): Verified with configurable clock skew leeway.
- **Context Attachment**: Verified claims are attached to `kale::Context` as `kale::VerifiedJwtClaims` and retrieved via `kale::get_jwt_claims(ctx)`.

### C++ API Usage

The following excerpt from [`../examples/jwt_auth.cpp`](../examples/jwt_auth.cpp) illustrates JWT validation setup and protected route integration:

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/security.h>
#include <kale/http.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/core/context.h"
// #include "kale/core/http_status.h"
// #include "kale/security/jwt.h"
// #include "kale/http/json.h"

#include <chrono>
#include <string>
#include <string_view>

void configure_jwt(kale::HttpServer &server, std::string_view jwt_secret) {
    constexpr std::string_view JWT_ISSUER = "kale-auth-service";
    constexpr std::string_view JWT_AUDIENCE = "kale-api-service";

    kale::JwtValidator validator;
    validator.allow_algorithm(kale::JwtAlgorithm::HS256)
             .require_issuer(std::string(JWT_ISSUER))
             .require_audience(std::string(JWT_AUDIENCE));

    kale::BearerAuthConfig auth_cfg;
    auth_cfg.validator = std::move(validator);
    auth_cfg.secret_or_public_key = std::string(jwt_secret);
    auth_cfg.is_pem_key = false;
    auth_cfg.skip_paths = {"/auth/token", "/public/info"};

    server.use(kale::bearer_auth(std::move(auth_cfg)));

    server.POST("/auth/token", [secret = std::string(jwt_secret)](kale::Context &ctx) {
        kale::JwtClaims claims;
        claims.set_issuer(JWT_ISSUER)
              .set_audience(JWT_AUDIENCE)
              .set_subject("alice")
              .set_issued_at()
              .set_expires_in(std::chrono::seconds(900))
              .set("role", "admin");

        auto sign_result = kale::JwtSigner::sign_hmac(claims, secret, kale::JwtAlgorithm::HS256);
        if (!sign_result) {
            ctx.set_status(kale::HttpStatus::InternalServerError).json(kale::Json::Object{
                {"error", "sign_failed"}
            });
            return;
        }

        ctx.json(kale::Json::Object{
            {"access_token", *sign_result},
            {"token_type", "Bearer"},
            {"expires_in", 900}
        });
    });

    server.GET("/api/profile", [](kale::Context &ctx) {
        const kale::VerifiedJwtClaims *claims = kale::get_jwt_claims(ctx);
        if (!claims) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "Unauthorized"}
            });
            return;
        }
        ctx.json(kale::Json::Object{
            {"subject", std::string(claims->subject())},
            {"role", (*claims)["role"].get<std::string>()}
        });
    });
}
```

### Building and Running

Compile the binary target using native `xmake`:

```bash
xmake build example_jwt_auth
```

Run the server on port `8080`:

```bash
xmake run example_jwt_auth
```

Optionally pass a production signing secret via the environment:

```bash
KALE_EXAMPLE_SECRET="your-high-entropy-256bit-secret-key-goes-here" xmake run example_jwt_auth 8080
```

---

### JWT Bearer Verification Workflow (curl)

The server exposes:
- `GET /public/info`: Unprotected metadata.
- `POST /auth/token`: Exchanges credentials for a signed Bearer token.
- `GET /api/profile`: Protected endpoint requiring a valid Bearer token.
- `GET /api/reports`: Role-governed endpoint requiring `role=admin`.

#### 1. Attempt Access Without Token

```bash
curl -i http://127.0.0.1:8080/api/profile
```

**Expected Response**:
```http
HTTP/1.1 401 Unauthorized
Content-Type: application/json; charset=utf-8
WWW-Authenticate: Bearer error="invalid_token"
X-Content-Type-Options: nosniff
X-Frame-Options: DENY

{
  "error": "Unauthorized",
  "message": "Missing or invalid bearer credentials"
}
```

#### 2. Request a JWT Token

Authenticate with demo credentials (`alice` / `secret123`):

```bash
curl -i -X POST http://127.0.0.1:8080/auth/token \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'
```

**Expected Response**:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{
  "access_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJhdWQiOiJrYWxlLWFwaS1zZXJ2aWNlIiwiZGVwYXJ0bWVudCI6ImVuZ2luZWVyaW5nIiwiZXhwIjoxNzMxMTEwOTAwLCJpYXQiOjE3MzExMTAwMDAsImlzcyI6ImthbGUtYXV0aC1zZXJ2aWNlIiwicm9sZSI6ImFkbWluIiwic3ViIjoiYWxpY2UifQ.signature...",
  "token_type": "Bearer",
  "expires_in": 900,
  "issuer": "kale-auth-service",
  "audience": "kale-api-service",
  "subject": "alice"
}
```

Export the issued token for subsequent requests:

```bash
TOKEN="<paste_access_token_here>"
```

#### 3. Access Protected Profile with Bearer Header

Pass the token via the standard RFC 6750 `Authorization` header:

```bash
curl -i http://127.0.0.1:8080/api/profile \
  -H "Authorization: Bearer $TOKEN"
```

**Expected Response**:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{
  "subject": "alice",
  "issuer": "kale-auth-service",
  "audience": "kale-api-service",
  "role": "admin",
  "department": "engineering",
  "expires_at": 1731110900
}
```

#### 4. Access Role-Restricted Endpoint

Since Alice possesses `role: "admin"`, access to `/api/reports` is granted:

```bash
curl -i http://127.0.0.1:8080/api/reports \
  -H "Authorization: Bearer $TOKEN"
```

**Expected Response**:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{
  "status": "authorized",
  "report_id": "REP-2026-X1",
  "department": "engineering",
  "authorized_user": "alice",
  "summary": "Monthly operational efficiency: 99.98% uptime across all clusters"
}
```

When authenticated as user `bob` (`role: "member"`), the same endpoint returns:
```http
HTTP/1.1 403 Forbidden

{
  "error": "Forbidden",
  "message": "Access restricted to users with the 'admin' role"
}
```

---

### Opaque Bearer Token Mode (`token_verifier`)

When integrating API keys, session-backed bearer tokens, or custom in-memory token registries, `BearerAuthConfig::token_verifier` enables **opaque bearer mode** instead of JWT verification:

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/middlewares.h>
#include <kale/security.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"
// #include "kale/core/http_status.h"
// #include "kale/middleware/access_log.h"
// #include "kale/security/jwt.h"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

void configure_opaque_bearer(kale::HttpServer &server,
                           const std::unordered_set<std::string> &valid_tokens,
                           std::mutex &tokens_mu) {
    server.use(kale::access_log([](kale::AccessLogRecord rec) {
        // Generic status/timing access logging (never log Authorization headers or raw tokens)
        (void)rec;
    }));

    kale::BearerAuthConfig cfg;
    cfg.realm = "service-api";
    cfg.skip_paths = {"/health"};
    cfg.token_verifier = [&valid_tokens, &tokens_mu](const kale::Context &ctx, std::string_view token) -> bool {
        (void)ctx;
        std::lock_guard<std::mutex> lock(tokens_mu);
        return valid_tokens.find(std::string(token)) != valid_tokens.end();
    };
    cfg.on_unauthorized = [](kale::Context &ctx, kale::JwtError err) {
        // Status 401 and WWW-Authenticate (including escaped realm) are pre-set before this hook runs.
        // App-specific logging may record kale::to_string(err) and ctx.path(), never the raw token.
        ctx.json(kale::Json::Object{
            {"error", "unauthorized"},
            {"reason", kale::to_string(err)}
        });
    };

    server.use(kale::bearer_auth(std::move(cfg)));
}
```

- **Replaces JWT mode**: Providing `token_verifier` allows a verifier-only `BearerAuthConfig` (no `secret_or_public_key` or `key_resolver` required). In opaque mode, `bearer_auth` never consults `key_resolver`, never falls back to `JwtValidator` if `token_verifier` returns `false`, and never attaches or erases `VerifiedJwtClaims` on `Context` (any pre-existing `VerifiedJwtClaims` attachment from an outer middleware is preserved).
- **Common routing/extraction & RFC 6750 behavior**: `skip_paths`, `header_name`, `header_prefix`, and `realm` work across both modes. Before invoking `token_verifier`, `bearer_auth` checks credentials per RFC 6750:
  - Missing `header_name` header or a non-matching authentication scheme (e.g., `Basic ...`) returns `401 Unauthorized` with `WWW-Authenticate: Bearer` (or `Bearer realm="..."` with escaped quotes/backslashes) and **no** `error` parameter, passing `JwtError::MissingToken` to `on_unauthorized` (and emitting `reason="missing_token"` in `ErrorEvent` when using default `reply_error`).
  - Duplicate headers, empty header values, malformed Bearer credentials, or tokens failing RFC 6750 `b64token` shape (`1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" ) *"="`, allowing dots and trailing `=` padding only) are rejected before `token_verifier` with `401 Unauthorized`, `WWW-Authenticate: Bearer [realm="...", ]error="invalid_token"`, and `JwtError::MalformedToken`.
  - A syntactically valid Bearer token rejected by `token_verifier` (`false`) returns `401 Unauthorized` with `error="invalid_token"` and `JwtError::TokenRejected`.
- **Synchronous execution & exceptions**: `token_verifier` is synchronous and must not perform blocking I/O on worker threads (for asynchronous introspection, validate inside a coroutine middleware). Any exception thrown by `token_verifier` or `on_unauthorized` propagates through normal middleware error handling (`error_handler` / `access_log` / `HttpServer`).

### Rejection Hook (`on_unauthorized`), Logging, Lifetimes & Thread-Safety

- **`on_unauthorized` hook**: `BearerAuthConfig::on_unauthorized` is the application-specific rejection callback for both JWT and opaque modes. `bearer_auth` pre-sets `HttpStatus::Unauthorized` (`401`) and `WWW-Authenticate` (including escaped `realm` when configured, and `error="invalid_token"` when applicable) before invoking `on_unauthorized`, so custom responders only need to write a response body or emit app-specific diagnostics (and may override status or headers if needed). Configuring `on_unauthorized` replaces the default `reply_error` call, so `ErrorPolicy::on_error` will not receive that rejection event unless `on_unauthorized` itself invokes `reply_error`.
- **Generic status logging**: `bearer_auth` defines no separate logging policy; install `kale::access_log(...)` outside `bearer_auth` (and outside `error_handler`) to record generic HTTP status codes (`401`, `200`, or exception status) and request timings.
- **No token logging**: Never log raw `Authorization` headers or extracted bearer tokens from `token_verifier`, `on_unauthorized`, or `access_log` sinks. Log only safe metadata such as `kale::to_string(err)`, `ctx.path()`, status codes, or request IDs.
- **Lifetimes & thread-safety**:
  - `BearerAuthConfig` callbacks (`token_verifier`, `on_unauthorized`, `key_resolver`, `validator` clock) are invoked concurrently across worker threads and must be thread-safe; any captured state must outlive the `HttpServer`.
  - The `const Context &` and `std::string_view token` arguments to `token_verifier` borrow request-scoped storage and are valid only during that synchronous call; copy into an owned `std::string` if data must outlive the call.

---
## Part 2: Secure Forms & CSRF Defense

### Architecture & Origin/Cookie/CSRF Boundary

Stateful session cookies are vulnerable to Cross-Site Request Forgery (CSRF) because web browsers automatically attach cookies to cross-origin requests initiated by foreign websites (e.g. `<form action="https://app.com/profile" method="POST">` hosted on `evil.com`).

Kale eliminates this attack vector using the **Double-Submit Cookie Pattern** with cryptographic HMAC verification:

1. **Origin Boundary (Same-Origin Policy)**:
   - Modern browsers prevent JavaScript running on `evil.com` from reading cookies set by `app.com`.
2. **Double-Submit Token Generation**:
   - The server issues an HMAC-signed token (`generate_csrf_token(secret)`) stored in a client-readable cookie (`_csrf`).
   - The cookie attribute `http_only` is set to `false` for the CSRF cookie so frontend scripts can read it and inject it into request headers.
   - The session cookie (`kale_session`) remains `http_only: true` so session identifiers cannot be stolen via XSS.
3. **Verification on State-Changing Methods**:
   - Safe HTTP methods (`GET`, `HEAD`, `OPTIONS`, `TRACE`) are exempt from validation and issue or refresh the token cookie.
   - Unsafe methods (`POST`, `PUT`, `DELETE`, `PATCH`) require the client to submit the token via the `X-CSRF-Token` header or `_csrf` form field.
   - The middleware verifies that:
     1. The submitted token has a valid HMAC signature matching the server secret.
     2. The submitted token matches the `_csrf` cookie value in constant time.
   - An attacker on `evil.com` cannot read or forge the matching token value, neutralizing cross-site requests with an immediate `403 Forbidden`.

### C++ API Usage

The following excerpt from [`../examples/secure_forms.cpp`](../examples/secure_forms.cpp) illustrates CSRF middleware setup and protected state mutation:

```cpp
// Convenience headers (active):
#include <kale/core.h>
#include <kale/security.h>
#include <kale/http.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/http_server.h"
// #include "kale/core/context.h"
// #include "kale/core/cookie.h"
// #include "kale/core/http_status.h"
// #include "kale/security/csrf.h"
// #include "kale/security/session.h"
// #include "kale/http/form.h"
// #include "kale/http/json.h"

#include <chrono>
#include <string>
#include <string_view>

void configure_csrf(kale::HttpServer &server, kale::MemorySessionStore &store, std::string_view secret, bool is_tls) {
    kale::SessionOptions sess_opts;
    sess_opts.cookie_name = "kale_session";
    sess_opts.cookie_path = "/";
    sess_opts.secure = is_tls;
    sess_opts.http_only = true;
    sess_opts.same_site = kale::SessionSameSite::Lax;
    server.use(kale::session(store, std::move(sess_opts)));

    kale::CsrfConfig csrf_cfg;
    csrf_cfg.cookie_name = "_csrf";
    csrf_cfg.secret = std::string(secret);
    csrf_cfg.header_name = "X-CSRF-Token";
    csrf_cfg.form_field_name = "_csrf";
    csrf_cfg.secure = is_tls;
    csrf_cfg.http_only = false;
    csrf_cfg.same_site = kale::Cookie::SameSite::Lax;
    csrf_cfg.skip_paths = {"/login"};
    server.use(kale::csrf(std::move(csrf_cfg)));

    server.POST("/profile", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        if (!sess.exists()) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "Unauthorized"}
            });
            co_return;
        }

        std::string display_name = kale::form(ctx, "display_name");
        if (display_name.empty()) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Missing display_name"}
            });
            co_return;
        }

        co_await sess.set("display_name", display_name);
        ctx.json(kale::Json::Object{
            {"status", "updated"},
            {"display_name", display_name}
        });
        co_return;
    });
}
```

### Building and Running

Compile the binary target using native `xmake`:

```bash
xmake build example_secure_forms
```

Run the server on port `8080`:

```bash
xmake run example_secure_forms
```

---

### CSRF Form Submission Workflow (curl)

The server exposes:
- `POST /login`: Initializes user session and default preferences (exempt from CSRF check for initial authentication).
- `GET /api/csrf-token`: Returns the active CSRF token.
- `GET /form`: Serves an HTML form with an embedded hidden `_csrf` input.
- `POST /profile`: State-changing preferences update requiring valid session + valid CSRF token.
- `GET /profile`: Checks current session profile preferences.

#### 1. Authenticate and Establish Session

Login as `alice` and store session cookies in `cookies.txt`:

```bash
curl -i -c cookies.txt \
  -X POST http://127.0.0.1:8080/login \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"secret123"}'
```

**Expected Response**:
```http
HTTP/1.1 200 OK
Set-Cookie: kale_session=...; Path=/; HttpOnly; SameSite=Lax

{
  "status": "authenticated",
  "message": "Login successful",
  "user": "alice",
  "display_name": "Alice Wonderland",
  "email": "alice@example.com",
  "theme": "light"
}
```

#### 2. Retrieve CSRF Token

Fetch the token and update the cookie jar with `_csrf`:

```bash
curl -i -b cookies.txt -c cookies.txt http://127.0.0.1:8080/api/csrf-token
```

**Expected Response**:
```http
HTTP/1.1 200 OK
Set-Cookie: _csrf=f8d2...6b.3c...9a; Path=/; SameSite=Lax

{
  "csrf_token": "f8d2...6b.3c...9a",
  "header_name": "X-CSRF-Token",
  "form_field": "_csrf"
}
```

Export the token:
```bash
CSRF_TOKEN="<paste_csrf_token_here>"
```

#### 3. Attempt Forged / Tokenless Request (Simulated CSRF Attack)

Attempt a state-changing POST request carrying the session cookie but omitting the CSRF token:

```bash
curl -i -b cookies.txt \
  -X POST http://127.0.0.1:8080/profile \
  -d "display_name=Hacked+Name&email=hacker@evil.com"
```

**Expected Response**:
```http
HTTP/1.1 403 Forbidden
Content-Type: application/json; charset=utf-8

{
  "error": "Forbidden",
  "message": "Invalid or missing CSRF token"
}
```
The request is stopped before reaching the application logic, protecting Alice's preferences from unauthorized mutation.

#### 4. Authorized Profile Update via Custom Header (AJAX/Fetch Pattern)

Submit the update with the matching `X-CSRF-Token` header:

```bash
curl -i -b cookies.txt \
  -X POST http://127.0.0.1:8080/profile \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"display_name":"Alice in Wonderland","email":"alice@example.com","theme":"dark"}'
```

**Expected Response**:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{
  "status": "updated",
  "username": "alice",
  "profile": {
    "display_name": "Alice in Wonderland",
    "email": "alice@example.com",
    "theme": "dark"
  }
}
```

#### 5. Authorized Profile Update via Hidden Form Input (Standard Form Submission)

Submit the update using URL-encoded form data with the `_csrf` field:

```bash
curl -i -b cookies.txt \
  -X POST http://127.0.0.1:8080/profile \
  -d "_csrf=$CSRF_TOKEN&display_name=Alice+Wonderland&email=alice@work.org&theme=light"
```

**Expected Response**:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8

{
  "status": "updated",
  "username": "alice",
  "profile": {
    "display_name": "Alice Wonderland",
    "email": "alice@work.org",
    "theme": "light"
  }
}
```

---

## Production Security Contract & Responsibilities

| Responsibility | Example Demonstration | Production Requirement |
| :--- | :--- | :--- |
| **Credential Storage** | Plaintext in memory struct | Store salted cryptographic hashes using Argon2id or scrypt. Rate-limit authentication endpoints to prevent brute-force attacks. |
| **Secret Management** | Local default string fallback | Load high-entropy 256-bit secrets from an environment variable (`KALE_EXAMPLE_SECRET`) injected via KMS / Vault. Do not commit keys to source control. |
| **JWT Key Rotation** | Symmetric HS256 HMAC | In multi-service ecosystems, sign with asymmetric keys (`RS256` or `Ed25519`) using public key distribution endpoints (JWKS). Implement short TTLs (e.g. 15 minutes) with secure refresh token rotation. |
| **Client-Side Storage** | Bearer header in curl | Never store sensitive access tokens in unencrypted browser `localStorage` where XSS can exfiltrate them. Use memory storage or `HttpOnly` cookies. |
| **CSRF Cookie Flags** | `Secure=false` on loopback HTTP | Ensure `secure=true` on all cookies in production. Modern production deployments should prefix cookies with `__Host-` to enforce `Path=/`, HTTPS, and domain isolation. |
| **Transport Security** | Local loopback HTTP (`127.0.0.1`) | Enforce TLS 1.3 across all communication. Attach HSTS headers with `includeSubDomains` and `preload`. |
