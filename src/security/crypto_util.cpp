#include "security/crypto_util.h"
#include "http/base64.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <iomanip>
#include <sstream>
#include <climits>
#include <stdexcept>

namespace kale {
namespace {
std::vector<unsigned char> checked_hmac(const EVP_MD *md, std::string_view data, std::string_view key) {
    if (key.size() > INT_MAX) throw std::length_error("HMAC key is too large");
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (!HMAC(md, key.empty() ? "" : key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char *>(data.data()), data.size(), digest, &length) ||
        length != static_cast<unsigned>(EVP_MD_size(md))) throw std::runtime_error("HMAC failed");
    return {digest, digest + length};
}
}




bool CryptoUtil::constant_time_equals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool CryptoUtil::random_bytes(unsigned char *buf, size_t len) {
    if (len == 0) return true;
    return buf && len <= INT_MAX && RAND_bytes(buf, static_cast<int>(len)) == 1;
}

std::string CryptoUtil::random_bytes_hex(size_t len) {
    if (len > INT_MAX) throw std::length_error("Random byte request is too large");
    std::vector<unsigned char> buf(len);
    if (!random_bytes(buf.data(), len)) {
        throw std::runtime_error("Secure random generation failed");
    }
    std::ostringstream oss;
    for (unsigned char b : buf) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}


std::vector<unsigned char> CryptoUtil::hmac_sha256(std::string_view data, std::string_view key) { return checked_hmac(EVP_sha256(), data, key); }

std::string CryptoUtil::hmac_sha256_base64url(std::string_view data, std::string_view key) {
    auto digest = hmac_sha256(data, key);
    return Base64::encode_url(digest.data(), digest.size());
}
std::vector<unsigned char> CryptoUtil::hmac_sha384(std::string_view data, std::string_view key) { return checked_hmac(EVP_sha384(), data, key); }

std::string CryptoUtil::hmac_sha384_base64url(std::string_view data, std::string_view key) {
    auto digest = hmac_sha384(data, key);
    return Base64::encode_url(digest.data(), digest.size());
}

std::vector<unsigned char> CryptoUtil::hmac_sha512(std::string_view data, std::string_view key) { return checked_hmac(EVP_sha512(), data, key); }

std::string CryptoUtil::hmac_sha512_base64url(std::string_view data, std::string_view key) {
    auto digest = hmac_sha512(data, key);
    return Base64::encode_url(digest.data(), digest.size());
}


std::vector<unsigned char> CryptoUtil::sha256(std::string_view data) {
    std::vector<unsigned char> hash(32);
    unsigned length = 0;
    if (EVP_Digest(data.data(), data.size(), hash.data(), &length, EVP_sha256(), nullptr) != 1 || length != hash.size())
        throw std::runtime_error("SHA-256 failed");
    return hash;
}

std::string CryptoUtil::sha256_hex(std::string_view data) {
    auto hash = sha256(data);
    std::ostringstream oss;
    for (unsigned char b : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}
std::optional<std::vector<unsigned char>> CryptoUtil::sign_evp(
    std::string_view data, std::string_view private_key_pem, const char *md_name) {
    if (private_key_pem.size() > INT_MAX) return std::nullopt;
    BIO *bio = BIO_new_mem_buf(private_key_pem.data(), static_cast<int>(private_key_pem.size()));
    if (!bio) return std::nullopt;

    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) return std::nullopt;

    const EVP_MD *md = nullptr;
    if (md_name && *md_name) {
        md = EVP_get_digestbyname(md_name);
        if (!md) {
            EVP_PKEY_free(pkey);
            return std::nullopt;
        }
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return std::nullopt;
    }

    if (EVP_DigestSignInit(ctx, nullptr, md, nullptr, pkey) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::nullopt;
    }

    size_t sig_len = 0;
    if (EVP_DigestSign(ctx, nullptr, &sig_len,
                       reinterpret_cast<const unsigned char *>(data.data()), data.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::nullopt;
    }

    std::vector<unsigned char> sig(sig_len);
    if (EVP_DigestSign(ctx, sig.data(), &sig_len,
                       reinterpret_cast<const unsigned char *>(data.data()), data.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::nullopt;
    }

    sig.resize(sig_len);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return sig;
}

bool CryptoUtil::verify_evp(
    std::string_view data, const unsigned char *sig, size_t sig_len,
    std::string_view public_key_pem, const char *md_name) {
    if (!sig || sig_len == 0) return false;
    if (public_key_pem.size() > INT_MAX) return false;

    BIO *bio = BIO_new_mem_buf(public_key_pem.data(), static_cast<int>(public_key_pem.size()));
    if (!bio) return false;

    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) return false;

    const EVP_MD *md = nullptr;
    if (md_name && *md_name) {
        md = EVP_get_digestbyname(md_name);
        if (!md) {
            EVP_PKEY_free(pkey);
            return false;
        }
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return false;
    }

    if (EVP_DigestVerifyInit(ctx, nullptr, md, nullptr, pkey) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return false;
    }

    int ret = EVP_DigestVerify(ctx, sig, sig_len,
                               reinterpret_cast<const unsigned char *>(data.data()), data.size());
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ret == 1;
}

} // namespace kale
