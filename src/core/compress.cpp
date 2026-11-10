#include "kale/core/compress.h"

#include <charconv>
#include <array>
#include <climits>
#include <cmath>
#include <memory>
#include <zlib.h>
#include <zstd.h>

namespace kale {


bool Compress::gzip(std::string_view src, std::string &dest, int level) {
    if (src.size() > UINT_MAX) { dest.clear(); return false; }

    if (level < 0) {
        level = Z_DEFAULT_COMPRESSION;
    }

    z_stream stream{};
    if (deflateInit2(&stream, level, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }

    dest.clear();
    char buffer[16384];
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(src.data()));
    stream.avail_in = static_cast<uInt>(src.size());

    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        status = deflate(&stream, Z_FINISH);
        size_t have = sizeof(buffer) - stream.avail_out;
        dest.append(buffer, have);
    }

    deflateEnd(&stream);
    return status == Z_STREAM_END;
}

bool Compress::ungzip(std::string_view src, std::string &dest, size_t max_output) {
    dest.clear();
    if (src.empty() || src.size() > UINT_MAX) return false;
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) return false;
    struct End { z_stream *stream; ~End() { inflateEnd(stream); } } cleanup{&stream};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(src.data()));
    stream.avail_in = static_cast<uInt>(src.size());
    std::string output;
    std::array<char, 16384> buffer;
    for (;;) {
        const auto before = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
        stream.avail_out = buffer.size();
        const int status = inflate(&stream, Z_NO_FLUSH);
        const size_t produced = buffer.size() - stream.avail_out;
        if (produced > max_output - output.size()) return false;
        output.append(buffer.data(), produced);
        if (status == Z_STREAM_END) {
            if (stream.avail_in == 0) { dest = std::move(output); return true; }
            // Concatenated gzip members share the same aggregate output limit.
            if (inflateReset2(&stream, 15 + 16) != Z_OK) return false;
        } else if (status != Z_OK || (produced == 0 && before == stream.avail_in)) return false;
    }
}

bool Compress::zstd(std::string_view src, std::string &dest, int compression_level) {

    size_t const bound = ZSTD_compressBound(src.size());
    dest.resize(bound);

    size_t const cSize = ZSTD_compress(
        dest.data(), bound,
        src.data(), src.size(),
        compression_level
    );

    if (ZSTD_isError(cSize)) {
        dest.clear();
        return false;
    }

    dest.resize(cSize);
    return true;
}

bool Compress::unzstd(std::string_view src, std::string &dest, size_t max_output) {
    dest.clear();
    if (src.empty()) return false;
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(), &ZSTD_freeDCtx);
    if (!context) return false;
    // Bound the decoder window independently of the declared/output size.
    if (ZSTD_isError(ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax, 26))) return false;
    ZSTD_inBuffer input{src.data(), src.size(), 0};
    std::array<char, 16384> buffer;
    std::string output;
    for (;;) {
        const size_t before = input.pos;
        ZSTD_outBuffer chunk{buffer.data(), buffer.size(), 0};
        const size_t remaining = ZSTD_decompressStream(context.get(), &chunk, &input);
        if (ZSTD_isError(remaining) || chunk.pos > max_output - output.size()) return false;
        output.append(buffer.data(), chunk.pos);
        if (remaining == 0 && input.pos == input.size) { dest = std::move(output); return true; }
        if (before == input.pos && chunk.pos == 0) return false;
    }
}

static bool case_eq_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

CompressionEncoding Compress::negotiate(
    std::string_view accept_encoding,
    const std::vector<CompressionEncoding> &preferred_encodings
) {
    if (accept_encoding.empty() || preferred_encodings.empty()) {
        return CompressionEncoding::None;
    }

    float zstd_q = -1.0f;
    float gzip_q = -1.0f;
    float any_q = -1.0f;

    size_t start = 0;
    while (start < accept_encoding.size()) {
        size_t comma = accept_encoding.find(',', start);
        std::string_view token = (comma == std::string_view::npos)
            ? accept_encoding.substr(start)
            : accept_encoding.substr(start, comma - start);

        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);

        if (!token.empty()) {
            std::string_view coding = token;
            float q = 1.0f;

            size_t semicolon = token.find(';');
            if (semicolon != std::string_view::npos) {
                coding = token.substr(0, semicolon);
                while (!coding.empty() && (coding.back() == ' ' || coding.back() == '\t')) coding.remove_suffix(1);

                std::string_view params = token.substr(semicolon + 1);
                size_t p_pos = 0;
                while (p_pos < params.size()) {
                    while (p_pos < params.size() && (params[p_pos] == ' ' || params[p_pos] == '\t' || params[p_pos] == ';')) {
                        ++p_pos;
                    }
                    if (p_pos >= params.size()) break;
                    if ((params[p_pos] == 'q' || params[p_pos] == 'Q') &&
                        p_pos + 1 < params.size() && params[p_pos + 1] == '=') {
                        std::string_view q_val = params.substr(p_pos + 2);
                        while (!q_val.empty() && (q_val.front() == ' ' || q_val.front() == '\t')) q_val.remove_prefix(1);
                        size_t end_val = q_val.find_first_of(" ;,");
                        if (end_val != std::string_view::npos) {
                            q_val = q_val.substr(0, end_val);
                        }
                        auto [ptr, ec] = std::from_chars(q_val.data(), q_val.data() + q_val.size(), q);
                        if (ec != std::errc{} || ptr != q_val.data() + q_val.size() || !std::isfinite(q) || q < 0 || q > 1) {
                            q = 0.0f;
                        }
                        break;
                    }
                    size_t next_semi = params.find(';', p_pos);
                    if (next_semi == std::string_view::npos) break;
                    p_pos = next_semi + 1;
                }
            }

            if (case_eq_ascii(coding, "zstd")) {
                zstd_q = q;
            } else if (case_eq_ascii(coding, "gzip")) {
                gzip_q = q;
            } else if (coding == "*") {
                any_q = q;
            }
        }

        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }

    if (zstd_q < 0.0f && any_q >= 0.0f) zstd_q = any_q;
    if (gzip_q < 0.0f && any_q >= 0.0f) gzip_q = any_q;

    auto get_q = [&](CompressionEncoding enc) -> float {
        if (enc == CompressionEncoding::Zstd) return zstd_q;
        if (enc == CompressionEncoding::Gzip) return gzip_q;
        return -1.0f;
    };

    float max_q = 0.0f;
    for (auto enc : preferred_encodings) {
        float q = get_q(enc);
        if (q > max_q) {
            max_q = q;
        }
    }

    if (max_q <= 0.0f) {
        return CompressionEncoding::None;
    }

    for (auto enc : preferred_encodings) {
        float q = get_q(enc);
        if (q >= max_q) {
            return enc;
        }
    }

    return CompressionEncoding::None;
}

} // namespace kale
