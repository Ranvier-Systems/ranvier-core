// Ranvier Core - Async Configuration Loader
//
// Reactor-safe entry point for the SIGHUP-driven config hot-reload path.
// Uses Seastar's DMA file I/O so the read does not stall the shard.
//
// The pre-reactor startup path in `main()` continues to use the sync
// `RanvierConfig::load()` declared in config_schema.hpp. Both paths share the
// YAML parsing body via `RanvierConfig::load_from_string()`.

#pragma once

#include "config_schema.hpp"

#include <seastar/core/future.hh>

#include <string>

namespace ranvier {

// Reactor-safe analogue of `RanvierConfig::load(path)`.
//
// Behaviour parity with the sync loader:
//   - Missing file → returns `RanvierConfig::defaults()` (env overrides applied).
//   - Malformed YAML → exceptional future carrying std::runtime_error.
//   - Otherwise → fully parsed config with env overrides applied.
//
// Uses `seastar::open_file_dma` + `dma_read_bulk` for the read; YAML parsing
// runs inline on the calling shard. Configs are typically <100KB so parse cost
// is sub-millisecond. A 10MB cap rejects malformed/oversized files before any
// allocation.
seastar::future<RanvierConfig> load_config_async(std::string config_path);

} // namespace ranvier
