#include "kale/security/jwt.h"
#include "kale/core/error_response.h"
#include "kale/core/http_error.h"

#include "kale/core/http_status.h"
#include "kale/core/http_util.h"
#include "http/base64.h"
#include "security/crypto_util.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/json_parser.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_set>

namespace kale {

namespace {

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY *p) const noexcept { if (p) EVP_PKEY_free(p); }
};
using UniqueEvpPkey = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;

struct BioDeleter {
    void operator()(BIO *b) const noexcept { if (b) BIO_free(b); }
};
using UniqueBio = std::unique_ptr<BIO, BioDeleter>;

struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX *c) const noexcept { if (c) EVP_MD_CTX_free(c); }
};
using UniqueEvpMdCtx = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

struct JsonValDeleter {
    void operator()(json_value_t *v) const noexcept { if (v) json_value_destroy(v); }
};
using UniqueJsonVal = std::unique_ptr<json_value_t, JsonValDeleter>;

bool is_strict_base64url(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) {
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

bool check_json_structure(const json_value_t *val, int depth, bool &has_duplicate) {
    if (!val) return false;
    if (depth > MAX_JWT_JSON_DEPTH) return false;

    int type = json_value_type(val);
    if (type == JSON_VALUE_OBJECT) {
        const json_object_t *obj = json_value_object(val);
        if (!obj) return false;
        std::unordered_set<std::string_view> seen_keys;
        const char *name = nullptr;
        const json_value_t *child = nullptr;
        json_object_for_each(name, child, obj) {
            if (!name) return false;
            std::string_view key_view(name);
            if (!seen_keys.insert(key_view).second) {
                has_duplicate = true;
                return false;
            }
            if (!check_json_structure(child, depth + 1, has_duplicate)) {
                return false;
            }
        }
    } else if (type == JSON_VALUE_ARRAY) {
        const json_array_t *arr = json_value_array(val);
        if (!arr) return false;
        const json_value_t *child = nullptr;
        json_array_for_each(child, arr) {
            if (!check_json_structure(child, depth + 1, has_duplicate)) {
                return false;
            }
        }
    }
    return true;
}

bool parse_numeric_date(const json_value_t *val, int64_t &out) noexcept {
    if (!val || json_value_type(val) != JSON_VALUE_NUMBER) return false;
    double d = json_value_number(val);
    if (!std::isfinite(d) || std::floor(d) != d) return false;
    if (d < MIN_SAFE_NUMERIC_DATE || d > MAX_SAFE_NUMERIC_DATE) return false;
    out = static_cast<int64_t>(d);
    return true;
}

// Genuinely overflow-safe time comparisons
inline bool is_expired(int64_t now, int64_t exp, int64_t leeway) noexcept {
    if (exp > now) return false;
    uint64_t diff = static_cast<uint64_t>(now) - static_cast<uint64_t>(exp);
    return static_cast<uint64_t>(leeway) <= diff;
}

inline bool is_not_yet_valid(int64_t now, int64_t nbf, int64_t leeway) noexcept {
    if (now >= nbf) return false;
    uint64_t diff = static_cast<uint64_t>(nbf) - static_cast<uint64_t>(now);
    return static_cast<uint64_t>(leeway) < diff;
}

inline bool is_issued_in_future(int64_t now, int64_t iat, int64_t leeway) noexcept {
    if (iat <= now) return false;
    uint64_t diff = static_cast<uint64_t>(iat) - static_cast<uint64_t>(now);
    return diff > static_cast<uint64_t>(leeway);
}

} // namespace

const char* to_string(JwtError err) noexcept {
    switch (err) {
        case JwtError::None: return "none";
        case JwtError::MalformedToken: return "malformed_token";
        case JwtError::InvalidBase64: return "invalid_base64";
        case JwtError::InvalidJson: return "invalid_json";
        case JwtError::MissingAlgorithm: return "missing_algorithm";
        case JwtError::UnsupportedAlgorithm: return "unsupported_algorithm";
        case JwtError::AlgorithmNotAllowed: return "algorithm_not_allowed";
        case JwtError::SignatureMismatch: return "signature_mismatch";
        case JwtError::TokenExpired: return "token_expired";
        case JwtError::TokenNotYetValid: return "token_not_yet_valid";
        case JwtError::TokenIssuedInFuture: return "token_issued_in_future";
        case JwtError::IssuerMismatch: return "issuer_mismatch";
        case JwtError::AudienceMismatch: return "audience_mismatch";
        case JwtError::SubjectMismatch: return "subject_mismatch";
        case JwtError::MissingRequiredClaim: return "missing_required_claim";
        case JwtError::KeyInvalid: return "key_invalid";
        case JwtError::DuplicateKey: return "duplicate_key";
        case JwtError::ClaimTypeMismatch: return "claim_type_mismatch";
        case JwtError::UnsupportedHeaderParameter: return "unsupported_header_parameter";
    }
    return "unknown_error";
}

