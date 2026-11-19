#include "doctest/doctest.h"

#include "coke/http/http_client.h"
#include "coke/wait.h"
#include "kale/core/http_error.h"
#include "kale/core/http_server.h"
#include "kale/core/http_util.h"
#include "kale/middleware/access_log.h"
#include "kale/core/error_response.h"
#include "kale/middleware/error_handler.h"
#include "kale/security/jwt.h"
#include "http/base64.h"
#include "security/crypto_util.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <charconv>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace {

struct KeyPair {
    std::string private_key_pem;
    std::string public_key_pem;
};

KeyPair generate_ed25519_keypair() {
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY *pkey = nullptr;
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);

    BIO *priv_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(priv_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    BUF_MEM *priv_bptr = nullptr;
    BIO_get_mem_ptr(priv_bio, &priv_bptr);
    std::string priv_str(priv_bptr->data, priv_bptr->length);
    BIO_free(priv_bio);

    BIO *pub_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(pub_bio, pkey);
    BUF_MEM *pub_bptr = nullptr;
    BIO_get_mem_ptr(pub_bio, &pub_bptr);
    std::string pub_str(pub_bptr->data, pub_bptr->length);
    BIO_free(pub_bio);

    EVP_PKEY_free(pkey);
    return {priv_str, pub_str};
}

KeyPair generate_rsa_keypair(int bits = 2048) {
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, bits);
    EVP_PKEY *pkey = nullptr;
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);

    BIO *priv_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(priv_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    BUF_MEM *priv_bptr = nullptr;
    BIO_get_mem_ptr(priv_bio, &priv_bptr);
    std::string priv_str(priv_bptr->data, priv_bptr->length);
    BIO_free(priv_bio);

    BIO *pub_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(pub_bio, pkey);
    BUF_MEM *pub_bptr = nullptr;
    BIO_get_mem_ptr(pub_bio, &pub_bptr);
    std::string pub_str(pub_bptr->data, pub_bptr->length);
    BIO_free(pub_bio);

    EVP_PKEY_free(pkey);
    return {priv_str, pub_str};
}

KeyPair generate_ec_keypair(const char *curve_name = "P-256") {
    EVP_PKEY *pkey = EVP_EC_gen(curve_name);
    REQUIRE(pkey != nullptr);

    BIO *priv_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(priv_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    BUF_MEM *priv_bptr = nullptr;
    BIO_get_mem_ptr(priv_bio, &priv_bptr);
    std::string priv_str(priv_bptr->data, priv_bptr->length);
    BIO_free(priv_bio);

    BIO *pub_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(pub_bio, pkey);
    BUF_MEM *pub_bptr = nullptr;
    BIO_get_mem_ptr(pub_bio, &pub_bptr);
    std::string pub_str(pub_bptr->data, pub_bptr->length);
    BIO_free(pub_bio);

    EVP_PKEY_free(pkey);
    return {priv_str, pub_str};
}

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

} // namespace

TEST_SUITE("jwt") {

TEST_CASE("JwtClaims manipulation and JSON serialization") {
    kale::JwtClaims claims;
    claims.set_issuer("kale-auth-server")
          .set_subject("user_42")
          .set_audience("kale-api")
          .set_jwt_id("jti-10086")
          .set_expires_in(std::chrono::seconds(3600))
          .set_not_before(1700000000)
          .set_issued_at(1700000000)
          .set("role", "admin")
          .set("dept_id", 101);

    CHECK_EQ(claims.issuer(), "kale-auth-server");
    CHECK_EQ(claims.subject(), "user_42");
    CHECK_EQ(claims.audience(), "kale-api");
    CHECK_EQ(claims.jwt_id(), "jti-10086");
    CHECK_GT(claims.expires_at(), 0);
    CHECK_EQ(claims.not_before(), 1700000000);
    CHECK_EQ(claims.issued_at(), 1700000000);
    CHECK(claims.has("role"));
    CHECK_EQ(claims["role"].get<std::string>(), "admin");
    CHECK_EQ(claims["dept_id"].get<int>(), 101);

    std::string dumped = claims.dump();
    CHECK_NE(dumped.find("kale-auth-server"), std::string::npos);
    CHECK_NE(dumped.find("user_42"), std::string::npos);
}

TEST_CASE("JWA symmetric HMAC matrix (HS256, HS384, HS512)") {
    // Secrets >= 64 bytes to satisfy HS256/384/512 minimum key lengths
    std::string secret = "kale_super_secret_key_1234567890_abcdefghijklmnopqrstuvwxyz_0123456789";

    kale::JwtClaims claims;
    claims.set_issuer("kale-test")
          .set_subject("alice")
          .set_expires_at(1800000000);

    const std::vector<kale::JwtAlgorithm> algs = {
        kale::JwtAlgorithm::HS256,
        kale::JwtAlgorithm::HS384,
        kale::JwtAlgorithm::HS512
    };

    for (auto alg : algs) {
        auto sign_res = kale::JwtSigner::sign_hmac(claims, secret, alg, "test-kid");
        REQUIRE(sign_res.has_value());
        std::string token = *sign_res;

        kale::JwtValidator validator;
        validator.allow_algorithm(alg)
                 .require_issuer("kale-test")
                 .require_subject("alice")
                 .set_clock([]() -> int64_t { return 1750000000; }); // Injected clock before exp

        auto verify_res = validator.verify_hmac(token, secret);
        REQUIRE(verify_res.has_value());
        CHECK_EQ(verify_res->issuer(), "kale-test");
        CHECK_EQ(verify_res->subject(), "alice");
        CHECK_EQ(verify_res->expires_at(), 1800000000);
    }
}

TEST_CASE("JWA asymmetric RSA matrix (RS256, RS384, RS512)") {
    auto rsa_keys = generate_rsa_keypair(2048);

    kale::JwtClaims claims;
    claims.set_issuer("rsa-issuer")
          .set_subject("bob")
          .set_expires_at(1800000000);

    const std::vector<kale::JwtAlgorithm> algs = {
        kale::JwtAlgorithm::RS256,
        kale::JwtAlgorithm::RS384,
        kale::JwtAlgorithm::RS512
    };

    for (auto alg : algs) {
        auto sign_res = kale::JwtSigner::sign_pem(claims, rsa_keys.private_key_pem, alg);
        REQUIRE(sign_res.has_value());
        std::string token = *sign_res;

        kale::JwtValidator validator;
        validator.allow_algorithm(alg)
                 .require_issuer("rsa-issuer")
                 .require_subject("bob")
                 .set_clock([]() -> int64_t { return 1750000000; });

        auto verify_res = validator.verify_pem(token, rsa_keys.public_key_pem);
        REQUIRE(verify_res.has_value());
        CHECK_EQ(verify_res->issuer(), "rsa-issuer");
        CHECK_EQ(verify_res->subject(), "bob");
    }
}

TEST_CASE("JWA asymmetric Ed25519 canonical enum and EdDSA wire format") {
    auto ed_keys = generate_ed25519_keypair();

    kale::JwtClaims claims;
    claims.set_issuer("ed-issuer")
          .set_subject("carol")
          .set_expires_at(1800000000);

    // 1. Sign using canonical C++ algorithm name Ed25519
    auto sign_res = kale::JwtSigner::sign_pem(claims, ed_keys.private_key_pem, kale::JwtAlgorithm::Ed25519);
    REQUIRE(sign_res.has_value());
    std::string token = *sign_res;

    kale::JwtValidator validator;
    validator.allow_algorithm(kale::JwtAlgorithm::Ed25519)
             .require_issuer("ed-issuer")
             .require_subject("carol")
             .set_clock([]() -> int64_t { return 1750000000; });

    auto verify_res = validator.verify_pem(token, ed_keys.public_key_pem);
    REQUIRE(verify_res.has_value());
    CHECK_EQ(verify_res->issuer(), "ed-issuer");
    CHECK_EQ(verify_res->subject(), "carol");

    // 2. Wire format: token carrying wire spelling "EdDSA" is recognized as Ed25519
    {
        std::string eddsa_header = kale::Base64::encode_url("{\"alg\":\"EdDSA\",\"typ\":\"JWT\"}");
        std::string payload_b64 = kale::Base64::encode_url(claims.dump());
        std::string signing_input = eddsa_header + "." + payload_b64;

        BIO *bio = BIO_new_mem_buf(ed_keys.private_key_pem.data(), static_cast<int>(ed_keys.private_key_pem.size()));
        REQUIRE(bio != nullptr);
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        REQUIRE(pkey != nullptr);

        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        REQUIRE_EQ(EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey), 1);
        size_t sig_len = 0;
        REQUIRE_EQ(EVP_DigestSign(ctx, nullptr, &sig_len, reinterpret_cast<const unsigned char*>(signing_input.data()), signing_input.size()), 1);
        std::vector<unsigned char> sig_bytes(sig_len);
        REQUIRE_EQ(EVP_DigestSign(ctx, sig_bytes.data(), &sig_len, reinterpret_cast<const unsigned char*>(signing_input.data()), signing_input.size()), 1);
        sig_bytes.resize(sig_len);
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);

        std::string eddsa_wire_token = signing_input + "." + kale::Base64::encode_url(sig_bytes.data(), sig_bytes.size());
        auto wire_verify = validator.verify_pem(eddsa_wire_token, ed_keys.public_key_pem);
        REQUIRE(wire_verify.has_value());
        CHECK_EQ(wire_verify->issuer(), "ed-issuer");
    }
}

