#include "example_server.h"
#include "kale/core/http_server.h"
#include "kale/core/context.h"
#include "kale/security/session.h"
#include "kale/middleware/security_headers.h"
#include "kale/http/json.h"

#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>

namespace {

struct UserRecord {
    int id{0};
    std::string username;
    std::string password; // In production: salted cryptographic hash (e.g. Argon2id)
    std::string role;
    std::string display_name;
};

class UserDatabase {
public:
    UserDatabase() {
        users_["alice"] = {101, "alice", "secret123", "admin", "Alice Wonderland"};
        users_["bob"]   = {102, "bob",   "builder",   "member", "Bob Builder"};
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

} // namespace

int main(int argc, char *argv[]) {
    // 1. Initialize server options and block signals (must be first statement in main)
    examples::ServerOptions options(argc, argv);

    // 2. Application-owned user store and session store (owned on main's stack)
    UserDatabase user_db;
    kale::MemorySessionStore session_store;

    // 3. Environment configuration
    const bool is_tls = (std::getenv("KALE_EXAMPLE_CERT") != nullptr &&
                         std::getenv("KALE_EXAMPLE_KEY") != nullptr);

    kale::HttpServer server;

    // 4. Security headers middleware
    kale::SecurityHeadersConfig sec_cfg;
    sec_cfg.content_type_nosniff = true;
    sec_cfg.frame_options = "DENY";
    sec_cfg.referrer_policy = "strict-origin-when-cross-origin";
    if (is_tls) {
        sec_cfg.hsts = "max-age=31536000; includeSubDomains";
    }
    server.use(kale::security_headers(std::move(sec_cfg)));

    // 5. Session middleware with opaque session IDs and cookie configuration
    kale::SessionOptions session_opts;
    session_opts.cookie_name = "kale_session";
    session_opts.cookie_path = "/";
    session_opts.http_only = true; // Protects cookie from JavaScript access (anti-XSS)
    session_opts.same_site = kale::SessionSameSite::Lax;
    session_opts.secure = is_tls; // Transmit cookie over HTTPS when TLS is configured
    session_opts.ttl = std::chrono::hours(12);

    server.use(kale::session(session_store, std::move(session_opts)));

    // 6. Application routes

    // Service index
    server.GET("/", [](kale::Context &ctx) {
        auto &sess = kale::get_session(ctx);
        bool auth = sess.exists();

        ctx.json(kale::Json::Object{
            {"service", "kale-sessions-demo"},
            {"authenticated", auth},
            {"current_user", auth ? sess.get("username").value_or("guest") : "guest"},
            {"endpoints", kale::Json::Object{
                {"login", "POST /login (JSON body: username, password)"},
                {"profile", "GET /profile (Requires active session cookie)"},
                {"logout", "POST /logout (Destroys active session)"}
            }}
        });
    });

    // Login endpoint: validates credentials and provisions session record
    server.POST("/login", [&user_db](kale::Context &ctx) -> coke::Task<void> {
        if (!ctx.is_json()) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Bad Request"},
                {"message", "Expected application/json payload"}
            });
            co_return;
        }

        kale::Json body;
        try {
            body = ctx.parse_json();
        } catch (...) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Bad Request"},
                {"message", "Malformed JSON payload"}
            });
            co_return;
        }

        if (!body.has("username") || !body.has("password")) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Bad Request"},
                {"message", "Missing required fields 'username' or 'password'"}
            });
            co_return;
        }

        std::string username = body["username"].get<std::string>();
        std::string password = body["password"].get<std::string>();

        const UserRecord *user = user_db.authenticate(username, password);
        if (!user) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "Unauthorized"},
                {"message", "Invalid username or password"}
            });
            co_return;
        }

        auto &sess = kale::get_session(ctx);
        // If an existing session is present, regenerate it to prevent session fixation
        if (sess.exists()) {
            co_await sess.regenerate({
                {"user_id", std::to_string(user->id)},
                {"username", user->username},
                {"role", user->role},
                {"display_name", user->display_name}
            });
        } else {
            co_await sess.create({
                {"user_id", std::to_string(user->id)},
                {"username", user->username},
                {"role", user->role},
                {"display_name", user->display_name}
            });
        }

        ctx.json(kale::Json::Object{
            {"status", "authenticated"},
            {"message", "Session established successfully"},
            {"user", kale::Json::Object{
                {"id", user->id},
                {"username", user->username},
                {"role", user->role},
                {"display_name", user->display_name}
            }}
        });
        co_return;
    });

    // Profile endpoint: reads authenticated session context
    server.GET("/profile", [](kale::Context &ctx) {
        auto &sess = kale::get_session(ctx);
        if (!sess.exists()) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "Unauthorized"},
                {"message", "No active session found. Please authenticate via POST /login"}
            });
            return;
        }

        ctx.json(kale::Json::Object{
            {"user_id", sess.get("user_id").value_or("")},
            {"username", sess.get("username").value_or("")},
            {"role", sess.get("role").value_or("")},
            {"display_name", sess.get("display_name").value_or("")}
        });
    });

    // Logout endpoint: invalidates session and clears client cookie
    server.POST("/logout", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        if (!sess.exists()) {
            ctx.json(kale::Json::Object{
                {"status", "no_session"},
                {"message", "No active session to terminate"}
            });
            co_return;
        }

        co_await sess.destroy();
        ctx.json(kale::Json::Object{
            {"status", "logged_out"},
            {"message", "Session terminated and cookie invalidated"}
        });
        co_return;
    });

    // 7. Hand off to lifecycle manager
    return examples::serve(server, options);
}