std::string_view to_string(JwtAlgorithm alg) noexcept {
    switch (alg) {
        case JwtAlgorithm::HS256: return "HS256";
        case JwtAlgorithm::HS384: return "HS384";
        case JwtAlgorithm::HS512: return "HS512";
        case JwtAlgorithm::RS256: return "RS256";
        case JwtAlgorithm::RS384: return "RS384";
        case JwtAlgorithm::RS512: return "RS512";
        case JwtAlgorithm::Ed25519: return "Ed25519";
    }
    return "unknown";
}

std::optional<JwtAlgorithm> parse_jwt_algorithm(std::string_view str) noexcept {
    if (str == "HS256") return JwtAlgorithm::HS256;
    if (str == "HS384") return JwtAlgorithm::HS384;
    if (str == "HS512") return JwtAlgorithm::HS512;
    if (str == "RS256") return JwtAlgorithm::RS256;
    if (str == "RS384") return JwtAlgorithm::RS384;
    if (str == "RS512") return JwtAlgorithm::RS512;
    if (str == "Ed25519" || str == "EdDSA") return JwtAlgorithm::Ed25519;
    return std::nullopt;
}

// ---------------- JwtClaims ----------------

JwtClaims::JwtClaims() : payload_(JsonObject()) {}
JwtClaims::JwtClaims(Json payload) : payload_(std::move(payload)) {}

std::string_view JwtClaims::issuer() const noexcept {
    if (payload_.has("iss") && payload_["iss"].is_string()) {
        return payload_["iss"].get<std::string_view>();
    }
    return {};
}

std::string_view JwtClaims::subject() const noexcept {
    if (payload_.has("sub") && payload_["sub"].is_string()) {
        return payload_["sub"].get<std::string_view>();
    }
    return {};
}

std::string_view JwtClaims::audience() const noexcept {
    if (payload_.has("aud")) {
        if (payload_["aud"].is_string()) {
            return payload_["aud"].get<std::string_view>();
        } else if (payload_["aud"].is_array() && payload_["aud"].size() > 0) {
            return payload_["aud"][0].get<std::string_view>();
        }
    }
    return {};
}

std::string_view JwtClaims::jwt_id() const noexcept {
    if (payload_.has("jti") && payload_["jti"].is_string()) {
        return payload_["jti"].get<std::string_view>();
    }
    return {};
}

int64_t JwtClaims::expires_at() const noexcept {
    if (payload_.has("exp") && payload_["exp"].is_number()) {
        return payload_["exp"].get<int64_t>();
    }
    return 0;
}

int64_t JwtClaims::not_before() const noexcept {
    if (payload_.has("nbf") && payload_["nbf"].is_number()) {
        return payload_["nbf"].get<int64_t>();
    }
    return 0;
}

int64_t JwtClaims::issued_at() const noexcept {
    if (payload_.has("iat") && payload_["iat"].is_number()) {
        return payload_["iat"].get<int64_t>();
    }
    return 0;
}

JwtClaims& JwtClaims::set_issuer(std::string_view iss) {
    payload_["iss"] = std::string(iss);
    return *this;
}

JwtClaims& JwtClaims::set_subject(std::string_view sub) {
    payload_["sub"] = std::string(sub);
    return *this;
}

JwtClaims& JwtClaims::set_audience(std::string_view aud) {
    payload_["aud"] = std::string(aud);
    return *this;
}

JwtClaims& JwtClaims::set_jwt_id(std::string_view jti) {
    payload_["jti"] = std::string(jti);
    return *this;
}

JwtClaims& JwtClaims::set_expires_at(std::chrono::system_clock::time_point tp) {
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    return set_expires_at(sec);
}

JwtClaims& JwtClaims::set_expires_at(int64_t epoch_sec) {
    payload_["exp"] = epoch_sec;
    return *this;
}

