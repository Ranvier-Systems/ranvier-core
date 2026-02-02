// Ranvier Core - Configuration Loader
//
// Provides YAML loading, validation, and environment variable overrides.
// The actual RanvierConfig struct and nested types are defined in config_schema.hpp.
//
// Usage:
//   #include "config_loader.hpp"  // For loading/validation only
//   #include "config_schema.hpp"  // For struct definitions only
//   #include "config.hpp"         // For both (backward compatible facade)

#pragma once

#include "config_schema.hpp"

// Note: The implementation is in config_loader.cpp.
// RanvierConfig::load(), defaults(), and validate() are declared in config_schema.hpp
// as part of the RanvierConfig struct.
