#include "doctest/doctest.h"

#include "coke/http/http_client.h"
#include "coke/wait.h"
#include "kale/core/http_server.h"
#include "kale/security/csrf.h"
#include "kale/http/form.h"
#include "kale/core/http_error.h"
#include "kale/core/http_util.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

#include <charconv>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct HttpResponseData {
    int state{-1};
    int status{0};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    std::string header(std::string_view name) const {
        for (const auto &[k, v] : headers) {
            if (kale::case_insensitive_equal(k, name)) return v;
        }
        return {};
    }

    std::vector<std::string> set_cookies() const {
        std::vector<std::string> cookies;
        for (const auto &[k, v] : headers) {
            if (kale::case_insensitive_equal(k, "Set-Cookie")) cookies.push_back(v);
        }
        return cookies;
    }
};

coke::Task<HttpResponseData> execute_request(
    std::string url,
    std::string method,
    std::vector<std::pair<std::string, std::string>> req_headers = {},
    std::string req_body = "") {

    auto *task = WFTaskFactory::create_http_task(url, 0, 0, nullptr);
    auto *request = task->get_req();
    request->set_method(method);
    request->add_header_pair("Connection", "close");
    for (const auto &[k, v] : req_headers) {
        request->add_header_pair(k, v);
    }
    if (!req_body.empty()) {
        request->append_output_body(req_body);
    }

    auto result = co_await coke::HttpAwaiter(task);
    HttpResponseData res;
    res.state = result.state;
    if (result.state == 0) {
        if (const char *sc = result.resp.get_status_code()) {
            std::from_chars(sc, sc + strlen(sc), res.status);
        }
        if (result.resp.is_chunked()) {
            res.body = protocol::HttpUtil::decode_chunked_body(&result.resp);
        } else {
            const void *data = nullptr;
            size_t size = 0;
            result.resp.get_parsed_body(&data, &size);
            if (data && size) res.body.assign(static_cast<const char *>(data), size);
        }
        protocol::HttpHeaderCursor cursor(&result.resp);
        std::string name, val;
        while (cursor.next(name, val)) {
            res.headers.emplace_back(name, val);
        }
    }
    co_return res;
}

class TestServerRunner {
public:
    ~TestServerRunner() { stop(); }
    kale::HttpServer *operator->() noexcept { return &server_; }
    void start() {
        REQUIRE_EQ(server_.start("127.0.0.1", 0), 0);
        REQUIRE_GT(server_.listen_port(), 0);
    }
    void stop() { server_.stop(); }

    HttpResponseData request(std::string_view path,
                             std::string method = "GET",
                             std::vector<std::pair<std::string, std::string>> headers = {},
                             std::string body = "") {
        std::string url = "http://127.0.0.1:" + std::to_string(server_.listen_port()) + std::string(path);
        auto resp = coke::sync_wait(execute_request(std::move(url), std::move(method), std::move(headers), std::move(body)));
        REQUIRE_EQ(resp.state, 0);
        return resp;
    }
private:
    kale::HttpServer server_;
};

std::string extract_cookie_value(std::string_view set_cookie, std::string_view name) {
    std::string prefix = std::string(name) + "=";
    auto pos = set_cookie.find(prefix);
    if (pos == std::string_view::npos) return {};
    auto start = pos + prefix.size();
    auto end = set_cookie.find(';', start);
    if (end == std::string_view::npos) end = set_cookie.size();
    return std::string(set_cookie.substr(start, end - start));
}

} // namespace