JwtClaims& JwtClaims::set_expires_in(std::chrono::seconds duration) {
    auto now = std::chrono::system_clock::now();
    return set_expires_at(now + duration);
}

JwtClaims& JwtClaims::set_not_before(std::chrono::system_clock::time_point tp) {
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    return set_not_before(sec);
}

JwtClaims& JwtClaims::set_not_before(int64_t epoch_sec) {
    payload_["nbf"] = epoch_sec;
    return *this;
}

JwtClaims& JwtClaims::set_issued_at(std::chrono::system_clock::time_point tp) {
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    return set_issued_at(sec);
}

JwtClaims& JwtClaims::set_issued_at(int64_t epoch_sec) {
    payload_["iat"] = epoch_sec;
    return *this;
}

Json JwtClaims::operator[](const std::string &key) const {
    return payload_[key];
}

bool JwtClaims::has(const std::string &key) const noexcept {
    return payload_.has(key);
}

std::string JwtClaims::dump() const {
    return payload_.dump();
}

// ---------------- VerifiedJwtClaims ----------------

std::string_view VerifiedJwtClaims::audience() const noexcept {
    if (!auds_.empty()) {
        return auds_[0];
    }
    return {};
}

Json VerifiedJwtClaims::operator[](const std::string &key) const {
    return payload_[key];
}

bool VerifiedJwtClaims::has(const std::string &key) const noexcept {
    return payload_.has(key);
}

std::string VerifiedJwtClaims::dump() const {
    return payload_.dump();
}

JwtClaims VerifiedJwtClaims::to_claims() const {
    return JwtClaims(payload_);
}

// ---------------- JwtSigner ----------------

JwtResult<std::string> JwtSigner::sign_hmac(
    const JwtClaims &claims,
    std::string_view secret,
    JwtAlgorithm alg,
    std::string_view kid) {

    if (secret.empty()) {
        return {JwtError::KeyInvalid};
    }
    // Reject PEM keys to prevent public-key-as-secret confusion
    if (secret.starts_with("-----BEGIN")) {
        return {JwtError::KeyInvalid};
    }
    // Enforce minimum key lengths per RFC 7518 §3.2
    if ((alg == JwtAlgorithm::HS256 && secret.size() < 32) ||
        (alg == JwtAlgorithm::HS384 && secret.size() < 48) ||
        (alg == JwtAlgorithm::HS512 && secret.size() < 64)) {
        return {JwtError::KeyInvalid};
    }
    if (alg != JwtAlgorithm::HS256 && alg != JwtAlgorithm::HS384 && alg != JwtAlgorithm::HS512) {
        return {JwtError::AlgorithmNotAllowed};
    }

    JsonObject header;
    header["alg"] = std::string(to_string(alg));
    header["typ"] = "JWT";
    if (!kid.empty()) {
        header["kid"] = std::string(kid);
    }

    std::string header_b64 = Base64::encode_url(header.dump());
    std::string payload_b64 = Base64::encode_url(claims.dump());
    std::string signing_input = header_b64 + "." + payload_b64;

    std::string sig;
    if (alg == JwtAlgorithm::HS256) {
        sig = CryptoUtil::hmac_sha256_base64url(signing_input, secret);
    } else if (alg == JwtAlgorithm::HS384) {
        sig = CryptoUtil::hmac_sha384_base64url(signing_input, secret);
    } else {
        sig = CryptoUtil::hmac_sha512_base64url(signing_input, secret);
    }

    return {signing_input + "." + sig, JwtError::None};
}

