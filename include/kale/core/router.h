#ifndef KALE_CORE_ROUTER_H
#define KALE_CORE_ROUTER_H

#include <concepts>
#include <functional>
#include <map>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "coke/task.h"
#include "kale/core/context.h"
#include "kale/core/http_method.h"

namespace kale {
class Blueprint;


class Next {
    // Borrowed continuation: do not escape the middleware invocation; await
    // downstream work before returning. Pipeline continuations are one-shot.
public:
    using InvokerFn = coke::Task<void>(*)(void*);

    constexpr Next() noexcept = default;

    template <typename F>
        requires (!std::same_as<std::remove_cvref_t<F>, Next> &&
                  !std::is_null_pointer_v<std::remove_cvref_t<F>> &&
                  std::is_invocable_r_v<coke::Task<void>, F&>)
    constexpr Next(F &callable) noexcept
        : target_(const_cast<void*>(static_cast<const void*>(std::addressof(callable)))),
          invoker_([](void *ptr) -> coke::Task<void> {
              return std::invoke(*static_cast<std::add_pointer_t<F>>(ptr));
          }) {}

    // Prevent binding to temporary rvalues to avoid dangling pointers in coroutines
    template <typename F>
        requires (!std::same_as<std::remove_cvref_t<F>, Next>)
    Next(F &&callable) = delete;

    coke::Task<void> operator()() const {
        return invoker_(target_);
    }

    constexpr explicit operator bool() const noexcept {
        return target_ != nullptr;
    }

private:
    static coke::Task<void> empty_invoker(void*) {
        co_return;
    }

    void *target_{nullptr}; // Borrowed: non-owning pointer to target callable on stack
    InvokerFn invoker_{&Next::empty_invoker};
};

class Handler;

template <typename F>
concept HandlerCallable =
    std::invocable<const std::decay_t<F>&, Context&> ||
    std::invocable<const std::decay_t<F>&>;

template <typename M>
concept MiddlewareCallable = std::invocable<M, Context&, Next> &&
    std::convertible_to<std::invoke_result_t<M, Context&, Next>, coke::Task<void>>;

using Middleware = std::function<coke::Task<void>(Context &ctx, Next next)>;

template <typename M>
    requires MiddlewareCallable<M>
Middleware make_middleware(M &&m) {
    return Middleware(std::forward<M>(m));
}

template <typename T>
struct is_coke_task : std::false_type {};

template <typename T>
struct is_coke_task<coke::Task<T>> : std::true_type {};

template <typename T>
struct coke_task_result;

template <typename T>
struct coke_task_result<coke::Task<T>> { using type = T; };

template <typename T>
concept JsonDumpable = requires(const T &value) {
    { value.dump() } -> std::convertible_to<std::string_view>;
};

template <typename T>
inline constexpr bool is_coke_task_v = is_coke_task<std::remove_cvref_t<T>>::value;

template <typename F>
    requires HandlerCallable<F>
Handler make_handler(F &&f);

class Handler {
public:
    using SyncFn = std::function<void(Context &ctx)>;
    using AsyncFn = std::function<coke::Task<void>(Context &ctx)>;
    using Storage = std::variant<std::monostate, SyncFn, AsyncFn>;

    Handler() noexcept = default;
    Handler(std::nullptr_t) noexcept {}

    static Handler from_sync(SyncFn fn) {
        Handler h;
        if (fn) {
            h.storage_ = std::move(fn);
        }
        return h;
    }

    static Handler from_async(AsyncFn fn) {
        Handler h;
        if (fn) {
            h.storage_ = std::move(fn);
        }
        return h;
    }

    /* implicit */ Handler(SyncFn fn) {
        if (fn) {
            storage_ = std::move(fn);
        }
    }

    /* implicit */ Handler(AsyncFn fn) {
        if (fn) {
            storage_ = std::move(fn);
        }
    }

