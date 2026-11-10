#ifndef KALE_BLUEPRINT_H
#define KALE_BLUEPRINT_H

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <kale/core/router.h>

namespace kale {

std::string normalize_prefix(std::string_view prefix);

class Blueprint {
public:
    struct RouteDeclaration {
        HttpMethod method{HttpMethod::GET};
        std::string path;
        RouteEndpoint endpoint;
    };

    struct ChildMount;

    class RouteBuilder {
    public:
        RouteBuilder() = default;

        RouteBuilder(Blueprint *bp, std::vector<RouteDeclaration *> decls)
            : bp_(bp), decls_(std::move(decls)) {}

        RouteBuilder(std::shared_ptr<Blueprint> owned, std::vector<RouteDeclaration *> decls)
            : owned_bp_(std::move(owned)), decls_(std::move(decls)) {}

        RouteBuilder(const RouteBuilder &) = default;
        RouteBuilder &operator=(const RouteBuilder &) = default;
        RouteBuilder(RouteBuilder &&) noexcept = default;
        RouteBuilder &operator=(RouteBuilder &&) noexcept = default;
        ~RouteBuilder() = default;

        RouteBuilder &name(std::string route_name) & {
            for (auto *d : decls_) {
                if (d) d->endpoint.name = route_name;
            }
            return *this;
        }

        RouteBuilder &&name(std::string route_name) && {
            for (auto *d : decls_) {
                if (d) d->endpoint.name = route_name;
            }
            return std::move(*this);
        }

        RouteBuilder &tag(std::string key, std::string val) & {
            for (auto *d : decls_) {
                if (d) d->endpoint.tags[key] = val;
            }
            return *this;
        }

        RouteBuilder &&tag(std::string key, std::string val) && {
            for (auto *d : decls_) {
                if (d) d->endpoint.tags[key] = val;
            }
            return std::move(*this);
        }

        RouteBuilder &max_body_size(size_t limit) & {
            for (auto *d : decls_) {
                if (d) d->endpoint.max_body_size = limit;
            }
            return *this;
        }

        RouteBuilder &&max_body_size(size_t limit) && {
            for (auto *d : decls_) {
                if (d) d->endpoint.max_body_size = limit;
            }
            return std::move(*this);
        }

        // Borrowed builder view; valid only while the underlying Blueprint exists.
        operator kale::RouteBuilder() & {
            if (decls_.empty() || !decls_.front()) {
                static RouteEndpoint dummy;
                return kale::RouteBuilder(dummy);
            }
            return kale::RouteBuilder(decls_.front()->endpoint);
        }

        operator kale::RouteBuilder() const & {
            if (decls_.empty() || !decls_.front()) {
                static RouteEndpoint dummy;
                return kale::RouteBuilder(dummy);
            }
            return kale::RouteBuilder(decls_.front()->endpoint);
        }

        operator kale::RouteBuilder() && = delete;
        operator kale::RouteBuilder() const && = delete;
        operator Blueprint() &&;
        operator Blueprint() const &;

        Blueprint &blueprint() noexcept;
        const Blueprint &blueprint() const noexcept;

        const std::vector<RouteDeclaration *> &declarations() const noexcept { return decls_; }

        template <typename... Args>
            requires (sizeof...(Args) >= 1)
        RouteBuilder route(HttpMethod method, std::string path, Args &&...args);

        template <typename... Args>
            requires (sizeof...(Args) >= 1)
        RouteBuilder any(std::string path, Args &&...args);

        template <typename... Args>
            requires (sizeof...(Args) >= 1)
        RouteBuilder GET(std::string path, Args &&...args);

        template <typename... Args>
            requires (sizeof...(Args) >= 1)
        RouteBuilder POST(std::string path, Args &&...args);

        template <typename... Args>
            requires (sizeof...(Args) >= 1)
        RouteBuilder PUT(std::string path, Args &&...args);

