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

} // TEST_SUITE("csrf")