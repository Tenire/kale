#ifndef KALE_MIDDLEWARE_PROMETHEUS_H
#define KALE_MIDDLEWARE_PROMETHEUS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kale/core/router.h"

namespace kale {
struct HttpMetricSample {
    std::string method;
    std::string route;
    int status{200};
    std::uint64_t requests{0};
    double duration_seconds{0};
};

// Thread-safe registry for HTTP request duration and count metrics.
class PrometheusRegistry {
public:
    explicit PrometheusRegistry(std::size_t max_series = 1024);
    ~PrometheusRegistry();
    PrometheusRegistry(const PrometheusRegistry &) = delete;
    PrometheusRegistry &operator=(const PrometheusRegistry &) = delete;
    // Throws invalid_argument for negative/nonfinite durations.
    void observe(std::string_view method, std::string_view route, int status, double seconds);
    std::vector<HttpMetricSample> snapshot() const;
    std::string serialize() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct PrometheusConfig {
    // Route tag used to exclude endpoints from metric collection.
    std::string skip_route_tag{"metrics.skip"};
};

// Records request counts and downstream durations into registry.
// Duration measures execution until downstream handlers complete or unwind.
// registry must outlive the middleware and active requests.
Middleware prometheus_metrics(PrometheusRegistry &registry, PrometheusConfig config = {});

// Exposes serialized Prometheus metrics from registry.
Handler prometheus_exporter(PrometheusRegistry &registry);
} // namespace kale

#endif // KALE_MIDDLEWARE_PROMETHEUS_H