TEST_CASE("JwtResult stores no verified claims on failure (exclusive validator provenance)") {
    // Ensure failed validation produces nullopt value with no default-constructed claims
    kale::JwtValidator v;
    v.allow_algorithm(kale::JwtAlgorithm::HS256);
    auto res = v.verify_hmac("malformed.token", "dummy_secret_at_least_32_bytes_long_key!");
    CHECK_FALSE(res.has_value());
    CHECK_FALSE(static_cast<bool>(res));
    CHECK_FALSE(res.value.has_value());
    CHECK_EQ(res.error, kale::JwtError::MalformedToken);
    static_assert(!std::is_default_constructible_v<kale::VerifiedJwtClaims>,
                  "VerifiedJwtClaims must not be publicly default-constructible");
}

TEST_CASE("Strict Anti-Confusion and Key Invariants") {
    std::string secret = "kale_super_secret_key_1234567890_abcdefghijklmnopqrstuvwxyz_0123456789";
    kale::JwtClaims claims;
    claims.set_issuer("iss").set_expires_at(1800000000);

    auto token_res = kale::JwtSigner::sign_hmac(claims, secret, kale::JwtAlgorithm::HS256);
    REQUIRE(token_res.has_value());
    std::string token = *token_res;

    // 1. Empty allowlist rejects all
    {
        kale::JwtValidator v;
        v.set_clock([]() -> int64_t { return 1750000000; });
        auto res = v.verify_hmac(token, secret);
        CHECK_FALSE(res.has_value());
        CHECK_EQ(res.error, kale::JwtError::AlgorithmNotAllowed);
    }

    // 2. Algorithm not in allowlist
    {
        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS512); // Only HS512 allowed, token is HS256
        v.set_clock([]() -> int64_t { return 1750000000; });
        auto res = v.verify_hmac(token, secret);
        CHECK_FALSE(res.has_value());
        CHECK_EQ(res.error, kale::JwtError::AlgorithmNotAllowed);
    }

    // 3. 'none' algorithm rejection
    {
        std::string none_token = "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.eyJpc3MiOiJpc3MiLCJleHAiOjE4MDAwMDAwMDB9.";
        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        auto res = v.verify_hmac(none_token, secret);
        CHECK_FALSE(res.has_value());
        CHECK_EQ(res.error, kale::JwtError::UnsupportedAlgorithm);
    }

    // 4. Public-key-as-HMAC-secret key confusion defense
    {
        auto rsa_keys = generate_rsa_keypair(2048);
        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        // Using RSA public key as HMAC secret must be rejected immediately
        auto res = v.verify_hmac(token, rsa_keys.public_key_pem);
        CHECK_FALSE(res.has_value());
        CHECK_EQ(res.error, kale::JwtError::KeyInvalid);
    }

    // 5. Symmetric token verified with verify_pem (fixed key family)
    {
        auto rsa_keys = generate_rsa_keypair(2048);
        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        auto res = v.verify_pem(token, rsa_keys.public_key_pem);
        CHECK_FALSE(res.has_value());
        CHECK_EQ(res.error, kale::JwtError::AlgorithmNotAllowed);
    }

    // 6. Asymmetric token verified with verify_hmac
    {
        auto rsa_keys = generate_rsa_keypair(2048);
        auto rsa_token = kale::JwtSigner::sign_pem(claims, rsa_keys.private_key_pem, kale::JwtAlgorithm::RS256);
        REQUIRE(rsa_token.has_value());

        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::RS256);
        auto res = v.verify_hmac(*rsa_token, secret);
        CHECK_FALSE(res.has_value());
        CHECK_EQ(res.error, kale::JwtError::AlgorithmNotAllowed);
    }

    // 7. Short HMAC secret (< 32 bytes) rejected
    {
        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        auto res = v.verify_hmac(token, "short_secret_under_32_bytes");
        CHECK_FALSE(res.has_value());
        CHECK_EQ(res.error, kale::JwtError::KeyInvalid);
    }

    // 8. Tampered payload
    {
        // Alter payload base64url
        size_t dot1 = token.find('.');
        size_t dot2 = token.find('.', dot1 + 1);
        std::string tampered = token.substr(0, dot1 + 1) + "eyJpc3MiOiJldmlsIn0" + token.substr(dot2);
        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        v.set_clock([]() -> int64_t { return 1750000000; });
        auto res = v.verify_hmac(tampered, secret);
        CHECK_FALSE(res.has_value());
        CHECK_EQ(res.error, kale::JwtError::SignatureMismatch);
    }

    // 9. Tampered signature
    {
        std::string tampered = token;
        tampered.back() = (tampered.back() == 'A' ? 'B' : 'A');
        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        v.set_clock([]() -> int64_t { return 1750000000; });
        auto res = v.verify_hmac(tampered, secret);
        CHECK_FALSE(res.has_value());
        CHECK_EQ(res.error, kale::JwtError::SignatureMismatch);
    }

    // 10. Corrupted signature length (bounded signature check)
    {
        std::string tampered = token + "xyz";
        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        v.set_clock([]() -> int64_t { return 1750000000; });
        auto res = v.verify_hmac(tampered, secret);
        CHECK_FALSE(res.has_value());
        CHECK_EQ(res.error, kale::JwtError::SignatureMismatch);
    }
}

