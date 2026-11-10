#include "kale/middleware/request_id.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <sys/random.h>
#else
#include <random>
#endif

#include "kale/core/http_util.h"
#include "workflow/HttpUtil.h"

namespace kale {
namespace {

bool valid_id(std::string_view value, std::size_t limit) noexcept {
    if (value.empty() || value.size() > limit) return false;
    for (unsigned char byte : value) {
        if (byte < 33 || byte > 126) return false;
    }
    return true;
}

bool incoming_id(const Context &ctx, std::string_view name, std::size_t limit, std::string_view &value) noexcept {
    protocol::HttpMessageHeader header;
    header.name = name.data();
    header.name_len = name.size();
    protocol::HttpHeaderCursor cursor(&ctx.raw_req());
    if (!cursor.find(&header)) return false;
    value = {static_cast<const char *>(header.value), header.value_len};
    return valid_id(value, limit) && !cursor.find(&header);
}

} // namespace

std::string default_request_id_generator() {
    std::array<unsigned char, 16> bytes{};
#if defined(__linux__)
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, GRND_NONBLOCK);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "request ID getrandom");
        }
        if (count == 0) throw std::system_error(EIO, std::generic_category(), "request ID getrandom");
        offset += static_cast<std::size_t>(count);
    }
#else
    std::random_device random;
    std::uniform_int_distribution<unsigned int> distribution(0, 255);
    for (auto &byte : bytes) byte = static_cast<unsigned char>(distribution(random));
#endif
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(32, 0);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        result[2 * i] = digits[bytes[i] >> 4];
        result[2 * i + 1] = digits[bytes[i] & 15];
    }
    return result;
}

Middleware request_id(RequestIdConfig config) {
    if (!is_valid_header_name(config.header_name)) throw std::invalid_argument("invalid request ID header name");
    if (config.max_length == 0 || config.max_length > 4096 || (!config.generator && config.max_length < 32)) {
        throw std::invalid_argument("invalid request ID length limit");
    }
    if (!config.generator) config.generator = default_request_id_generator;
    return [config = std::move(config)](Context &ctx, Next next) -> coke::Task<void> {
        std::string_view incoming;
        std::string selected;
        if (incoming_id(ctx, config.header_name, config.max_length, incoming)) {
            selected.assign(incoming);
        } else {
            selected = config.generator();
            if (!valid_id(selected, config.max_length)) throw std::invalid_argument("invalid generated request ID");
        }
        ctx.set_attachment(RequestId{std::move(selected)});
        ctx.set_header(config.header_name, get_request_id(ctx));
        co_await next();
    };
}

} // namespace kale
