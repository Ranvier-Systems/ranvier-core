// Minimal Seastar stub for unit testing headers that include seastar/core/metrics.hh
// without requiring a full Seastar installation.
//
// Provides just enough of the metrics types for MetricsService to compile:
// histogram, description, make_counter, make_histogram, make_gauge.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace seastar {
namespace metrics {

struct histogram_bucket {
    uint64_t count;
    double upper_bound;
};

struct histogram {
    uint64_t sample_count = 0;
    double sample_sum = 0;
    std::vector<histogram_bucket> buckets;
};

struct description {
    explicit description(const char*) {}
    explicit description(const std::string&) {}
};

// Label key-value pair for per-backend metrics
using label_instance = std::pair<std::string, std::string>;

// Stub metric definition — all make_* functions return this
struct metric_definition {};

template<typename... Args>
inline metric_definition make_counter(Args&&...) { return {}; }

template<typename... Args>
inline metric_definition make_histogram(Args&&...) { return {}; }

template<typename... Args>
inline metric_definition make_gauge(Args&&...) { return {}; }

// Explicit overloads for labeled metrics — braced-init-lists like
// {{"backend_id", id}} cannot be deduced by variadic templates.
template<typename... Args>
inline metric_definition make_histogram(const char*, description,
    std::initializer_list<label_instance>, Args&&...) { return {}; }

template<typename... Args>
inline metric_definition make_gauge(const char*, description,
    std::initializer_list<label_instance>, Args&&...) { return {}; }

// Explicit overload for a labeled counter in canonical Seastar order
// (name, value/function, description, labels). The trailing braced label list
// cannot be deduced by the variadic make_counter above.
template<typename T>
inline metric_definition make_counter(const char*, T&&, description,
    std::initializer_list<label_instance>) { return {}; }

} // namespace metrics
} // namespace seastar