TEST_SUITE("csrf") {

TEST_CASE("CSRF Token generation and constant-time verification unit tests") {
    std::string secret = "kale_csrf_test_secret_32_bytes_long_key!";

    // 1. Generation
    std::string token = kale::generate_csrf_token(secret);
    REQUIRE_FALSE(token.empty());

    // 2. Format: 43 chars base64url entropy + '.' + 43 chars base64url HMAC = 87 chars
    CHECK_EQ(token.size(), 87);
    size_t dot_pos = token.find('.');
    REQUIRE_NE(dot_pos, std::string::npos);
    CHECK_EQ(dot_pos, 43);

    // 3. Verification succeeds with matching secret
    CHECK(kale::verify_csrf_token(token, secret));

    // 4. Verification fails with wrong secret
    CHECK_FALSE(kale::verify_csrf_token(token, "wrong_secret_at_least_16_bytes_long"));

    // 5. Verification fails with tampered entropy
    std::string tampered_entropy = token;
    tampered_entropy[0] = (tampered_entropy[0] == 'A' ? 'B' : 'A');
    CHECK_FALSE(kale::verify_csrf_token(tampered_entropy, secret));

    // 6. Verification fails with tampered signature
    std::string tampered_sig = token;
    tampered_sig.back() = (tampered_sig.back() == 'A' ? 'B' : 'A');
    CHECK_FALSE(kale::verify_csrf_token(tampered_sig, secret));

    // 7. Verification fails with malformed token structure
    CHECK_FALSE(kale::verify_csrf_token("invalid_token_no_dot", secret));
    CHECK_FALSE(kale::verify_csrf_token("a.b.c", secret));
    CHECK_FALSE(kale::verify_csrf_token("", secret));
}

TEST_CASE("CSRF Config validation at startup") {
    // Secret must be >= 16 bytes
    kale::CsrfConfig bad_cfg1;
    bad_cfg1.secret = "short";
    CHECK_THROWS_AS(kale::csrf(bad_cfg1), std::invalid_argument);

    kale::CsrfConfig bad_cfg2;
    bad_cfg2.secret = "";
    CHECK_THROWS_AS(kale::csrf(bad_cfg2), std::invalid_argument);

    // SameSite=None requires Secure=true
    kale::CsrfConfig bad_cfg3;
    bad_cfg3.secret = "valid_secret_over_16_bytes";
    bad_cfg3.same_site = kale::Cookie::SameSite::None;
    bad_cfg3.secure = false;
    CHECK_THROWS_AS(kale::csrf(bad_cfg3), std::invalid_argument);

    // __Host- prefix validation
    {
        // Valid __Host- configuration
        kale::CsrfConfig host_cfg;
        host_cfg.secret = "valid_secret_over_16_bytes";
        host_cfg.cookie_name = "__Host-csrf";
        host_cfg.secure = true;
        host_cfg.path = "/";
        host_cfg.domain = "";
        CHECK_NOTHROW(kale::csrf(host_cfg));

        // Insecure __Host-
        host_cfg.secure = false;
        CHECK_THROWS_AS(kale::csrf(host_cfg), std::invalid_argument);
        host_cfg.secure = true;

        // Non-root path __Host-
        host_cfg.path = "/api";
        CHECK_THROWS_AS(kale::csrf(host_cfg), std::invalid_argument);
        host_cfg.path = "/";

        // Domain set on __Host-
        host_cfg.domain = "example.com";
        CHECK_THROWS_AS(kale::csrf(host_cfg), std::invalid_argument);
    }
}

TEST_CASE("CSRF HTTP Server E2E Lifecycle") {
    std::string secret = "kale_csrf_test_secret_32_bytes_long_key!";

    kale::CsrfConfig cfg;
    cfg.secret = secret;
    cfg.cookie_name = "_csrf";
    cfg.header_name = "X-CSRF-Token";
    cfg.form_field_name = "_csrf";
    cfg.skip_paths = {"/public_webhook"};

    TestServerRunner srv;
    srv->use(kale::csrf(cfg));

    srv->GET("/form", [](kale::Context &ctx) {
        ctx.string("form page");
    });

    srv->POST("/submit", [](kale::Context &ctx) {
        ctx.string("submit ok");
    });

    srv->POST("/submit_form", [](kale::Context &ctx) {
        ctx.string("submit form ok");
    });

    srv->POST("/public_webhook", [](kale::Context &ctx) {
        ctx.string("webhook ok");
    });

    srv->POST("/fail", [](kale::Context &) -> coke::Task<void> {
        throw kale::HttpError(422, "Unprocessable entity");
        co_return;
    });
    std::atomic<bool> downstream_invoked{false};
    srv->set_not_found_handler([&](kale::Context &ctx) {
        downstream_invoked = true;
        ctx.set_status(404).string("not found fallback");
    });
    srv->set_method_not_allowed_handler([&](kale::Context &ctx) {
        downstream_invoked = true;
        ctx.set_status(405).string("method not allowed fallback");
    });
    srv.start();

    // 1. Safe GET request without cookie: issues Set-Cookie
    std::string cookie_val;
    {
        auto r = srv.request("/form");
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, "form page");

        auto cookies = r.set_cookies();
        REQUIRE_FALSE(cookies.empty());
        std::string set_cookie_str = cookies[0];

        // Verify Host-Only: no "Domain=" attribute
        CHECK_EQ(set_cookie_str.find("Domain="), std::string::npos);
        // Verify Secure default
        CHECK_NE(set_cookie_str.find("Secure"), std::string::npos);
        // Verify SameSite=Lax
        CHECK_NE(set_cookie_str.find("SameSite=Lax"), std::string::npos);
        // Verify HttpOnly is NOT present so JS can read it for Double Submit
        CHECK_EQ(set_cookie_str.find("HttpOnly"), std::string::npos);

        cookie_val = extract_cookie_value(set_cookie_str, "_csrf");
        REQUIRE_FALSE(cookie_val.empty());
        CHECK(kale::verify_csrf_token(cookie_val, secret));
    }

    // 2. Safe GET request with valid cookie: retains cookie, does NOT issue redundant Set-Cookie
    {
        auto r = srv.request("/form", "GET", {{"Cookie", "_csrf=" + cookie_val}});
        CHECK_EQ(r.status, 200);
        CHECK(r.set_cookies().empty());
    }

    // 3. Unsafe POST request without cookie -> 403 Forbidden!
    // Never generates replacement token to authorize an unsafe request!
    {
        auto r = srv.request("/submit", "POST");
        CHECK_EQ(r.status, 403);
        CHECK_NE(r.body.find("Forbidden"), std::string::npos);
        CHECK(r.set_cookies().empty());
    }

    // 4. Unsafe POST request with cookie but missing submitted token -> 403 Forbidden
    {
        auto r = srv.request("/submit", "POST", {{"Cookie", "_csrf=" + cookie_val}});
        CHECK_EQ(r.status, 403);
    }

    // 5. Unsafe POST request with cookie but forged X-CSRF-Token header -> 403 Forbidden
    {
        auto r = srv.request("/submit", "POST", {
            {"Cookie", "_csrf=" + cookie_val},
            {"X-CSRF-Token", "forged_token.123456"}
        });
        CHECK_EQ(r.status, 403);
    }

    // 6. Unsafe POST request with valid cookie and matching X-CSRF-Token header -> 200 OK!
    {
        auto r = srv.request("/submit", "POST", {
            {"Cookie", "_csrf=" + cookie_val},
            {"X-CSRF-Token", cookie_val}
        });
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, "submit ok");
    }

    // 7. Unsafe POST request with valid cookie and matching form field _csrf -> 200 OK!
    {
        std::string form_body = "_csrf=" + cookie_val + "&name=alice";
        auto r = srv.request("/submit_form", "POST", {
            {"Cookie", "_csrf=" + cookie_val},
            {"Content-Type", "application/x-www-form-urlencoded"}
        }, form_body);
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, "submit form ok");
    }

    // 8. Unsafe POST with both header and form token: conflicting tokens -> 403 Forbidden!
    {
        std::string form_body = "_csrf=another_token_value&name=alice";
        auto r = srv.request("/submit_form", "POST", {
            {"Cookie", "_csrf=" + cookie_val},
            {"X-CSRF-Token", cookie_val},
            {"Content-Type", "application/x-www-form-urlencoded"}
        }, form_body);
        CHECK_EQ(r.status, 403);
    }

    // 9. Unsafe POST with duplicate conflicting _csrf cookies -> 403 Forbidden!
    {
        auto r = srv.request("/submit", "POST", {
            {"Cookie", "_csrf=" + cookie_val + "; _csrf=different_val"},
            {"X-CSRF-Token", cookie_val}
        });
        CHECK_EQ(r.status, 403);
    }

    // 10. Unsafe POST with duplicate X-CSRF-Token headers -> 403 Forbidden!
    {
        auto r = srv.request("/submit", "POST", {
            {"Cookie", "_csrf=" + cookie_val},
            {"X-CSRF-Token", cookie_val},
            {"X-CSRF-Token", cookie_val}
        });
        CHECK_EQ(r.status, 403);
    }

    // 11. Extension / unknown HTTP method (e.g. PURGE) fails closed -> 403 Forbidden!
    // Asserts downstream fallback is NOT invoked when rejected
    {
        downstream_invoked = false;
        auto r = srv.request("/cache", "PURGE");
        CHECK_EQ(r.status, 403);
        CHECK_NE(r.body.find("Forbidden"), std::string::npos);
        CHECK_FALSE(downstream_invoked);

        // Passing valid CSRF token with extension method passes CSRF and reaches downstream
        auto r_valid = srv.request("/cache", "PURGE", {
            {"Cookie", "_csrf=" + cookie_val},
            {"X-CSRF-Token", cookie_val}
        });
        CHECK(downstream_invoked);
        CHECK_EQ(r_valid.status, 404); // reached fallback handler
    }

    // 12. Skip path passes without credentials -> 200 OK
    {
        auto r = srv.request("/public_webhook", "POST");
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, "webhook ok");
    }

    // 13. Downstream exception preservation: HttpError(422) propagates as 422
    {
        auto r = srv.request("/fail", "POST", {
            {"Cookie", "_csrf=" + cookie_val},
            {"X-CSRF-Token", cookie_val}
        });
        CHECK_EQ(r.status, 422);
    }
}

