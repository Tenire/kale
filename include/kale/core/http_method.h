#ifndef KALE_HTTP_METHOD_H
#define KALE_HTTP_METHOD_H

#include <string_view>

namespace kale {

enum class HttpMethod {
    UNKNOWN = 0,
    GET,
    POST,
    PUT,
    DELETE,
    PATCH,
    HEAD,
    OPTIONS,
    CONNECT,
    TRACE
};

constexpr std::string_view method_to_string(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::DELETE:  return "DELETE";
        case HttpMethod::PATCH:   return "PATCH";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::CONNECT: return "CONNECT";
        case HttpMethod::TRACE:   return "TRACE";
        default:                  return "UNKNOWN";
    }
}

inline HttpMethod string_to_method(std::string_view str) noexcept {
    if (str == "GET")     return HttpMethod::GET;
    if (str == "POST")    return HttpMethod::POST;
    if (str == "PUT")     return HttpMethod::PUT;
    if (str == "DELETE")  return HttpMethod::DELETE;
    if (str == "PATCH")   return HttpMethod::PATCH;
    if (str == "HEAD")    return HttpMethod::HEAD;
    if (str == "OPTIONS") return HttpMethod::OPTIONS;
    if (str == "CONNECT") return HttpMethod::CONNECT;
    if (str == "TRACE")   return HttpMethod::TRACE;
    return HttpMethod::UNKNOWN;
}

} // namespace kale

#endif // KALE_HTTP_METHOD_H