    template <typename F>
        requires (!std::same_as<std::remove_cvref_t<F>, Handler> &&
                  !std::same_as<std::remove_cvref_t<F>, SyncFn> &&
                  !std::same_as<std::remove_cvref_t<F>, AsyncFn> &&
                  !std::same_as<std::remove_cvref_t<F>, std::nullptr_t> &&
                  HandlerCallable<F>)
    /* implicit */ Handler(F &&f);

    bool is_sync() const noexcept {
        return std::holds_alternative<SyncFn>(storage_) && bool(std::get<SyncFn>(storage_));
    }

    bool is_async() const noexcept {
        return std::holds_alternative<AsyncFn>(storage_) && bool(std::get<AsyncFn>(storage_));
    }

    explicit operator bool() const noexcept {
        if (std::holds_alternative<SyncFn>(storage_)) {
            return bool(std::get<SyncFn>(storage_));
        }
        if (std::holds_alternative<AsyncFn>(storage_)) {
            return bool(std::get<AsyncFn>(storage_));
        }
        return false;
    }

    void invoke_sync(Context &ctx) const {
        if (std::holds_alternative<SyncFn>(storage_)) {
            const auto &fn = std::get<SyncFn>(storage_);
            if (fn) {
                fn(ctx);
                return;
            }
        }
        throw std::bad_function_call();
    }

    coke::Task<void> invoke_async(Context &ctx) const {
        if (std::holds_alternative<AsyncFn>(storage_)) {
            const auto &fn = std::get<AsyncFn>(storage_);
            if (fn) {
                return fn(ctx);
            }
        }
        throw std::bad_function_call();
    }

