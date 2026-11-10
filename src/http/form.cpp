#include "kale/http/form.h"

#include <cctype>

#include "kale/core/context.h"
#include "kale/core/http_util.h"
#include "http/multipart_parser.h"

namespace kale {

namespace {

inline bool url_decode_matches(std::string_view encoded, std::string_view target) noexcept {
    if (encoded.find('%') == std::string_view::npos && encoded.find('+') == std::string_view::npos) {
        return encoded == target;
    }

    auto hex_to_val = [](char ch) noexcept -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };

    size_t i = 0;
    size_t j = 0;
    while (i < encoded.size() && j < target.size()) {
        char c = encoded[i];
        if (c == '%' && i + 2 < encoded.size()) {
            int h1 = hex_to_val(encoded[i + 1]);
            int h2 = hex_to_val(encoded[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                c = static_cast<char>((h1 << 4) | h2);
                i += 2;
            }
        } else if (c == '+') {
            c = ' ';
        }
        if (c != target[j]) return false;
        ++i;
        ++j;
    }
    return i == encoded.size() && j == target.size();
}

bool is_multipart_form_data(std::string_view ct) noexcept {
    constexpr std::string_view prefix = "multipart/form-data";
    if (ct.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(ct[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    if (ct.size() > prefix.size()) {
        char c = ct[prefix.size()];
        if (c != ';' && c != ' ' && c != '\t') return false;
    }
    return true;
}

} // namespace

std::map<std::string, std::string, std::less<>> parse_form(const Context &ctx) {
    if (!is_multipart_form_data(ctx.content_type())) return split_query(ctx.req_body());
    std::map<std::string, std::string, std::less<>> params;
    for (const auto &part : parse_multipart(ctx)) {
        if (part.filename.empty()) params.insert_or_assign(part.name, std::string(part.data));
    }
    return params;
}

std::string form(const Context &ctx, std::string_view key) {
    if (key.empty()) return {};
    if (is_multipart_form_data(ctx.content_type())) {
        const auto boundary = MultiPartParser::extract_boundary(ctx.content_type());
        if (boundary.empty()) return {};
        auto match = MultiPartParser::find_field(ctx.req_body(), boundary, key);
        return match ? std::string(*match) : std::string{};
    }

    std::string_view q = ctx.req_body();
    while (!q.empty()) {
        size_t amp = q.find('&');
        std::string_view field = (amp != std::string_view::npos) ? q.substr(0, amp) : q;
        if (!field.empty()) {
            size_t eq = field.find('=');
            std::string_view raw_k = (eq != std::string_view::npos) ? field.substr(0, eq) : field;
            if (!raw_k.empty() && url_decode_matches(raw_k, key)) {
                std::string_view raw_v = (eq != std::string_view::npos) ? field.substr(eq + 1) : std::string_view{};
                return url_decode(raw_v);
            }
        }
        if (amp == std::string_view::npos) break;
        q.remove_prefix(amp + 1);
    }
    return {};
}

bool has_form(const Context &ctx, std::string_view key) {
    if (key.empty()) return false;
    if (is_multipart_form_data(ctx.content_type())) {
        const auto boundary = MultiPartParser::extract_boundary(ctx.content_type());
        if (boundary.empty()) return false;
        return MultiPartParser::find_field(ctx.req_body(), boundary, key).has_value();
    }

    std::string_view q = ctx.req_body();
    while (!q.empty()) {
        size_t amp = q.find('&');
        std::string_view field = (amp != std::string_view::npos) ? q.substr(0, amp) : q;
        if (!field.empty()) {
            size_t eq = field.find('=');
            std::string_view raw_k = (eq != std::string_view::npos) ? field.substr(0, eq) : field;
            if (!raw_k.empty() && url_decode_matches(raw_k, key)) {
                return true;
            }
        }
        if (amp == std::string_view::npos) break;
        q.remove_prefix(amp + 1);
    }
    return false;
}

} // namespace kale
