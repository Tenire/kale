#ifndef KALE_SECURITY_CRYPTO_UTIL_H
#define KALE_SECURITY_CRYPTO_UTIL_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kale {

class CryptoUtil {
public:
    static bool constant_time_equals(std::string_view a, std::string_view b) noexcept;

    [[nodiscard]] static bool random_bytes(unsigned char *buf, size_t len);
    static std::string random_bytes_hex(size_t len);

    static std::vector<unsigned char> hmac_sha256(std::string_view data, std::string_view key);
    static std::string hmac_sha256_base64url(std::string_view data, std::string_view key);
    static std::vector<unsigned char> hmac_sha384(std::string_view data, std::string_view key);
    static std::string hmac_sha384_base64url(std::string_view data, std::string_view key);
    static std::vector<unsigned char> hmac_sha512(std::string_view data, std::string_view key);
    static std::string hmac_sha512_base64url(std::string_view data, std::string_view key);

    static std::vector<unsigned char> sha256(std::string_view data);
    static std::string sha256_hex(std::string_view data);

    // md_name: "SHA256", "SHA384", "SHA512" for RSA; nullptr or "" for Ed25519.
    static std::optional<std::vector<unsigned char>> sign_evp(
        std::string_view data, std::string_view private_key_pem, const char *md_name = nullptr);
    static bool verify_evp(
        std::string_view data, const unsigned char *sig, size_t sig_len,
        std::string_view public_key_pem, const char *md_name = nullptr);
};

} // namespace kale

#endif // KALE_SECURITY_CRYPTO_UTIL_H