    coke::Task<void> operator()(Context &ctx) const {
        if (is_sync()) {
            std::get<SyncFn>(storage_)(ctx);
            co_return;
        }
        if (is_async()) {
            co_await std::get<AsyncFn>(storage_)(ctx);
            co_return;
        }
        throw std::bad_function_call();
    }

private:
    Storage storage_{std::monostate{}};
};

template <typename F>
    requires HandlerCallable<F>
Handler make_handler(F &&f) {
    if constexpr (std::is_invocable_v<const std::decay_t<F>&, Context&>) {
        using Ret = std::invoke_result_t<const std::decay_t<F>&, Context&>;
        if constexpr (std::same_as<Ret, coke::Task<void>>) {
            return Handler::from_async(std::forward<F>(f));
        } else if constexpr (std::same_as<Ret, coke::Task<std::string>>) {
            return Handler::from_async([fn = std::forward<F>(f)](Context &ctx) -> coke::Task<void> {
                ctx.string(co_await fn(ctx));
            });
        } else if constexpr (std::same_as<Ret, coke::Task<std::string_view>>) {
            return Handler::from_async([fn = std::forward<F>(f)](Context &ctx) -> coke::Task<void> {
                ctx.string(co_await fn(ctx));
            });
        } else if constexpr (std::same_as<Ret, coke::Task<const char*>>) {
            return Handler::from_async([fn = std::forward<F>(f)](Context &ctx) -> coke::Task<void> {
                ctx.string(co_await fn(ctx));
            });
        } else if constexpr (is_coke_task_v<Ret>) {
            using Value = typename coke_task_result<std::remove_cvref_t<Ret>>::type;
            static_assert(JsonDumpable<Value>, "Handler return value must be text, void, or provide dump() convertible to string_view");
            return Handler::from_async([fn = std::forward<F>(f)](Context &ctx) -> coke::Task<void> {
                ctx.json(co_await fn(ctx));
            });
        } else if constexpr (std::same_as<Ret, void>) {
            return Handler::from_sync([fn = std::forward<F>(f)](Context &ctx) {
                fn(ctx);
            });
        } else if constexpr (std::same_as<Ret, std::string> || std::same_as<Ret, const char*>) {
            return Handler::from_sync([fn = std::forward<F>(f)](Context &ctx) {
                ctx.string(fn(ctx));
            });
        } else if constexpr (std::same_as<Ret, std::string_view>) {
            return Handler::from_sync([fn = std::forward<F>(f)](Context &ctx) {
                ctx.string(fn(ctx));
            });
        } else {
            static_assert(JsonDumpable<Ret>, "Handler return value must be text, void, or provide dump() convertible to string_view");
            return Handler::from_sync([fn = std::forward<F>(f)](Context &ctx) {
                ctx.json(fn(ctx));
            });
        }
    } else {
        using Ret = std::invoke_result_t<const std::decay_t<F>&>;
        if constexpr (std::same_as<Ret, coke::Task<void>>) {
            return Handler::from_async([fn = std::forward<F>(f)](Context &) -> coke::Task<void> {
                co_await fn();
            });
        } else if constexpr (std::same_as<Ret, coke::Task<std::string>>) {
            return Handler::from_async([fn = std::forward<F>(f)](Context &ctx) -> coke::Task<void> {
                ctx.string(co_await fn());
            });
        } else if constexpr (std::same_as<Ret, coke::Task<std::string_view>>) {
            return Handler::from_async([fn = std::forward<F>(f)](Context &ctx) -> coke::Task<void> {
                ctx.string(co_await fn());
            });
        } else if constexpr (std::same_as<Ret, coke::Task<const char*>>) {
            return Handler::from_async([fn = std::forward<F>(f)](Context &ctx) -> coke::Task<void> {
                ctx.string(co_await fn());
            });
        } else if constexpr (is_coke_task_v<Ret>) {
            using Value = typename coke_task_result<std::remove_cvref_t<Ret>>::type;
            static_assert(JsonDumpable<Value>, "Handler return value must be text, void, or provide dump() convertible to string_view");
            return Handler::from_async([fn = std::forward<F>(f)](Context &ctx) -> coke::Task<void> {
                ctx.json(co_await fn());
            });
        } else if constexpr (std::same_as<Ret, void>) {
            return Handler::from_sync([fn = std::forward<F>(f)](Context &) {
                fn();
            });
        } else if constexpr (std::same_as<Ret, std::string> || std::same_as<Ret, const char*>) {
            return Handler::from_sync([fn = std::forward<F>(f)](Context &ctx) {
                ctx.string(fn());
            });
        } else if constexpr (std::same_as<Ret, std::string_view>) {
            return Handler::from_sync([fn = std::forward<F>(f)](Context &ctx) {
                ctx.string(fn());
            });
        } else {
            static_assert(JsonDumpable<Ret>, "Handler return value must be text, void, or provide dump() convertible to string_view");
            return Handler::from_sync([fn = std::forward<F>(f)](Context &ctx) {
                ctx.json(fn());
            });
        }
    }
}

template <typename F>
    requires (!std::same_as<std::remove_cvref_t<F>, Handler> &&
              !std::same_as<std::remove_cvref_t<F>, Handler::SyncFn> &&
              !std::same_as<std::remove_cvref_t<F>, Handler::AsyncFn> &&
              !std::same_as<std::remove_cvref_t<F>, std::nullptr_t> &&
              HandlerCallable<F>)
inline Handler::Handler(F &&f) : Handler(make_handler(std::forward<F>(f))) {}

coke::Task<void> compose_and_run(
    const std::vector<Middleware> &global_mws,
    const std::vector<Middleware> &route_mws,
    Context &ctx,
    const Handler &final_handler
);

inline const std::vector<Middleware> &empty_route_middlewares() noexcept {
    static const std::vector<Middleware> empty;
    return empty;
}

inline coke::Task<void> compose_and_run(
    const std::vector<Middleware> &mws,
    Context &ctx,
    const Handler &final_handler) {
    return compose_and_run(mws, empty_route_middlewares(), ctx, final_handler);
}

// Owning route metadata, independent of endpoint storage and Router lifetime.
struct RouteInfo {
    HttpMethod method;
    std::string pattern;
    std::string name;
    std::map<std::string, std::string, std::less<>> tags;
    size_t middleware_count{0};
    size_t max_body_size{0};
};

struct RouteEndpoint {
    Handler handler;
    std::vector<Middleware> middlewares;
    std::string name;
    std::string pattern;
    std::map<std::string, std::string, std::less<>> tags;
    size_t max_body_size{0};

};

class RouteBuilder {
public:
    explicit RouteBuilder(RouteEndpoint &endpoint) : endpoint_(&endpoint) {}