        template <typename... Args>
            requires (sizeof...(Args) >= 1)
        RouteBuilder DELETE(std::string path, Args &&...args);

        template <typename... Args>
            requires (sizeof...(Args) >= 1)
        RouteBuilder PATCH(std::string path, Args &&...args);

        template <typename... Args>
            requires (sizeof...(Args) >= 1)
        RouteBuilder HEAD(std::string path, Args &&...args);

        template <typename... Args>
            requires (sizeof...(Args) >= 1)
        RouteBuilder OPTIONS(std::string path, Args &&...args);

        RouteBuilder &use(Middleware mw) &;
        RouteBuilder &&use(Middleware mw) &&;

        template <typename M>
            requires MiddlewareCallable<M>
        RouteBuilder &use(M &&m) &;

        template <typename M>
            requires MiddlewareCallable<M>
        RouteBuilder &&use(M &&m) &&;

        RouteBuilder &group_tag(std::string key, std::string val) &;
        RouteBuilder &&group_tag(std::string key, std::string val) &&;

        RouteBuilder &mount(std::string_view prefix, Blueprint bp) &;
        RouteBuilder &&mount(std::string_view prefix, Blueprint bp) &&;
        RouteBuilder &mount(Blueprint bp) &;
        RouteBuilder &&mount(Blueprint bp) &&;
    private:
        Blueprint *bp_{nullptr};
        std::shared_ptr<Blueprint> owned_bp_;
        std::vector<RouteDeclaration *> decls_;
    };

    Blueprint() = default;
    ~Blueprint();

    Blueprint(const Blueprint &other);
    Blueprint &operator=(const Blueprint &other);

    Blueprint(Blueprint &&) noexcept;
    Blueprint &operator=(Blueprint &&) noexcept;

    RouteBuilder add_route(HttpMethod method, std::string path, Handler handler, std::vector<Middleware> middlewares = {}) &;
    RouteBuilder add_route(HttpMethod method, std::string path, Handler handler, std::vector<Middleware> middlewares = {}) &&;

    template <HandlerCallable H>
        requires (!std::is_same_v<std::decay_t<H>, Handler>)
    RouteBuilder add_route(HttpMethod method, std::string path, H &&handler, std::vector<Middleware> middlewares = {}) & {
        return add_route(method, std::move(path), make_handler(std::forward<H>(handler)), std::move(middlewares));
    }