JwtResult<std::string> JwtSigner::sign_pem(
    const JwtClaims &claims,
    std::string_view private_key_pem,
    JwtAlgorithm alg,
    std::string_view kid) {

    if (private_key_pem.empty()) {
        return {JwtError::KeyInvalid};
    }

    BIO *bio = BIO_new_mem_buf(private_key_pem.data(), static_cast<int>(private_key_pem.size()));
    if (!bio) return {JwtError::KeyInvalid};
    UniqueBio u_bio(bio);

    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    if (!pkey) return {JwtError::KeyInvalid};
    UniqueEvpPkey u_pkey(pkey);

    const char *md_name = nullptr;
    if (alg == JwtAlgorithm::RS256) {
        md_name = "SHA256";
    } else if (alg == JwtAlgorithm::RS384) {
        md_name = "SHA384";
    } else if (alg == JwtAlgorithm::RS512) {
        md_name = "SHA512";
    } else if (alg == JwtAlgorithm::Ed25519) {
        md_name = nullptr;
    } else {
        return {JwtError::AlgorithmNotAllowed};
    }

    // Verify key type & size
    if (md_name) {
        if (EVP_PKEY_base_id(pkey) != EVP_PKEY_RSA || EVP_PKEY_bits(pkey) < 2048) {
            return {JwtError::KeyInvalid};
        }
    } else {
        if (EVP_PKEY_base_id(pkey) != EVP_PKEY_ED25519) {
            return {JwtError::KeyInvalid};
        }
    }

    JsonObject header;
    header["alg"] = std::string(to_string(alg));
    header["typ"] = "JWT";
    if (!kid.empty()) {
        header["kid"] = std::string(kid);
    }

    std::string header_b64 = Base64::encode_url(header.dump());
    std::string payload_b64 = Base64::encode_url(claims.dump());
    std::string signing_input = header_b64 + "." + payload_b64;

    const EVP_MD *md = md_name ? EVP_get_digestbyname(md_name) : nullptr;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return {JwtError::KeyInvalid};
    UniqueEvpMdCtx u_ctx(ctx);

    if (EVP_DigestSignInit(ctx, nullptr, md, nullptr, pkey) != 1) {
        return {JwtError::KeyInvalid};
    }

    size_t sig_len = 0;
    if (EVP_DigestSign(ctx, nullptr, &sig_len,
                       reinterpret_cast<const unsigned char *>(signing_input.data()),
                       signing_input.size()) != 1) {
        return {JwtError::KeyInvalid};
    }

    std::vector<unsigned char> sig_bytes(sig_len);
    if (EVP_DigestSign(ctx, sig_bytes.data(), &sig_len,
                       reinterpret_cast<const unsigned char *>(signing_input.data()),
                       signing_input.size()) != 1) {
        return {JwtError::KeyInvalid};
    }
    sig_bytes.resize(sig_len);

    std::string sig_b64 = Base64::encode_url(sig_bytes.data(), sig_bytes.size());
    return {signing_input + "." + sig_b64, JwtError::None};
}

// ---------------- JwtValidator ----------------

JwtValidator& JwtValidator::allow_algorithm(JwtAlgorithm alg) {
    allowed_algs_.push_back(alg);
    return *this;
}

JwtValidator& JwtValidator::allow_algorithms(std::vector<JwtAlgorithm> algs) {
    allowed_algs_ = std::move(algs);
    return *this;
}

JwtValidator& JwtValidator::require_issuer(std::string iss) {
    expected_iss_ = std::move(iss);
    return *this;
}

JwtValidator& JwtValidator::require_audience(std::string aud) {
    expected_aud_ = std::move(aud);
    return *this;
}

JwtValidator& JwtValidator::require_subject(std::string sub) {
    expected_sub_ = std::move(sub);
    return *this;
}

JwtValidator& JwtValidator::set_clock_skew(std::chrono::seconds skew) {
    if (skew < std::chrono::seconds(0)) {
        throw std::invalid_argument("JwtValidator: clock skew must be non-negative");
    }
    clock_skew_ = skew;
    return *this;
}

JwtValidator& JwtValidator::validate_exp(bool enable) {
    check_exp_ = enable;
    return *this;
}

JwtValidator& JwtValidator::require_exp(bool enable) {
    require_exp_ = enable;
    return *this;
}

JwtValidator& JwtValidator::validate_nbf(bool enable) {
    check_nbf_ = enable;
    return *this;
}

JwtValidator& JwtValidator::validate_iat(bool enable) {
    check_iat_ = enable;
    return *this;
}

JwtValidator& JwtValidator::set_clock(std::function<int64_t()> clock_fn) {
    clock_fn_ = std::move(clock_fn);
    return *this;
}