    RouteBuilder &name(std::string route_name) {
        if (endpoint_) endpoint_->name = std::move(route_name);
        return *this;
    }

    RouteBuilder &tag(std::string key, std::string val) {
        if (endpoint_) endpoint_->tags[std::move(key)] = std::move(val);
        return *this;
    }

    RouteBuilder &max_body_size(size_t limit) {
        if (endpoint_) endpoint_->max_body_size = limit;
        return *this;
    }

private:
    // Borrowed: non-owning pointer to endpoint owned by RadixNode
    RouteEndpoint *endpoint_{nullptr}; 
};

struct RouteResult {
    bool matched{false};
    bool method_not_allowed { false };
    // Borrowed: non-owning pointer to matched endpoint owned by RadixNode
    const RouteEndpoint *endpoint{nullptr}; 
    // Captures own both names and decoded values, independently of the input path
    // and Router lifetime. Endpoint metadata above still borrows from the Router.
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<HttpMethod> allowed_methods;

    std::string_view route_name() const noexcept {
        return endpoint ? std::string_view(endpoint->name) : "";
    }
    std::string_view route_pattern() const noexcept {
        return endpoint ? std::string_view(endpoint->pattern) : "";
    }
    std::string_view route_tag(std::string_view key) const noexcept {
        if (!endpoint) return "";
        const auto it = endpoint->tags.find(key);
        if (it != endpoint->tags.end()) return it->second;
        return "";
    }
    std::string allow_header() const {
        std::string res;
        for (size_t i = 0; i < allowed_methods.size(); ++i) {
            if (i > 0) {
                res.append(", ");
            }
            res.append(method_to_string(allowed_methods[i]));
        }
        return res;
    }
    std::string_view param(std::string_view key) const noexcept {
        for (const auto &[k, v] : params) {
            if (k == key) {
                return v;
            }
        }
        return "";
    }
};

class RadixNode {
public:
    enum class NodeType {
        STATIC,
        PARAM,
        WILDCARD
    };

    explicit RadixNode(std::string segment = "", NodeType type = NodeType::STATIC);
    ~RadixNode();

    RadixNode(const RadixNode &) = delete;
    RadixNode &operator=(const RadixNode &) = delete;
    RadixNode(RadixNode &&) noexcept = default;
    RadixNode &operator=(RadixNode &&) noexcept = default;

    RouteEndpoint &add_route(HttpMethod method, std::string_view path, Handler handler, std::vector<Middleware> middlewares = {});
    bool match_route(HttpMethod method,
                     std::span<const std::string_view> segments,
                     size_t index,
                     std::vector<std::pair<std::string, std::string>> &params,
                     RouteResult &res) const;

private:
    friend class Router;
    void collect_routes(std::vector<RouteInfo> &routes) const;

    std::string segment_;
    NodeType type_{NodeType::STATIC};
    std::string param_name_;
    // Endpoint addresses survive method registration; intermediate nodes allocate no endpoint objects.
    std::vector<std::unique_ptr<std::pair<HttpMethod, RouteEndpoint>>> endpoints_;
    std::vector<std::unique_ptr<RadixNode>> static_children_;
    std::vector<std::unique_ptr<RadixNode>> param_children_;
    std::vector<std::unique_ptr<RadixNode>> wildcard_children_;