TEST_CASE("Bounded decoding and structural validation") {
    std::string secret = "kale_super_secret_key_1234567890_abcdefghijklmnopqrstuvwxyz_0123456789";

    // 1. Oversized compact token (> 16 KiB)
    {
        std::string huge_token(16385, 'a');
        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        auto res = v.verify_hmac(huge_token, secret);
        CHECK_EQ(res.error, kale::JwtError::MalformedToken);
    }

    // 2. Illegal padding '=' in compact token
    {
        // Valid header and payload but padded with '='
        std::string padded = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9==.eyJpc3MiOiJpc3MifQ==.c2lnbmF0dXJl";
        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        auto res = v.verify_hmac(padded, secret);
        CHECK_EQ(res.error, kale::JwtError::InvalidBase64);
    }

    // 3. Duplicate key in header
    {
        // Header with duplicate "alg"
        std::string dup_header_json = "{\"alg\":\"HS256\",\"typ\":\"JWT\",\"alg\":\"HS256\"}";
        std::string dup_header_b64 = kale::Base64::encode_url(dup_header_json);
        std::string payload_b64 = kale::Base64::encode_url("{\"iss\":\"test\"}");
        std::string signing_input = dup_header_b64 + "." + payload_b64;
        std::string sig = kale::CryptoUtil::hmac_sha256_base64url(signing_input, secret);
        std::string token = signing_input + "." + sig;

        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        auto res = v.verify_hmac(token, secret);
        CHECK_EQ(res.error, kale::JwtError::DuplicateKey);
    }

    // 4. Duplicate key in payload
    {
        std::string header_b64 = kale::Base64::encode_url("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
        std::string dup_payload_json = "{\"iss\":\"test\",\"sub\":\"1\",\"iss\":\"test2\"}";
        std::string dup_payload_b64 = kale::Base64::encode_url(dup_payload_json);
        std::string signing_input = header_b64 + "." + dup_payload_b64;
        std::string sig = kale::CryptoUtil::hmac_sha256_base64url(signing_input, secret);
        std::string token = signing_input + "." + sig;

        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        auto res = v.verify_hmac(token, secret);
        CHECK_EQ(res.error, kale::JwtError::DuplicateKey);
    }

    // 5. Unsupported critical header parameters (e.g. "crit")
    {
        std::string crit_header = "{\"alg\":\"HS256\",\"typ\":\"JWT\",\"crit\":[\"custom\"]}";
        std::string crit_header_b64 = kale::Base64::encode_url(crit_header);
        std::string payload_b64 = kale::Base64::encode_url("{\"iss\":\"test\"}");
        std::string signing_input = crit_header_b64 + "." + payload_b64;
        std::string sig = kale::CryptoUtil::hmac_sha256_base64url(signing_input, secret);
        std::string token = signing_input + "." + sig;

        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        auto res = v.verify_hmac(token, secret);
        CHECK_EQ(res.error, kale::JwtError::UnsupportedHeaderParameter);
    }
}

TEST_CASE("NumericDate overflow safety, clock injection, and date boundaries") {
    std::string secret = "kale_super_secret_key_1234567890_abcdefghijklmnopqrstuvwxyz_0123456789";

    // 1. Non-numeric or non-integral exp
    {
        std::string header_b64 = kale::Base64::encode_url("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
        std::string bad_exp_payload = kale::Base64::encode_url("{\"exp\":\"not_a_number\"}");
        std::string signing_input = header_b64 + "." + bad_exp_payload;
        std::string sig = kale::CryptoUtil::hmac_sha256_base64url(signing_input, secret);
        std::string token = signing_input + "." + sig;

        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        auto res = v.verify_hmac(token, secret);
        CHECK_EQ(res.error, kale::JwtError::ClaimTypeMismatch);
    }

    // 2. Float non-integer exp (e.g. 1700000000.5)
    {
        std::string header_b64 = kale::Base64::encode_url("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
        std::string float_exp_payload = kale::Base64::encode_url("{\"exp\":1700000000.5}");
        std::string signing_input = header_b64 + "." + float_exp_payload;
        std::string sig = kale::CryptoUtil::hmac_sha256_base64url(signing_input, secret);
        std::string token = signing_input + "." + sig;

        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256);
        auto res = v.verify_hmac(token, secret);
        CHECK_EQ(res.error, kale::JwtError::ClaimTypeMismatch);
    }

    // 3. Expiration boundary with injected clock and clock skew
    {
        kale::JwtClaims claims;
        claims.set_expires_at(1000); // Exp = 1000

        auto token_res = kale::JwtSigner::sign_hmac(claims, secret, kale::JwtAlgorithm::HS256);
        REQUIRE(token_res.has_value());
        std::string token = *token_res;

        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256)
         .set_clock_skew(std::chrono::seconds(30)); // Leeway = 30s

        // Now = 1020 (within 30s leeway of exp 1000) -> Valid!
        v.set_clock([]() -> int64_t { return 1020; });
        auto res1 = v.verify_hmac(token, secret);
        CHECK(res1.has_value());

        // Now = 1030 (exactly at exp + leeway) -> Expired!
        v.set_clock([]() -> int64_t { return 1030; });
        auto res2 = v.verify_hmac(token, secret);
        CHECK_EQ(res2.error, kale::JwtError::TokenExpired);

        // Now = 1050 (past exp + leeway) -> Expired!
        v.set_clock([]() -> int64_t { return 1050; });
        auto res3 = v.verify_hmac(token, secret);
        CHECK_EQ(res3.error, kale::JwtError::TokenExpired);
    }

    // 4. Not-before boundary (nbf)
    {
        kale::JwtClaims claims;
        claims.set_expires_at(2000)
              .set_not_before(1000);

        auto token_res = kale::JwtSigner::sign_hmac(claims, secret, kale::JwtAlgorithm::HS256);
        REQUIRE(token_res.has_value());
        std::string token = *token_res;

        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256)
         .set_clock_skew(std::chrono::seconds(30));

        // Now = 960 (outside 30s leeway before nbf 1000) -> TokenNotYetValid
        v.set_clock([]() -> int64_t { return 960; });
        auto res1 = v.verify_hmac(token, secret);
        CHECK_EQ(res1.error, kale::JwtError::TokenNotYetValid);

        // Now = 980 (within 30s leeway before nbf 1000) -> Valid!
        v.set_clock([]() -> int64_t { return 980; });
        auto res2 = v.verify_hmac(token, secret);
        CHECK(res2.has_value());
    }

    // 5. Overflow safety with large timestamp
    {
        kale::JwtClaims claims;
        claims.set_expires_at(9007199254740990LL); // Safe max double integer

        auto token_res = kale::JwtSigner::sign_hmac(claims, secret, kale::JwtAlgorithm::HS256);
        REQUIRE(token_res.has_value());
        std::string token = *token_res;

        kale::JwtValidator v;
        v.allow_algorithm(kale::JwtAlgorithm::HS256)
         .set_clock_skew(std::chrono::seconds(3600));

        // Now = 1000 -> Valid, no integer overflow!
        v.set_clock([]() -> int64_t { return 1000; });
        auto res = v.verify_hmac(token, secret);
        CHECK(res.has_value());
    }
}