TEST_CASE("CSRF OriginOnly, OriginOrToken, and TokenRequired with trusted_origins") {
    auto make_srv = [](kale::CsrfConfig cfg) {
        auto srv = std::make_unique<TestServerRunner>();
        (*srv)->use(kale::csrf(std::move(cfg)));
        (*srv)->GET("/submit", [](kale::Context &ctx) { ctx.string("ok"); });
        (*srv)->POST("/submit", [](kale::Context &ctx) { ctx.string("ok"); });
        srv->start();
        return srv;
    };

    // 1. OriginOnly mode: no secret required, no Set-Cookie on GET, enforces Origin/Referer
    {
        kale::CsrfConfig cfg;
        cfg.mode = kale::CsrfMode::OriginOnly;
        cfg.trusted_origins = {"https://app.example.com"};
        cfg.allow_referer_fallback = true;

        auto srv_ptr = make_srv(std::move(cfg));
        auto &srv = *srv_ptr;
        auto r_get = srv.request("/submit", "GET");
        CHECK_EQ(r_get.status, 200);
        CHECK_EQ(r_get.set_cookies().size(), 0);
        // Valid Origin -> 200
        auto r_ok = srv.request("/submit", "POST", {{"Origin", "https://app.example.com"}});
        CHECK_EQ(r_ok.status, 200);

        // Valid Referer fallback -> 200
        auto r_ref = srv.request("/submit", "POST", {{"Referer", "https://app.example.com/form"}});
        CHECK_EQ(r_ref.status, 200);

        // Null Origin -> 403
        auto r_null = srv.request("/submit", "POST", {{"Origin", "null"}});
        CHECK_EQ(r_null.status, 403);

        // Cross-origin -> 403
        auto r_evil = srv.request("/submit", "POST", {{"Origin", "https://evil.example.com"}});
        CHECK_EQ(r_evil.status, 403);

        // Missing Origin and Referer -> 403
        auto r_missing = srv.request("/submit", "POST");
        CHECK_EQ(r_missing.status, 403);
    }

    // 2. OriginOnly with trusted_origin_resolver returning empty -> falls back to Host
    {
        kale::CsrfConfig cfg;
        cfg.mode = kale::CsrfMode::OriginOnly;
        cfg.allow_referer_fallback = false;
        cfg.trusted_origin_resolver = [](const kale::Context &ctx) -> std::string {
            return std::string(ctx.header_view("X-Custom-Origin"));
        };

        auto srv_ptr = make_srv(std::move(cfg));
        auto &srv = *srv_ptr;
        std::string expected_origin = "http://127.0.0.1:" + std::to_string(srv->listen_port());
        auto r_host = srv.request("/submit", "POST", {
            {"Origin", expected_origin}
        });
        CHECK_EQ(r_host.status, 200);

        // Referer fallback disabled -> 403
        auto r_ref = srv.request("/submit", "POST", {
            {"Referer", expected_origin + "/page"}
        });
        CHECK_EQ(r_ref.status, 403);
    }

    // 3. OriginOrToken mode
    {
        kale::CsrfConfig cfg;
        cfg.secret = "0123456789abcdef0123456789abcdef";
        cfg.secure = false;
        cfg.mode = kale::CsrfMode::OriginOrToken;
        cfg.trusted_origins = {"https://app.example.com"};

        auto srv_ptr = make_srv(cfg);
        auto &srv = *srv_ptr;
        // Passes with valid Origin even without _csrf cookie
        auto r_origin = srv.request("/submit", "POST", {{"Origin", "https://app.example.com"}});
        CHECK_EQ(r_origin.status, 200);

        // Passes with valid Double-Submit token when Origin is absent
        std::string tok = kale::generate_csrf_token(cfg.secret);
        auto r_tok = srv.request("/submit", "POST", {
            {"Cookie", "_csrf=" + tok},
            {"X-CSRF-Token", tok}
        });
        CHECK_EQ(r_tok.status, 200);

        // Fails when Origin is cross-origin and no valid token
        auto r_fail = srv.request("/submit", "POST", {{"Origin", "https://evil.example.com"}});
        CHECK_EQ(r_fail.status, 403);
    }

    // 4. TokenRequired with trusted_origins and allow_form_field = false
    {
        kale::CsrfConfig cfg;
        cfg.secret = "0123456789abcdef0123456789abcdef";
        cfg.secure = false;
        cfg.mode = kale::CsrfMode::TokenRequired;
        cfg.trusted_origins = {"https://app.example.com"};
        cfg.allow_form_field = false;

        auto srv_ptr = make_srv(cfg);
        auto &srv = *srv_ptr;
        std::string tok = kale::generate_csrf_token(cfg.secret);

        // Valid Origin + valid header token -> 200
        auto r_ok = srv.request("/submit", "POST", {
            {"Origin", "https://app.example.com"},
            {"Cookie", "_csrf=" + tok},
            {"X-CSRF-Token", tok}
        });
        CHECK_EQ(r_ok.status, 200);

        // Valid header token but wrong Origin -> 403
        auto r_bad_origin = srv.request("/submit", "POST", {
            {"Origin", "https://evil.example.com"},
            {"Cookie", "_csrf=" + tok},
            {"X-CSRF-Token", tok}
        });
        CHECK_EQ(r_bad_origin.status, 403);

        // Form-field token rejected when allow_form_field = false -> 403
        auto r_form = srv.request("/submit", "POST", {
            {"Origin", "https://app.example.com"},
            {"Cookie", "_csrf=" + tok},
            {"Content-Type", "application/x-www-form-urlencoded"}
        }, "_csrf=" + tok);
        CHECK_EQ(r_form.status, 403);
    }
}

} // TEST_SUITE("csrf")