#pragma once

#include <seastar/util/log.hh>

namespace ranvier {

// Ranvier loggers - one per component for fine-grained control
//
// Log levels (from seastar/util/log.hh):
//   trace, debug, info, warn, error
//
// Usage:
//   ranvier::log_router.info("Cache hit for backend {}", backend_id);
//   ranvier::log_proxy.debug("Forwarding {} bytes", size);
//   ranvier::log_health.warn("Backend {} is DOWN", id);

// Main application logger
inline seastar::logger log_main("ranvier");

// Router/routing decisions
inline seastar::logger log_router("ranvier.router");

// HTTP proxy operations
inline seastar::logger log_proxy("ranvier.proxy");

// Health check service
inline seastar::logger log_health("ranvier.health");

// Control plane operations
inline seastar::logger log_control("ranvier.control");

// Connection pool
inline seastar::logger log_pool("ranvier.pool");

} // namespace ranvier