TEST_CASE("Audience array membership and string claims") {
    std::string secret = "kale_super_secret_key_1234567890_abcdefghijklmnopqrstuvwxyz_0123456789";

    // Array of audiences: ["api-v1", "api-v2"]
    std::string header_b64 = kale::Base64::encode_url("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
    std::string payload_json = "{\"iss\":\"auth\",\"sub\":\"user1\",\"aud\":[\"api-v1\",\"api-v2\"],\"exp\":1800000000}";
    std::string payload_b64 = kale::Base64::encode_url(payload_json);
    std::string signing_input = header_b64 + "." + payload_b64;
    std::string sig = kale::CryptoUtil::hmac_sha256_base64url(signing_input, secret);
    std::string token = signing_input + "." + sig;

    kale::JwtValidator v;
    v.allow_algorithm(kale::JwtAlgorithm::HS256)
     .require_issuer("auth")
     .require_subject("user1")
     .require_audience("api-v2") // Matches element of array
     .set_clock([]() -> int64_t { return 1750000000; });

    auto res = v.verify_hmac(token, secret);
    REQUIRE(res.has_value());
    CHECK_EQ(res->audiences().size(), 2);
    CHECK_EQ(res->audiences()[0], "api-v1");
    CHECK_EQ(res->audiences()[1], "api-v2");

    // Requiring an audience not in the array
    v.require_audience("api-v3");
    auto res_mismatch = v.verify_hmac(token, secret);
    CHECK_EQ(res_mismatch.error, kale::JwtError::AudienceMismatch);
}

TEST_CASE("BearerAuth HTTP Server E2E") {
    std::string secret = "kale_super_secret_key_1234567890_abcdefghijklmnopqrstuvwxyz_0123456789";

    kale::JwtClaims claims;
    claims.set_issuer("kale-server")
          .set_subject("bob_42")
          .set_expires_at(1800000000);

    auto token_res = kale::JwtSigner::sign_hmac(claims, secret, kale::JwtAlgorithm::HS256);
    REQUIRE(token_res.has_value());
    std::string valid_token = *token_res;

    kale::BearerAuthConfig auth_cfg;
    auth_cfg.secret_or_public_key = secret;
    auth_cfg.is_pem_key = false;
    auth_cfg.skip_paths = {"/public"};
    auth_cfg.validator.allow_algorithm(kale::JwtAlgorithm::HS256)
                      .require_issuer("kale-server")
                      .set_clock([]() -> int64_t { return 1750000000; });

    TestServerRunner srv;
    srv->use(kale::bearer_auth(std::move(auth_cfg)));

    srv->GET("/public", [](kale::Context &ctx) {
        ctx.string("public content");
    });

    srv->GET("/protected", [](kale::Context &ctx) {
        const auto *claims = kale::get_jwt_claims(ctx);
        if (claims) {
            ctx.string("hello " + std::string(claims->subject()));
        } else {
            ctx.set_status(500).string("claims missing");
        }
    });

    srv->GET("/fail", [](kale::Context &) -> coke::Task<void> {
        throw kale::HttpError(418, "I'm a teapot");
        co_return;
    });

    srv.start();

    // 1. Skip path passes without credentials
    {
        auto r = srv.request("/public");
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, "public content");
    }

    // 2. Protected endpoint without Authorization header -> 401 Unauthorized
    {
        auto r = srv.request("/protected");
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), "Bearer");
        CHECK_NE(r.body.find("Unauthorized"), std::string::npos);
        // Ensure no internal diagnostics leaked
        CHECK_EQ(r.body.find("JwtError"), std::string::npos);
    }

    // 3. Protected endpoint with duplicate Authorization headers -> 401 Unauthorized
    {
        auto r = srv.request("/protected", "GET", {
            {"Authorization", "Bearer " + valid_token},
            {"Authorization", "Bearer " + valid_token}
        });
        CHECK_EQ(r.status, 401);
    }

    // 4. Protected endpoint with non-matching scheme -> 401 plain Bearer challenge
    {
        auto r = srv.request("/protected", "GET", {{"Authorization", "Basic dXNlcjpwYXNz"}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), "Bearer");
    }

    // 5. Protected endpoint with invalid token -> 401
    {
        auto r = srv.request("/protected", "GET", {{"Authorization", "Bearer invalid.jwt.token"}});
        CHECK_EQ(r.status, 401);
    }

    // 6. Protected endpoint with valid token -> 200 OK and attached claims retrieved
    {
        auto r = srv.request("/protected", "GET", {{"Authorization", "Bearer " + valid_token}});
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, "hello bob_42");
    }

    // 7. Downstream exception preservation: HttpError(418) propagates as 418
    {
        auto r = srv.request("/fail", "GET", {{"Authorization", "Bearer " + valid_token}});
        CHECK_EQ(r.status, 418);
    }
}


TEST_CASE("JWA asymmetric ES256 P-256 sign, verify, and rejection matrix") {
    auto p256_keys = generate_ec_keypair("P-256");
    auto p384_keys = generate_ec_keypair("P-384");
    auto rsa_keys = generate_rsa_keypair(2048);

    kale::JwtClaims claims;
    claims.set_issuer("ec-issuer")
          .set_subject("dave")
          .set_expires_at(1800000000);

    // 1. Valid P-256 sign & verify with fixed 64-byte R || S (86-char Base64URL)
    auto sign_res = kale::JwtSigner::sign_pem(
        claims, p256_keys.private_key_pem, kale::JwtAlgorithm::ES256, "ec-k1");
    REQUIRE(sign_res.has_value());
    std::string token = *sign_res;

    size_t last_dot = token.rfind('.');
    REQUIRE_NE(last_dot, std::string::npos);
    std::string_view sig_b64 = std::string_view(token).substr(last_dot + 1);
    CHECK_EQ(sig_b64.size(), 86);

    kale::JwtValidator validator;
    validator.allow_algorithm(kale::JwtAlgorithm::ES256)
             .require_issuer("ec-issuer")
             .require_subject("dave")
             .set_clock([]() -> int64_t { return 1750000000; });

    auto verify_res = validator.verify_pem(token, p256_keys.public_key_pem);
    REQUIRE(verify_res.has_value());
    CHECK_EQ(verify_res->algorithm(), kale::JwtAlgorithm::ES256);
    CHECK_EQ(verify_res->token_type(), "JWT");
    CHECK_EQ(verify_res->kid(), "ec-k1");
    CHECK_EQ(verify_res->issuer(), "ec-issuer");
    CHECK_EQ(verify_res->subject(), "dave");

    // 2. Wrong curve (P-384) and RSA key rejected on both sign and verify
    CHECK_EQ(kale::JwtSigner::sign_pem(claims, p384_keys.private_key_pem, kale::JwtAlgorithm::ES256).error,
             kale::JwtError::KeyInvalid);
    CHECK_EQ(kale::JwtSigner::sign_pem(claims, rsa_keys.private_key_pem, kale::JwtAlgorithm::ES256).error,
             kale::JwtError::KeyInvalid);
    CHECK_EQ(validator.verify_pem(token, p384_keys.public_key_pem).error,
             kale::JwtError::KeyInvalid);
    CHECK_EQ(validator.verify_pem(token, rsa_keys.public_key_pem).error,
             kale::JwtError::KeyInvalid);

    // 3. Signature tamper and invalid length rejection
    std::string tampered = token;
    tampered.back() = (tampered.back() == 'A') ? 'B' : 'A';
    CHECK_EQ(validator.verify_pem(tampered, p256_keys.public_key_pem).error,
             kale::JwtError::SignatureMismatch);

    std::string short_sig = token.substr(0, token.size() - 2);
    CHECK_EQ(validator.verify_pem(short_sig, p256_keys.public_key_pem).error,
             kale::JwtError::SignatureMismatch);

    // 4. HMAC / PEM confusion rejection
    std::string secret = "kale_super_secret_key_1234567890_abcdefghijklmnopqrstuvwxyz_0123456789";
    CHECK_EQ(kale::JwtSigner::sign_hmac(claims, secret, kale::JwtAlgorithm::ES256).error,
             kale::JwtError::AlgorithmNotAllowed);
    CHECK_EQ(validator.verify_hmac(token, secret).error,
             kale::JwtError::AlgorithmNotAllowed);
}

