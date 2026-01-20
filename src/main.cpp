// Ranvier Core - Content-aware Layer 7+ Load Balancer for LLM Inference
//
// Architecture:
// 1. Infrastructure Layer (TokenizerService): Handles tokenization
// 2. Domain Layer (RouterService): Handles routing logic (Radix Tree, Broadcasting)
// 3. Presentation Layer (HttpController): Handles HTTP endpoints
// 4. Persistence Layer (SqlitePersistence): Handles durable storage of routes/backends
//
// This file handles CLI argument parsing and delegates to Application for
// service orchestration.

#include "application.hpp"
#include "config.hpp"

#include <fstream>
#include <iostream>
#include <streambuf>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/program_options.hpp>
#include <seastar/core/app-template.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/smp_options.hh>

using namespace seastar;

// UTF-8 status symbols for console output
constexpr const char* kCheckMark = "\xE2\x9C\x93";  // ✓ (U+2713)
constexpr const char* kCrossMark = "\xE2\x9C\x97";  // ✗ (U+2717)

// Dry-run validation - checks configuration, tokenizer, database, and TLS without starting services
// Returns 0 if all checks pass, 1 if any fail
static int run_dry_run_validation(const std::string& config_path, const ranvier::RanvierConfig& config) {
    int error_count = 0;

    std::cout << "\nRanvier Core - Dry Run Validation\n\n";

    // ============================================================
    // Configuration validation
    // ============================================================
    std::cout << "Configuration: " << config_path << "\n";

    // Check if config file actually exists
    std::ifstream config_file(config_path);
    if (config_file.is_open()) {
        config_file.close();
        std::cout << "  " << kCheckMark << " Config file parsed successfully\n";
    } else {
        std::cout << "  ! Config file not found, using defaults\n";
    }
    std::cout << "  " << kCheckMark << " API port: " << config.server.api_port << "\n";
    std::cout << "  " << kCheckMark << " Metrics port: " << config.server.metrics_port << "\n";

    // Run config validation
    auto validation_error = ranvier::RanvierConfig::validate(config);
    if (validation_error) {
        std::cout << "  " << kCrossMark << " Validation error: " << *validation_error << "\n";
        error_count++;
    } else {
        std::cout << "  " << kCheckMark << " Configuration validation passed\n";
    }

    // ============================================================
    // Tokenizer validation
    // ============================================================
    std::cout << "\nTokenizers:\n";

    // Check tokenizer file exists
    std::ifstream tokenizer_file(config.assets.tokenizer_path);
    if (!tokenizer_file.is_open()) {
        std::cout << "  " << kCrossMark << " " << config.assets.tokenizer_path << " (file not found)\n";
        error_count++;
    } else {
        // Try to read and parse the JSON
        try {
            std::string json_str((std::istreambuf_iterator<char>(tokenizer_file)),
                                 std::istreambuf_iterator<char>());
            tokenizer_file.close();

            // Try to load the tokenizer to validate the JSON
            ranvier::TokenizerService test_tokenizer;
            test_tokenizer.load_from_json(json_str);

            if (test_tokenizer.is_loaded()) {
                std::cout << "  " << kCheckMark << " " << config.assets.tokenizer_path << " (valid)\n";
            } else {
                std::cout << "  " << kCrossMark << " " << config.assets.tokenizer_path << " (failed to parse)\n";
                error_count++;
            }
        } catch (const std::exception& e) {
            std::cout << "  " << kCrossMark << " " << config.assets.tokenizer_path << " (invalid: " << e.what() << ")\n";
            error_count++;
        }
    }

    // ============================================================
    // Database validation
    // ============================================================
    std::cout << "\nDatabase:\n";

    const std::string& db_path = config.database.path;

    // Check if the database file exists
    struct stat db_stat;
    if (stat(db_path.c_str(), &db_stat) == 0) {
        // File exists, check if it's writable
        if (access(db_path.c_str(), W_OK) == 0) {
            std::cout << "  " << kCheckMark << " " << db_path << " (writable)\n";
        } else {
            std::cout << "  " << kCrossMark << " " << db_path << " (not writable)\n";
            error_count++;
        }
    } else {
        // File doesn't exist, check if parent directory is writable
        std::string parent_dir = ".";
        size_t last_slash = db_path.find_last_of('/');
        if (last_slash != std::string::npos) {
            parent_dir = db_path.substr(0, last_slash);
            if (parent_dir.empty()) {
                parent_dir = "/";
            }
        }

        if (access(parent_dir.c_str(), W_OK) == 0) {
            std::cout << "  " << kCheckMark << " " << db_path << " (can be created)\n";
        } else {
            std::cout << "  " << kCrossMark << " " << db_path << " (parent directory not writable: " << parent_dir << ")\n";
            error_count++;
        }
    }

    // ============================================================
    // TLS validation
    // ============================================================
    std::cout << "\nTLS:\n";

    if (!config.tls.enabled) {
        std::cout << "  - TLS disabled\n";
    } else {
        // Check certificate file
        if (config.tls.cert_path.empty()) {
            std::cout << "  " << kCrossMark << " Certificate: (path not configured)\n";
            error_count++;
        } else if (access(config.tls.cert_path.c_str(), R_OK) == 0) {
            std::cout << "  " << kCheckMark << " Certificate: " << config.tls.cert_path << "\n";
        } else {
            std::cout << "  " << kCrossMark << " Certificate: " << config.tls.cert_path << " (not readable)\n";
            error_count++;
        }

        // Check private key file
        if (config.tls.key_path.empty()) {
            std::cout << "  " << kCrossMark << " Private key: (path not configured)\n";
            error_count++;
        } else if (access(config.tls.key_path.c_str(), R_OK) == 0) {
            std::cout << "  " << kCheckMark << " Private key: " << config.tls.key_path << "\n";
        } else {
            std::cout << "  " << kCrossMark << " Private key: " << config.tls.key_path << " (not readable)\n";
            error_count++;
        }
    }

    // ============================================================
    // Summary
    // ============================================================
    std::cout << "\n";
    if (error_count == 0) {
        std::cout << "Result: PASSED\n";
        return 0;
    } else {
        std::cout << "Result: FAILED (" << error_count << " error" << (error_count == 1 ? "" : "s") << ")\n";
        return 1;
    }
}

