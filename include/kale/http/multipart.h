#ifndef KALE_MULTIPART_H
#define KALE_MULTIPART_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kale {

class Context;

// Form file with owned metadata and a borrowed payload view (data). For parsed
// requests, data references the body buffer and remains valid while Context lives.
struct FormFile {
    std::string name;
    std::string filename;
    std::string content_type;
    std::string_view data;
};

/**
 * @brief Parses multipart/form-data into FormFile entries using Content-Type boundary.
 * @return Parsed FormFile entries, or empty vector if not multipart or boundary is invalid.
 */
std::vector<FormFile> parse_multipart(const Context &ctx);

/**
 * @brief Returns the first FormFile matching key, or std::nullopt if not found.
 */
std::optional<FormFile> form_file(const Context &ctx, std::string_view key);

} // namespace kale

#endif // KALE_MULTIPART_H
