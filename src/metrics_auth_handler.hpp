// Ranvier Core - Metrics Endpoint Auth Handler
//
// Optional Bearer token and IP allowlist authentication for the Prometheus
// metrics endpoint (port 9180). When configured, wraps the Seastar prometheus
// handler to check credentials before serving metrics.
//
// When both auth_token and allowed_ips are configured, both checks must pass
// (AND logic). Rate-limits warn logs to avoid flood from misconfigured scrapers.

#pragma once

#include "config_schema.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <seastar/http/httpd.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/reply.hh>
#include <seastar/util/log.hh>

namespace ranvier {

static seastar::logger log_metrics_auth("metrics_auth");

// CIDR entry for IP allowlist matching
struct CidrEntry {
    uint32_t network;      // Network address in host byte order
    uint32_t mask;         // Subnet mask (e.g., /8 -> 0xFF000000)
};

// Parse an IPv4 address string to a 32-bit integer (host byte order).
// Returns 0 on failure (note: 0.0.0.0 is explicitly handled by callers).
inline uint32_t parse_ipv4(std::string_view ip) {
    uint32_t result = 0;
    int dots = 0;
    int octet_val = 0;
    bool has_digit = false;

    for (char c : ip) {
        if (c >= '0' && c <= '9') {
            octet_val = octet_val * 10 + (c - '0');
            if (octet_val > 255) return 0;
            has_digit = true;
        } else if (c == '.') {
            if (!has_digit || dots >= 3) return 0;
            result = (result << 8) | static_cast<uint32_t>(octet_val);
            octet_val = 0;
            has_digit = false;
            ++dots;
        } else {
            return 0;
        }
    }
    if (!has_digit || dots != 3) return 0;
    result = (result << 8) | static_cast<uint32_t>(octet_val);
    return result;
}

// Parse a CIDR entry like "10.0.0.0/8" or an exact IP like "10.0.0.1".
// For exact IPs, mask = 0xFFFFFFFF (equivalent to /32).
inline bool parse_cidr(std::string_view entry, CidrEntry& out) {
    auto slash = entry.find('/');
    if (slash == std::string_view::npos) {
        out.network = parse_ipv4(entry);
        if (out.network == 0 && entry != "0.0.0.0") return false;
        out.mask = 0xFFFFFFFF;
        return true;
    }

    auto ip_part = entry.substr(0, slash);
    auto prefix_str = entry.substr(slash + 1);

    out.network = parse_ipv4(ip_part);
    if (out.network == 0 && ip_part != "0.0.0.0") return false;

    int prefix_len = 0;
    for (char c : prefix_str) {
        if (c < '0' || c > '9') return false;
        prefix_len = prefix_len * 10 + (c - '0');
        if (prefix_len > 32) return false;
    }
    if (prefix_str.empty()) return false;

    out.mask = (prefix_len == 0) ? 0 : (~uint32_t(0) << (32 - prefix_len));
    out.network &= out.mask;
    return true;
}

// Pre-parsed metrics auth configuration for efficient request-time checking.
// Immutable after construction — safe to share across requests on one shard.
class MetricsAuthConfig {
public:
    MetricsAuthConfig() = default;

    explicit MetricsAuthConfig(const MetricsConfig& config)
        : _auth_token(config.auth_token)
        , _auth_enabled(config.auth_enabled())
        , _ip_filter_enabled(config.ip_filter_enabled())
    {
        for (const auto& ip_str : config.allowed_ips) {
            CidrEntry entry;
            if (parse_cidr(ip_str, entry)) {
                _allowed_cidrs.push_back(entry);
            } else {
                log_metrics_auth.warn("Invalid IP/CIDR in metrics.allowed_ips: '{}', skipping", ip_str);
            }
        }

        if (_auth_enabled) {
            log_metrics_auth.info("Metrics endpoint Bearer token auth enabled");
        }
        if (_ip_filter_enabled) {
            log_metrics_auth.info("Metrics endpoint IP allowlist enabled ({} entries, {} valid)",
                config.allowed_ips.size(), _allowed_cidrs.size());
        }
    }

