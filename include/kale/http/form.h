#ifndef KALE_FORM_H
#define KALE_FORM_H

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kale/http/multipart.h"

namespace kale {

class Context;

/**
 * @brief Parses application/x-www-form-urlencoded or multipart/form-data body into a key-value map.
 * File parts with non-empty filenames are excluded.
 */
std::map<std::string, std::string, std::less<>> parse_form(const Context &ctx);

/**
 * @brief Returns the form field value matching key, or an empty string if missing.
 */
std::string form(const Context &ctx, std::string_view key);

bool has_form(const Context &ctx, std::string_view key);

std::vector<FormFile> parse_multipart(const Context &ctx);
std::optional<FormFile> form_file(const Context &ctx, std::string_view key);

} // namespace kale

#endif // KALE_FORM_H
