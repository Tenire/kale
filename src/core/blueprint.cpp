#include "kale/core/blueprint.h"
#include "kale/core/http_server.h"
#include "kale/core/router.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kale {

std::string normalize_prefix(std::string_view prefix) {
    if (prefix.empty()) {
        return "";
    }
    size_t first = prefix.find_first_not_of('/');
    if (first == std::string_view::npos) {
        return "";
    }
    if (prefix.front() != '/') {
        throw std::invalid_argument("Prefix must begin with '/': " + std::string(prefix));
    }
    size_t last = prefix.find_last_not_of('/');
    return "/" + std::string(prefix.substr(first, last - first + 1));
}


namespace {


std::string join_prefix(std::string_view parent, std::string_view child) {
    std::string norm_parent = normalize_prefix(parent);
    std::string norm_child = normalize_prefix(child);
    if (norm_parent.empty()) return norm_child;
    if (norm_child.empty()) return norm_parent;
    return norm_parent + norm_child;
}

std::string join_route_path(std::string_view prefix, std::string_view path) {
    std::string norm_prefix = normalize_prefix(prefix);
    if (norm_prefix.empty()) {
        if (path.empty()) return "/";
        if (path.front() != '/') return "/" + std::string(path);
        return std::string(path);
    }
    if (path.empty()) {
        return norm_prefix;
    }
    if (path.front() == '/') {
        return norm_prefix + std::string(path);
    }
    return norm_prefix + "/" + std::string(path);
}

} // namespace

Router &Router::mount(std::string_view prefix, Blueprint blueprint) {
    blueprint.register_into(*this, prefix);
    return *this;
}

Router &Router::mount(Blueprint blueprint) {
    return mount("", std::move(blueprint));
}

HttpServer &HttpServer::mount(std::string_view prefix, Blueprint blueprint) {
    router_.mount(prefix, std::move(blueprint));
    return *this;
}

HttpServer &HttpServer::mount(Blueprint blueprint) {
    return mount("", std::move(blueprint));
}

void Blueprint::register_into(
    Router &router,
    std::string_view current_prefix,
    const std::vector<Middleware> &parent_mws,
    const std::map<std::string, std::string, std::less<>> &parent_tags) const
{
    std::string norm_prefix = normalize_prefix(current_prefix);

    std::vector<Middleware> combined_mws;
    combined_mws.reserve(parent_mws.size() + middlewares_.size());
    combined_mws.insert(combined_mws.end(), parent_mws.begin(), parent_mws.end());
    combined_mws.insert(combined_mws.end(), middlewares_.begin(), middlewares_.end());

    std::map<std::string, std::string, std::less<>> combined_tags = parent_tags;
    for (const auto &[k, v] : tags_) {
        combined_tags[k] = v;
    }

    for (const auto &decl : routes_) {
        std::string full_path = join_route_path(norm_prefix, decl->path);

        std::vector<Middleware> route_final_mws;
        route_final_mws.reserve(combined_mws.size() + decl->endpoint.middlewares.size());
        route_final_mws.insert(route_final_mws.end(), combined_mws.begin(), combined_mws.end());
        route_final_mws.insert(route_final_mws.end(),
                               decl->endpoint.middlewares.begin(),
                               decl->endpoint.middlewares.end());

        auto route_final_tags = combined_tags;
        for (const auto &[k, v] : decl->endpoint.tags) {
            route_final_tags[k] = v;
        }

        // Handlers are copied so Blueprint declarations remain reusable.
        kale::RouteBuilder builder = router.add_route(
            decl->method,
            std::move(full_path),
            decl->endpoint.handler,
            std::move(route_final_mws)
        );

        if (!decl->endpoint.name.empty()) {
            builder.name(decl->endpoint.name);
        }

        for (auto &[k, v] : route_final_tags) {
            builder.tag(k, std::move(v));
        }

        if (decl->endpoint.max_body_size > 0) {
            builder.max_body_size(decl->endpoint.max_body_size);
        }
    }

    for (const auto &child : children_) {
        if (child.blueprint) {
            std::string next_prefix = join_prefix(norm_prefix, child.prefix);
            child.blueprint->register_into(router, next_prefix, combined_mws, combined_tags);
        }
    }
}

} // namespace kale
