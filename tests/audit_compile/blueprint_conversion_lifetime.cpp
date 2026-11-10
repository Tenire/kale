#include "kale/core/blueprint.h"
#include "kale/core/router.h"

int main() {
    static_assert(!std::is_convertible_v<decltype(std::declval<kale::Blueprint>().GET("/temp", [](kale::Context &) {})), kale::RouteBuilder>,
                  "Rvalue Blueprint::RouteBuilder must not convert to non-owning kale::RouteBuilder");

    auto rb = kale::Blueprint().GET("/temp", [](kale::Context &ctx) {
        ctx.string("ok");
    });
    rb.name("assigned_name");
    rb.tag("tag_k", "tag_v");
    return 0;
}
