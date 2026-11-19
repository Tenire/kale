#ifndef KALE_JWT_H
#define KALE_JWT_H

#include "kale/core/context.h"
#include "kale/core/router.h"
#include "kale/http/json.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kale {

inline constexpr size_t MAX_JWT_COMPACT_SIZE = 16384; // 16 KiB total compact token limit
inline constexpr size_t MAX_JWT_HEADER_SIZE = 2048;   // 2 KiB base64url protected header limit
inline constexpr size_t MAX_JWT_PAYLOAD_SIZE = 8192;  // 8 KiB base64url claims payload limit
inline constexpr int MAX_JWT_JSON_DEPTH = 16;         // Maximum allowed JSON nesting depth

// IEEE 754 double exact integer representation range [-(2^53 - 1), (2^53 - 1)]
inline constexpr double MIN_SAFE_NUMERIC_DATE = -9007199254740991.0;
inline constexpr double MAX_SAFE_NUMERIC_DATE = 9007199254740991.0;

/**
 * Cryptographic algorithms supported for JSON Web Signatures (JWS).
 * Conforms to RFC 7518, RFC 8037, and RFC 9864.
 */
enum class JwtAlgorithm {
    HS256,   // HMAC using SHA-256 (RFC 7518 §3.2)
    HS384,   // HMAC using SHA-384 (RFC 7518 §3.2)
    HS512,   // HMAC using SHA-512 (RFC 7518 §3.2)
    RS256,   // RSASSA-PKCS1-v1_5 using SHA-256 (RFC 7518 §3.3)
    RS384,   // RSASSA-PKCS1-v1_5 using SHA-384 (RFC 7518 §3.3)
    RS512,   // RSASSA-PKCS1-v1_5 using SHA-512 (RFC 7518 §3.3)
    ES256,   // ECDSA using P-256 and SHA-256 (RFC 7518 §3.4)
    Ed25519  // EdDSA using Ed25519 curve (RFC 9864 / RFC 8037)
};

/**
 * Validation and decoding error codes.
 */
enum class JwtError {
    None = 0,
    MalformedToken,             // Token is not 3 dot-separated segments or exceeds bounds
    InvalidBase64,              // Base64URL decoding failed, invalid alphabet, or illegal padding '='
    InvalidJson,                // JSON syntax error, non-object root, or exceeds depth limit
    MissingAlgorithm,           // Protected header is missing 'alg' parameter
    UnsupportedAlgorithm,       // Algorithm is unknown or disallowed (e.g. 'none')
    AlgorithmNotAllowed,        // Algorithm does not match configured whitelist (anti-confusion)
    SignatureMismatch,          // Cryptographic signature verification failed or invalid length
    TokenExpired,               // 'exp' claim is in the past
    TokenNotYetValid,           // 'nbf' claim is in the future
    TokenIssuedInFuture,        // 'iat' claim exceeds current time plus leeway
    IssuerMismatch,             // 'iss' claim does not match expected value
    AudienceMismatch,           // 'aud' claim does not match expected audience
    SubjectMismatch,            // 'sub' claim does not match expected value
    MissingRequiredClaim,       // Required claim ('exp', 'iss', 'aud', or 'sub') is missing
    KeyInvalid,                 // Secret or PEM key is invalid, empty, or wrong key family/length
    DuplicateKey,               // Duplicate member key detected in header or claims object
    ClaimTypeMismatch,          // Claim value has wrong JSON type (e.g. non-numeric exp or non-string iss)
    UnsupportedHeaderParameter, // Unsupported critical header parameter (e.g. 'crit', 'b64', 'jku')
    TypeMismatch,               // Header 'typ' is invalid, missing when required, or mismatched
    KidMismatch,                // Header 'kid' is invalid, missing when required, or mismatched
    TokenRejected,              // Opaque bearer token was rejected by application token_verifier
    MissingToken,               // Authorization header is missing or has non-matching scheme
};

const char* to_string(JwtError err) noexcept;
std::string_view to_string(JwtAlgorithm alg) noexcept;
std::optional<JwtAlgorithm> parse_jwt_algorithm(std::string_view str) noexcept;

/**
 * Strongly typed operation result for JWT sign and verify.
 */
template <typename T>
struct JwtResult {
    std::optional<T> value{std::nullopt};
    JwtError error{JwtError::None};

    JwtResult() = default;
    JwtResult(T val) : value(std::move(val)), error(JwtError::None) {}
    JwtResult(JwtError err) : value(std::nullopt), error(err) {}
    JwtResult(T val, JwtError err) : value(std::move(val)), error(err) {}

    bool has_value() const noexcept { return error == JwtError::None && value.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
    const T& operator*() const noexcept { return *value; }
    T& operator*() noexcept { return *value; }
    const T* operator->() const noexcept { return &*value; }
    T* operator->() noexcept { return &*value; }
};

/**
 * Mutable JWT Claims builder container for constructing tokens to sign.
 */
class JwtClaims {
public:
    JwtClaims();
    explicit JwtClaims(Json payload);

