#include "kale/http/upload.h"

#include <cerrno>
#include <utility>

#include "kale/core/context.h"
#include "http/file_util.h"

namespace kale {

namespace {

coke::Task<int> make_error_task(int err) {
    co_return err;
}

coke::Task<int> do_write_file(std::string dest, std::string_view data, bool append) {
    co_return co_await FileUtil::write_file(dest, data, append);
}

} // namespace

coke::Task<int> save_body(const Context &ctx, std::string dest, bool append) {
    return do_write_file(std::move(dest), ctx.req_body(), append);
}

coke::Task<int> save_form_file(const FormFile &file, std::string dest, bool append) {
    return do_write_file(std::move(dest), file.data, append);
}

coke::Task<int> save_upload(const Context &ctx, std::string_view field, std::string dest, bool append) {
    auto f = form_file(ctx, field);
    if (!f) {
        return make_error_task(-ENOENT);
    }
    return do_write_file(std::move(dest), f->data, append);
}

} // namespace kale
