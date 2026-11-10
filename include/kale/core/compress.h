#ifndef KALE_CORE_COMPRESS_H
#define KALE_CORE_COMPRESS_H

#include <string>
#include <string_view>
#include <vector>

namespace kale {


enum class CompressionEncoding {
    None,
    Gzip,
    Zstd,
};

inline std::string_view encoding_to_string(CompressionEncoding encoding) noexcept {
    switch (encoding) {
        case CompressionEncoding::Gzip: return "gzip";
        case CompressionEncoding::Zstd: return "zstd";
        default: return "";
    }
}



class Compress {
public:
    static bool gzip(std::string_view src, std::string &dest, int level = -1);
    static bool ungzip(std::string_view src, std::string &dest, size_t max_output = 64 * 1024 * 1024);
    static bool zstd(std::string_view src, std::string &dest, int compression_level = 3);
    static bool unzstd(std::string_view src, std::string &dest, size_t max_output = 64 * 1024 * 1024);

    /**
     * Negotiates encoding from Accept-Encoding header respecting q-values and preference order.
     */
    static CompressionEncoding negotiate(
        std::string_view accept_encoding,
        const std::vector<CompressionEncoding> &preferred_encodings = {
            CompressionEncoding::Zstd,
            CompressionEncoding::Gzip
        }
    );
};

} // namespace kale

#endif // KALE_CORE_COMPRESS_H
