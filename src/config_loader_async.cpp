// Ranvier Core - Async Configuration Loader Implementation

#include "config_loader_async.hpp"
#include "logging.hpp"

#include <seastar/core/file.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/temporary_buffer.hh>

#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace ranvier {

namespace {
// 10MB cap. Real configs are <100KB; this defends against a malformed or
// runaway file wedging the reload path.
constexpr uint64_t MAX_CONFIG_SIZE = 10 * 1024 * 1024;

bool is_no_such_file(const std::system_error& e) {
    const auto& code = e.code();
    return code == std::errc::no_such_file_or_directory
        || code.value() == ENOENT;
}
} // namespace

seastar::future<RanvierConfig> load_config_async(std::string config_path) {
    return seastar::open_file_dma(config_path, seastar::open_flags::ro).then(
        [config_path](seastar::file f) {
            return f.size().then([f, config_path](uint64_t size) mutable {
                if (size == 0) {
                    log_main.warn("Config file {} is empty, using defaults", config_path);
                    return seastar::make_ready_future<RanvierConfig>(RanvierConfig::defaults());
                }
                if (size > MAX_CONFIG_SIZE) {
                    return seastar::make_exception_future<RanvierConfig>(
                        std::runtime_error(
                            "Config file exceeds 10MB limit: " + config_path));
                }
                return f.dma_read_bulk<char>(0, size).then(
                    [size, config_path](seastar::temporary_buffer<char> buf) {
                        if (buf.size() < size) {
                            return seastar::make_exception_future<RanvierConfig>(
                                std::runtime_error(
                                    "Short read on config file: " + config_path));
                        }
                        std::string yaml_text(buf.get(), buf.size());
                        return seastar::make_ready_future<RanvierConfig>(
                            RanvierConfig::load_from_string(yaml_text));
                    });
            }).finally([f]() mutable {
                return f.close();
            });
        }).handle_exception([config_path](std::exception_ptr ep) {
            // Mirror sync load(): a missing config file is not an error,
            // it falls back to defaults + env overrides.
            try {
                std::rethrow_exception(ep);
            } catch (const std::system_error& e) {
                if (is_no_such_file(e)) {
                    log_main.info("Config file not found at {}, using defaults",
                                  config_path);
                    return seastar::make_ready_future<RanvierConfig>(
                        RanvierConfig::defaults());
                }
                return seastar::make_exception_future<RanvierConfig>(ep);
            } catch (...) {
                return seastar::make_exception_future<RanvierConfig>(ep);
            }
        });
}

} // namespace ranvier