TEST_CASE("Header typ validation, kid validation, and VerifiedJwtClaims scalar accessors") {
    std::string secret = "kale_super_secret_key_1234567890_abcdefghijklmnopqrstuvwxyz_0123456789";
    auto p256_keys = generate_ec_keypair("P-256");

    kale::JwtClaims claims;
    claims.set_issuer("auth")
          .set_subject("eve")
          .set_expires_at(1800000000);

    // 1. Empty typ rejected at signing time
    CHECK_EQ(kale::JwtSigner::sign_hmac(claims, secret, kale::JwtAlgorithm::HS256, "k1", "").error,
             kale::JwtError::MalformedToken);
    CHECK_EQ(kale::JwtSigner::sign_pem(claims, p256_keys.private_key_pem, kale::JwtAlgorithm::ES256, "k1", "").error,
             kale::JwtError::MalformedToken);

    // 2. Custom typ ("at+jwt") and kid ("key-2026-01")
    auto at_res = kale::JwtSigner::sign_pem(
        claims, p256_keys.private_key_pem, kale::JwtAlgorithm::ES256, "key-2026-01", "at+jwt");
    REQUIRE(at_res.has_value());
    std::string at_token = *at_res;

    kale::JwtValidator at_validator;
    at_validator.allow_algorithm(kale::JwtAlgorithm::ES256)
                .require_type("at+jwt")
                .require_kid("key-2026-01")
                .set_clock([]() -> int64_t { return 1750000000; });

    auto ok = at_validator.verify_pem(at_token, p256_keys.public_key_pem);
    REQUIRE(ok.has_value());
    CHECK_EQ(ok->algorithm(), kale::JwtAlgorithm::ES256);
    CHECK_EQ(ok->token_type(), "at+jwt");
    CHECK_EQ(ok->kid(), "key-2026-01");

    // 3. typ mismatch ("JWT" token against require_type("at+jwt"))
    auto jwt_res = kale::JwtSigner::sign_pem(
        claims, p256_keys.private_key_pem, kale::JwtAlgorithm::ES256, "key-2026-01", "JWT");
    REQUIRE(jwt_res.has_value());
    CHECK_EQ(at_validator.verify_pem(*jwt_res, p256_keys.public_key_pem).error,
             kale::JwtError::TypeMismatch);

    // 4. kid mismatch and missing kid
    auto other_kid_res = kale::JwtSigner::sign_pem(
        claims, p256_keys.private_key_pem, kale::JwtAlgorithm::ES256, "other-kid", "at+jwt");
    REQUIRE(other_kid_res.has_value());
    CHECK_EQ(at_validator.verify_pem(*other_kid_res, p256_keys.public_key_pem).error,
             kale::JwtError::KidMismatch);

    auto no_kid_res = kale::JwtSigner::sign_pem(
        claims, p256_keys.private_key_pem, kale::JwtAlgorithm::ES256, "", "at+jwt");
    REQUIRE(no_kid_res.has_value());
    CHECK_EQ(at_validator.verify_pem(*no_kid_res, p256_keys.public_key_pem).error,
             kale::JwtError::KidMismatch);

    // 5. Missing/invalid typ or non-string kid in raw header
    auto make_hmac_token = [&](std::string_view hdr_json) {
        std::string h_b64 = kale::Base64::encode_url(hdr_json);
        std::string p_b64 = kale::Base64::encode_url(claims.dump());
        std::string input = h_b64 + "." + p_b64;
        return input + "." + kale::CryptoUtil::hmac_sha256_base64url(input, secret);
    };

    kale::JwtValidator hmac_plain;
    hmac_plain.allow_algorithm(kale::JwtAlgorithm::HS256)
              .set_clock([]() -> int64_t { return 1750000000; });

    CHECK_EQ(hmac_plain.verify_hmac(make_hmac_token("{\"alg\":\"HS256\",\"typ\":123}"), secret).error,
             kale::JwtError::TypeMismatch);
    CHECK_EQ(hmac_plain.verify_hmac(make_hmac_token("{\"alg\":\"HS256\",\"typ\":\"\"}"), secret).error,
             kale::JwtError::TypeMismatch);
    CHECK_EQ(hmac_plain.verify_hmac(make_hmac_token("{\"alg\":\"HS256\",\"kid\":42}"), secret).error,
             kale::JwtError::KidMismatch);

    kale::JwtValidator hmac_req_typ;
    hmac_req_typ.allow_algorithm(kale::JwtAlgorithm::HS256)
                .require_type("JWT")
                .set_clock([]() -> int64_t { return 1750000000; });
    CHECK_EQ(hmac_req_typ.verify_hmac(make_hmac_token("{\"alg\":\"HS256\"}"), secret).error,
             kale::JwtError::TypeMismatch);

    // 6. Embedded NUL (\u0000) in typ or kid must be rejected
    CHECK_EQ(hmac_req_typ.verify_hmac(make_hmac_token("{\"alg\":\"HS256\",\"typ\":\"JWT\\u0000other\"}"), secret).error, kale::JwtError::InvalidJson);

    kale::JwtValidator hmac_req_kid;
    hmac_req_kid.allow_algorithm(kale::JwtAlgorithm::HS256)
                .require_kid("k1")
                .set_clock([]() -> int64_t { return 1750000000; });
    CHECK_EQ(hmac_req_kid.verify_hmac(make_hmac_token("{\"alg\":\"HS256\",\"kid\":\"k1\\u0000other\"}"), secret).error, kale::JwtError::InvalidJson);
}
TEST_CASE("JwtValidator::verify with dynamic JwtKeyResolver and bearer_auth integration") {
    const std::string key_a = "key-a-secret-must-be-at-least-32-bytes-long!";
    const std::string key_b = "key-b-secret-must-be-at-least-32-bytes-long!";

    kale::JwtClaims claims;
    claims.set_subject("user-123").set_expires_at(1750003600);

    auto tok_a = kale::JwtSigner::sign_hmac(claims, key_a, kale::JwtAlgorithm::HS256, "key-2026-a");
    auto tok_b = kale::JwtSigner::sign_hmac(claims, key_b, kale::JwtAlgorithm::HS256, "key-2026-b");
    auto tok_unknown = kale::JwtSigner::sign_hmac(claims, key_a, kale::JwtAlgorithm::HS256, "unknown-kid");
    REQUIRE(tok_a.has_value());
    REQUIRE(tok_b.has_value());
    REQUIRE(tok_unknown.has_value());

    int resolver_calls = 0;
    kale::JwtKeyResolver resolver = [&](std::string_view kid, kale::JwtAlgorithm) -> std::optional<kale::JwtKeyMaterial> {
        resolver_calls++;
        if (kid == "key-2026-a") return kale::JwtKeyMaterial{key_a, false};
        if (kid == "key-2026-b") return kale::JwtKeyMaterial{key_b, false};
        return std::nullopt;
    };

    kale::JwtValidator validator;
    validator.allow_algorithm(kale::JwtAlgorithm::HS256)
             .require_type("JWT")
             .set_clock([]() -> int64_t { return 1750000000; });

    auto res_a = validator.verify(*tok_a, resolver);
    REQUIRE(res_a.has_value());
    CHECK_EQ(res_a->kid(), "key-2026-a");
    CHECK_EQ(res_a->subject(), "user-123");

    auto res_b = validator.verify(*tok_b, resolver);
    REQUIRE(res_b.has_value());
    CHECK_EQ(res_b->kid(), "key-2026-b");

    auto res_unknown = validator.verify(*tok_unknown, resolver);
    CHECK_FALSE(res_unknown.has_value());
    CHECK_EQ(res_unknown.error, kale::JwtError::KeyInvalid);

    // Disallowed algorithm must be rejected BEFORE invoking key_resolver
    auto tok_hs384 = kale::JwtSigner::sign_hmac(claims, "key-384-secret-must-be-at-least-48-bytes-long-0000", kale::JwtAlgorithm::HS384, "key-2026-a");
    REQUIRE(tok_hs384.has_value());
    int before_calls = resolver_calls;
    auto res_disallowed = validator.verify(*tok_hs384, resolver);
    CHECK_EQ(res_disallowed.error, kale::JwtError::AlgorithmNotAllowed);
    CHECK_EQ(resolver_calls, before_calls);

    // bearer_auth middleware with key_resolver
    kale::BearerAuthConfig auth_cfg;
    auth_cfg.validator = validator;
    auth_cfg.key_resolver = resolver;

    TestServerRunner srv;
    srv->use(kale::bearer_auth(std::move(auth_cfg)));
    srv->GET("/me", [](kale::Context &ctx) {
        const auto *c = kale::get_jwt_claims(ctx);
        ctx.string(c ? std::string(c->kid()) : "none");
    });
    srv.start();

    auto r_ok = srv.request("/me", "GET", {{"Authorization", "Bearer " + *tok_b}});
    CHECK_EQ(r_ok.status, 200);
    CHECK_EQ(r_ok.body, "key-2026-b");

    auto r_bad = srv.request("/me", "GET", {{"Authorization", "Bearer " + *tok_unknown}});
    CHECK_EQ(r_bad.status, 401);
    CHECK(r_bad.header("WWW-Authenticate").find("error=\"invalid_token\"") != std::string::npos);
}

