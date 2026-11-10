#ifndef KALE_UPLOAD_H
#define KALE_UPLOAD_H

#include <string>
#include <string_view>

#include "coke/task.h"
#include "kale/http/multipart.h"
namespace kale {

class Context;

/**
 * @brief Saves an uploaded multipart file to destination.
 * @return 0 on success, -ENOENT if field is missing or not a file, or negative errno on failure.
 */
coke::Task<int> save_upload(const Context &ctx, std::string_view field, std::string dest, bool append = false);

/**
 * @brief Saves request body to destination.
 * @return 0 on success, or negative errno on failure.
 */
coke::Task<int> save_body(const Context &ctx, std::string dest, bool append = false);

/**
 * @brief Saves a FormFile payload to destination.
 * Backing file.data buffer must remain valid until the task completes.
 * @return 0 on success, or negative errno on failure.
 */
coke::Task<int> save_form_file(const FormFile &file, std::string dest, bool append = false);

} // namespace kale

#endif // KALE_UPLOAD_H