JwtResult<VerifiedJwtClaims> JwtValidator::verify_common(
    std::string_view token,
    bool is_pem_verifier,
    std::function<JwtError(std::string_view signing_input, std::string_view sig_b64, JwtAlgorithm alg)> signature_verifier) const {

    if (token.empty() || token.size() > MAX_JWT_COMPACT_SIZE) {
        return {JwtError::MalformedToken};
    }

    size_t first_dot = token.find('.');
    if (first_dot == std::string_view::npos || first_dot == 0) {
        return {JwtError::MalformedToken};
    }
    size_t second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string_view::npos || second_dot == first_dot + 1) {
        return {JwtError::MalformedToken};
    }
    if (token.find('.', second_dot + 1) != std::string_view::npos) {
        return {JwtError::MalformedToken};
    }

    std::string_view header_b64 = token.substr(0, first_dot);
    std::string_view payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
    std::string_view sig_b64 = token.substr(second_dot + 1);
    std::string_view signing_input = token.substr(0, second_dot);

    if (header_b64.size() > MAX_JWT_HEADER_SIZE || payload_b64.size() > MAX_JWT_PAYLOAD_SIZE) {
        return {JwtError::MalformedToken};
    }

    if (!is_strict_base64url(header_b64) || !is_strict_base64url(payload_b64) ||
        (!sig_b64.empty() && !is_strict_base64url(sig_b64))) {
        return {JwtError::InvalidBase64};
    }

    auto header_decoded = Base64::decode_url(header_b64);
    if (!header_decoded) {
        return {JwtError::InvalidBase64};
    }

    json_value_t *h_val = json_value_parse(header_decoded->c_str());
    if (!h_val) {
        return {JwtError::InvalidJson};
    }
    UniqueJsonVal u_h_val(h_val);

    if (json_value_type(h_val) != JSON_VALUE_OBJECT) {
        return {JwtError::InvalidJson};
    }

    bool has_dup = false;
    if (!check_json_structure(h_val, 1, has_dup)) {
        return {has_dup ? JwtError::DuplicateKey : JwtError::InvalidJson};
    }

    const json_object_t *h_obj = json_value_object(h_val);

    // Reject unsupported critical parameters
    if (json_object_find("crit", h_obj) != nullptr ||
        json_object_find("b64", h_obj) != nullptr ||
        json_object_find("jku", h_obj) != nullptr ||
        json_object_find("jwk", h_obj) != nullptr ||
        json_object_find("x5u", h_obj) != nullptr ||
        json_object_find("x5c", h_obj) != nullptr) {
        return {JwtError::UnsupportedHeaderParameter};
    }

    // Extract algorithm
    const json_value_t *alg_val = json_object_find("alg", h_obj);
    if (!alg_val || json_value_type(alg_val) != JSON_VALUE_STRING) {
        return {JwtError::MissingAlgorithm};
    }
    const char *alg_str = json_value_string(alg_val);
    if (!alg_str || !*alg_str) {
        return {JwtError::MissingAlgorithm};
    }

    if (strcmp(alg_str, "none") == 0 || strcmp(alg_str, "NONE") == 0) {
        return {JwtError::UnsupportedAlgorithm};
    }

    auto alg_opt = parse_jwt_algorithm(alg_str);
    if (!alg_opt) {
        return {JwtError::UnsupportedAlgorithm};
    }
    JwtAlgorithm alg = *alg_opt;

    // Strict Anti-Confusion Whitelist
    if (allowed_algs_.empty()) {
        return {JwtError::AlgorithmNotAllowed};
    }
    if (std::find(allowed_algs_.begin(), allowed_algs_.end(), alg) == allowed_algs_.end()) {
        return {JwtError::AlgorithmNotAllowed};
    }

    // Key family isolation: prevent symmetric/asymmetric key confusion
    bool is_hmac_alg = (alg == JwtAlgorithm::HS256 || alg == JwtAlgorithm::HS384 || alg == JwtAlgorithm::HS512);
    if (is_pem_verifier && is_hmac_alg) {
        return {JwtError::AlgorithmNotAllowed};
    }
    if (!is_pem_verifier && !is_hmac_alg) {
        return {JwtError::AlgorithmNotAllowed};
    }

    // Verify signature before parsing claims payload.
    JwtError sig_err = signature_verifier(signing_input, sig_b64, alg);
    if (sig_err != JwtError::None) {
        return {sig_err};
    }

    auto payload_decoded = Base64::decode_url(payload_b64);
    if (!payload_decoded) {
        return {JwtError::InvalidBase64};
    }

    json_value_t *p_val = json_value_parse(payload_decoded->c_str());
    if (!p_val) {
        return {JwtError::InvalidJson};
    }
    UniqueJsonVal u_p_val(p_val);

    if (json_value_type(p_val) != JSON_VALUE_OBJECT) {
        return {JwtError::InvalidJson};
    }

    has_dup = false;
    if (!check_json_structure(p_val, 1, has_dup)) {
        return {has_dup ? JwtError::DuplicateKey : JwtError::InvalidJson};
    }

    const json_object_t *p_obj = json_value_object(p_val);

    int64_t exp = 0;
    bool has_exp = false;
    const json_value_t *exp_val = json_object_find("exp", p_obj);
    if (exp_val) {
        if (!parse_numeric_date(exp_val, exp)) {
            return {JwtError::ClaimTypeMismatch};
        }
        has_exp = true;
    } else if (require_exp_) {
        return {JwtError::MissingRequiredClaim};
    }

    int64_t nbf = 0;
    bool has_nbf = false;
    const json_value_t *nbf_val = json_object_find("nbf", p_obj);
    if (nbf_val) {
        if (!parse_numeric_date(nbf_val, nbf)) {
            return {JwtError::ClaimTypeMismatch};
        }
        has_nbf = true;
    }

    int64_t iat = 0;
    bool has_iat = false;
    const json_value_t *iat_val = json_object_find("iat", p_obj);
    if (iat_val) {
        if (!parse_numeric_date(iat_val, iat)) {
            return {JwtError::ClaimTypeMismatch};
        }
        has_iat = true;
    }

    // iss
    std::string iss;
    const json_value_t *iss_val = json_object_find("iss", p_obj);
    if (iss_val) {
        if (json_value_type(iss_val) != JSON_VALUE_STRING) {
            return {JwtError::ClaimTypeMismatch};
        }
        iss = json_value_string(iss_val);
    }
    if (expected_iss_) {
        if (!iss_val) return {JwtError::MissingRequiredClaim};
        if (iss != *expected_iss_) return {JwtError::IssuerMismatch};
    }

    // sub
    std::string sub;
    const json_value_t *sub_val = json_object_find("sub", p_obj);
    if (sub_val) {
        if (json_value_type(sub_val) != JSON_VALUE_STRING) {
            return {JwtError::ClaimTypeMismatch};
        }
        sub = json_value_string(sub_val);
    }
    if (expected_sub_) {
        if (!sub_val) return {JwtError::MissingRequiredClaim};
        if (sub != *expected_sub_) return {JwtError::SubjectMismatch};
    }

    // jti
    std::string jti;
    const json_value_t *jti_val = json_object_find("jti", p_obj);
    if (jti_val) {
        if (json_value_type(jti_val) != JSON_VALUE_STRING) {
            return {JwtError::ClaimTypeMismatch};
        }
        jti = json_value_string(jti_val);
    }

    // aud
    std::vector<std::string> auds;
    const json_value_t *aud_val = json_object_find("aud", p_obj);
    if (aud_val) {
        int aud_type = json_value_type(aud_val);
        if (aud_type == JSON_VALUE_STRING) {
            auds.push_back(json_value_string(aud_val));
        } else if (aud_type == JSON_VALUE_ARRAY) {
            const json_array_t *arr = json_value_array(aud_val);
            if (!arr || json_array_size(arr) == 0) {
                return {JwtError::ClaimTypeMismatch};
            }
            const json_value_t *elem = nullptr;
            json_array_for_each(elem, arr) {
                if (json_value_type(elem) != JSON_VALUE_STRING) {
                    return {JwtError::ClaimTypeMismatch};
                }
                auds.push_back(json_value_string(elem));
            }
        } else {
            return {JwtError::ClaimTypeMismatch};
        }
    }
    if (expected_aud_) {
        if (!aud_val) return {JwtError::MissingRequiredClaim};
        bool matched = false;
        for (const auto &a : auds) {
            if (a == *expected_aud_) {
                matched = true;
                break;
            }
        }
        if (!matched) return {JwtError::AudienceMismatch};
    }

    // Validate time-based claims with clock skew leeway.
    int64_t now = clock_fn_ ? clock_fn_() :
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t leeway = clock_skew_.count();

    if (has_exp && check_exp_ && is_expired(now, exp, leeway)) {
        return {JwtError::TokenExpired};
    }
    if (has_nbf && check_nbf_ && is_not_yet_valid(now, nbf, leeway)) {
        return {JwtError::TokenNotYetValid};
    }
    if (has_iat && check_iat_ && is_issued_in_future(now, iat, leeway)) {
        return {JwtError::TokenIssuedInFuture};
    }

    VerifiedJwtClaims verified(
        Json::parse(*payload_decoded),
        std::move(iss),
        std::move(sub),
        std::move(auds),
        std::move(jti),
        exp, has_exp,
        nbf, has_nbf,
        iat, has_iat);

    return {std::move(verified), JwtError::None};
}

