#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <cstdlib>
#include <openssl/crypto.h>

int main(int argc, char **argv) {
    // Initialize crypto before Workflow so its scheduler joins request workers
    // before OpenSSL automatic cleanup releases their thread-local state.
    if (OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr) != 1) {
        return EXIT_FAILURE;
    }
    return doctest::Context(argc, argv).run();
}
