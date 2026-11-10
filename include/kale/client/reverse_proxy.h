#ifndef KALE_REVERSE_PROXY_H
#define KALE_REVERSE_PROXY_H

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

#include "kale/core/context.h"
#include "kale/core/router.h"
#include "kale/client/http_client.h"

namespace kale {

struct ProxyOptions {
    std::string target_base_url;
    std::string strip_prefix{};
    bool preserve_host{false};
    bool trust_downstream_forwarded{false};
    std::size_t max_body_bytes{16 * 1024 * 1024};
    std::chrono::milliseconds timeout{0};
    bool stream_response{false};
    std::function<void(const Context &, HttpClientRequest &)> rewrite_request{};
    std::function<void(Context &, const HttpClientResponse &)> modify_response{};

    ProxyOptions() = default;
    /* implicit */ ProxyOptions(std::string target_base_url) : target_base_url(std::move(target_base_url)) {}
};

/**
 * Creates a reverse proxy Handler for forwarding requests to upstream.
 * @throws std::invalid_argument if options.target_base_url is invalid.
 */
Handler proxy(HttpClient &upstream, ProxyOptions options);

Handler proxy(HttpClient &upstream, std::string target_base_url);

} // namespace kale

#endif // KALE_REVERSE_PROXY_H