JwtResult<VerifiedJwtClaims> JwtValidator::verify_hmac(
    std::string_view token,
    std::string_view secret) const {

    if (secret.empty() || secret.size() < 32 || secret.starts_with("-----BEGIN")) {
        return {JwtError::KeyInvalid};
    }

    return verify_common(token, false, [secret](std::string_view signing_input, std::string_view sig_b64, JwtAlgorithm alg) -> JwtError {
        // Enforce minimum key lengths per RFC 7518 §3.2
        if (alg == JwtAlgorithm::HS256) {
            if (secret.size() < 32) return JwtError::KeyInvalid;
            if (sig_b64.size() != 43) return JwtError::SignatureMismatch;
            std::string expected_sig = CryptoUtil::hmac_sha256_base64url(signing_input, secret);
            return CryptoUtil::constant_time_equals(sig_b64, expected_sig) ? JwtError::None : JwtError::SignatureMismatch;
        } else if (alg == JwtAlgorithm::HS384) {
            if (secret.size() < 48) return JwtError::KeyInvalid;
            if (sig_b64.size() != 64) return JwtError::SignatureMismatch;
            std::string expected_sig = CryptoUtil::hmac_sha384_base64url(signing_input, secret);
            return CryptoUtil::constant_time_equals(sig_b64, expected_sig) ? JwtError::None : JwtError::SignatureMismatch;
        } else if (alg == JwtAlgorithm::HS512) {
            if (secret.size() < 64) return JwtError::KeyInvalid;
            if (sig_b64.size() != 86) return JwtError::SignatureMismatch;
            std::string expected_sig = CryptoUtil::hmac_sha512_base64url(signing_input, secret);
            return CryptoUtil::constant_time_equals(sig_b64, expected_sig) ? JwtError::None : JwtError::SignatureMismatch;
        }
        return JwtError::AlgorithmNotAllowed;
    });
}