    template <HandlerCallable H>
        requires (!std::is_same_v<std::decay_t<H>, Handler>)
    RouteBuilder add_route(HttpMethod method, std::string path, H &&handler, std::vector<Middleware> middlewares = {}) && {
        return std::move(*this).add_route(method, std::move(path), make_handler(std::forward<H>(handler)), std::move(middlewares));
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder route(HttpMethod method, std::string path, Args&&... args) & {
        return add_route_variadic(method, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder route(HttpMethod method, std::string path, Args&&... args) && {
        return std::move(*this).add_route_variadic(method, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder any(std::string path, Args&&... args) &;

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder any(std::string path, Args&&... args) &&;

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder GET(std::string path, Args&&... args) & {
        return route(HttpMethod::GET, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder GET(std::string path, Args&&... args) && {
        return std::move(*this).route(HttpMethod::GET, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder POST(std::string path, Args&&... args) & {
        return route(HttpMethod::POST, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder POST(std::string path, Args&&... args) && {
        return std::move(*this).route(HttpMethod::POST, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder PUT(std::string path, Args&&... args) & {
        return route(HttpMethod::PUT, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder PUT(std::string path, Args&&... args) && {
        return std::move(*this).route(HttpMethod::PUT, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder DELETE(std::string path, Args&&... args) & {
        return route(HttpMethod::DELETE, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder DELETE(std::string path, Args&&... args) && {
        return std::move(*this).route(HttpMethod::DELETE, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder PATCH(std::string path, Args&&... args) & {
        return route(HttpMethod::PATCH, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder PATCH(std::string path, Args&&... args) && {
        return std::move(*this).route(HttpMethod::PATCH, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder HEAD(std::string path, Args&&... args) & {
        return route(HttpMethod::HEAD, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder HEAD(std::string path, Args&&... args) && {
        return std::move(*this).route(HttpMethod::HEAD, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder OPTIONS(std::string path, Args&&... args) & {
        return route(HttpMethod::OPTIONS, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder OPTIONS(std::string path, Args&&... args) && {
        return std::move(*this).route(HttpMethod::OPTIONS, std::move(path), std::forward<Args>(args)...);
    }

    Blueprint &use(Middleware mw) & {
        middlewares_.push_back(std::move(mw));
        return *this;
    }

    Blueprint &&use(Middleware mw) && {
        middlewares_.push_back(std::move(mw));
        return std::move(*this);
    }

    template <typename M>
        requires MiddlewareCallable<M>
    Blueprint &use(M &&m) & {
        return use(make_middleware(std::forward<M>(m)));
    }

    template <typename M>
        requires MiddlewareCallable<M>
    Blueprint &&use(M &&m) && {
        middlewares_.push_back(make_middleware(std::forward<M>(m)));
        return std::move(*this);
    }

    template <typename M1, typename M2, typename... More>
        requires MiddlewareCallable<M1> && MiddlewareCallable<M2>
    Blueprint &use(M1 &&m1, M2 &&m2, More&&... more) & {
        use(std::forward<M1>(m1));
        use(std::forward<M2>(m2));
        (use(std::forward<More>(more)), ...);
        return *this;
    }

    template <typename M1, typename M2, typename... More>
        requires MiddlewareCallable<M1> && MiddlewareCallable<M2>
    Blueprint &&use(M1 &&m1, M2 &&m2, More&&... more) && {
        middlewares_.push_back(make_middleware(std::forward<M1>(m1)));
        middlewares_.push_back(make_middleware(std::forward<M2>(m2)));
        (middlewares_.push_back(make_middleware(std::forward<More>(more))), ...);
        return std::move(*this);
    }

    Blueprint &tag(std::string key, std::string val) & {
        tags_[std::move(key)] = std::move(val);
        return *this;
    }

    Blueprint &&tag(std::string key, std::string val) && {
        tags_[std::move(key)] = std::move(val);
        return std::move(*this);
    }

    Blueprint &mount(std::string_view prefix, Blueprint blueprint) &;
    Blueprint &&mount(std::string_view prefix, Blueprint blueprint) &&;

    Blueprint &mount(Blueprint blueprint) & {
        return mount("", std::move(blueprint));
    }

    Blueprint &&mount(Blueprint blueprint) && {
        return std::move(*this).mount("", std::move(blueprint));
    }
    const std::vector<std::unique_ptr<RouteDeclaration>> &routes() const noexcept { return routes_; }
    std::vector<std::unique_ptr<RouteDeclaration>> &routes() noexcept { return routes_; }

    const std::vector<Middleware> &middlewares() const noexcept { return middlewares_; }
    std::vector<Middleware> &middlewares() noexcept { return middlewares_; }

    const std::map<std::string, std::string, std::less<>> &tags() const noexcept { return tags_; }
    std::map<std::string, std::string, std::less<>> &tags() noexcept { return tags_; }

    const std::vector<ChildMount> &children() const noexcept { return children_; }
    std::vector<ChildMount> &children() noexcept { return children_; }

    void register_into(Router &router,
                       std::string_view current_prefix,
                       const std::vector<Middleware> &parent_mws = {},
                       const std::map<std::string, std::string, std::less<>> &parent_tags = {}) const;

private:
    RouteDeclaration *add_route_internal(HttpMethod method, std::string path, Handler handler, std::vector<Middleware> middlewares) {
        auto decl = std::make_unique<RouteDeclaration>();
        decl->method = method;
        decl->path = std::move(path);
        decl->endpoint.handler = std::move(handler);
        decl->endpoint.middlewares = std::move(middlewares);
        RouteDeclaration *ptr = decl.get();
        routes_.push_back(std::move(decl));
        return ptr;
    }

    template <typename Last>
    RouteBuilder add_route_variadic_impl(HttpMethod method, std::string path, std::vector<Middleware> mws, Last &&last) & {
        return add_route(method, std::move(path), std::forward<Last>(last), std::move(mws));
    }

    template <typename Last>
    RouteBuilder add_route_variadic_impl(HttpMethod method, std::string path, std::vector<Middleware> mws, Last &&last) && {
        return std::move(*this).add_route(method, std::move(path), std::forward<Last>(last), std::move(mws));
    }

    template <typename First, typename... Rest>
        requires (sizeof...(Rest) > 0)
    RouteBuilder add_route_variadic_impl(HttpMethod method, std::string path, std::vector<Middleware> mws, First &&first, Rest&&... rest) & {
        mws.push_back(make_middleware(std::forward<First>(first)));
        return add_route_variadic_impl(method, std::move(path), std::move(mws), std::forward<Rest>(rest)...);
    }

    template <typename First, typename... Rest>
        requires (sizeof...(Rest) > 0)
    RouteBuilder add_route_variadic_impl(HttpMethod method, std::string path, std::vector<Middleware> mws, First &&first, Rest&&... rest) && {
        mws.push_back(make_middleware(std::forward<First>(first)));
        return std::move(*this).add_route_variadic_impl(method, std::move(path), std::move(mws), std::forward<Rest>(rest)...);
    }

    template <typename... Args>
    RouteBuilder add_route_variadic(HttpMethod method, std::string path, Args&&... args) & {
        return add_route_variadic_impl(method, std::move(path), std::vector<Middleware>{}, std::forward<Args>(args)...);
    }

    template <typename... Args>
    RouteBuilder add_route_variadic(HttpMethod method, std::string path, Args&&... args) && {
        return std::move(*this).add_route_variadic_impl(method, std::move(path), std::vector<Middleware>{}, std::forward<Args>(args)...);
    }

    std::vector<std::unique_ptr<RouteDeclaration>> routes_;
    std::vector<Middleware> middlewares_;
    std::map<std::string, std::string, std::less<>> tags_;
    std::vector<ChildMount> children_;
};

struct Blueprint::ChildMount {
    std::string prefix;
    std::unique_ptr<Blueprint> blueprint;

    ChildMount(std::string p, Blueprint bp)
        : prefix(std::move(p)), blueprint(std::make_unique<Blueprint>(std::move(bp))) {}

    ChildMount(const ChildMount &other)
        : prefix(other.prefix),
          blueprint(other.blueprint ? std::make_unique<Blueprint>(*other.blueprint) : nullptr) {}

    ChildMount &operator=(const ChildMount &other) {
        if (this != &other) {
            prefix = other.prefix;
            blueprint = other.blueprint ? std::make_unique<Blueprint>(*other.blueprint) : nullptr;
        }
        return *this;
    }

    ChildMount(ChildMount &&) noexcept = default;
    ChildMount &operator=(ChildMount &&) noexcept = default;
    ~ChildMount() = default;
};

inline Blueprint::~Blueprint() = default;
inline Blueprint::Blueprint(Blueprint &&) noexcept = default;
inline Blueprint &Blueprint::operator=(Blueprint &&) noexcept = default;

inline Blueprint::Blueprint(const Blueprint &other)
    : middlewares_(other.middlewares_),
      tags_(other.tags_),
      children_(other.children_) {
    routes_.reserve(other.routes_.size());
    for (const auto &r : other.routes_) {
        if (r) {
            routes_.push_back(std::make_unique<RouteDeclaration>(*r));
        }
    }
}

inline Blueprint &Blueprint::operator=(const Blueprint &other) {
    if (this != &other) {
        middlewares_ = other.middlewares_;
        tags_ = other.tags_;
        children_ = other.children_;
        routes_.clear();
        routes_.reserve(other.routes_.size());
        for (const auto &r : other.routes_) {
            if (r) {
                routes_.push_back(std::make_unique<RouteDeclaration>(*r));
            }
        }
    }
    return *this;
}

inline Blueprint::RouteBuilder::operator Blueprint() && {
    if (owned_bp_) {
        return std::move(*owned_bp_);
    }
    if (bp_) {
        return *bp_;
    }
    return Blueprint{};
}

inline Blueprint::RouteBuilder::operator Blueprint() const & {
    if (owned_bp_) {
        return *owned_bp_;
    }
    if (bp_) {
        return *bp_;
    }
    return Blueprint{};
}

inline Blueprint &Blueprint::RouteBuilder::blueprint() noexcept {
    return owned_bp_ ? *owned_bp_ : *bp_;
}

inline const Blueprint &Blueprint::RouteBuilder::blueprint() const noexcept {
    return owned_bp_ ? *owned_bp_ : *bp_;
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::RouteBuilder::route(HttpMethod method, std::string path, Args &&...args) {
    if (owned_bp_) {
        RouteBuilder rb = owned_bp_->route(method, std::move(path), std::forward<Args>(args)...);
        return RouteBuilder(owned_bp_, rb.declarations());
    }
    if (bp_) {
        return bp_->route(method, std::move(path), std::forward<Args>(args)...);
    }
    return RouteBuilder{};
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::RouteBuilder::any(std::string path, Args &&...args) {
    if (owned_bp_) {
        RouteBuilder rb = owned_bp_->any(std::move(path), std::forward<Args>(args)...);
        return RouteBuilder(owned_bp_, rb.declarations());
    }
    if (bp_) {
        return bp_->any(std::move(path), std::forward<Args>(args)...);
    }
    return RouteBuilder{};
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::RouteBuilder::GET(std::string path, Args &&...args) {
    return route(HttpMethod::GET, std::move(path), std::forward<Args>(args)...);
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::RouteBuilder::POST(std::string path, Args &&...args) {
    return route(HttpMethod::POST, std::move(path), std::forward<Args>(args)...);
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::RouteBuilder::PUT(std::string path, Args &&...args) {
    return route(HttpMethod::PUT, std::move(path), std::forward<Args>(args)...);
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::RouteBuilder::DELETE(std::string path, Args &&...args) {
    return route(HttpMethod::DELETE, std::move(path), std::forward<Args>(args)...);
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::RouteBuilder::PATCH(std::string path, Args &&...args) {
    return route(HttpMethod::PATCH, std::move(path), std::forward<Args>(args)...);
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::RouteBuilder::HEAD(std::string path, Args &&...args) {
    return route(HttpMethod::HEAD, std::move(path), std::forward<Args>(args)...);
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::RouteBuilder::OPTIONS(std::string path, Args &&...args) {
    return route(HttpMethod::OPTIONS, std::move(path), std::forward<Args>(args)...);
}

inline Blueprint::RouteBuilder &Blueprint::RouteBuilder::use(Middleware mw) & {
    blueprint().use(std::move(mw));
    return *this;
}

inline Blueprint::RouteBuilder &&Blueprint::RouteBuilder::use(Middleware mw) && {
    blueprint().use(std::move(mw));
    return std::move(*this);
}

template <typename M>
    requires MiddlewareCallable<M>
inline Blueprint::RouteBuilder &Blueprint::RouteBuilder::use(M &&m) & {
    blueprint().use(std::forward<M>(m));
    return *this;
}

template <typename M>
    requires MiddlewareCallable<M>
inline Blueprint::RouteBuilder &&Blueprint::RouteBuilder::use(M &&m) && {
    blueprint().use(std::forward<M>(m));
    return std::move(*this);
}

inline Blueprint::RouteBuilder &Blueprint::RouteBuilder::group_tag(std::string key, std::string val) & {
    blueprint().tag(std::move(key), std::move(val));
    return *this;
}

inline Blueprint::RouteBuilder &&Blueprint::RouteBuilder::group_tag(std::string key, std::string val) && {
    blueprint().tag(std::move(key), std::move(val));
    return std::move(*this);
}

inline Blueprint::RouteBuilder &Blueprint::RouteBuilder::mount(std::string_view prefix, Blueprint bp) & {
    blueprint().mount(prefix, std::move(bp));
    return *this;
}

inline Blueprint::RouteBuilder &&Blueprint::RouteBuilder::mount(std::string_view prefix, Blueprint bp) && {
    blueprint().mount(prefix, std::move(bp));
    return std::move(*this);
}

inline Blueprint::RouteBuilder &Blueprint::RouteBuilder::mount(Blueprint bp) & {
    blueprint().mount("", std::move(bp));
    return *this;
}

inline Blueprint::RouteBuilder &&Blueprint::RouteBuilder::mount(Blueprint bp) && {
    blueprint().mount("", std::move(bp));
    return std::move(*this);
}

inline Blueprint::RouteBuilder Blueprint::add_route(
    HttpMethod method, std::string path, Handler handler, std::vector<Middleware> middlewares) &
{
    RouteDeclaration *decl = add_route_internal(method, std::move(path), std::move(handler), std::move(middlewares));
    return RouteBuilder(this, {decl});
}

inline Blueprint::RouteBuilder Blueprint::add_route(
    HttpMethod method, std::string path, Handler handler, std::vector<Middleware> middlewares) &&
{
    auto owned = std::make_shared<Blueprint>(std::move(*this));
    RouteDeclaration *decl = owned->add_route_internal(method, std::move(path), std::move(handler), std::move(middlewares));
    return RouteBuilder(std::move(owned), {decl});
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::any(std::string path, Args&&... args) & {
    static constexpr HttpMethod methods[] = {
        HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT, HttpMethod::DELETE,
        HttpMethod::PATCH, HttpMethod::HEAD, HttpMethod::OPTIONS,
        HttpMethod::CONNECT, HttpMethod::TRACE
    };
    std::vector<RouteDeclaration*> decls;
    decls.reserve(9);
    for (HttpMethod m : methods) {
        RouteBuilder rb = route(m, path, args...);
        for (auto *d : rb.declarations()) {
            decls.push_back(d);
        }
    }
    return RouteBuilder(this, std::move(decls));
}

template <typename... Args>
    requires (sizeof...(Args) >= 1)
Blueprint::RouteBuilder Blueprint::any(std::string path, Args&&... args) && {
    auto owned = std::make_shared<Blueprint>(std::move(*this));
    static constexpr HttpMethod methods[] = {
        HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT, HttpMethod::DELETE,
        HttpMethod::PATCH, HttpMethod::HEAD, HttpMethod::OPTIONS,
        HttpMethod::CONNECT, HttpMethod::TRACE
    };
    std::vector<RouteDeclaration*> decls;
    decls.reserve(9);
    for (HttpMethod m : methods) {
        RouteBuilder rb = owned->route(m, path, args...);
        for (auto *d : rb.declarations()) {
            decls.push_back(d);
        }
    }
    return RouteBuilder(std::move(owned), std::move(decls));
}

inline Blueprint &Blueprint::mount(std::string_view prefix, Blueprint blueprint) & {
    std::string norm = normalize_prefix(prefix);
    children_.emplace_back(std::move(norm), std::move(blueprint));
    return *this;
}

inline Blueprint &&Blueprint::mount(std::string_view prefix, Blueprint blueprint) && {
    std::string norm = normalize_prefix(prefix);
    children_.emplace_back(std::move(norm), std::move(blueprint));
    return std::move(*this);
}

} // namespace kale

#endif // KALE_BLUEPRINT_H
