// Ranvier Core - Lightweight Prometheus Text Format Parser
//
// Extracts specific metric values from Prometheus text exposition format.
// NOT a full parser — only handles exact metric name matching for the
// metrics we care about from vLLM's /metrics endpoint.

#pragma once

#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

namespace ranvier {

// Extract a gauge/counter value by exact metric name from Prometheus
// text format. Returns nullopt if not found.
//
// Scans line by line:
//   - Skips lines starting with '#' (comments/TYPE/HELP)
//   - Matches lines starting with metric_name followed by ' ' or '{'
//   - Extracts the numeric value after the last space
//   - Uses std::from_chars (no locale, no allocation)
//   - Returns first match
//
// Example input line: `vllm:num_requests_running 42`
// Example with labels: `vllm:num_requests_running{model="llama"} 42`
inline std::optional<double> extract_prometheus_metric(
        std::string_view body, std::string_view metric_name) {
    // Iterate over lines in the body
    size_t pos = 0;
    while (pos < body.size()) {
        // Find end of current line
        size_t eol = body.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = body.size();
        }

        std::string_view line = body.substr(pos, eol - pos);
        pos = eol + 1;

        // Skip empty lines and comments (# HELP, # TYPE, etc.)
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Check if line starts with metric_name followed by ' ' or '{'
        if (line.size() <= metric_name.size()) {
            continue;
        }
        if (line.substr(0, metric_name.size()) != metric_name) {
            continue;
        }

        char after = line[metric_name.size()];
        if (after != ' ' && after != '{') {
            continue;
        }

        // Found a match — extract value after the last space
        size_t last_space = line.rfind(' ');
        if (last_space == std::string_view::npos || last_space + 1 >= line.size()) {
            continue;
        }

        std::string_view value_str = line.substr(last_space + 1);

        // Handle special Prometheus values
        if (value_str == "+Inf" || value_str == "Inf") {
            return std::numeric_limits<double>::infinity();
        }
        if (value_str == "-Inf") {
            return -std::numeric_limits<double>::infinity();
        }
        if (value_str == "NaN") {
            return std::nan("");
        }

        // Parse numeric value with std::from_chars (no locale, no allocation)
        double value = 0.0;
        auto [ptr, ec] = std::from_chars(value_str.data(),
                                          value_str.data() + value_str.size(),
                                          value);
        if (ec == std::errc()) {
            return value;
        }

        // Parse failed — continue looking (shouldn't happen with valid Prometheus output)
    }

    return std::nullopt;
}

}  // namespace ranvier