    // Standard RFC 7519 registered claims accessors
    std::string_view issuer() const noexcept;        // iss
    std::string_view subject() const noexcept;       // sub
    std::string_view audience() const noexcept;      // aud (first if array, or string)
    std::string_view jwt_id() const noexcept;        // jti
    int64_t expires_at() const noexcept;             // exp (Unix epoch seconds)
    int64_t not_before() const noexcept;             // nbf
    int64_t issued_at() const noexcept;              // iat

    // Standard claims setters (fluent API)
    JwtClaims& set_issuer(std::string_view iss);
    JwtClaims& set_subject(std::string_view sub);
    JwtClaims& set_audience(std::string_view aud);
    JwtClaims& set_jwt_id(std::string_view jti);
    JwtClaims& set_expires_at(std::chrono::system_clock::time_point tp);
    JwtClaims& set_expires_at(int64_t epoch_sec);
    JwtClaims& set_expires_in(std::chrono::seconds duration);
    JwtClaims& set_not_before(std::chrono::system_clock::time_point tp);
    JwtClaims& set_not_before(int64_t epoch_sec);
    JwtClaims& set_issued_at(std::chrono::system_clock::time_point tp = std::chrono::system_clock::now());
    JwtClaims& set_issued_at(int64_t epoch_sec);

    // Arbitrary claim setter
    template <typename T>
    JwtClaims& set(const std::string &key, T &&value) {
        payload_[key] = std::forward<T>(value);
        return *this;
    }

    // Arbitrary claim getter
    Json operator[](const std::string &key) const;
    bool has(const std::string &key) const noexcept;
    const Json& raw_payload() const noexcept { return payload_; }
    Json& raw_payload() noexcept { return payload_; }

    std::string dump() const;

private:
    Json payload_;
};

/**
 * Immutable container holding cryptographically verified claims.
 * Attached to Context upon successful verification.
 */
class VerifiedJwtClaims {
public:
    VerifiedJwtClaims(const VerifiedJwtClaims &) = default;
    VerifiedJwtClaims &operator=(const VerifiedJwtClaims &) = default;
    VerifiedJwtClaims(VerifiedJwtClaims &&) noexcept = default;
    VerifiedJwtClaims &operator=(VerifiedJwtClaims &&) noexcept = default;
    ~VerifiedJwtClaims() = default;

    std::string_view issuer() const noexcept { return iss_; }
    std::string_view subject() const noexcept { return sub_; }
    std::string_view audience() const noexcept; // Returns first audience
    const std::vector<std::string>& audiences() const noexcept { return auds_; }
    std::string_view jwt_id() const noexcept { return jti_; }
    int64_t expires_at() const noexcept { return exp_; }
    int64_t not_before() const noexcept { return nbf_; }
    int64_t issued_at() const noexcept { return iat_; }

    bool has_exp() const noexcept { return has_exp_; }
    bool has_nbf() const noexcept { return has_nbf_; }
    bool has_iat() const noexcept { return has_iat_; }

    JwtAlgorithm algorithm() const noexcept { return alg_; }
    std::string_view token_type() const noexcept { return typ_; }
    std::string_view kid() const noexcept { return kid_; }

    Json operator[](const std::string &key) const;
    bool has(const std::string &key) const noexcept;
    const Json& raw_payload() const noexcept { return payload_; }
    std::string dump() const;

    JwtClaims to_claims() const;

private:
    friend class JwtValidator;

    VerifiedJwtClaims(Json payload,
                      std::string iss,
                      std::string sub,
                      std::vector<std::string> auds,
                      std::string jti,
                      int64_t exp, bool has_exp,
                      int64_t nbf, bool has_nbf,
                      int64_t iat, bool has_iat,
                      JwtAlgorithm alg,
                      std::string typ,
                      std::string kid)
        : payload_(std::move(payload)),
          iss_(std::move(iss)),
          sub_(std::move(sub)),
          auds_(std::move(auds)),
          jti_(std::move(jti)),
          exp_(exp), has_exp_(has_exp),
          nbf_(nbf), has_nbf_(has_nbf),
          iat_(iat), has_iat_(has_iat),
          alg_(alg),
          typ_(std::move(typ)),
          kid_(std::move(kid)) {}

    Json payload_;
    std::string iss_;
    std::string sub_;
    std::vector<std::string> auds_;
    std::string jti_;
    int64_t exp_{0};
    bool has_exp_{false};
    int64_t nbf_{0};
    bool has_nbf_{false};
    int64_t iat_{0};
    bool has_iat_{false};
    JwtAlgorithm alg_{JwtAlgorithm::HS256};
    std::string typ_;
    std::string kid_;
};

/**
 * JWT Signer producing compact JWS tokens.
 */
class JwtSigner {
public:
    // Signs token using HMAC (HS256, HS384, HS512)
    static JwtResult<std::string> sign_hmac(
        const JwtClaims &claims,
        std::string_view secret,
        JwtAlgorithm alg = JwtAlgorithm::HS256,
        std::string_view kid = "",
        std::string_view typ = "JWT");

