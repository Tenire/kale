#include "example_server.h"
#include "kale/core/http_server.h"
#include "kale/core/context.h"
#include "kale/core/cookie.h"
#include "kale/core/http_status.h"
#include "kale/security/csrf.h"
#include "kale/security/session.h"
#include "kale/http/form.h"
#include "kale/middleware/security_headers.h"
#include "kale/http/json.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace {

struct UserRecord {
    std::string username;
    std::string password; // In production: salted cryptographic hash (e.g. Argon2id)
    std::string display_name;
    std::string email;
};

class UserDatabase {
public:
    UserDatabase() {
        users_["alice"] = {"alice", "secret123", "Alice Wonderland", "alice@example.com"};
        users_["bob"]   = {"bob",   "builder",   "Bob Builder",      "bob@example.com"};
    }

    const UserRecord *authenticate(std::string_view username, std::string_view password) const {
        auto it = users_.find(username);
        if (it != users_.end() && it->second.password == password) {
            return &it->second;
        }
        return nullptr;
    }

private:
    std::map<std::string, UserRecord, std::less<>> users_;
};

std::string escape_html_attr(std::string_view input) {
    std::string out;
    out.reserve(input.size() * 2);
    for (char c : input) {
        switch (c) {
            case '&':  out.append("&amp;"); break;
            case '<':  out.append("&lt;"); break;
            case '>':  out.append("&gt;"); break;
            case '"':  out.append("&quot;"); break;
            case '\'': out.append("&#39;"); break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}
} // namespace

int main(int argc, char *argv[]) {
    // 1. Initialize server options and block signals (must be first statement in main)
    examples::ServerOptions options(argc, argv);

    // 2. Application-owned user directory and in-memory session store (owned on main's stack)
    UserDatabase user_db;
    kale::MemorySessionStore session_store;

    // 3. Security and environment configuration
    const bool is_tls = (std::getenv("KALE_EXAMPLE_CERT") != nullptr &&
                         std::getenv("KALE_EXAMPLE_KEY") != nullptr);

    const char *secret_env = std::getenv("KALE_EXAMPLE_SECRET");
    std::string csrf_secret = secret_env ? secret_env : "insecure-dev-csrf-secret-change-in-production-32b";
    if (!secret_env) {
        std::cout << "[Notice] KALE_EXAMPLE_SECRET is not set; using local development secret for CSRF HMAC.\n"
                  << "         In production, configure a high-entropy secret via KALE_EXAMPLE_SECRET.\n";
    }

    kale::HttpServer server;

    // 4. Mount security headers
    kale::SecurityHeadersConfig sec_cfg;
    sec_cfg.content_type_nosniff = true;
    sec_cfg.frame_options = "DENY";
    sec_cfg.referrer_policy = "strict-origin-when-cross-origin";
    if (is_tls) {
        sec_cfg.hsts = "max-age=31536000; includeSubDomains";
    }
    server.use(kale::security_headers(std::move(sec_cfg)));

    // 5. Mount session middleware
    kale::SessionOptions sess_opts;
    sess_opts.cookie_name = "kale_session";
    sess_opts.cookie_path = "/";
    sess_opts.secure = is_tls;
    sess_opts.http_only = true; // Essential: scripts cannot read session credentials
    sess_opts.same_site = kale::SessionSameSite::Lax;
    sess_opts.ttl = std::chrono::hours(12);
    server.use(kale::session(session_store, std::move(sess_opts)));

    // 6. Mount CSRF protection middleware (Double-Submit Cookie Pattern)
    kale::CsrfConfig csrf_cfg;
    csrf_cfg.cookie_name = "_csrf";
    csrf_cfg.secret = csrf_secret;
    csrf_cfg.header_name = "X-CSRF-Token";
    csrf_cfg.form_field_name = "_csrf";
    csrf_cfg.secure = is_tls;
    // Note: http_only is false so frontend JavaScript can read the cookie to inject X-CSRF-Token header
    csrf_cfg.http_only = false;
    csrf_cfg.same_site = kale::Cookie::SameSite::Lax;
    csrf_cfg.skip_paths = {"/login"}; // Skip unauthenticated initial login
    server.use(kale::csrf(std::move(csrf_cfg)));

    // 7. Application routes

    // Service index
    server.GET("/", [](kale::Context &ctx) {
        auto &sess = kale::get_session(ctx);
        bool auth = sess.exists();

        ctx.json(kale::Json::Object{
            {"service", "kale-secure-forms-demo"},
            {"authenticated", auth},
            {"user", auth ? sess.get("username").value_or("guest") : "guest"},
            {"endpoints", kale::Json::Object{
                {"login", "POST /login (JSON body: username, password)"},
                {"csrf_token", "GET /api/csrf-token (Retrieve active CSRF token)"},
                {"html_form", "GET /form (Rendered HTML form with hidden _csrf token)"},
                {"update_profile", "POST /profile (State-changing: requires valid session and CSRF token)"},
                {"view_profile", "GET /profile (Inspect current session profile preferences)"},
                {"logout", "POST /logout (Terminate session)"}
            }}
        });
    });

    // Login endpoint: initializes session and user preferences in session context
    server.POST("/login", [&user_db](kale::Context &ctx) -> coke::Task<void> {
        if (!ctx.is_json()) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Bad Request"},
                {"message", "Expected application/json credentials payload"}
            });
            co_return;
        }

        kale::Json payload;
        try {
            payload = ctx.parse_json();
        } catch (...) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Bad Request"},
                {"message", "Malformed JSON credentials payload"}
            });
            co_return;
        }

        if (!payload.has("username") || !payload.has("password")) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Bad Request"},
                {"message", "Missing required fields 'username' or 'password'"}
            });
            co_return;
        }

        std::string username = payload["username"].get<std::string>();
        std::string password = payload["password"].get<std::string>();

        const UserRecord *user = user_db.authenticate(username, password);
        if (!user) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "Unauthorized"},
                {"message", "Invalid username or password"}
            });
            co_return;
        }

        auto &sess = kale::get_session(ctx);
        if (sess.exists()) {
            co_await sess.regenerate({
                {"username", user->username},
                {"display_name", user->display_name},
                {"email", user->email},
                {"theme", "light"}
            });
        } else {
            co_await sess.create({
                {"username", user->username},
                {"display_name", user->display_name},
                {"email", user->email},
                {"theme", "light"}
            });
        }

        ctx.json(kale::Json::Object{
            {"status", "authenticated"},
            {"message", "Login successful"},
            {"user", user->username},
            {"display_name", user->display_name},
            {"email", user->email},
            {"theme", "light"}
        });
        co_return;
    });

    // Endpoint to retrieve active CSRF token for AJAX / API clients
    server.GET("/api/csrf-token", [&csrf_secret, is_tls](kale::Context &ctx) {
        std::string_view cookie_token = ctx.cookie("_csrf");
        std::string token;
        if (!cookie_token.empty() && kale::verify_csrf_token(cookie_token, csrf_secret)) {
            token = std::string(cookie_token);
        } else {
            token = kale::generate_csrf_token(csrf_secret);
            kale::Cookie c("_csrf", token);
            c.set_path("/").set_http_only(false).set_same_site(kale::Cookie::SameSite::Lax).set_secure(is_tls);
            ctx.set_cookie(std::move(c));
        }

        ctx.json(kale::Json::Object{
            {"csrf_token", token},
            {"header_name", "X-CSRF-Token"},
            {"form_field", "_csrf"}
        });
    });

    // Server-rendered HTML form demonstrating hidden token embedding
    server.GET("/form", [&csrf_secret, is_tls](kale::Context &ctx) {
        std::string_view cookie_token = ctx.cookie("_csrf");
        std::string token;
        if (!cookie_token.empty() && kale::verify_csrf_token(cookie_token, csrf_secret)) {
            token = std::string(cookie_token);
        } else {
            token = kale::generate_csrf_token(csrf_secret);
            kale::Cookie c("_csrf", token);
            c.set_path("/").set_http_only(false).set_same_site(kale::Cookie::SameSite::Lax).set_secure(is_tls);
            ctx.set_cookie(std::move(c));
        }

        auto &sess = kale::get_session(ctx);
        std::string current_name = sess.exists() ? sess.get("display_name").value_or("Alice Wonderland") : "Alice Wonderland";
        std::string current_email = sess.exists() ? sess.get("email").value_or("alice@example.com") : "alice@example.com";
        std::string current_theme = sess.exists() ? sess.get("theme").value_or("light") : "light";

        std::string form_html =
            "<!DOCTYPE html>\n"
            "<html lang=\"en\">\n"
            "<head><meta charset=\"utf-8\"><title>Update Preferences</title></head>\n"
            "<body>\n"
            "  <h2>Profile Preferences</h2>\n"
            "  <form action=\"/profile\" method=\"POST\">\n"
            "    <input type=\"hidden\" name=\"_csrf\" value=\"" + escape_html_attr(token) + "\"/>\n"
            "    <p><label>Display Name: <input type=\"text\" name=\"display_name\" value=\"" + escape_html_attr(current_name) + "\"/></label></p>\n"
            "    <p><label>Email: <input type=\"email\" name=\"email\" value=\"" + escape_html_attr(current_email) + "\"/></label></p>\n"
            "    <p><label>Theme: <input type=\"text\" name=\"theme\" value=\"" + escape_html_attr(current_theme) + "\"/></label></p>\n"
            "    <p><button type=\"submit\">Save Preferences</button></p>\n"
            "  </form>\n"
            "</body>\n"
            "</html>\n";

        ctx.html(form_html);
    });

    // Protected state-changing endpoint: updates per-session preferences
    server.POST("/profile", [](kale::Context &ctx) -> coke::Task<void> {
        // Step 1: Verify authenticated session
        auto &sess = kale::get_session(ctx);
        if (!sess.exists()) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "Unauthorized"},
                {"message", "Authentication required. Please POST /login first."}
            });
            co_return;
        }

        std::string display_name;
        std::string email;
        std::string theme;

        // Step 2: Extract form fields or JSON payload
        if (kale::has_form(ctx, "display_name")) {
            display_name = kale::form(ctx, "display_name");
            email = kale::form(ctx, "email");
            theme = kale::form(ctx, "theme");
        } else if (ctx.is_json()) {
            try {
                auto body = ctx.parse_json();
                if (body.has("display_name")) display_name = body["display_name"].get<std::string>();
                if (body.has("email")) email = body["email"].get<std::string>();
                if (body.has("theme")) theme = body["theme"].get<std::string>();
            } catch (...) {
                // fall through to validation check
            }
        }

        if (display_name.empty() || email.empty()) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Bad Request"},
                {"message", "Missing required fields 'display_name' or 'email'"}
            });
            co_return;
        }

        if (theme.empty()) {
            theme = "light";
        }

        // Step 3: Mutate only per-session state (no shared mutable maps)
        co_await sess.set("display_name", display_name);
        co_await sess.set("email", email);
        co_await sess.set("theme", theme);

        // Step 4: Return meaningful update result
        ctx.json(kale::Json::Object{
            {"status", "updated"},
            {"username", sess.get("username").value_or("")},
            {"profile", kale::Json::Object{
                {"display_name", display_name},
                {"email", email},
                {"theme", theme}
            }}
        });
        co_return;
    });

    // Profile inspection endpoint
    server.GET("/profile", [](kale::Context &ctx) {
        auto &sess = kale::get_session(ctx);
        if (!sess.exists()) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "Unauthorized"},
                {"message", "Active session required"}
            });
            return;
        }

        ctx.json(kale::Json::Object{
            {"username", sess.get("username").value_or("")},
            {"profile", kale::Json::Object{
                {"display_name", sess.get("display_name").value_or("")},
                {"email", sess.get("email").value_or("")},
                {"theme", sess.get("theme").value_or("light")}
            }}
        });
    });

    // Logout endpoint: destroys session
    server.POST("/logout", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        if (sess.exists()) {
            co_await sess.destroy();
        }
        ctx.json(kale::Json::Object{
            {"status", "logged_out"},
            {"message", "Session terminated and cookie cleared"}
        });
        co_return;
    });

    // 8. Hand off to lifecycle manager
    return examples::serve(server, options);
}
