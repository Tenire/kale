#include "example_server.h"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "kale/core/context.h"
#include "kale/core/cookie.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/security/signed_cookie.h"

namespace {

constexpr std::string_view kSecretKey = "super-secret-hmac-key-for-kale-examples-2026";
constexpr std::string_view kCookieName = "user_sig";

} // namespace

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);
        kale::HttpServer server;

        // GET /set-cookie: sets standard and secure cookies
        server.GET("/set-cookie", [](kale::Context &ctx) {
            // Standard tracking cookie
            kale::Cookie std_cookie("standard_pref", "dark_mode");
            std_cookie.set_path("/")
                      .set_max_age(86400)
                      .set_http_only(false)
                      .set_same_site(kale::Cookie::SameSite::Lax);
            ctx.set_cookie(std_cookie);

            // Secure session-like cookie
            kale::Cookie sec_cookie("secure_token", "sec_val_998877");
            sec_cookie.set_path("/")
                      .set_max_age(3600)
                      .set_secure(true)
                      .set_http_only(true)
                      .set_same_site(kale::Cookie::SameSite::Strict);
            ctx.set_cookie(sec_cookie);

            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Standard and secure cookies set successfully.\n");
        });

        // POST /login: authenticates and sets an HMAC signed cookie
        server.POST("/login", [](kale::Context &ctx) {
            std::string username = std::string(ctx.default_query("username", "admin"));

            // Build cookie with security attributes and sign via kale::set_signed_cookie
            kale::Cookie auth_cookie(std::string(kCookieName), username);
            auth_cookie.set_path("/")
                       .set_max_age(7200) // 2 hours
                       .set_http_only(true)
                       .set_same_site(kale::Cookie::SameSite::Lax);

            kale::set_signed_cookie(ctx, auth_cookie, kSecretKey);

            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Logged in as '" + username + "'. Signed cookie '" +
                       std::string(kCookieName) + "' issued.\n");
        });

        // GET /profile: verifies signed cookie; rejects tampered or missing cookies with 401
        server.GET("/profile", [](kale::Context &ctx) {
            std::optional<std::string> user = kale::signed_cookie(ctx, kCookieName, kSecretKey);

            if (!user.has_value()) {
                ctx.set_status(kale::HttpStatus::Unauthorized);
                ctx.set_content_type("text/plain; charset=utf-8");
                ctx.string("401 Unauthorized: missing or invalid/tampered signature on cookie '" +
                           std::string(kCookieName) + "'\n");
                return;
            }

            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Welcome to your profile, " + *user + "!\n");
        });

        // POST /logout: expires the cookies
        server.POST("/logout", [](kale::Context &ctx) {
            // Expire signed cookie by setting max_age = 0
            kale::Cookie auth_cookie(std::string(kCookieName), "");
            auth_cookie.set_path("/").set_max_age(0);
            ctx.set_cookie(auth_cookie);

            // Expire standard cookie
            kale::Cookie std_cookie("standard_pref", "");
            std_cookie.set_path("/").set_max_age(0);
            ctx.set_cookie(std_cookie);

            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Logged out. Cookies expired.\n");
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
