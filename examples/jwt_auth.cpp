#include "example_server.h"
#include "kale/core/http_server.h"
#include "kale/core/context.h"
#include "kale/core/http_status.h"
#include "kale/security/jwt.h"
#include "kale/middleware/security_headers.h"
#include "kale/http/json.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view JWT_ISSUER = "kale-auth-service";
constexpr std::string_view JWT_AUDIENCE = "kale-api-service";

struct UserRecord {
    std::string username;
    std::string password; // In production: salted cryptographic hash (e.g. Argon2id)
    std::string role;
    std::string department;
};

class UserDatabase {
public:
    UserDatabase() {
        users_["alice"] = {"alice", "secret123", "admin", "engineering"};
        users_["bob"]   = {"bob",   "builder",   "member", "operations"};
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

    // 2. Application-owned user store (owned on main's stack)
    UserDatabase user_db;

    // 3. Secret configuration
    const char *secret_env = std::getenv("KALE_EXAMPLE_SECRET");
    std::string jwt_secret = secret_env ? secret_env : "insecure-dev-jwt-secret-do-not-use-in-production-32b";
    if (!secret_env) {
        std::cout << "[Notice] KALE_EXAMPLE_SECRET is not set; using local development secret.\n"
                  << "         In production, configure a 256-bit cryptographically secure key via KALE_EXAMPLE_SECRET.\n";
    }

    kale::HttpServer server;

    // 4. Security headers middleware
    kale::SecurityHeadersConfig sec_cfg;
    sec_cfg.content_type_nosniff = true;
    sec_cfg.frame_options = "DENY";
    sec_cfg.referrer_policy = "strict-origin-when-cross-origin";
    server.use(kale::security_headers(std::move(sec_cfg)));

    // 5. Configure RFC 6750 Bearer authentication middleware
    kale::JwtValidator validator;
    validator.allow_algorithm(kale::JwtAlgorithm::HS256)
             .require_issuer(std::string(JWT_ISSUER))
             .require_audience(std::string(JWT_AUDIENCE));

    kale::BearerAuthConfig auth_cfg;
    auth_cfg.validator = std::move(validator);
    auth_cfg.secret_or_public_key = jwt_secret;
    auth_cfg.is_pem_key = false;
    auth_cfg.skip_paths = {"/", "/public/info", "/auth/token"};

    server.use(kale::bearer_auth(std::move(auth_cfg)));

    // 6. Application routes

    // Service index
    server.GET("/", [](kale::Context &ctx) {
        ctx.json(kale::Json::Object{
            {"service", "kale-jwt-auth-demo"},
            {"auth_type", "Bearer RFC 6750"},
            {"endpoints", kale::Json::Object{
                {"info", "GET /public/info (Unprotected metadata)"},
                {"token", "POST /auth/token (JSON body: username, password)"},
                {"profile", "GET /api/profile (Protected, requires Authorization: Bearer <token>)"},
                {"admin_reports", "GET /api/reports (Protected, requires role=admin)"}
            }}
        });
    });

    // Public service metadata
    server.GET("/public/info", [](kale::Context &ctx) {
        ctx.json(kale::Json::Object{
            {"issuer", std::string(JWT_ISSUER)},
            {"audience", std::string(JWT_AUDIENCE)},
            {"algorithm", "HS256"},
            {"token_lifetime_seconds", 900}
        });
    });

    // Token issuance endpoint
    server.POST("/auth/token", [&user_db, &jwt_secret](kale::Context &ctx) {
        if (!ctx.is_json()) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Bad Request"},
                {"message", "Expected application/json credentials payload"}
            });
            return;
        }

        kale::Json payload;
        try {
            payload = ctx.parse_json();
        } catch (...) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Bad Request"},
                {"message", "Malformed JSON credentials payload"}
            });
            return;
        }

        if (!payload.has("username") || !payload.has("password")) {
            ctx.set_status(kale::HttpStatus::BadRequest).json(kale::Json::Object{
                {"error", "Bad Request"},
                {"message", "Payload must contain 'username' and 'password'"}
            });
            return;
        }

        std::string username = payload["username"].get<std::string>();
        std::string password = payload["password"].get<std::string>();

        const UserRecord *user = user_db.authenticate(username, password);
        if (!user) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "invalid_grant"},
                {"message", "Invalid username or password"}
            });
            return;
        }

        // Construct claims conforming to RFC 7519
        kale::JwtClaims claims;
        claims.set_issuer(JWT_ISSUER)
              .set_audience(JWT_AUDIENCE)
              .set_subject(user->username)
              .set_issued_at()
              .set_expires_in(std::chrono::seconds(900)) // 15 minutes
              .set("role", user->role)
              .set("department", user->department);

        auto sign_result = kale::JwtSigner::sign_hmac(claims, jwt_secret, kale::JwtAlgorithm::HS256);
        if (!sign_result) {
            ctx.set_status(kale::HttpStatus::InternalServerError).json(kale::Json::Object{
                {"error", "token_generation_failed"},
                {"message", kale::to_string(sign_result.error)}
            });
            return;
        }

        ctx.json(kale::Json::Object{
            {"access_token", *sign_result},
            {"token_type", "Bearer"},
            {"expires_in", 900},
            {"issuer", std::string(JWT_ISSUER)},
            {"audience", std::string(JWT_AUDIENCE)},
            {"subject", user->username}
        });
    });

    // Protected resource: returns verified claims injected into Context
    server.GET("/api/profile", [](kale::Context &ctx) {
        const kale::VerifiedJwtClaims *claims = kale::get_jwt_claims(ctx);
        if (!claims) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "Unauthorized"},
                {"message", "Missing or invalid bearer credentials"}
            });
            return;
        }

        ctx.json(kale::Json::Object{
            {"subject", std::string(claims->subject())},
            {"issuer", std::string(claims->issuer())},
            {"audience", std::string(claims->audience())},
            {"role", (*claims)["role"].get<std::string>()},
            {"department", (*claims)["department"].get<std::string>()},
            {"expires_at", claims->expires_at()}
        });
    });

    // Role-governed protected resource
    server.GET("/api/reports", [](kale::Context &ctx) {
        const kale::VerifiedJwtClaims *claims = kale::get_jwt_claims(ctx);
        if (!claims) {
            ctx.set_status(kale::HttpStatus::Unauthorized).json(kale::Json::Object{
                {"error", "Unauthorized"},
                {"message", "Missing bearer token"}
            });
            return;
        }

        std::string role = (*claims)["role"].get<std::string>();
        if (role != "admin") {
            ctx.set_status(kale::HttpStatus::Forbidden).json(kale::Json::Object{
                {"error", "Forbidden"},
                {"message", "Access restricted to users with the 'admin' role"}
            });
            return;
        }

        ctx.json(kale::Json::Object{
            {"status", "authorized"},
            {"report_id", "REP-2026-X1"},
            {"department", (*claims)["department"].get<std::string>()},
            {"authorized_user", std::string(claims->subject())},
            {"summary", "Monthly operational efficiency: 99.98% uptime across all clusters"}
        });
    });

    // 7. Hand off to lifecycle manager
    return examples::serve(server, options);
}
