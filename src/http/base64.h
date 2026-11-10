#ifndef KALE_HTTP_BASE64_H
#define KALE_HTTP_BASE64_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace kale {

class Base64 {
public:
    static std::string encode_url(const unsigned char *data, size_t len);
    static std::string encode_url(std::string_view data) {
        return encode_url(reinterpret_cast<const unsigned char *>(data.data()), data.size());
    }

    static std::optional<std::string> decode_url(std::string_view in);

    static std::string encode(const unsigned char *data, size_t len);
    static std::string encode(std::string_view data) {
        return encode(reinterpret_cast<const unsigned char *>(data.data()), data.size());
    }

    static std::optional<std::string> decode(std::string_view in);
};

} // namespace kale

#endif // KALE_HTTP_BASE64_H