    bool auth_enabled() const { return _auth_enabled; }
    bool ip_filter_enabled() const { return _ip_filter_enabled; }
    bool any_enabled() const { return _auth_enabled || _ip_filter_enabled; }

    bool is_ip_allowed(std::string_view source_ip) const {
        if (!_ip_filter_enabled) return true;
        uint32_t ip = parse_ipv4(source_ip);
        if (ip == 0 && source_ip != "0.0.0.0") return false;
        for (const auto& cidr : _allowed_cidrs) {
            if ((ip & cidr.mask) == cidr.network) return true;
        }
        return false;
    }

    bool is_token_valid(std::string_view token) const {
        if (!_auth_enabled) return true;
        return AuthConfig::secure_compare(_auth_token, std::string(token));
    }

private:
    std::string _auth_token;
    std::vector<CidrEntry> _allowed_cidrs;
    bool _auth_enabled = false;
    bool _ip_filter_enabled = false;
};

// Extracts the Bearer token from an Authorization header value.
// Returns empty string_view if not in "Bearer <token>" format.
inline std::string_view extract_bearer_token(std::string_view auth_header) {
    constexpr std::string_view prefix = "Bearer ";
    if (auth_header.size() > prefix.size() &&
        std::string_view(auth_header.data(), prefix.size()) == prefix) {
        return auth_header.substr(prefix.size());
    }
    return {};
}

// HTTP handler wrapper that checks Bearer token and IP allowlist before
// delegating to the underlying Prometheus handler.
//
// Registered at GET /metrics AFTER seastar::prometheus::start() registers
// its handler. The original prometheus handler pointer is obtained via
// routes::get_handler() and stored as the delegate.
//
// Rate-limits warn logs to 1 per 10 seconds per shard to avoid log flood
// from misconfigured Prometheus scrapers.
class MetricsAuthHandler : public seastar::httpd::handler_base {
public:
    MetricsAuthHandler(MetricsAuthConfig config, seastar::httpd::handler_base* delegate)
        : _config(std::move(config))
        , _delegate(delegate)
    {}

    seastar::future<std::unique_ptr<seastar::http::reply>>
    handle(const seastar::sstring& path,
           std::unique_ptr<seastar::http::request> req,
           std::unique_ptr<seastar::http::reply> rep) override {

        // Extract source IP (prefer X-Forwarded-For for reverse proxy setups)
        auto source_ip = req->get_header("X-Forwarded-For");
        if (source_ip.empty()) {
            source_ip = req->get_client_address().addr();
        }

        // IP allowlist check (fast path — cheaper than token comparison)
        if (_config.ip_filter_enabled() && !_config.is_ip_allowed(source_ip)) {
            log_rejected("ip_denied", source_ip);
            rep->set_status(seastar::http::reply::status_type::forbidden);
            rep->_content = "Forbidden\n";
            rep->done("text/plain");
            return seastar::make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
        }

        // Bearer token check
        if (_config.auth_enabled()) {
            auto token = extract_bearer_token(req->get_header("Authorization"));
            if (token.empty() || !_config.is_token_valid(token)) {
                log_rejected("auth_failed", source_ip);
                rep->set_status(seastar::http::reply::status_type::unauthorized);
                rep->_content = "Unauthorized\n";
                rep->done("text/plain");
                return seastar::make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
            }
        }

        // Auth passed — delegate to Prometheus handler
        return _delegate->handle(path, std::move(req), std::move(rep));
    }

private:
    MetricsAuthConfig _config;
    seastar::httpd::handler_base* _delegate;  // Prometheus handler, owned by routes

    // Rate-limited warn log: at most 1 message per 10 seconds per shard
    void log_rejected(const char* reason, const seastar::sstring& source_ip) {
        auto now = std::chrono::steady_clock::now();
        ++_rejected_count;
        if (now - _last_log_time >= std::chrono::seconds(10)) {
            _last_log_time = now;
            log_metrics_auth.warn("Metrics scrape rejected: reason={}, source_ip={}, rejected_since_last_log={}",
                reason, source_ip, _rejected_count);
            _rejected_count = 0;
        }
    }

    std::chrono::steady_clock::time_point _last_log_time;
    uint64_t _rejected_count = 0;
};

}  // namespace ranvier