TEST_CASE("Opaque bearer mode configuration, to_string, and validation boundaries") {
    CHECK_EQ(std::string_view(kale::to_string(kale::JwtError::TokenRejected)), "token_rejected");

    // Neither JWT credentials nor token_verifier -> invalid_argument
    {
        kale::BearerAuthConfig cfg;
        CHECK_THROWS_AS(kale::bearer_auth(std::move(cfg)), std::invalid_argument);
    }

    // Verifier-only configuration succeeds
    {
        kale::BearerAuthConfig cfg;
        cfg.token_verifier = [](const kale::Context &, std::string_view tok) {
            return tok == "ok-token";
        };
        CHECK_NOTHROW(kale::bearer_auth(std::move(cfg)));
    }

    // Empty header_name or header_prefix still throws in opaque mode
    {
        kale::BearerAuthConfig cfg;
        cfg.header_name.clear();
        cfg.token_verifier = [](const kale::Context &, std::string_view) { return true; };
        CHECK_THROWS_AS(kale::bearer_auth(std::move(cfg)), std::invalid_argument);
    }
    {
        kale::BearerAuthConfig cfg;
        cfg.header_prefix.clear();
        cfg.token_verifier = [](const kale::Context &, std::string_view) { return true; };
        CHECK_THROWS_AS(kale::bearer_auth(std::move(cfg)), std::invalid_argument);
    }
}

TEST_CASE("Opaque bearer mode replaces JWT, preserves claims, and disables fallback") {
    const std::string secret = "kale_super_secret_key_1234567890_abcdefghijklmnopqrstuvwxyz_0123456789";
    kale::JwtClaims claims;
    claims.set_issuer("outer-issuer")
          .set_subject("outer-subject")
          .set_expires_at(1800000000);
    auto jwt_res = kale::JwtSigner::sign_hmac(claims, secret, kale::JwtAlgorithm::HS256, "k1");
    REQUIRE(jwt_res.has_value());
    const std::string valid_jwt = *jwt_res;

    kale::JwtValidator validator;
    validator.allow_algorithm(kale::JwtAlgorithm::HS256)
             .require_issuer("outer-issuer")
             .set_clock([]() -> int64_t { return 1750000000; });
    auto verified_outer = validator.verify_hmac(valid_jwt, secret);
    REQUIRE(verified_outer.has_value());

    int resolver_calls = 0;
    int verifier_calls = 0;
    std::string last_seen_token;

    kale::BearerAuthConfig cfg;
    cfg.validator = validator;
    cfg.secret_or_public_key = secret;
    cfg.key_resolver = [&](std::string_view, kale::JwtAlgorithm) -> std::optional<kale::JwtKeyMaterial> {
        ++resolver_calls;
        return kale::JwtKeyMaterial{secret, false};
    };
    cfg.token_verifier = [&](const kale::Context &ctx, std::string_view token) {
        ++verifier_calls;
        last_seen_token.assign(token);
        return token == "opaque.token_123-XYZ~+/==" && ctx.header_view("X-Tenant") == "acme";
    };

    // Case A: without pre-existing claims, opaque auth never attaches VerifiedJwtClaims
    {
        TestServerRunner srv;
        srv->use(kale::bearer_auth(cfg));
        srv->GET("/data", [](kale::Context &ctx) {
            CHECK_EQ(kale::get_jwt_claims(ctx), nullptr);
            ctx.string("ok");
        });
        srv.start();

        auto r_ok = srv.request("/data", "GET", {
            {"Authorization", "Bearer opaque.token_123-XYZ~+/=="},
            {"X-Tenant", "acme"}
        });
        CHECK_EQ(r_ok.status, 200);
        CHECK_EQ(r_ok.body, "ok");
        CHECK_EQ(last_seen_token, "opaque.token_123-XYZ~+/==");
        CHECK_EQ(resolver_calls, 0);

        // Valid signed JWT rejected by opaque verifier must NOT fall back to JWT verification
        auto r_jwt = srv.request("/data", "GET", {
            {"Authorization", "Bearer " + valid_jwt},
            {"X-Tenant", "acme"}
        });
        CHECK_EQ(r_jwt.status, 401);
        CHECK_EQ(r_jwt.header("WWW-Authenticate"), "Bearer error=\"invalid_token\"");
        CHECK_EQ(resolver_calls, 0);
    }

    // Case B: pre-existing VerifiedJwtClaims on Context are preserved (not erased or overwritten)
    {
        TestServerRunner srv;
        srv->use([pre_claims = *verified_outer](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            ctx.set_attachment<kale::VerifiedJwtClaims>(kale::VerifiedJwtClaims(pre_claims));
            co_await next();
        });
        srv->use(kale::bearer_auth(cfg));
        srv->GET("/preserved", [](kale::Context &ctx) {
            const auto *claims_ptr = kale::get_jwt_claims(ctx);
            REQUIRE_NE(claims_ptr, nullptr);
            ctx.string(std::string(claims_ptr->subject()));
        });
        srv.start();

        auto r = srv.request("/preserved", "GET", {
            {"Authorization", "Bearer opaque.token_123-XYZ~+/=="},
            {"X-Tenant", "acme"}
        });
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, "outer-subject");
        CHECK_EQ(resolver_calls, 0);
    }
}

