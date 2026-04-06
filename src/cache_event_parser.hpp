// Ranvier Core - Cache Event Parser
//
// Pure parsing logic for POST /v1/cache/events JSON payloads.
// No Seastar, no I/O, no metrics — testable without a reactor.
//
// Depends on RapidJSON (header-only) and parse_utils.hpp for hex decoding.

#pragma once

#include "parse_utils.hpp"

#include <rapidjson/document.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ranvier {

// A single validated cache event ready for dispatch
struct ParsedCacheEvent {
    std::string event_type;        // "evicted" or "loaded"
    uint64_t prefix_hash = 0;     // Decoded from hex
    uint64_t timestamp_ms = 0;
    int32_t backend_id = 0;       // 0 if not specified by sender
    bool age_expired = false;     // True if event older than max_age
};

// Result of parsing the cache events JSON envelope and its events
struct CacheEventParseResult {
    bool ok = false;
    std::string error;                       // Non-empty when ok is false
    std::vector<ParsedCacheEvent> events;    // Validated events (ok must be true)
    uint32_t skipped = 0;                    // Events skipped due to missing fields or bad hex
};

/**
 * Parse and validate a cache events JSON body.
 *
 * Pure function: no Seastar, no metrics, no logging. Validates the JSON
 * envelope (type, version, events array, batch size) and each event's
 * required fields. Decodes hex hashes and checks event age.
 *
 * @param body           Raw JSON string
 * @param max_events     Maximum allowed events per request (Rule #4)
 * @param max_age_ms     Maximum event age in milliseconds
 * @param now_ms         Current wall-clock time in milliseconds since epoch
 * @return Parse result with validated events or error message
 */
inline CacheEventParseResult parse_cache_events(
    std::string_view body,
    uint32_t max_events,
    uint64_t max_age_ms,
    uint64_t now_ms) {

    CacheEventParseResult result;

    if (body.empty()) {
        result.error = "Empty request body";
        return result;
    }

    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());
    if (doc.HasParseError() || !doc.IsObject()) {
        result.error = "Invalid JSON";
        return result;
    }

    // Validate envelope
    if (!doc.HasMember("type") || !doc["type"].IsString() ||
        std::string_view(doc["type"].GetString()) != "cache_event") {
        result.error = "Missing or invalid 'type' field (expected 'cache_event')";
        return result;
    }

    if (!doc.HasMember("version") || !doc["version"].IsInt() ||
        doc["version"].GetInt() != 1) {
        result.error = "Missing or unsupported 'version' (expected 1)";
        return result;
    }

    if (!doc.HasMember("events") || !doc["events"].IsArray()) {
        result.error = "Missing or invalid 'events' array";
        return result;
    }

    const auto& events = doc["events"].GetArray();
    if (events.Size() > max_events) {
        result.error = "Too many events (max: " + std::to_string(max_events) + ")";
        return result;
    }

    // Parse individual events
    result.events.reserve(events.Size());
    for (rapidjson::SizeType i = 0; i < events.Size(); ++i) {
        const auto& ev = events[i];
        if (!ev.IsObject()) {
            result.skipped++;
            continue;
        }

        // Validate required fields
        if (!ev.HasMember("event") || !ev["event"].IsString() ||
            !ev.HasMember("prefix_hash") || !ev["prefix_hash"].IsString() ||
            !ev.HasMember("timestamp_ms") || !ev["timestamp_ms"].IsUint64()) {
            result.skipped++;
            continue;
        }

        // Decode prefix hash
        auto prefix_hash_opt = decode_prefix_hash_hex(ev["prefix_hash"].GetString());
        if (!prefix_hash_opt) {
            result.skipped++;
            continue;
        }

        ParsedCacheEvent parsed;
        parsed.event_type = ev["event"].GetString();
        parsed.prefix_hash = *prefix_hash_opt;
        parsed.timestamp_ms = ev["timestamp_ms"].GetUint64();

        // Check event age
        if (max_age_ms > 0 && now_ms > parsed.timestamp_ms &&
            (now_ms - parsed.timestamp_ms) > max_age_ms) {
            parsed.age_expired = true;
        }

        // Optional backend_id
        if (ev.HasMember("backend_id") && ev["backend_id"].IsInt()) {
            parsed.backend_id = ev["backend_id"].GetInt();
        }

        result.events.push_back(std::move(parsed));
    }

    result.ok = true;
    return result;
}

} // namespace ranvier