JwtResult<VerifiedJwtClaims> JwtValidator::verify_pem(
    std::string_view token,
    std::string_view public_key_pem) const {

    if (public_key_pem.empty()) {
        return {JwtError::KeyInvalid};
    }

    return verify_common(token, true, [public_key_pem](std::string_view signing_input, std::string_view sig_b64, JwtAlgorithm alg) -> JwtError {
        BIO *bio = BIO_new_mem_buf(public_key_pem.data(), static_cast<int>(public_key_pem.size()));
        if (!bio) return JwtError::KeyInvalid;
        UniqueBio u_bio(bio);

        EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        if (!pkey) return JwtError::KeyInvalid;
        UniqueEvpPkey u_pkey(pkey);

        const char *md_name = nullptr;
        if (alg == JwtAlgorithm::RS256) {
            md_name = "SHA256";
        } else if (alg == JwtAlgorithm::RS384) {
            md_name = "SHA384";
        } else if (alg == JwtAlgorithm::RS512) {
            md_name = "SHA512";
        } else if (alg == JwtAlgorithm::Ed25519) {
            md_name = nullptr;
        } else {
            return JwtError::AlgorithmNotAllowed;
        }

        // Verify key family & size
        if (md_name) {
            if (EVP_PKEY_base_id(pkey) != EVP_PKEY_RSA || EVP_PKEY_bits(pkey) < 2048) {
                return JwtError::KeyInvalid;
            }
        } else {
            if (EVP_PKEY_base_id(pkey) != EVP_PKEY_ED25519) {
                return JwtError::KeyInvalid;
            }
        }

        // Bounded signature length check before crypto
        if (alg == JwtAlgorithm::Ed25519) {
            if (sig_b64.size() != 86) return JwtError::SignatureMismatch;
        }

        auto raw_sig = Base64::decode_url(sig_b64);
        if (!raw_sig) return JwtError::SignatureMismatch;

        if (md_name) {
            int key_size = EVP_PKEY_size(pkey);
            if (key_size <= 0 || static_cast<size_t>(key_size) != raw_sig->size()) {
                return JwtError::SignatureMismatch;
            }
        } else {
            if (raw_sig->size() != 64) return JwtError::SignatureMismatch;
        }

        const EVP_MD *md = md_name ? EVP_get_digestbyname(md_name) : nullptr;
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx) return JwtError::KeyInvalid;
        UniqueEvpMdCtx u_ctx(ctx);

        if (EVP_DigestVerifyInit(ctx, nullptr, md, nullptr, pkey) != 1) {
            return JwtError::KeyInvalid;
        }

        int ret = EVP_DigestVerify(ctx,
                                   reinterpret_cast<const unsigned char *>(raw_sig->data()),
                                   raw_sig->size(),
                                   reinterpret_cast<const unsigned char *>(signing_input.data()),
                                   signing_input.size());
        return ret == 1 ? JwtError::None : JwtError::SignatureMismatch;
    });
}

