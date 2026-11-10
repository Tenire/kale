#include "kale/core/router.h"
#include <stdexcept>
#include "kale/core/http_util.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace kale {
namespace {

constexpr size_t MAX_INLINE_SEGMENTS = 32;
struct PathSegments {
    std::string_view inline_segs[MAX_INLINE_SEGMENTS];
    std::vector<std::string_view> heap_segs;
    size_t count{0};

    explicit PathSegments(std::string_view path) {
        size_t start = 0;
        while (start < path.size()) {
            while (start < path.size() && path[start] == '/') {
                ++start;
            }
            if (start >= path.size()) {
                break;
            }
            size_t end = path.find('/', start);
            if (end == std::string_view::npos) {
                end = path.size();
            }
            push(path.substr(start, end - start));
            start = end;
        }
        if (path.size() > 1 && path.back() == '/') {
            push("");
        }
    }

    size_t size() const noexcept { return count; }
    std::span<const std::string_view> span() const noexcept {
        if (heap_segs.empty()) {
            return std::span<const std::string_view>(inline_segs, count);
        }
        return std::span<const std::string_view>(heap_segs.data(), heap_segs.size());
    }

private:
    void push(std::string_view seg) {
        if (count < MAX_INLINE_SEGMENTS) {
            inline_segs[count++] = seg;
        } else {
            if (heap_segs.empty()) {
                heap_segs.reserve(MAX_INLINE_SEGMENTS * 2);
                for (size_t i = 0; i < MAX_INLINE_SEGMENTS; ++i) {
                    heap_segs.push_back(inline_segs[i]);
                }
            }
            heap_segs.push_back(seg);
            ++count;
        }
    }

};

struct PipelineRunner {
    const std::vector<Middleware> &global_mws;
    const std::vector<Middleware> &route_mws;
    Context &ctx;
    const Handler &final_handler;
    struct Continuation {
        PipelineRunner &runner;
        size_t index;
        bool used = false;
        coke::Task<void> operator()() {
            if (used)
                throw std::logic_error("Middleware next() may only be called once");
            used = true;
            return runner.step(index);
        }
    };
    coke::Task<void> step(size_t index) {
        const size_t global_size = global_mws.size();
        if (index < global_size + route_mws.size()) {
            Continuation continuation{*this, index + 1};
            Next next(continuation);
            if (index < global_size) co_await global_mws[index](ctx, next);
            else co_await route_mws[index - global_size](ctx, next);
        } else {
            if (final_handler.is_sync()) {
                final_handler.invoke_sync(ctx);
            } else if (final_handler.is_async()) {
                co_await final_handler.invoke_async(ctx);
            }
        }
    }
};

} // namespace

coke::Task<void> compose_and_run(
    const std::vector<Middleware> &global_mws,
    const std::vector<Middleware> &route_mws,
    Context &ctx,
    const Handler &final_handler) {
    if (global_mws.empty() && route_mws.empty()) {
        if (final_handler.is_sync()) {
            final_handler.invoke_sync(ctx);
            co_return;
        } else if (final_handler.is_async()) {
            co_await final_handler.invoke_async(ctx);
            co_return;
        }
        co_return;
    }

    PipelineRunner runner{global_mws, route_mws, ctx, final_handler};
    co_await runner.step(0);
}

RadixNode::RadixNode(std::string segment, NodeType type)
    : segment_(std::move(segment)), type_(type) {
    if (type_ == NodeType::PARAM) {
        if (!segment_.empty() && segment_[0] == ':') {
            param_name_ = segment_.substr(1);
        } else if (segment_.size() >= 2 && segment_.front() == '{' && segment_.back() == '}') {
            std::string_view inner = std::string_view(segment_).substr(1, segment_.size() - 2);
            while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\t')) inner.remove_prefix(1);
            while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\t')) inner.remove_suffix(1);
            param_name_ = std::string(inner);
        } else {
            param_name_ = segment_;
        }
    } else if (type_ == NodeType::WILDCARD) {
        if (!segment_.empty() && segment_[0] == '*') {
            param_name_ = segment_.substr(1);
        } else {
            param_name_ = "*";
        }
        if (param_name_.empty()) {
            param_name_ = "*";
        }
    }
}

RadixNode::~RadixNode() = default;

