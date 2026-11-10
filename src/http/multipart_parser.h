#ifndef KALE_SRC_MULTIPART_PARSER_H
#define KALE_SRC_MULTIPART_PARSER_H

#include "kale/http/multipart.h"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace kale {

class MultiPartParser {
public:
    static bool case_eq(std::string_view s1, std::string_view s2) noexcept {
        if (s1.size() != s2.size()) return false;
        for (size_t i = 0; i < s1.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(s1[i])) != std::tolower(static_cast<unsigned char>(s2[i]))) {
                return false;
            }
        }
        return true;
    }

    static std::string_view extract_boundary(std::string_view content_type) {
        size_t pos = content_type.find(';');
        while (pos != std::string_view::npos) {
            content_type.remove_prefix(pos + 1);
            while (!content_type.empty() && (content_type.front() == ' ' || content_type.front() == '\t')) content_type.remove_prefix(1);
            const size_t equals = content_type.find('=');
            const size_t semi = content_type.find(';');
            if (equals == std::string_view::npos || (semi != std::string_view::npos && semi < equals)) { pos = semi; continue; }
            auto key = content_type.substr(0, equals);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
            auto value = content_type.substr(equals + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            if (value.starts_with('"')) {
                value.remove_prefix(1);
                const size_t quote = value.find('"');
                if (quote == std::string_view::npos) return {};
                if (case_eq(key, "boundary")) return value.substr(0, quote);
                pos = content_type.find(';', equals + 2 + quote);
            } else {
                value = value.substr(0, value.find(';'));
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
                if (case_eq(key, "boundary")) return value;
                pos = semi;
            }
        }
        return {};
    }

    static std::vector<FormFile> parse(std::string_view body, std::string_view boundary) {
        std::vector<FormFile> files;
        if (boundary.empty() || body.empty()) {
            return files;
        }

        std::string dash_boundary = "--" + std::string(boundary);

        size_t pos = 0;
        while (pos < body.size()) {
            size_t bstart = body.find(dash_boundary, pos);
            if (bstart == std::string_view::npos) {
                break;
            }
            // Boundary must be at body start or preceded by newline
            if (bstart > 0 && body[bstart - 1] != '\n') {
                pos = bstart + dash_boundary.size();
                continue;
            }

            pos = bstart + dash_boundary.size();
            if (pos + 2 <= body.size() && body.substr(pos, 2) == "--") {
                break;
            }

            if (pos + 2 <= body.size() && body.substr(pos, 2) == "\r\n") {
                pos += 2;
            } else if (pos < body.size() && body[pos] == '\n') {
                pos += 1;
            }

            size_t header_end = body.find("\r\n\r\n", pos);
            size_t body_start = header_end + 4;
            if (header_end == std::string_view::npos) {
                header_end = body.find("\n\n", pos);
                body_start = header_end + 2;
            }
            if (header_end == std::string_view::npos) {
                break;
            }

            std::string_view headers_part = body.substr(pos, header_end - pos);
            FormFile file;

            parse_headers(headers_part, file);

            size_t search = body_start;
            size_t next_b = std::string_view::npos;
            size_t b_offset = 0;
            while (search < body.size()) {
                const size_t newline = body.find('\n', search);
                if (newline == std::string_view::npos) break;
                const size_t marker = newline + 1;
                const size_t suffix = marker + dash_boundary.size();
                if (body.substr(marker).starts_with(dash_boundary) &&
                    (body.substr(suffix).starts_with("--") || body.substr(suffix).starts_with("\r\n") ||
                     body.substr(suffix).starts_with("\n"))) {
                    const bool crlf = newline > 0 && body[newline - 1] == '\r';
                    next_b = crlf ? newline - 1 : newline;
                    b_offset = crlf ? 2 : 1;
                    break;
                }
                search = newline + 1;
            }
            if (next_b == std::string_view::npos) break;

            size_t data_end = next_b;
            file.data = body.substr(body_start, data_end - body_start);
            files.push_back(std::move(file));
            pos = next_b + b_offset;
        }

        return files;
    }

    static std::string_view extract_disposition_param(std::string_view s, std::string_view key) noexcept {
        size_t pos = 0;
        while (pos < s.size()) {
            size_t p = s.find(key, pos);
            if (p == std::string_view::npos) return {};
            if (p == 0 || s[p - 1] == ' ' || s[p - 1] == '\t' || s[p - 1] == ';') {
                std::string_view val = s.substr(p + key.size());
                if (!val.empty() && val.front() == '"') {
                    val.remove_prefix(1);
                    size_t q = val.find('"');
                    if (q != std::string_view::npos) return val.substr(0, q);
                    return val;
                } else {
                    size_t semi = val.find(';');
                    if (semi != std::string_view::npos) val = val.substr(0, semi);
                    while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.remove_suffix(1);
                    return val;
                }
            }
            pos = p + key.size();
        }
        return {};
    }

    static std::optional<std::string_view> find_field(std::string_view body, std::string_view boundary, std::string_view target_name) {
        if (boundary.empty() || body.empty()) {
            return std::nullopt;
        }

        std::string dash_boundary = "--" + std::string(boundary);
        std::optional<std::string_view> winner;
        size_t pos = 0;
        while (pos < body.size()) {
            size_t bstart = body.find(dash_boundary, pos);
            if (bstart == std::string_view::npos) {
                break;
            }
            if (bstart > 0 && body[bstart - 1] != '\n') {
                pos = bstart + dash_boundary.size();
                continue;
            }

            pos = bstart + dash_boundary.size();
            if (pos + 2 <= body.size() && body.substr(pos, 2) == "--") {
                break;
            }

            if (pos + 2 <= body.size() && body.substr(pos, 2) == "\r\n") {
                pos += 2;
            } else if (pos < body.size() && body[pos] == '\n') {
                pos += 1;
            }

            size_t header_end = body.find("\r\n\r\n", pos);
            size_t body_start = header_end + 4;
            if (header_end == std::string_view::npos) {
                header_end = body.find("\n\n", pos);
                body_start = header_end + 2;
            }
            if (header_end == std::string_view::npos) {
                break;
            }

            std::string_view headers_str = body.substr(pos, header_end - pos);
            std::string_view part_name;
            std::string_view part_filename;

            while (!headers_str.empty()) {
                size_t line_end = headers_str.find("\r\n");
                size_t step = 2;
                if (line_end == std::string_view::npos) {
                    line_end = headers_str.find('\n');
                    step = 1;
                }
                std::string_view line = (line_end != std::string_view::npos) ? headers_str.substr(0, line_end) : headers_str;

                size_t colon = line.find(':');
                if (colon != std::string_view::npos) {
                    std::string_view k = line.substr(0, colon);
                    std::string_view v = line.substr(colon + 1);
                    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);

                    if (case_eq(k, "Content-Disposition")) {
                        std::string_view n = extract_disposition_param(v, "name=");
                        if (!n.empty()) part_name = n;
                        std::string_view fn = extract_disposition_param(v, "filename=");
                        if (!fn.empty()) part_filename = fn;
                    }
                }

                if (line_end == std::string_view::npos) break;
                headers_str.remove_prefix(line_end + step);
            }

            size_t search = body_start;
            size_t next_b = std::string_view::npos;
            size_t b_offset = 0;
            while (search < body.size()) {
                const size_t newline = body.find('\n', search);
                if (newline == std::string_view::npos) break;
                const size_t marker = newline + 1;
                const size_t suffix = marker + dash_boundary.size();
                if (body.substr(marker).starts_with(dash_boundary) &&
                    (body.substr(suffix).starts_with("--") || body.substr(suffix).starts_with("\r\n") ||
                     body.substr(suffix).starts_with("\n"))) {
                    const bool crlf = newline > 0 && body[newline - 1] == '\r';
                    next_b = crlf ? newline - 1 : newline;
                    b_offset = crlf ? 2 : 1;
                    break;
                }
                search = newline + 1;
            }
            if (next_b == std::string_view::npos) break;

            size_t data_end = next_b;
            if (part_filename.empty() && part_name == target_name) {
                winner = body.substr(body_start, data_end - body_start);
            }
            pos = next_b + b_offset;
        }
        return winner;
    }