    RadixNode *find_or_create_child(std::string_view segment, NodeType type); 
    void collect_allowed_methods(std::vector<HttpMethod> &methods) const;
    bool match_route_impl(HttpMethod method,
                          std::span<const std::string_view> segments,
                          size_t index,
                          size_t capture_count,
                          std::vector<std::pair<std::string, std::string>> &params,
                          RouteResult &res) const;
};

class Router {
public:
    Router();
    ~Router();

    Router(const Router &) = delete;
    Router &operator=(const Router &) = delete;
    Router(Router &&) noexcept;
    Router &operator=(Router &&) noexcept;

    RouteBuilder add_route(HttpMethod method, std::string path, Handler handler, std::vector<Middleware> middlewares = {});

    template <HandlerCallable H>
        requires (!std::is_same_v<std::decay_t<H>, Handler>)
    RouteBuilder add_route(HttpMethod method, std::string path, H &&handler, std::vector<Middleware> middlewares = {}) {
        return add_route(method, std::move(path), make_handler(std::forward<H>(handler)), std::move(middlewares));
    }

    // Trailing slash is significant. Named parameters require a nonempty segment;
    // wildcards may match the empty segment after '/'. Captures are percent-decoded
    // once and preserve '+'. Registration must not race request dispatch.
    RouteResult route(HttpMethod method, std::string_view path) const;

    // Returns an owning snapshot of registered routes.
    // Must not be called concurrently with route registration or RouteBuilder mutation.
    [[nodiscard]] std::vector<RouteInfo> routes() const;

    [[nodiscard]] std::string format_routes() const;
    void list_routes(std::ostream &os = std::cout) const;

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder route(HttpMethod method, std::string path, Args&&... args) {
        return add_route_variadic(method, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    void any(std::string path, Args&&... args) {
        static constexpr HttpMethod methods[] = {
            HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT, HttpMethod::DELETE,
            HttpMethod::PATCH, HttpMethod::HEAD, HttpMethod::OPTIONS,
            HttpMethod::CONNECT, HttpMethod::TRACE
        };
        for (HttpMethod m : methods) {
            route(m, path, args...);
        }
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder GET(std::string path, Args&&... args) {
        return route(HttpMethod::GET, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder POST(std::string path, Args&&... args) {
        return route(HttpMethod::POST, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder PUT(std::string path, Args&&... args) {
        return route(HttpMethod::PUT, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder DELETE(std::string path, Args&&... args) {
        return route(HttpMethod::DELETE, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder PATCH(std::string path, Args&&... args) {
        return route(HttpMethod::PATCH, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder HEAD(std::string path, Args&&... args) {
        return route(HttpMethod::HEAD, std::move(path), std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires (sizeof...(Args) >= 1)
    RouteBuilder OPTIONS(std::string path, Args&&... args) {
        return route(HttpMethod::OPTIONS, std::move(path), std::forward<Args>(args)...);
    }

    Router &mount(std::string_view prefix, Blueprint blueprint);
    Router &mount(Blueprint blueprint);

private:
    template <typename Last>
    RouteBuilder add_route_variadic_impl(HttpMethod method, std::string path, std::vector<Middleware> mws, Last &&last) {
        return add_route(method, std::move(path), std::forward<Last>(last), std::move(mws));
    }

    template <typename First, typename... Rest>
        requires (sizeof...(Rest) > 0)
    RouteBuilder add_route_variadic_impl(HttpMethod method, std::string path, std::vector<Middleware> mws, First &&first, Rest&&... rest) {
        mws.push_back(make_middleware(std::forward<First>(first)));
        return add_route_variadic_impl(method, std::move(path), std::move(mws), std::forward<Rest>(rest)...);
    }

    template <typename... Args>
    RouteBuilder add_route_variadic(HttpMethod method, std::string path, Args&&... args) {
        return add_route_variadic_impl(method, std::move(path), std::vector<Middleware>{}, std::forward<Args>(args)...);
    }

    std::unique_ptr<RadixNode> root_;
};

} // namespace kale

#endif // KALE_CORE_ROUTER_H
