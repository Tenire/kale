#include "kale/middleware/prometheus.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "kale/core/context.h"
#include "kale/core/http_error.h"

namespace kale {
namespace {
struct PrometheusExporterMarker {};

std::string_view normalized_method(std::string_view method) {
    constexpr std::array methods{"GET", "HEAD", "POST", "PUT", "DELETE",
                                 "CONNECT", "OPTIONS", "TRACE", "PATCH"};
    for (const auto *known : methods) if (method == known) return known;
    return "OTHER";
}
void append_label(std::string &out, std::string_view value) {
    for (const char ch : value) {
        if (ch == char(92) || ch == char(34)) {
            out += char(92);
            out += ch;
        } else if (ch == char(10)) {
            out += char(92);
            out += char(110);
        } else out += ch;
    }
}
void append_number(std::string &out, double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                      std::chars_format::general, std::numeric_limits<double>::max_digits10);
    out.append(buffer, result.ptr);
}
void record(PrometheusRegistry &registry, const Context &ctx,
            std::chrono::steady_clock::time_point start, int status) noexcept {
    try {
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        registry.observe(ctx.method_string(), ctx.route_pattern(), status,
                         std::isfinite(elapsed) ? std::max(0.0, elapsed) : 0.0);
    } catch (...) {
        // Best-effort: failures must not fail the request or mask downstream exceptions.
    }
}
} // namespace

class PrometheusRegistry::Impl {
public:
    explicit Impl(std::size_t capacity) : max_series(capacity) {
        if (capacity == 0 || capacity > samples.max_size())
            throw std::invalid_argument("PrometheusRegistry requires positive bounded capacity");
    }
    using Key = std::tuple<std::string, std::string, int>;
    using KeyView = std::tuple<std::string_view, std::string_view, int>;
    struct Less {
        using is_transparent = void;
        template <class A, class B> bool operator()(const A &a, const B &b) const {
            return KeyView(std::get<0>(a), std::get<1>(a), std::get<2>(a)) <
                   KeyView(std::get<0>(b), std::get<1>(b), std::get<2>(b));
        }
    };
    struct Value { std::uint64_t count{0}; double sum{0}; };
    const std::size_t max_series;
    std::mutex mutex;
    std::map<Key, Value, Less> samples;
    Value overflow;
};

PrometheusRegistry::PrometheusRegistry(std::size_t max_series)
    : impl_(std::make_unique<Impl>(max_series)) {}
PrometheusRegistry::~PrometheusRegistry() = default;

void PrometheusRegistry::observe(std::string_view method, std::string_view route, int status, double seconds) {
    if (!std::isfinite(seconds) || seconds < 0)
        throw std::invalid_argument("metric duration must be finite and nonnegative");
    method = normalized_method(method);
    if (route.empty()) route = "<unmatched>";
    if (status < 100 || status > 599) status = 500;
    const std::lock_guard lock(impl_->mutex);
    auto *value = &impl_->overflow;
    if (route.size() <= 1024 && route != "<overflow>") {
        const Impl::KeyView key{method, route, status};
        auto found = impl_->samples.find(key);
        if (found != impl_->samples.end()) value = &found->second;
        else if (impl_->samples.size() < impl_->max_series - 1) {
            value = &impl_->samples.emplace(
                Impl::Key{std::string(method), std::string(route), status}, Impl::Value{}).first->second;
        }
    }
    if (value->count < std::numeric_limits<std::uint64_t>::max()) ++value->count;
    value->sum = seconds > std::numeric_limits<double>::max() - value->sum
        ? std::numeric_limits<double>::max() : value->sum + seconds;
}

std::vector<HttpMetricSample> PrometheusRegistry::snapshot() const {
    const std::lock_guard lock(impl_->mutex);
    std::vector<HttpMetricSample> result;
    result.reserve(impl_->samples.size() + (impl_->overflow.count != 0));
    for (const auto &[key, value] : impl_->samples) {
        result.push_back({std::get<0>(key), std::get<1>(key), std::get<2>(key), value.count, value.sum});
    }
    if (impl_->overflow.count != 0)
        result.push_back({"OTHER", "<overflow>", 500, impl_->overflow.count, impl_->overflow.sum});
    return result;
}

std::string PrometheusRegistry::serialize() const {
    const auto samples = snapshot();
    std::string out = "# HELP http_requests_total Completed downstream HTTP requests.\n"
                      "# TYPE http_requests_total counter\n"
                      "# HELP http_request_duration_seconds Downstream request duration in seconds.\n"
                      "# TYPE http_request_duration_seconds summary\n";
    for (const auto &sample : samples) {
        std::string labels = "{method=\"";
        append_label(labels, sample.method);
        labels += "\",route=\"";
        append_label(labels, sample.route);
        labels += "\",status=\"" + std::to_string(sample.status) + "\"}";
        out += "http_requests_total" + labels + " " + std::to_string(sample.requests) + "\n";
        out += "http_request_duration_seconds_count" + labels + " " + std::to_string(sample.requests) + "\n";
        out += "http_request_duration_seconds_sum" + labels + " ";
        append_number(out, sample.duration_seconds);
        out += char(10);
    }
    return out;
}

Middleware prometheus_metrics(PrometheusRegistry &registry, PrometheusConfig config) {
    return [&registry, config = std::move(config)](Context &ctx, Next next) -> coke::Task<void> {
        if (!config.skip_route_tag.empty() && ctx.route_tag(config.skip_route_tag) == "true") {
            co_await next();
            co_return;
        }
        const auto start = std::chrono::steady_clock::now();
        try {
            co_await next();
        } catch (const HttpError &error) {
            if (ctx.has_attachment<PrometheusExporterMarker>()) {
                throw;
            }
            record(registry, ctx, start, (ctx.is_replied() || ctx.is_stream()) ? ctx.status_code() : error.status_code());
            throw;
        } catch (...) {
            if (ctx.has_attachment<PrometheusExporterMarker>()) {
                throw;
            }
            record(registry, ctx, start, (ctx.is_replied() || ctx.is_stream()) ? ctx.status_code() : 500);
            throw;
        }
        if (ctx.has_attachment<PrometheusExporterMarker>()) {
            co_return;
        }
        record(registry, ctx, start, ctx.status_code());
    };
}

Handler prometheus_exporter(PrometheusRegistry &registry) {
    return make_handler([&registry](Context &ctx) {
        ctx.set_attachment<PrometheusExporterMarker>(PrometheusExporterMarker{});
        ctx.set_status(200).body(registry.serialize());
        ctx.set_content_type("text/plain; version=0.0.4; charset=utf-8");
    });
}
} // namespace kale