private:
    static void parse_headers(std::string_view headers_str, FormFile &file) {
        while (!headers_str.empty()) {
            size_t line_end = headers_str.find("\r\n");
            size_t step = 2;
            if (line_end == std::string_view::npos) {
                line_end = headers_str.find('\n');
                step = 1;
            }
            std::string_view line = (line_end != std::string_view::npos) ? headers_str.substr(0, line_end) : headers_str;

            size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string_view k = line.substr(0, colon);
                std::string_view v = line.substr(colon + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);

                if (case_eq(k, "Content-Disposition")) {
                    parse_disposition(v, file);
                } else if (case_eq(k, "Content-Type")) {
                    file.content_type = std::string(v);
                }
            }

            if (line_end == std::string_view::npos) break;
            headers_str.remove_prefix(line_end + step);
        }
    }
    static void parse_disposition(std::string_view disp, FormFile &file) {
        auto extract_param = [](std::string_view s, std::string_view key) -> std::string {
            size_t pos = 0;
            while (pos < s.size()) {
                size_t p = s.find(key, pos);
                if (p == std::string_view::npos) return "";
                if (p == 0 || s[p - 1] == ' ' || s[p - 1] == '\t' || s[p - 1] == ';') {
                    std::string_view val = s.substr(p + key.size());
                    if (!val.empty() && val.front() == '"') {
                        val.remove_prefix(1);
                        size_t q = val.find('"');
                        if (q != std::string_view::npos) return std::string(val.substr(0, q));
                        return std::string(val);
                    } else {
                        size_t semi = val.find(';');
                        if (semi != std::string_view::npos) val = val.substr(0, semi);
                        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.remove_suffix(1);
                        return std::string(val);
                    }
                }
                pos = p + key.size();
            }
            return "";
        };

        std::string n = extract_param(disp, "name=");
        if (!n.empty()) file.name = std::move(n);
        std::string fn = extract_param(disp, "filename=");
        if (!fn.empty()) file.filename = std::move(fn);
    }
};

} // namespace kale

#endif // KALE_SRC_MULTIPART_PARSER_H
