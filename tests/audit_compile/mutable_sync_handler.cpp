#include "kale/core/router.h"
#include "kale/core/context.h"
#include <string>

// Direct adapters invoke their stored callable through a const reference.
// The concept must reject a non-const call operator before adapter instantiation.
int main() {
    auto mutable_lambda = [count = 0](kale::Context &ctx) mutable {
        ++count;
        ctx.string("count: " + std::to_string(count));
    };
    static_assert(!kale::HandlerCallable<decltype(mutable_lambda)>,
                  "Mutable stateful lambdas must be rejected by HandlerCallable");
    return 0;
}
