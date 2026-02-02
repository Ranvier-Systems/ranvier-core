// Ranvier Core - Configuration Management
//
// This is a facade header that provides backward compatibility.
// It includes both config_schema.hpp (data structures) and config_loader.hpp (loading).
//
// For new code, you may prefer to include only what you need:
//   #include "config_schema.hpp"  - For struct definitions only
//   #include "config_loader.hpp"  - For loading/validation (includes schema)
//   #include "config.hpp"         - For both (this file, backward compatible)
//
// Supports loading configuration from:
// 1. YAML config file (default: ranvier.yaml)
// 2. Environment variables (override file settings)
// 3. Built-in defaults (fallback)

#pragma once

#include "config_schema.hpp"
#include "config_loader.hpp"