TEST_CASE("Opaque bearer mode RFC 6750 extraction, realm escaping, and rejection edges") {
    int verifier_calls = 0;
    std::vector<std::pair<kale::JwtError, std::string>> hook_events;
    bool hook_saw_preset_401 = false;
    std::string hook_preset_challenge;

    kale::BearerAuthConfig cfg;
    cfg.realm = "api \"secure\" \\ test";
    cfg.skip_paths = {"/health"};
    cfg.token_verifier = [&](const kale::Context &, std::string_view token) {
        ++verifier_calls;
        return token == "good.opaque.tok==";
    };

    const std::string expected_plain_challenge = "Bearer realm=\"api \\\"secure\\\" \\\\ test\"";
    const std::string expected_err_challenge = "Bearer realm=\"api \\\"secure\\\" \\\\ test\", error=\"invalid_token\"";

    // Default responder server
    TestServerRunner default_srv;
    default_srv->use(kale::bearer_auth(cfg));
    default_srv->GET("/health", [](kale::Context &ctx) { ctx.string("healthy"); });
    default_srv->GET("/resource", [](kale::Context &ctx) { ctx.string("granted"); });
    default_srv.start();

    // 1. Skip path bypasses verifier
    CHECK_EQ(default_srv.request("/health").status, 200);
    CHECK_EQ(verifier_calls, 0);

    // 2. Absent credentials -> 401 with realm challenge and NO error="invalid_token"
    {
        auto r = default_srv.request("/resource");
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), expected_plain_challenge);
        CHECK_EQ(verifier_calls, 0);
    }

    // 3. Non-matching schemes -> 401 with realm challenge and NO error="invalid_token"
    for (const std::string &hdr : {
             std::string("Basic dXNlcjpwYXNz"),
             std::string("Digest username=\"alice\""),
             std::string("BearerToken abcdef")}) {
        auto r = default_srv.request("/resource", "GET", {{"Authorization", hdr}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), expected_plain_challenge);
        CHECK_EQ(verifier_calls, 0);
    }

    // 4. Duplicate / empty / malformed Bearer credentials -> 401 invalid_token BEFORE verifier
    {
        auto r_dup = default_srv.request("/resource", "GET", {
            {"Authorization", "Bearer good.opaque.tok=="},
            {"Authorization", "Bearer good.opaque.tok=="}
        });
        CHECK_EQ(r_dup.status, 401);
        CHECK_EQ(r_dup.header("WWW-Authenticate"), expected_err_challenge);
        CHECK_EQ(verifier_calls, 0);
    }
    for (const std::string &hdr : {
             std::string(""),
             std::string("Bearer"),
             std::string("bearer"),
             std::string("Bearer "),
             std::string("Bearer\tgood.opaque.tok=="),
             std::string("Bearer tok1 tok2"),
             std::string("Bearer tok1,tok2"),
             std::string("Bearer abc=def"),
             std::string("Bearer ="),
             std::string("Bearer =="),
             std::string("Bearer bad!token")}) {
        auto r = default_srv.request("/resource", "GET", {{"Authorization", hdr}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), expected_err_challenge);
        CHECK_EQ(verifier_calls, 0);
    }

    // 5. Valid shape rejected by verifier -> 401 invalid_token
    {
        auto r = default_srv.request("/resource", "GET", {{"Authorization", "bearer wrong.opaque.tok=="}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), expected_err_challenge);
        CHECK_EQ(verifier_calls, 1);
    }

    // 6. Valid shape with extra OWS around token accepted
    {
        auto r = default_srv.request("/resource", "GET", {{"Authorization", "Bearer   good.opaque.tok==  "}});
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, "granted");
        CHECK_EQ(verifier_calls, 2);
    }

    // Custom on_unauthorized hook honors escaped realm and pre-sets 401 + header before callback
    kale::BearerAuthConfig hook_cfg = cfg;
    hook_cfg.on_unauthorized = [&](kale::Context &ctx, kale::JwtError err) {
        hook_saw_preset_401 = (ctx.status_code() == 401);
        hook_preset_challenge = std::string(ctx.resp_header("WWW-Authenticate"));
        hook_events.emplace_back(err, std::string(kale::to_string(err)));
        ctx.string(std::string("custom:") + kale::to_string(err));
    };

    TestServerRunner hook_srv;
    hook_srv->use(kale::bearer_auth(std::move(hook_cfg)));
    hook_srv->GET("/resource", [](kale::Context &ctx) { ctx.string("granted"); });
    hook_srv.start();

    // Absent -> JwtError::None + plain challenge pre-set
    {
        auto r = hook_srv.request("/resource");
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), expected_plain_challenge);
        CHECK_EQ(r.body, "custom:missing_token");
        CHECK(hook_saw_preset_401);
        CHECK_EQ(hook_preset_challenge, expected_plain_challenge);
    }

    // Malformed Bearer -> JwtError::MalformedToken + invalid_token challenge pre-set
    {
        auto r = hook_srv.request("/resource", "GET", {{"Authorization", "Bearer bad=tok"}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), expected_err_challenge);
        CHECK_EQ(r.body, "custom:malformed_token");
        CHECK(hook_saw_preset_401);
        CHECK_EQ(hook_preset_challenge, expected_err_challenge);
    }

    // Rejected valid token -> JwtError::TokenRejected + invalid_token challenge pre-set
    {
        auto r = hook_srv.request("/resource", "GET", {{"Authorization", "Bearer unknown.tok"}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), expected_err_challenge);
        CHECK_EQ(r.body, "custom:token_rejected");
        CHECK(hook_saw_preset_401);
        CHECK_EQ(hook_preset_challenge, expected_err_challenge);
    }
}

TEST_CASE("Opaque bearer mode custom header_prefix boundary, outer access_log, and callback exceptions") {
    std::mutex mu;
    std::vector<kale::AccessLogRecord> logs;
    std::vector<std::string> app_audit;

    kale::BearerAuthConfig cfg;
    cfg.header_name = "X-Api-Auth";
    cfg.header_prefix = "Bearer"; // No trailing space: must still distinguish "BearerToken" from "Bearer"
    cfg.realm = "service";
    cfg.token_verifier = [](const kale::Context &, std::string_view token) {
        if (token == "throw-http") {
            throw kale::HttpError(503, "introspection unavailable", "AUTH_BACKEND_DOWN");
        }
        if (token == "throw-runtime") {
            throw std::runtime_error("unexpected verifier failure");
        }
        return token == "secret-opaque-token";
    };
    cfg.on_unauthorized = [&](kale::Context &ctx, kale::JwtError err) {
        // App hook logs only safe metadata (never raw bearer token)
        {
            std::lock_guard<std::mutex> lock(mu);
            app_audit.push_back(std::string(ctx.path()) + ":" + kale::to_string(err));
        }
        if (ctx.has_header("X-Hook-Throw")) {
            throw kale::HttpError(502, "hook failed", "HOOK_ERROR");
        }
        ctx.string(std::string("rejected:") + kale::to_string(err));
    };

    TestServerRunner srv;
    srv->use(kale::access_log([&](kale::AccessLogRecord rec) {
        std::lock_guard<std::mutex> lock(mu);
        logs.push_back(std::move(rec));
    }));
    srv->use(kale::bearer_auth(std::move(cfg)));
    srv->GET("/items", [](kale::Context &ctx) { ctx.string("items-ok"); });
    srv.start();

    // 1. Different scheme starting with prefix ("BearerToken ...") -> no matching scheme (JwtError::None)
    {
        auto r = srv.request("/items", "GET", {{"X-Api-Auth", "BearerToken secret-opaque-token"}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), "Bearer realm=\"service\"");
        CHECK_EQ(r.body, "rejected:missing_token");
    }

    // 2. Rejected token -> 401 TokenRejected, outer access_log sees 401 without exception
    {
        auto r = srv.request("/items", "GET", {{"X-Api-Auth", "Bearer wrong-token"}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), "Bearer realm=\"service\", error=\"invalid_token\"");
        CHECK_EQ(r.body, "rejected:token_rejected");
    }

    // 3. Accepted token -> 200 OK
    {
        auto r = srv.request("/items", "GET", {{"X-Api-Auth", "Bearer secret-opaque-token"}});
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body, "items-ok");
    }

    // 4. Verifier HttpError(503) propagates cleanly through outer middleware
    {
        auto r = srv.request("/items", "GET", {{"X-Api-Auth", "Bearer throw-http"}});
        CHECK_EQ(r.status, 503);
    }

    // 5. Verifier std::runtime_error propagates as 500
    {
        auto r = srv.request("/items", "GET", {{"X-Api-Auth", "Bearer throw-runtime"}});
        CHECK_EQ(r.status, 500);
    }

    // 6. on_unauthorized exception propagates cleanly
    {
        auto r = srv.request("/items", "GET", {
            {"X-Api-Auth", "Bearer wrong-token"},
            {"X-Hook-Throw", "1"}
        });
        CHECK_EQ(r.status, 502);
    }

    srv.stop();

    std::lock_guard<std::mutex> lock(mu);
    REQUIRE_EQ(logs.size(), 6);
    CHECK_EQ(logs[0].status_code, 401);
    CHECK_FALSE(logs[0].downstream_exception);
    CHECK_EQ(logs[1].status_code, 401);
    CHECK_FALSE(logs[1].downstream_exception);
    CHECK_EQ(logs[2].status_code, 200);
    CHECK_FALSE(logs[2].downstream_exception);
    CHECK_EQ(logs[3].status_code, 503);
    CHECK(logs[3].downstream_exception);
    CHECK_EQ(logs[4].status_code, 500);
    CHECK(logs[4].downstream_exception);
    CHECK_EQ(logs[5].status_code, 502);
    CHECK(logs[5].downstream_exception);

    REQUIRE_EQ(app_audit.size(), 3);
    CHECK_EQ(app_audit[0], "/items:missing_token");
    CHECK_EQ(app_audit[1], "/items:token_rejected");
    CHECK_EQ(app_audit[2], "/items:token_rejected");
}