RadixNode *RadixNode::find_or_create_child(std::string_view segment, NodeType type) {
    if (type == NodeType::STATIC) {
        for (auto &child : static_children_) {
            if (child->segment_ == segment) {
                return child.get();
            }
        }
        auto node = std::make_unique<RadixNode>(std::string(segment), NodeType::STATIC);
        RadixNode *ptr = node.get();
        static_children_.push_back(std::move(node));
        return ptr;
    }

    if (type == NodeType::PARAM) {
        for (auto &child : param_children_) {
            if (child->segment_ == segment) {
                return child.get();
            }
        }
        auto node = std::make_unique<RadixNode>(std::string(segment), NodeType::PARAM);
        RadixNode *ptr = node.get();
        param_children_.push_back(std::move(node));
        return ptr;
    }

    if (type == NodeType::WILDCARD) {
        for (auto &child : wildcard_children_) {
            if (child->segment_ == segment) {
                return child.get();
            }
        }
        auto node = std::make_unique<RadixNode>(std::string(segment), NodeType::WILDCARD);
        RadixNode *ptr = node.get();
        wildcard_children_.push_back(std::move(node));
        return ptr;
    }

    return nullptr;
}

RouteEndpoint &RadixNode::add_route(HttpMethod method, std::string_view path, Handler handler, std::vector<Middleware> middlewares) {
    PathSegments segments(path);
    RadixNode *curr = this;

    auto seg_span = segments.span();
    for (size_t i = 0; i < seg_span.size(); ++i) {
        std::string_view seg = seg_span[i];
        NodeType ntype = NodeType::STATIC;
        if (!seg.empty() && seg[0] == ':') {
            ntype = NodeType::PARAM;
        } else if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
            ntype = NodeType::PARAM;
        } else if (!seg.empty() && seg[0] == '*') {
            ntype = NodeType::WILDCARD;
        }

        if (ntype == NodeType::WILDCARD && i + 1 < seg_span.size()) {
            throw std::invalid_argument("Wildcard segment must be the final segment in path: " + std::string(path));
        }

        curr = curr->find_or_create_child(seg, ntype);

        if (ntype == NodeType::WILDCARD) {
            break;
        }
    }

    for (auto &item : curr->endpoints_) {
        if (item->first == method) {
            throw std::logic_error("Duplicate route registered: " +
                                   std::string(method_to_string(method)) + " " + std::string(path));
        }
    }
    RouteEndpoint new_ep;
    new_ep.handler = std::move(handler);
    new_ep.middlewares = std::move(middlewares);
    new_ep.pattern = std::string(path);
    curr->endpoints_.push_back(std::make_unique<std::pair<HttpMethod, RouteEndpoint>>(method, std::move(new_ep)));
    return curr->endpoints_.back()->second;
}

void RadixNode::collect_routes(std::vector<RouteInfo> &routes) const {
    for (const auto &item : endpoints_) {
        const auto &[method, endpoint] = *item;
        routes.push_back({
            method,
            endpoint.pattern,
            endpoint.name,
            endpoint.tags,
            endpoint.middlewares.size(),
            endpoint.max_body_size
        });
    }
    for (const auto &child : static_children_) child->collect_routes(routes);
    for (const auto &child : param_children_) child->collect_routes(routes);
    for (const auto &child : wildcard_children_) child->collect_routes(routes);
}

void RadixNode::collect_allowed_methods(std::vector<HttpMethod> &methods) const {
    for (const auto &item : endpoints_) {
        if (std::find(methods.begin(), methods.end(), item->first) == methods.end()) {
            methods.push_back(item->first);
        }
    }
}

bool RadixNode::match_route(HttpMethod method,
                            std::span<const std::string_view> segments,
                            size_t index,
                            std::vector<std::pair<std::string, std::string>> &params,
                            RouteResult &res) const {
    return match_route_impl(method, segments, index, params.size(), params, res);
}

bool RadixNode::match_route_impl(HttpMethod method,
                                 std::span<const std::string_view> segments,
                                 size_t index,
                                 size_t capture_count,
                                 std::vector<std::pair<std::string, std::string>> &params,
                                 RouteResult &res) const {
    if (index == segments.size()) {
        if (!endpoints_.empty()) {
            for (const auto &item : endpoints_) {
                if (item->first == method) {
                    // Allocate the exact capture count only after finding a match.
                    // Successful recursion fills these slots on unwind in path order.
                    params.resize(capture_count);
                    res.matched = true;
                    res.endpoint = &item->second;
                    res.method_not_allowed = false;
                    res.allowed_methods.clear();
                    return true;
                }
            }
            collect_allowed_methods(res.allowed_methods);
            res.method_not_allowed = true;
        }
        return false;
    }

    std::string_view seg = segments[index];

    for (const auto &child : static_children_) {
        if (child->segment_ == seg &&
            child->match_route_impl(method, segments, index + 1, capture_count, params, res)) {
            return true;
        }
    }

    if (!seg.empty()) {
        for (const auto &child : param_children_) {
            if (child->match_route_impl(method, segments, index + 1,
                                        capture_count + 1, params, res)) {
                auto &capture = params[capture_count];
                capture.first = child->param_name_;
                capture.second = path_percent_decode(seg);
                return true;
            }
        }
    }
    for (const auto &child : wildcard_children_) {
        if (!child->endpoints_.empty()) {
            for (const auto &item : child->endpoints_) {
                if (item->first == method) {
                    params.resize(capture_count + 1);
                    auto &capture = params[capture_count];
                    capture.first = child->param_name_;
                    for (size_t s = index; s < segments.size(); ++s) {
                        if (s > index) {
                            capture.second.push_back('/');
                        }
                        capture.second.append(path_percent_decode(segments[s]));
                    }
                    res.matched = true;
                    res.endpoint = &item->second;
                    res.method_not_allowed = false;
                    res.allowed_methods.clear();
                    return true;
                }
            }
            child->collect_allowed_methods(res.allowed_methods);
            res.method_not_allowed = true;
        }
    }
    return false;
}

