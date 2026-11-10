#ifndef KALE_FILE_RESPONSE_H
#define KALE_FILE_RESPONSE_H

#include <cstddef>
#include <string_view>

#include "coke/task.h"

namespace kale {

class Context;

/**
 * @brief Asynchronously transmits a file within byte range [start, end).
 * @return 0 on success, or negative errno on failure.
 */
coke::Task<int> send_file(Context &ctx, std::string_view path, size_t start = 0, size_t end = static_cast<size_t>(-1));

/**
 * @brief Asynchronously transmits a file satisfying a single byte range per RFC 7233.
 * Sets 206 Partial Content or 416 Range Not Satisfiable as appropriate.
 * @return 0 on success, or negative errno on failure.
 */
coke::Task<int> send_file_range(Context &ctx, std::string_view path, std::string_view range_header);

} // namespace kale

#endif // KALE_FILE_RESPONSE_H
