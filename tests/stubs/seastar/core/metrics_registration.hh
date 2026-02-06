// Minimal Seastar stub for unit testing headers that include
// seastar/core/metrics_registration.hh without requiring a full Seastar installation.
//
// Provides metric_groups with no-op add_group/clear for compilation.
#pragma once

#include <initializer_list>
#include <string>

namespace seastar {
namespace metrics {

struct metric_definition;

class metric_groups {
public:
    metric_groups() = default;
    void add_group(const std::string&, std::initializer_list<metric_definition>) {}
    void clear() {}
};

} // namespace metrics
} // namespace seastar