// Print configuration summary to stdout
static void print_config_summary(const std::string& config_path, const ranvier::RanvierConfig& config) {
    // Check if config file actually exists
    std::ifstream config_check(config_path);
    if (config_check.is_open()) {
        config_check.close();
        std::cout << "Ranvier Core - Configuration loaded from " << config_path << "\n";
    } else {
        std::cout << "Ranvier Core - Using built-in defaults (" << config_path << " not found)\n";
    }

    std::cout << "  API Port:     " << config.server.api_port << "\n";
    std::cout << "  Metrics Port: " << config.server.metrics_port << "\n";
    std::cout << "  Database:     " << config.database.path << "\n";
    std::cout << "  Health Check: " << config.health.check_interval.count() << "s interval\n";
    std::cout << "  Pool Size:    " << config.pool.max_connections_per_host << " per host, "
              << config.pool.max_total_connections << " total\n";
    std::cout << "  Timeouts:     " << config.timeouts.connect_timeout.count() << "s connect, "
              << config.timeouts.request_timeout.count() << "s request\n";
    std::cout << "  Min Tokens:   " << config.routing.min_token_length << "\n";

    if (config.tls.enabled) {
        std::cout << "  TLS:          enabled (cert: " << config.tls.cert_path << ")\n";
    } else {
        std::cout << "  TLS:          disabled\n";
    }

    if (!config.auth.admin_api_key.empty()) {
        std::cout << "  Admin Auth:   enabled (API key configured)\n";
    } else {
        std::cout << "  Admin Auth:   disabled (no API key)\n";
    }

    if (config.rate_limit.enabled) {
        std::cout << "  Rate Limit:   " << config.rate_limit.requests_per_second
                  << " req/s, burst " << config.rate_limit.burst_size << "\n";
    } else {
        std::cout << "  Rate Limit:   disabled\n";
    }

    if (config.retry.max_retries > 0) {
        std::cout << "  Retry:        " << config.retry.max_retries << " retries, "
                  << config.retry.initial_backoff.count() << "-"
                  << config.retry.max_backoff.count() << "ms backoff\n";
    } else {
        std::cout << "  Retry:        disabled\n";
    }

    if (config.circuit_breaker.enabled) {
        std::cout << "  Circuit:      " << config.circuit_breaker.failure_threshold << " failures, "
                  << config.circuit_breaker.recovery_timeout.count() << "s recovery";
        if (config.circuit_breaker.fallback_enabled) {
            std::cout << " (fallback on)";
        }
        std::cout << "\n";
    } else {
        std::cout << "  Circuit:      disabled\n";
    }

    std::cout << "  Drain Timeout: " << config.shutdown.drain_timeout.count() << "s\n";

    if (config.cluster.enabled) {
        std::cout << "  Cluster:      port " << config.cluster.gossip_port
                  << ", " << config.cluster.peers.size() << " peers\n";
    } else {
        std::cout << "  Cluster:      disabled (standalone mode)\n";
    }

    if (config.k8s_discovery.enabled) {
        std::cout << "  K8s Discovery: " << config.k8s_discovery.namespace_name
                  << "/" << config.k8s_discovery.service_name
                  << " (port " << config.k8s_discovery.target_port << ")\n";
    } else {
        std::cout << "  K8s Discovery: disabled\n";
    }

    if (config.telemetry.enabled) {
        std::cout << "  Telemetry:    " << config.telemetry.otlp_endpoint
                  << " (sample_rate: " << config.telemetry.sample_rate << ")\n";
    } else {
        std::cout << "  Telemetry:    disabled\n";
    }
}