// ---------------- BearerAuth Middleware ----------------

const VerifiedJwtClaims* get_jwt_claims(const Context &ctx) noexcept {
    return ctx.get_attachment<VerifiedJwtClaims>();
}

Middleware bearer_auth(BearerAuthConfig config) {
    if (config.secret_or_public_key.empty()) {
        throw std::invalid_argument("BearerAuth: secret or public key must not be empty");
    }
    if (config.header_name.empty()) {
        throw std::invalid_argument("BearerAuth: header_name must not be empty");
    }
    if (config.header_prefix.empty()) {
        throw std::invalid_argument("BearerAuth: header_prefix must not be empty");
    }

    return [cfg = std::move(config)](Context &ctx, Next next) -> coke::Task<void> {
        // Skip paths
        for (const auto &skip : cfg.skip_paths) {
            if (ctx.path() == skip) {
                co_await next();
                co_return;
            }
        }

        // Enumerate headers with cursor to detect duplicate Authorization headers
        int auth_header_count = 0;
        std::string raw_auth_header;
        protocol::HttpHeaderCursor cursor(&ctx.raw_req());
        std::string hname, hval;
        while (cursor.next(hname, hval)) {
            if (case_insensitive_equal(hname, cfg.header_name)) {
                auth_header_count++;
                if (auth_header_count == 1) {
                    raw_auth_header = hval;
                }
            }
        }

        auto handle_unauthorized = [&](JwtError err) {
            if (cfg.on_unauthorized) {
                ctx.set_status(HttpStatus::Unauthorized);
                ctx.set_header("WWW-Authenticate", "Bearer error=\"invalid_token\"");
                cfg.on_unauthorized(ctx, err);
            } else {
                auto http_err = HttpError::unauthorized("Unauthorized", "INVALID_TOKEN", cfg.realm);
                if (cfg.realm.empty()) {
                    http_err.with_header("WWW-Authenticate", "Bearer error=\"invalid_token\"");
                } else {
                    std::string auth_val = "Bearer realm=\"";
                    for (char c : cfg.realm) {
                        if (c == '"' || c == '\\') auth_val.push_back('\\');
                        auth_val.push_back(c);
                    }
                    auth_val += "\", error=\"invalid_token\"";
                    http_err.with_header("WWW-Authenticate", std::move(auth_val));
                }
                reply_error(ctx, http_err);
            }
        };

        if (auth_header_count != 1 || raw_auth_header.empty()) {
            handle_unauthorized(JwtError::MalformedToken);
            co_return;
        }

        std::string_view prefix = cfg.header_prefix;
        if (raw_auth_header.size() <= prefix.size()) {
            handle_unauthorized(JwtError::MalformedToken);
            co_return;
        }

        std::string_view actual_prefix = std::string_view(raw_auth_header).substr(0, prefix.size());
        if (!case_insensitive_equal(actual_prefix, prefix)) {
            handle_unauthorized(JwtError::MalformedToken);
            co_return;
        }

        std::string_view token = std::string_view(raw_auth_header).substr(prefix.size());
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);

        if (token.empty() || token.find(',') != std::string_view::npos || token.find(' ') != std::string_view::npos) {
            handle_unauthorized(JwtError::MalformedToken);
            co_return;
        }

        JwtResult<VerifiedJwtClaims> res;
        if (cfg.is_pem_key) {
            res = cfg.validator.verify_pem(token, cfg.secret_or_public_key);
        } else {
            res = cfg.validator.verify_hmac(token, cfg.secret_or_public_key);
        }

        if (!res) {
            handle_unauthorized(res.error);
            co_return;
        }

        // Attach verified claims
        ctx.set_attachment<VerifiedJwtClaims>(std::move(*res.value));

        // Downstream coroutine is invoked outside auth error handling
        co_await next();
    };
}

} // namespace kale