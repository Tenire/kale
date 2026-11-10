#include "kale/core/router.h"
#include "kale/core/context.h"

// Non-dumpable return type without text conversion or dump() method
struct NonDumpableType {
    int id{42};
};

int main() {
    // Synchronous handler returning non-dumpable custom type must fail static_assert
    kale::Router router;
    router.GET("/invalid_sync", [](kale::Context &) {
        return NonDumpableType{};
    });
    return 0;
}
