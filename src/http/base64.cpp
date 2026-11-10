#include "http/base64.h"

#include <cstdint>

namespace kale {
namespace {
std::optional<std::string> decode_base64(std::string_view input, bool url) {
    const auto padding_start = input.find('=');
    const size_t count = padding_start == std::string_view::npos ? input.size() : padding_start;
    const size_t padding = input.size() - count;
    if (count % 4 == 1 || padding > 2) return std::nullopt;
    if (padding) {
        if (input.size() % 4 != 0 || padding != (4 - count % 4) % 4) return std::nullopt;
        for (size_t i = count; i < input.size(); ++i) if (input[i] != '=') return std::nullopt;
    } else if (!url && count % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve(count / 4 * 3 + count % 4);
    unsigned buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < count; ++i) {
        const char c = input[i];
        int value = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 :
                    c >= '0' && c <= '9' ? c - '0' + 52 : c == (url ? '-' : '+') ? 62 : c == (url ? '_' : '/') ? 63 : -1;
        if (value < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += static_cast<char>((buffer >> bits) & 255); }
    }
    if (bits && (buffer & ((1u << bits) - 1u))) return std::nullopt;
    return out;
}
} // namespace

std::string Base64::encode_url(const unsigned char *data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    while (i < len) {
        size_t rem = len - i;
        uint32_t octet_a = data[i++];
        uint32_t octet_b = rem > 1 ? data[i++] : 0;
        uint32_t octet_c = rem > 2 ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        if (rem > 1) {
            out.push_back(table[(triple >> 6) & 0x3F]);
        }
        if (rem > 2) {
            out.push_back(table[triple & 0x3F]);
        }
    }
    return out;
}

std::optional<std::string> Base64::decode_url(std::string_view input) { return decode_base64(input, true); }

std::string Base64::encode(const unsigned char *data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    while (i < len) {
        size_t rem = len - i;
        uint32_t octet_a = data[i++];
        uint32_t octet_b = rem > 1 ? data[i++] : 0;
        uint32_t octet_c = rem > 2 ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back(rem > 1 ? table[(triple >> 6) & 0x3F] : '=');
        out.push_back(rem > 2 ? table[triple & 0x3F] : '=');
    }
    return out;
}

std::optional<std::string> Base64::decode(std::string_view input) { return decode_base64(input, false); }

} // namespace kale
