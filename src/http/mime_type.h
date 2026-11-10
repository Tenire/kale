#ifndef KALE_MIME_TYPE_H
#define KALE_MIME_TYPE_H

#include <string_view>

namespace kale {

// Accepts an extension or a path; only the basename participates in detection.
// ASCII folding avoids allocation and locale-dependent behavior.
inline std::string_view get_mime_type(std::string_view name) noexcept {
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
        const size_t dot = name.rfind('.');
        if (dot == std::string_view::npos) {
            return "application/octet-stream";
        }
        name.remove_prefix(dot + 1);
    } else if (const size_t dot = name.rfind('.'); dot != std::string_view::npos) {
        name.remove_prefix(dot + 1);
    }
    const auto is = [name](std::string_view extension) noexcept {
        if (name.size() != extension.size()) {
            return false;
        }
        for (size_t i = 0; i < name.size(); ++i) {
            const char c = name[i] >= 'A' && name[i] <= 'Z' ? name[i] + ('a' - 'A') : name[i];
            if (c != extension[i]) {
                return false;
            }
        }
        return true;
    };

    if (is("html") || is("htm")) return "text/html; charset=utf-8";
    if (is("css")) return "text/css; charset=utf-8";
    if (is("js") || is("mjs") || is("cjs")) return "application/javascript; charset=utf-8";
    if (is("json") || is("map")) return "application/json; charset=utf-8";
    if (is("txt") || is("text") || is("log")) return "text/plain; charset=utf-8";
    if (is("csv")) return "text/csv; charset=utf-8";
    if (is("md")) return "text/markdown; charset=utf-8";
    if (is("xml")) return "application/xml; charset=utf-8";
    if (is("xhtml")) return "application/xhtml+xml";
    if (is("yaml") || is("yml")) return "application/yaml";
    if (is("ics")) return "text/calendar; charset=utf-8";
    if (is("svg")) return "image/svg+xml";
    if (is("png")) return "image/png";
    if (is("jpg") || is("jpeg")) return "image/jpeg";
    if (is("gif")) return "image/gif";
    if (is("webp")) return "image/webp";
    if (is("avif")) return "image/avif";
    if (is("ico")) return "image/x-icon";
    if (is("bmp")) return "image/bmp";
    if (is("tif") || is("tiff")) return "image/tiff";
    if (is("mp4") || is("m4v")) return "video/mp4";
    if (is("webm")) return "video/webm";
    if (is("ogv")) return "video/ogg";
    if (is("mov")) return "video/quicktime";
    if (is("avi")) return "video/x-msvideo";
    if (is("mpeg") || is("mpg")) return "video/mpeg";
    if (is("mp3")) return "audio/mpeg";
    if (is("wav")) return "audio/wav";
    if (is("ogg") || is("oga") || is("opus")) return "audio/ogg";
    if (is("flac")) return "audio/flac";
    if (is("aac")) return "audio/aac";
    if (is("m4a")) return "audio/mp4";
    if (is("woff")) return "font/woff";
    if (is("woff2")) return "font/woff2";
    if (is("ttf")) return "font/ttf";
    if (is("otf")) return "font/otf";
    if (is("wasm")) return "application/wasm";
    if (is("pdf")) return "application/pdf";
    if (is("zip")) return "application/zip";
    if (is("tar")) return "application/x-tar";
    if (is("gz")) return "application/gzip";
    if (is("bz2")) return "application/x-bzip2";
    if (is("xz")) return "application/x-xz";
    if (is("7z")) return "application/x-7z-compressed";
    if (is("rar")) return "application/vnd.rar";
    if (is("doc")) return "application/msword";
    if (is("docx")) return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (is("xls")) return "application/vnd.ms-excel";
    if (is("xlsx")) return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (is("ppt")) return "application/vnd.ms-powerpoint";
    if (is("pptx")) return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    return "application/octet-stream";
}

} // namespace kale

#endif // KALE_MIME_TYPE_H