TEST_CASE("bearer_auth emits structured ErrorEvent diagnostics without leaking tokens") {
    struct CapturedEvent {
        int status{0};
        std::string error_code;
        std::string source;
        std::string reason;
    };
    std::mutex mu;
    std::vector<CapturedEvent> events;

    kale::ErrorPolicy policy;
    policy.on_error = [&](const kale::Context &, const kale::ErrorEvent &ev) {
        std::lock_guard<std::mutex> lock(mu);
        events.push_back({
            ev.status_code,
            std::string(ev.error_code),
            std::string(ev.source),
            std::string(ev.reason)
        });
    };

    const std::string secret = "0123456789abcdef0123456789abcdef";
    kale::JwtValidator validator;
    validator.allow_algorithm(kale::JwtAlgorithm::HS256);
    kale::BearerAuthConfig jwt_cfg;
    jwt_cfg.secret_or_public_key = secret;
    jwt_cfg.validator = validator;

    TestServerRunner srv;
    srv->use(kale::error_handler(kale::default_standard_json_formatter(), std::move(policy)));
    srv->GET("/jwt", kale::bearer_auth(jwt_cfg), [](kale::Context &ctx) -> coke::Task<void> {
        ctx.string("ok");
        co_return;
    });

    kale::BearerAuthConfig opaque_cfg;
    opaque_cfg.token_verifier = [](const kale::Context &, std::string_view token) {
        return token == "good-opaque-token";
    };
    srv->GET("/opaque", kale::bearer_auth(std::move(opaque_cfg)), [](kale::Context &ctx) -> coke::Task<void> {
        ctx.string("ok");
        co_return;
    });

    srv.start();

    // 1. Missing Authorization header in JWT mode -> RFC 6750 UNAUTHORIZED (no invalid_token)
    {
        auto r = srv.request("/jwt", "GET");
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), "Bearer");
        CHECK_NE(r.body.find("UNAUTHORIZED"), std::string::npos);
        CHECK_EQ(r.body.find("invalid_token"), std::string::npos);
    }

    // 2. Expired JWT -> invalid_token + token_expired
    {
        kale::JwtClaims claims;
        claims.set_subject("u1");
        claims.set_expires_at(1);
        auto expired_tok = kale::JwtSigner::sign_hmac(claims, secret, kale::JwtAlgorithm::HS256);
        REQUIRE(expired_tok.has_value());
        auto r = srv.request("/jwt", "GET", {{"Authorization", "Bearer " + *expired_tok}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.body.find(*expired_tok), std::string::npos);
    }

    // 3. Invalid signature JWT -> signature_mismatch
    {
        kale::JwtClaims claims;
        claims.set_subject("u1");
        auto bad_sig_tok = kale::JwtSigner::sign_hmac(
            claims,
            "wrong-secret-0123456789abcdef0123",
            kale::JwtAlgorithm::HS256);
        REQUIRE(bad_sig_tok.has_value());
        auto r = srv.request("/jwt", "GET", {{"Authorization", "Bearer " + *bad_sig_tok}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.body.find(*bad_sig_tok), std::string::npos);
    }

    // 4. Opaque rejected -> token_rejected
    {
        auto r = srv.request("/opaque", "GET", {{"Authorization", "Bearer bad-opaque-secret"}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.body.find("bad-opaque-secret"), std::string::npos);
    }

    // 5. Non-matching Basic scheme in JWT mode -> MissingToken + plain Bearer challenge
    {
        auto r = srv.request("/jwt", "GET", {{"Authorization", "Basic dXNlcjpwYXNz"}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), "Bearer");
        CHECK_EQ(r.body.find("invalid_token"), std::string::npos);
    }

    // 6. Non-matching Basic scheme in opaque mode -> MissingToken + plain Bearer challenge
    {
        auto r = srv.request("/opaque", "GET", {{"Authorization", "Basic dXNlcjpwYXNz"}});
        CHECK_EQ(r.status, 401);
        CHECK_EQ(r.header("WWW-Authenticate"), "Bearer");
        CHECK_EQ(r.body.find("invalid_token"), std::string::npos);
    }

    srv.stop();

    std::lock_guard<std::mutex> lock(mu);
    REQUIRE_EQ(events.size(), 6);
    CHECK_EQ(events[0].status, 401);
    CHECK_EQ(events[0].error_code, "UNAUTHORIZED");
    CHECK_EQ(events[0].source, "bearer_auth");
    CHECK_EQ(events[0].reason, "missing_token");

    CHECK_EQ(events[1].status, 401);
    CHECK_EQ(events[1].error_code, "INVALID_TOKEN");
    CHECK_EQ(events[1].source, "bearer_auth");
    CHECK_EQ(events[1].reason, "token_expired");

    CHECK_EQ(events[2].status, 401);
    CHECK_EQ(events[2].error_code, "INVALID_TOKEN");
    CHECK_EQ(events[2].source, "bearer_auth");
    CHECK_EQ(events[2].reason, "signature_mismatch");

    CHECK_EQ(events[3].status, 401);
    CHECK_EQ(events[3].error_code, "INVALID_TOKEN");
    CHECK_EQ(events[3].source, "bearer_auth");
    CHECK_EQ(events[3].reason, "token_rejected");

    CHECK_EQ(events[4].status, 401);
    CHECK_EQ(events[4].error_code, "UNAUTHORIZED");
    CHECK_EQ(events[4].source, "bearer_auth");
    CHECK_EQ(events[4].reason, "missing_token");

    CHECK_EQ(events[5].status, 401);
    CHECK_EQ(events[5].error_code, "UNAUTHORIZED");
    CHECK_EQ(events[5].source, "bearer_auth");
    CHECK_EQ(events[5].reason, "missing_token");
}
} // TEST_SUITE("jwt")