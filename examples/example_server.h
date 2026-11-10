#ifndef KALE_EXAMPLES_EXAMPLE_SERVER_H
#define KALE_EXAMPLES_EXAMPLE_SERVER_H

#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>

#include "kale/core/http_server.h"

namespace examples {

class ServerOptions {
public:
    ServerOptions(int argc, char *argv[]) {
        block_signals();
        parse_args(argc, argv);
    }

    ServerOptions(int argc, const char *const argv[]) {
        block_signals();
        parse_args(argc, argv);
    }

    [[nodiscard]] unsigned short port() const noexcept {
        return port_;
    }

private:
    static void block_signals() {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGINT);
        sigaddset(&set, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &set, nullptr) != 0) {
            std::cerr << "Warning: Failed to block SIGINT/SIGTERM in main thread\n";
        }
    }

    template <typename T>
    void parse_args(int argc, T argv) {
        if (argc > 1) {
            std::string arg = argv[1];
            if (arg == "-h" || arg == "--help") {
                std::cout << "Usage: " << (argc > 0 ? argv[0] : "server") << " [PORT]\n"
                          << "  PORT: Optional listen port (default: 8080, 0 for ephemeral port)\n"
                          << "\nEnvironment variables:\n"
                          << "  KALE_EXAMPLE_CERT: Path to TLS certificate PEM\n"
                          << "  KALE_EXAMPLE_KEY:  Path to TLS private key PEM\n";
                std::exit(0);
            }
            try {
                size_t idx = 0;
                int val = std::stoi(arg, &idx);
                if (idx != arg.size() || val < 0 || val > 65535) {
                    throw std::invalid_argument("Port out of range [0, 65535]: " + arg);
                }
                port_ = static_cast<unsigned short>(val);
            } catch (const std::invalid_argument &) {
                throw;
            } catch (const std::exception &e) {
                throw std::invalid_argument("Invalid port argument: " + arg);
            }
        }
        if (argc > 2) {
            throw std::invalid_argument("Too many arguments; only optional positional PORT is supported");
        }
    }

    unsigned short port_{8080};
};

inline int serve(kale::HttpServer &server, const ServerOptions &options) {
    const char *cert_env = std::getenv("KALE_EXAMPLE_CERT");
    const char *key_env = std::getenv("KALE_EXAMPLE_KEY");

    if ((cert_env != nullptr) != (key_env != nullptr)) {
        std::cerr << "Error: Both KALE_EXAMPLE_CERT and KALE_EXAMPLE_KEY must be set to enable TLS\n";
        return 1;
    }

    int ret = server.start(AF_INET, "127.0.0.1", options.port(), cert_env, key_env);
    if (ret != 0) {
        std::cerr << "Error: Failed to start HTTP server on 127.0.0.1:" << options.port() << "\n";
        return 1;
    }

    int actual_port = server.listen_port();
    if (actual_port <= 0) {
        actual_port = options.port();
    }

    const char *scheme = (cert_env && key_env) ? "https" : "http";
    std::cout << "Listening on " << scheme << "://127.0.0.1:" << actual_port << "/" << std::endl;

    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGINT);
    sigaddset(&wait_set, SIGTERM);

    int sig = 0;
    sigwait(&wait_set, &sig);

    std::cout << "\nReceived signal " << sig << ", shutting down..." << std::endl;
    server.stop();
    return 0;
}

} // namespace examples

#endif // KALE_EXAMPLES_EXAMPLE_SERVER_H