    // Signs token using PEM Private Key (RS256, RS384, RS512, ES256, Ed25519)
    static JwtResult<std::string> sign_pem(
        const JwtClaims &claims,
        std::string_view private_key_pem,
        JwtAlgorithm alg = JwtAlgorithm::RS256,
        std::string_view kid = "",
        std::string_view typ = "JWT");
};

/**
 * JWT Validator enforcing algorithm whitelist, time bounds, and claim checks.
 */
struct JwtKeyMaterial {
    std::string key;
    bool is_pem{false}; // false: HMAC secret; true: PEM Public Key
};

using JwtKeyResolver = std::function<std::optional<JwtKeyMaterial>(std::string_view kid, JwtAlgorithm alg)>;

class JwtValidator {
public:
    JwtValidator() = default;

    // Allowed algorithms (rejects all if empty).
    JwtValidator& allow_algorithm(JwtAlgorithm alg);
    JwtValidator& allow_algorithms(std::vector<JwtAlgorithm> algs);

    // Claims constraints
    JwtValidator& require_issuer(std::string iss);
    JwtValidator& require_audience(std::string aud);
    JwtValidator& require_subject(std::string sub);
    JwtValidator& require_type(std::string typ);
    JwtValidator& require_kid(std::string kid);

    // Clock tolerance and claim verification toggles
    JwtValidator& set_clock_skew(std::chrono::seconds skew); // Default: 60s
    JwtValidator& validate_exp(bool enable = true);          // Default: true
    JwtValidator& require_exp(bool enable = true);           // Default: true (exp required by default)
    JwtValidator& validate_nbf(bool enable = true);          // Default: true
    JwtValidator& validate_iat(bool enable = true);          // Default: false
    JwtValidator& set_clock(std::function<int64_t()> clock_fn); // Injected clock in epoch seconds

    // Verifies symmetric token (HS256, HS384, HS512)
    JwtResult<VerifiedJwtClaims> verify_hmac(
        std::string_view token,
        std::string_view secret) const;

    // Verifies asymmetric token (RS256, RS384, RS512, ES256, Ed25519)
    JwtResult<VerifiedJwtClaims> verify_pem(
        std::string_view token,
        std::string_view public_key_pem) const;

    // Verifies token by resolving key material dynamically after header (alg, typ, kid) validation
    JwtResult<VerifiedJwtClaims> verify(
        std::string_view token,
        const JwtKeyResolver &key_resolver) const;

private:
    JwtResult<VerifiedJwtClaims> verify_common(
        std::string_view token,
        std::optional<bool> is_pem_verifier,
        std::function<JwtError(std::string_view signing_input, std::string_view sig_b64, JwtAlgorithm alg, std::string_view kid)> signature_verifier) const;
    std::vector<JwtAlgorithm> allowed_algs_;
    std::optional<std::string> expected_iss_;
    std::optional<std::string> expected_aud_;
    std::optional<std::string> expected_sub_;
    std::optional<std::string> expected_typ_;
    std::optional<std::string> expected_kid_;
    std::chrono::seconds clock_skew_{60};
    bool check_exp_{true};
    bool require_exp_{true};
    bool check_nbf_{true};
    bool check_iat_{false};
    std::function<int64_t()> clock_fn_;
};

/**
 * Bearer authentication middleware configuration.
 */
struct BearerAuthConfig {
    JwtValidator validator;
    std::string secret_or_public_key;
    bool is_pem_key{false}; // false: HMAC secret; true: PEM Public Key
    JwtKeyResolver key_resolver{};

    std::string header_name{"Authorization"};
    std::string header_prefix{"Bearer "};
    std::string realm;
    std::vector<std::string> skip_paths;

    // Optional custom unauthorized response handler
    std::function<void(Context &ctx, JwtError err)> on_unauthorized;

    // Optional synchronous opaque bearer token verifier (replaces JWT verification when set)
    std::function<bool(const Context &ctx, std::string_view token)> token_verifier;
};

/**
 * Creates Bearer authentication middleware adhering to RFC 6750.
 * In JWT mode, verifies the token and attaches VerifiedJwtClaims to Context on success.
 * When token_verifier is set, runs in opaque bearer token mode instead of JWT verification
 * without consulting key_resolver or modifying VerifiedJwtClaims attachments.
 * Responds with 401 on failure.
 */
Middleware bearer_auth(BearerAuthConfig config);
/**
 * Retrieves verified claims injected into Context by bearer_auth middleware.
 * Returns nullptr if authentication was not performed or failed.
 */
const VerifiedJwtClaims* get_jwt_claims(const Context &ctx) noexcept;

} // namespace kale

#endif // KALE_JWT_H