// Print help message
static void print_help() {
    std::cout << R"(Ranvier Core - Content-aware Layer 7+ Load Balancer for LLM Inference

USAGE:
    ranvier_server [OPTIONS]

DESCRIPTION:
    Ranvier routes LLM requests based on token prefixes rather than
    connection availability, reducing GPU cache thrashing by directing
    requests to backends that already hold relevant KV cache state.

OPTIONS:
    -h, --help              Print this help message and exit
    --help-seastar          Show Seastar framework options
    --help-loggers          Print available logger names
    --config <PATH>         Path to configuration file (default: ranvier.yaml,
                            falls back to built-in defaults if not found)
    --dry-run               Validate configuration and exit (no server start)
    --smp <N>               Number of CPU cores to use
    --memory <SIZE>         Memory to allocate (e.g., 4G)
    --std-alloc             Use standard libc allocator (required for Rust FFI
                            with multiple shards)

SIGNALS:
    SIGHUP                  Reload configuration (hot-reload)
    SIGINT, SIGTERM         Graceful shutdown with connection draining

EXAMPLES:
    ranvier_server
        Start with ranvier.yaml if present, otherwise use built-in defaults

    ranvier_server --config /etc/ranvier/config.yaml
        Start with custom config file

    ranvier_server --dry-run
        Validate configuration without starting the server

    ranvier_server --smp 4 --memory 8G
        Start with 4 CPU cores and 8GB memory

For more information, see: https://github.com/ranvier-systems/ranvier-core
)";
}

int main(int argc, char** argv) {
    // Check for --help BEFORE loading config (avoids config errors blocking help)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        }
    }

    // Load configuration BEFORE Seastar starts
    std::string config_path = "ranvier.yaml";
    bool dry_run = false;

    // Check for --config and --dry-run arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[i + 1];
        } else if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.substr(9);
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--std-alloc") {
            // Deprecated: use --memory-allocator=standard instead
            std::cerr << "WARNING: --std-alloc is deprecated and non-functional.\n"
                      << "         Use Seastar's native --memory-allocator=standard instead.\n";
        }
    }

    ranvier::RanvierConfig config;
    try {
        config = ranvier::RanvierConfig::load(config_path);

        // Run dry-run validation if requested
        if (dry_run) {
            return run_dry_run_validation(config_path, config);
        }

        // Print config summary
        print_config_summary(config_path, config);

    } catch (const std::exception& e) {
        std::cerr << "Failed to load config: " << e.what() << "\n";
        return 1;
    }

    // Create Seastar app template
    app_template::config app_cfg;
    app_cfg.name = "ranvier_server";
    app_template app(std::move(app_cfg));

    // Register our custom options with Seastar so they're recognized
    // NOTE: For standard allocator, use Seastar's native --memory-allocator=standard
    app.add_options()
        ("config", boost::program_options::value<std::string>()->default_value("ranvier.yaml"),
         "Path to configuration file")
        ("dry-run", "Validate configuration and exit (no server start)")
        ("std-alloc", "DEPRECATED: use --memory-allocator=standard instead");

    // Run the application
    return app.run(argc, argv, [config = std::move(config), config_path]() mutable {
        // Create the Application instance
        auto app_ptr = std::make_unique<ranvier::Application>(std::move(config), config_path);

        // Startup the application, then run the server loop
        return app_ptr->startup().then([app_ptr = std::move(app_ptr)]() mutable {
            // Run the server loop (blocks until shutdown)
            return app_ptr->run().finally([app_ptr = std::move(app_ptr)] {
                // Application cleanup happens in destructor
            });
        });
    });
}