Router::Router()
    : root_(std::make_unique<RadixNode>("", RadixNode::NodeType::STATIC)) {
}

Router::~Router() = default;
Router::Router(Router &&) noexcept = default;
Router &Router::operator=(Router &&) noexcept = default;

RouteBuilder Router::add_route(HttpMethod method, std::string path, Handler handler, std::vector<Middleware> middlewares) {
    if (path.empty() || path[0] != '/') {
        path = "/" + path;
    }
    RouteEndpoint &ep = root_->add_route(method, path, std::move(handler), std::move(middlewares));
    return RouteBuilder(ep);
}

std::vector<RouteInfo> Router::routes() const {
    std::vector<RouteInfo> result;
    if (root_) root_->collect_routes(result);
    std::sort(result.begin(), result.end(), [](const RouteInfo &a, const RouteInfo &b) {
        if (a.pattern != b.pattern) return a.pattern < b.pattern;
        return method_to_string(a.method) < method_to_string(b.method);
    });
    return result;
}

RouteResult Router::route(HttpMethod method, std::string_view path) const {
    RouteResult res;
    PathSegments segments(path);
    std::vector<std::pair<std::string, std::string>> params;
    if (root_->match_route(method, segments.span(), 0, params, res)) {
        res.params = std::move(params);
        res.allowed_methods.clear();
        res.method_not_allowed = false;
    }
    return res;
}

namespace {

constexpr int method_order_rank(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::GET:     return 0;
        case HttpMethod::POST:    return 1;
        case HttpMethod::PUT:     return 2;
        case HttpMethod::DELETE:  return 3;
        case HttpMethod::PATCH:   return 4;
        case HttpMethod::HEAD:    return 5;
        case HttpMethod::OPTIONS: return 6;
        case HttpMethod::CONNECT: return 7;
        case HttpMethod::TRACE:   return 8;
        default:                  return 9;
    }
}

} // namespace

std::string Router::format_routes() const {
    auto list = routes();
    if (list.empty()) {
        return "";
    }

    std::sort(list.begin(), list.end(), [](const RouteInfo &a, const RouteInfo &b) {
        if (a.pattern != b.pattern) {
            return a.pattern < b.pattern;
        }
        int ra = method_order_rank(a.method);
        int rb = method_order_rank(b.method);
        if (ra != rb) {
            return ra < rb;
        }
        return static_cast<int>(a.method) < static_cast<int>(b.method);
    });

    size_t w_method = 6;
    size_t w_path = 4;
    size_t w_name = 4;
    size_t w_mws = 11;
    size_t w_body = 8;

    for (const auto &r : list) {
        w_method = std::max(w_method, method_to_string(r.method).size());
        w_path = std::max(w_path, r.pattern.size());
        w_name = std::max(w_name, r.name.size());
        w_mws = std::max(w_mws, std::to_string(r.middleware_count).size());
        w_body = std::max(w_body, std::to_string(r.max_body_size).size());
    }

    std::ostringstream ss;
    ss << std::left << std::setw(static_cast<int>(w_method)) << "METHOD" << "  "
       << std::left << std::setw(static_cast<int>(w_path)) << "PATH" << "  "
       << std::left << std::setw(static_cast<int>(w_name)) << "NAME" << "  "
       << std::right << std::setw(static_cast<int>(w_mws)) << "MIDDLEWARES" << "  "
       << std::right << std::setw(static_cast<int>(w_body)) << "MAX_BODY";

    for (const auto &r : list) {
        ss << "\n"
           << std::left << std::setw(static_cast<int>(w_method)) << method_to_string(r.method) << "  "
           << std::left << std::setw(static_cast<int>(w_path)) << r.pattern << "  "
           << std::left << std::setw(static_cast<int>(w_name)) << r.name << "  "
           << std::right << std::setw(static_cast<int>(w_mws)) << r.middleware_count << "  "
           << std::right << std::setw(static_cast<int>(w_body)) << r.max_body_size;
    }

    return ss.str();
}

void Router::list_routes(std::ostream &os) const {
    std::string formatted = format_routes();
    if (!formatted.empty()) {
        os << formatted << "\n";
    }
}


} // namespace kale
