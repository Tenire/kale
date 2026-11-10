#include "kale/http/multipart.h"

#include "kale/core/context.h"
#include "http/multipart_parser.h"

namespace kale {

std::vector<FormFile> parse_multipart(const Context &ctx) {
    const auto boundary = MultiPartParser::extract_boundary(ctx.content_type());
    return boundary.empty() ? std::vector<FormFile>{} : MultiPartParser::parse(ctx.req_body(), boundary);
}

std::optional<FormFile> form_file(const Context &ctx, std::string_view key) {
    auto parts = parse_multipart(ctx);
    for (auto &part : parts) {
        if (part.name == key) return std::move(part);
    }
    return std::nullopt;
}

} // namespace kale
