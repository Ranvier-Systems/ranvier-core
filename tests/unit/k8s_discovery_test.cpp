// Ranvier Core - K8s Discovery Service Unit Tests
//
// Tests for K8s endpoint parsing and JSON helper functions.
// These tests don't require Seastar runtime or actual K8s API access.

#include <gtest/gtest.h>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Include only the types we need (avoid Seastar headers)
#include "types.hpp"

using namespace ranvier;

// =============================================================================
// K8s JSON Helper Functions - Replicated here to avoid Seastar dependencies
// =============================================================================

namespace k8s_json {

// Find the end of a JSON string (handles escapes)
static size_t find_string_end(const std::string& json, size_t start) {
    size_t pos = start;
    while (pos < json.size()) {
        if (json[pos] == '\\') {
            pos += 2;  // Skip escaped char
        } else if (json[pos] == '"') {
            return pos;
        } else {
            ++pos;
        }
    }
    return std::string::npos;
}

// Find matching bracket/brace
static size_t find_matching(const std::string& json, size_t start, char open, char close) {
    int depth = 1;
    size_t pos = start;
    while (pos < json.size() && depth > 0) {
        if (json[pos] == '"') {
            pos = find_string_end(json, pos + 1);
            if (pos == std::string::npos) return std::string::npos;
        } else if (json[pos] == open) {
            ++depth;
        } else if (json[pos] == close) {
            --depth;
            if (depth == 0) return pos;
        }
        ++pos;
    }
    return std::string::npos;
}

// Find key in JSON object
static std::optional<std::pair<size_t, size_t>> find_key_value(const std::string& json, const std::string& key) {
    std::string search_key = "\"" + key + "\"";
    size_t pos = 0;

    while (pos < json.size()) {
        pos = json.find(search_key, pos);
        if (pos == std::string::npos) return std::nullopt;

        // Find colon after key
        size_t colon = json.find(':', pos + search_key.size());
        if (colon == std::string::npos) return std::nullopt;

        // Skip whitespace after colon
        size_t value_start = colon + 1;
        while (value_start < json.size() && std::isspace(json[value_start])) {
            ++value_start;
        }

        if (value_start >= json.size()) return std::nullopt;

        // Determine value end based on value type
        size_t value_end;
        char c = json[value_start];

        if (c == '"') {
            // String value
            value_end = find_string_end(json, value_start + 1);
            if (value_end != std::string::npos) ++value_end;
        } else if (c == '{') {
            // Object value
            value_end = find_matching(json, value_start + 1, '{', '}');
            if (value_end != std::string::npos) ++value_end;
        } else if (c == '[') {
            // Array value
            value_end = find_matching(json, value_start + 1, '[', ']');
            if (value_end != std::string::npos) ++value_end;
        } else {
            // Number, boolean, or null
            value_end = value_start;
            while (value_end < json.size() &&
                   !std::isspace(json[value_end]) &&
                   json[value_end] != ',' &&
                   json[value_end] != '}' &&
                   json[value_end] != ']') {
                ++value_end;
            }
        }

        if (value_end != std::string::npos && value_end > value_start) {
            return std::make_pair(value_start, value_end);
        }

        pos += search_key.size();
    }

    return std::nullopt;
}

std::optional<std::string> get_string(const std::string& json, const std::string& key) {
    auto result = find_key_value(json, key);
    if (!result) return std::nullopt;

    auto [start, end] = *result;
    if (start >= json.size() || json[start] != '"') return std::nullopt;

    // Extract string content (without quotes)
    return json.substr(start + 1, end - start - 2);
}

std::optional<int64_t> get_int(const std::string& json, const std::string& key) {
    auto result = find_key_value(json, key);
    if (!result) return std::nullopt;

    auto [start, end] = *result;
    std::string value = json.substr(start, end - start);

    try {
        return std::stoll(value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> get_bool(const std::string& json, const std::string& key) {
    auto result = find_key_value(json, key);
    if (!result) return std::nullopt;

    auto [start, end] = *result;
    std::string value = json.substr(start, end - start);

    if (value == "true") return true;
    if (value == "false") return false;
    return std::nullopt;
}

std::vector<std::string> get_array(const std::string& json, const std::string& key) {
    std::vector<std::string> items;

    auto result = find_key_value(json, key);
    if (!result) return items;

    auto [start, end] = *result;
    if (start >= json.size() || json[start] != '[') return items;

    // Parse array elements
    size_t pos = start + 1;
    while (pos < end - 1) {
        // Skip whitespace
        while (pos < end && std::isspace(json[pos])) ++pos;
        if (pos >= end - 1 || json[pos] == ']') break;

        // Find element
        size_t elem_start = pos;
        size_t elem_end;
        char c = json[pos];

        if (c == '"') {
            elem_end = find_string_end(json, pos + 1);
            if (elem_end != std::string::npos) ++elem_end;
        } else if (c == '{') {
            elem_end = find_matching(json, pos + 1, '{', '}');
            if (elem_end != std::string::npos) ++elem_end;
        } else if (c == '[') {
            elem_end = find_matching(json, pos + 1, '[', ']');
            if (elem_end != std::string::npos) ++elem_end;
        } else {
            elem_end = pos;
            while (elem_end < end && json[elem_end] != ',' && json[elem_end] != ']') {
                ++elem_end;
            }
        }

        if (elem_end != std::string::npos && elem_end > elem_start) {
            items.push_back(json.substr(elem_start, elem_end - elem_start));
        }

        // Skip comma
        pos = elem_end;
        while (pos < end && (std::isspace(json[pos]) || json[pos] == ',')) ++pos;
    }

    return items;
}

std::optional<std::string> get_object(const std::string& json, const std::string& key) {
    auto result = find_key_value(json, key);
    if (!result) return std::nullopt;

    auto [start, end] = *result;
    if (start >= json.size() || json[start] != '{') return std::nullopt;

    return json.substr(start, end - start);
}

bool is_error(const std::string& json) {
    auto kind = get_string(json, "kind");
    return kind && *kind == "Status";
}

std::string get_error_message(const std::string& json) {
    auto message = get_string(json, "message");
    return message.value_or("Unknown error");
}

}  // namespace k8s_json

// =============================================================================
// K8sEndpoint - Replicated here to avoid Seastar dependencies
// =============================================================================

constexpr uint32_t K8S_DEFAULT_WEIGHT = 100;
constexpr uint32_t K8S_DEFAULT_PRIORITY = 0;
constexpr uint32_t K8S_MAX_WEIGHT = 1000000;
constexpr uint32_t K8S_MAX_PRIORITY = 1000;

struct K8sEndpoint {
    std::string uid;
    std::string address;
    uint16_t port;
    bool ready;
    uint32_t weight = K8S_DEFAULT_WEIGHT;
    uint32_t priority = K8S_DEFAULT_PRIORITY;

    // FNV-1a 64-bit hash, truncated to 31 bits for positive BackendId.
    // Mirrors the production implementation in k8s_discovery_service.cpp.
    BackendId to_backend_id() const {
        constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
        constexpr uint64_t kFnvPrime = 1099511628211ULL;

        uint64_t hash = kFnvOffsetBasis;
        for (unsigned char c : uid) {
            hash ^= c;
            hash *= kFnvPrime;
        }
        return static_cast<BackendId>(hash & 0x7FFFFFFF);
    }

    bool operator==(const K8sEndpoint& other) const {
        return uid == other.uid;
    }
};

// =============================================================================
// JSON Parser Tests
// =============================================================================

class K8sJsonTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(K8sJsonTest, GetStringSimple) {
    std::string json = R"({"name": "test-service"})";
    auto result = k8s_json::get_string(json, "name");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "test-service");
}

TEST_F(K8sJsonTest, GetStringNotFound) {
    std::string json = R"({"name": "test"})";
    auto result = k8s_json::get_string(json, "missing");
    EXPECT_FALSE(result.has_value());
}

TEST_F(K8sJsonTest, GetStringWithSpaces) {
    std::string json = R"({ "name" : "hello world" })";
    auto result = k8s_json::get_string(json, "name");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello world");
}

TEST_F(K8sJsonTest, GetStringNested) {
    std::string json = R"({"metadata": {"name": "my-pod", "namespace": "default"}})";
    auto metadata = k8s_json::get_object(json, "metadata");
    ASSERT_TRUE(metadata.has_value());

    auto name = k8s_json::get_string(*metadata, "name");
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(*name, "my-pod");

    auto ns = k8s_json::get_string(*metadata, "namespace");
    ASSERT_TRUE(ns.has_value());
    EXPECT_EQ(*ns, "default");
}

TEST_F(K8sJsonTest, GetIntSimple) {
    std::string json = R"({"port": 8080})";
    auto result = k8s_json::get_int(json, "port");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 8080);
}

TEST_F(K8sJsonTest, GetIntNegative) {
    std::string json = R"({"offset": -100})";
    auto result = k8s_json::get_int(json, "offset");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, -100);
}

TEST_F(K8sJsonTest, GetBoolTrue) {
    std::string json = R"({"ready": true})";
    auto result = k8s_json::get_bool(json, "ready");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result);
}

TEST_F(K8sJsonTest, GetBoolFalse) {
    std::string json = R"({"ready": false})";
    auto result = k8s_json::get_bool(json, "ready");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(*result);
}

TEST_F(K8sJsonTest, GetArrayStrings) {
    std::string json = R"({"addresses": ["10.0.0.1", "10.0.0.2", "10.0.0.3"]})";
    auto result = k8s_json::get_array(json, "addresses");
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], "\"10.0.0.1\"");
    EXPECT_EQ(result[1], "\"10.0.0.2\"");
    EXPECT_EQ(result[2], "\"10.0.0.3\"");
}

TEST_F(K8sJsonTest, GetArrayObjects) {
    std::string json = R"({"items": [{"name": "a"}, {"name": "b"}]})";
    auto result = k8s_json::get_array(json, "items");
    ASSERT_EQ(result.size(), 2u);

    auto name1 = k8s_json::get_string(result[0], "name");
    ASSERT_TRUE(name1.has_value());
    EXPECT_EQ(*name1, "a");

    auto name2 = k8s_json::get_string(result[1], "name");
    ASSERT_TRUE(name2.has_value());
    EXPECT_EQ(*name2, "b");
}

TEST_F(K8sJsonTest, GetArrayEmpty) {
    std::string json = R"({"items": []})";
    auto result = k8s_json::get_array(json, "items");
    EXPECT_TRUE(result.empty());
}

TEST_F(K8sJsonTest, GetObjectSimple) {
    std::string json = R"({"metadata": {"uid": "abc123"}})";
    auto result = k8s_json::get_object(json, "metadata");
    ASSERT_TRUE(result.has_value());

    auto uid = k8s_json::get_string(*result, "uid");
    ASSERT_TRUE(uid.has_value());
    EXPECT_EQ(*uid, "abc123");
}

TEST_F(K8sJsonTest, IsErrorDetectsStatus) {
    std::string json = R"({"kind": "Status", "message": "not found"})";
    EXPECT_TRUE(k8s_json::is_error(json));
}

TEST_F(K8sJsonTest, IsErrorReturnsFalseForNormal) {
    std::string json = R"({"kind": "EndpointSliceList", "items": []})";
    EXPECT_FALSE(k8s_json::is_error(json));
}

TEST_F(K8sJsonTest, GetErrorMessage) {
    std::string json = R"({"kind": "Status", "message": "service not found"})";
    auto msg = k8s_json::get_error_message(json);
    EXPECT_EQ(msg, "service not found");
}

// =============================================================================
// K8s EndpointSlice Parsing Tests
// =============================================================================

class K8sEndpointSliceTest : public ::testing::Test {
protected:
    // Simulate parsing an EndpointSlice
    // This mirrors the logic in k8s_discovery_service.cpp::parse_endpoint_slice()
    std::vector<K8sEndpoint> parse_endpoint_slice(const std::string& json, uint16_t default_port = 8080) {
        std::vector<K8sEndpoint> endpoints;

        // Get metadata for annotations
        auto metadata = k8s_json::get_object(json, "metadata");
        uint32_t default_weight = K8S_DEFAULT_WEIGHT;
        uint32_t default_priority = K8S_DEFAULT_PRIORITY;

        if (metadata) {
            auto annotations = k8s_json::get_object(*metadata, "annotations");
            if (annotations) {
                auto weight_str = k8s_json::get_string(*annotations, "ranvier.io/weight");
                if (weight_str) {
                    try {
                        // Reject negative values (stoul wraps them)
                        if (!weight_str->empty() && (*weight_str)[0] == '-') {
                            throw std::invalid_argument("negative value not allowed");
                        }
                        size_t pos = 0;
                        unsigned long parsed = std::stoul(*weight_str, &pos);
                        // Ensure entire string was consumed (no trailing garbage)
                        if (pos != weight_str->size()) {
                            throw std::invalid_argument("contains non-numeric characters");
                        }
                        if (parsed > K8S_MAX_WEIGHT) {
                            default_weight = K8S_MAX_WEIGHT;  // Clamp to max
                        } else {
                            default_weight = static_cast<uint32_t>(parsed);
                        }
                    } catch (const std::exception&) {
                        // Keep default_weight on parse failure (logging in real impl)
                    }
                }

                auto priority_str = k8s_json::get_string(*annotations, "ranvier.io/priority");
                if (priority_str) {
                    try {
                        // Reject negative values (stoul wraps them)
                        if (!priority_str->empty() && (*priority_str)[0] == '-') {
                            throw std::invalid_argument("negative value not allowed");
                        }
                        size_t pos = 0;
                        unsigned long parsed = std::stoul(*priority_str, &pos);
                        // Ensure entire string was consumed (no trailing garbage)
                        if (pos != priority_str->size()) {
                            throw std::invalid_argument("contains non-numeric characters");
                        }
                        if (parsed > K8S_MAX_PRIORITY) {
                            default_priority = K8S_MAX_PRIORITY;  // Clamp to max
                        } else {
                            default_priority = static_cast<uint32_t>(parsed);
                        }
                    } catch (const std::exception&) {
                        // Keep default_priority on parse failure (logging in real impl)
                    }
                }
            }
        }

        // Get ports to find the target port
        uint16_t port = default_port;
        auto ports = k8s_json::get_array(json, "ports");
        for (const auto& p : ports) {
            auto port_num = k8s_json::get_int(p, "port");
            if (port_num) {
                port = static_cast<uint16_t>(*port_num);
                break;
            }
        }

        // Get endpoints array
        auto endpoint_items = k8s_json::get_array(json, "endpoints");

        for (const auto& ep : endpoint_items) {
            // Check if endpoint is ready
            auto conditions = k8s_json::get_object(ep, "conditions");
            bool ready = true;
            if (conditions) {
                auto ready_val = k8s_json::get_bool(*conditions, "ready");
                if (ready_val) {
                    ready = *ready_val;
                }
            }

            // Get target reference for UID
            std::string uid;
            auto target_ref = k8s_json::get_object(ep, "targetRef");
            if (target_ref) {
                auto ref_uid = k8s_json::get_string(*target_ref, "uid");
                if (ref_uid) {
                    uid = *ref_uid;
                }
            }

            // Get addresses
            auto addresses = k8s_json::get_array(ep, "addresses");
            for (const auto& addr : addresses) {
                std::string address = addr;
                // Remove quotes if present
                if (address.size() >= 2 && address.front() == '"' && address.back() == '"') {
                    address = address.substr(1, address.size() - 2);
                }

                K8sEndpoint endpoint;
                endpoint.uid = uid.empty() ? address : uid + "-" + address;
                endpoint.address = address;
                endpoint.port = port;
                endpoint.ready = ready;
                endpoint.weight = default_weight;
                endpoint.priority = default_priority;

                endpoints.push_back(std::move(endpoint));
            }
        }

        return endpoints;
    }
};

TEST_F(K8sEndpointSliceTest, ParseSingleEndpoint) {
    std::string json = R"({
        "metadata": {"name": "gpu-service-abc"},
        "ports": [{"port": 8080, "protocol": "TCP"}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-uid-123"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints[0].address, "10.0.0.1");
    EXPECT_EQ(endpoints[0].port, 8080);
    EXPECT_TRUE(endpoints[0].ready);
    EXPECT_EQ(endpoints[0].uid, "pod-uid-123-10.0.0.1");
    EXPECT_EQ(endpoints[0].weight, K8S_DEFAULT_WEIGHT);
    EXPECT_EQ(endpoints[0].priority, K8S_DEFAULT_PRIORITY);
}

TEST_F(K8sEndpointSliceTest, ParseMultipleEndpoints) {
    std::string json = R"({
        "ports": [{"port": 9000}],
        "endpoints": [
            {"addresses": ["10.0.0.1"], "conditions": {"ready": true}, "targetRef": {"uid": "pod-1"}},
            {"addresses": ["10.0.0.2"], "conditions": {"ready": true}, "targetRef": {"uid": "pod-2"}},
            {"addresses": ["10.0.0.3"], "conditions": {"ready": false}, "targetRef": {"uid": "pod-3"}}
        ]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 3u);

    EXPECT_EQ(endpoints[0].address, "10.0.0.1");
    EXPECT_TRUE(endpoints[0].ready);

    EXPECT_EQ(endpoints[1].address, "10.0.0.2");
    EXPECT_TRUE(endpoints[1].ready);

    EXPECT_EQ(endpoints[2].address, "10.0.0.3");
    EXPECT_FALSE(endpoints[2].ready);
}

TEST_F(K8sEndpointSliceTest, ParseWithAnnotations) {
    std::string json = R"({
        "metadata": {
            "name": "gpu-service-abc",
            "annotations": {
                "ranvier.io/weight": "200",
                "ranvier.io/priority": "1"
            }
        },
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-uid-123"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    EXPECT_EQ(endpoints[0].weight, 200u);
    EXPECT_EQ(endpoints[0].priority, 1u);
}

// =============================================================================
// Invalid Annotation Tests - Verify defaults are used on parse errors
// =============================================================================

TEST_F(K8sEndpointSliceTest, ParseWithInvalidWeightTypo) {
    // Common typo: letter 'O' instead of digit '0'
    std::string json = R"({
        "metadata": {
            "name": "gpu-service-abc",
            "annotations": {
                "ranvier.io/weight": "1O0",
                "ranvier.io/priority": "1"
            }
        },
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-uid-123"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    // Invalid weight should fall back to default
    EXPECT_EQ(endpoints[0].weight, K8S_DEFAULT_WEIGHT);
    // Valid priority should be parsed correctly
    EXPECT_EQ(endpoints[0].priority, 1u);
}

TEST_F(K8sEndpointSliceTest, ParseWithInvalidPriorityNonNumeric) {
    std::string json = R"({
        "metadata": {
            "name": "gpu-service-abc",
            "annotations": {
                "ranvier.io/weight": "200",
                "ranvier.io/priority": "high"
            }
        },
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-uid-123"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    // Valid weight should be parsed correctly
    EXPECT_EQ(endpoints[0].weight, 200u);
    // Invalid priority should fall back to default
    EXPECT_EQ(endpoints[0].priority, K8S_DEFAULT_PRIORITY);
}

TEST_F(K8sEndpointSliceTest, ParseWithNegativeWeight) {
    std::string json = R"({
        "metadata": {
            "name": "gpu-service-abc",
            "annotations": {
                "ranvier.io/weight": "-100",
                "ranvier.io/priority": "0"
            }
        },
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-uid-123"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    // Negative weight should fall back to default (stoul throws on negative)
    EXPECT_EQ(endpoints[0].weight, K8S_DEFAULT_WEIGHT);
}

TEST_F(K8sEndpointSliceTest, ParseWithEmptyAnnotationValues) {
    std::string json = R"({
        "metadata": {
            "name": "gpu-service-abc",
            "annotations": {
                "ranvier.io/weight": "",
                "ranvier.io/priority": ""
            }
        },
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-uid-123"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    // Empty values should fall back to defaults
    EXPECT_EQ(endpoints[0].weight, K8S_DEFAULT_WEIGHT);
    EXPECT_EQ(endpoints[0].priority, K8S_DEFAULT_PRIORITY);
}

// =============================================================================
// Range Validation Tests - Verify clamping for out-of-range values
// =============================================================================

TEST_F(K8sEndpointSliceTest, ParseWithWeightExceedingMax) {
    std::string json = R"({
        "metadata": {
            "name": "gpu-service-abc",
            "annotations": {
                "ranvier.io/weight": "2000000",
                "ranvier.io/priority": "0"
            }
        },
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-uid-123"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    // Weight exceeding max should be clamped to K8S_MAX_WEIGHT
    EXPECT_EQ(endpoints[0].weight, K8S_MAX_WEIGHT);
}

TEST_F(K8sEndpointSliceTest, ParseWithPriorityExceedingMax) {
    std::string json = R"({
        "metadata": {
            "name": "gpu-service-abc",
            "annotations": {
                "ranvier.io/weight": "100",
                "ranvier.io/priority": "5000"
            }
        },
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-uid-123"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    // Priority exceeding max should be clamped to K8S_MAX_PRIORITY
    EXPECT_EQ(endpoints[0].priority, K8S_MAX_PRIORITY);
}

TEST_F(K8sEndpointSliceTest, ParseWithWeightAtExactMax) {
    std::string json = R"({
        "metadata": {
            "name": "gpu-service-abc",
            "annotations": {
                "ranvier.io/weight": "1000000",
                "ranvier.io/priority": "0"
            }
        },
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-uid-123"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    // Weight at exact max should be accepted
    EXPECT_EQ(endpoints[0].weight, K8S_MAX_WEIGHT);
}

TEST_F(K8sEndpointSliceTest, ParseWithPriorityAtExactMax) {
    std::string json = R"({
        "metadata": {
            "name": "gpu-service-abc",
            "annotations": {
                "ranvier.io/weight": "100",
                "ranvier.io/priority": "1000"
            }
        },
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-uid-123"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    // Priority at exact max should be accepted
    EXPECT_EQ(endpoints[0].priority, K8S_MAX_PRIORITY);
}

TEST_F(K8sEndpointSliceTest, ParseMultipleAddressesPerEndpoint) {
    std::string json = R"({
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1", "10.0.0.2"],
            "conditions": {"ready": true},
            "targetRef": {"uid": "pod-1"}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 2u);
    EXPECT_EQ(endpoints[0].address, "10.0.0.1");
    EXPECT_EQ(endpoints[1].address, "10.0.0.2");
}

TEST_F(K8sEndpointSliceTest, ParseWithNoTargetRef) {
    std::string json = R"({
        "ports": [{"port": 8080}],
        "endpoints": [{
            "addresses": ["10.0.0.1"],
            "conditions": {"ready": true}
        }]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 1u);
    // UID should be the address when targetRef is missing
    EXPECT_EQ(endpoints[0].uid, "10.0.0.1");
}

TEST_F(K8sEndpointSliceTest, ParseEmptyEndpoints) {
    std::string json = R"({
        "ports": [{"port": 8080}],
        "endpoints": []
    })";

    auto endpoints = parse_endpoint_slice(json);
    EXPECT_TRUE(endpoints.empty());
}

// =============================================================================
// Backend ID Generation Tests
// =============================================================================

class K8sBackendIdTest : public ::testing::Test {};

TEST_F(K8sBackendIdTest, SameUidProducesSameId) {
    K8sEndpoint ep1;
    ep1.uid = "pod-123-10.0.0.1";

    K8sEndpoint ep2;
    ep2.uid = "pod-123-10.0.0.1";

    EXPECT_EQ(ep1.to_backend_id(), ep2.to_backend_id());
}

TEST_F(K8sBackendIdTest, DifferentUidProducesDifferentId) {
    K8sEndpoint ep1;
    ep1.uid = "pod-123-10.0.0.1";

    K8sEndpoint ep2;
    ep2.uid = "pod-456-10.0.0.2";

    EXPECT_NE(ep1.to_backend_id(), ep2.to_backend_id());
}

TEST_F(K8sBackendIdTest, IdIsPositive) {
    K8sEndpoint ep;
    ep.uid = "test-endpoint";

    EXPECT_GE(ep.to_backend_id(), 0);
}

TEST_F(K8sBackendIdTest, IdIsDeterministic) {
    K8sEndpoint ep;
    ep.uid = "deterministic-test";

    BackendId id1 = ep.to_backend_id();
    BackendId id2 = ep.to_backend_id();

    EXPECT_EQ(id1, id2);
}

// =============================================================================
// FNV-1a Hash Quality Tests
// =============================================================================
// Verify that the FNV-1a hash produces well-distributed BackendIds
// and avoids the clustering issues of the old hash*31+c polynomial.

class K8sFnvHashQualityTest : public ::testing::Test {};

TEST_F(K8sFnvHashQualityTest, SimilarUidsProduceDifferentIds) {
    // The old hash*31+c had poor distribution for similar strings.
    // FNV-1a should distribute these well.
    std::vector<std::string> similar_uids = {
        "pod-uid-001", "pod-uid-002", "pod-uid-003",
        "pod-uid-010", "pod-uid-011", "pod-uid-012",
        "pod-uid-100", "pod-uid-101", "pod-uid-102"
    };

    std::set<BackendId> ids;
    for (const auto& uid : similar_uids) {
        K8sEndpoint ep;
        ep.uid = uid;
        ids.insert(ep.to_backend_id());
    }
    // All similar UIDs should produce unique BackendIds
    EXPECT_EQ(ids.size(), similar_uids.size());
}

TEST_F(K8sFnvHashQualityTest, RealK8sUidsProduceUniqueIds) {
    // Test with realistic K8s pod UIDs (UUID v4 format)
    std::vector<std::string> k8s_uids = {
        "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
        "b2c3d4e5-f6a7-8901-bcde-f12345678901",
        "c3d4e5f6-a7b8-9012-cdef-123456789012",
        "d4e5f6a7-b8c9-0123-def1-234567890123",
        "e5f6a7b8-c9d0-1234-ef12-345678901234",
        "f6a7b8c9-d0e1-2345-f123-456789012345",
        "00000000-0000-0000-0000-000000000001",
        "00000000-0000-0000-0000-000000000002",
        "ffffffff-ffff-ffff-ffff-ffffffffffff",
        "12345678-1234-1234-1234-123456789abc"
    };

    std::set<BackendId> ids;
    for (const auto& uid : k8s_uids) {
        K8sEndpoint ep;
        ep.uid = uid;
        ids.insert(ep.to_backend_id());
    }
    EXPECT_EQ(ids.size(), k8s_uids.size());
}

TEST_F(K8sFnvHashQualityTest, NoCollisionsInFirst1000SequentialUids) {
    // Test that 1000 sequential UIDs (the MAX_ENDPOINTS limit)
    // produce no collisions
    std::set<BackendId> ids;
    for (int i = 0; i < 1000; ++i) {
        K8sEndpoint ep;
        ep.uid = "pod-" + std::to_string(i);
        ids.insert(ep.to_backend_id());
    }
    EXPECT_EQ(ids.size(), 1000u);
}

TEST_F(K8sFnvHashQualityTest, EmptyUidProducesValidId) {
    K8sEndpoint ep;
    ep.uid = "";
    BackendId id = ep.to_backend_id();
    EXPECT_GE(id, 0);
}

TEST_F(K8sFnvHashQualityTest, SingleCharUidsProduceUniqueIds) {
    std::set<BackendId> ids;
    for (int c = 0; c < 128; ++c) {
        K8sEndpoint ep;
        ep.uid = std::string(1, static_cast<char>(c));
        ids.insert(ep.to_backend_id());
    }
    // All single-char UIDs should produce unique BackendIds
    EXPECT_EQ(ids.size(), 128u);
}

// =============================================================================
// BackendId Collision Detection Tests
// =============================================================================
// Tests for the collision detection logic added to handle_endpoint_added().
// Replicates the reverse-map collision check without Seastar dependencies.

class K8sCollisionDetectionTest : public ::testing::Test {
protected:
    // Simulates the collision detection from handle_endpoint_added()
    struct CollisionTracker {
        std::map<BackendId, std::string> backend_id_to_uid;  // reverse map
        std::map<std::string, K8sEndpoint> endpoints;
        uint64_t collision_count = 0;

        // Returns true if collision was detected
        bool try_add(const K8sEndpoint& ep) {
            auto backend_id = ep.to_backend_id();
            bool collision = false;

            auto it = backend_id_to_uid.find(backend_id);
            if (it != backend_id_to_uid.end() && it->second != ep.uid) {
                ++collision_count;
                collision = true;
            }
            backend_id_to_uid[backend_id] = ep.uid;
            endpoints[ep.uid] = ep;
            return collision;
        }

        void remove(const std::string& uid) {
            auto it = endpoints.find(uid);
            if (it == endpoints.end()) return;

            auto backend_id = it->second.to_backend_id();
            auto rev_it = backend_id_to_uid.find(backend_id);
            if (rev_it != backend_id_to_uid.end() && rev_it->second == uid) {
                backend_id_to_uid.erase(rev_it);
            }
            endpoints.erase(it);
        }
    };

    static K8sEndpoint make_ep(const std::string& uid) {
        K8sEndpoint ep;
        ep.uid = uid;
        ep.address = "10.0.0.1";
        ep.port = 8080;
        ep.ready = true;
        return ep;
    }
};

TEST_F(K8sCollisionDetectionTest, NoCollisionForDistinctUids) {
    CollisionTracker tracker;
    EXPECT_FALSE(tracker.try_add(make_ep("uid-alpha")));
    EXPECT_FALSE(tracker.try_add(make_ep("uid-beta")));
    EXPECT_FALSE(tracker.try_add(make_ep("uid-gamma")));
    EXPECT_EQ(tracker.collision_count, 0u);
}

TEST_F(K8sCollisionDetectionTest, SameUidReaddIsNotCollision) {
    CollisionTracker tracker;
    EXPECT_FALSE(tracker.try_add(make_ep("uid-same")));
    // Re-adding the same UID should NOT be flagged as collision
    EXPECT_FALSE(tracker.try_add(make_ep("uid-same")));
    EXPECT_EQ(tracker.collision_count, 0u);
}

TEST_F(K8sCollisionDetectionTest, ReverseMapCleanedOnRemove) {
    CollisionTracker tracker;
    tracker.try_add(make_ep("uid-1"));
    EXPECT_EQ(tracker.backend_id_to_uid.size(), 1u);

    tracker.remove("uid-1");
    EXPECT_EQ(tracker.backend_id_to_uid.size(), 0u);
    EXPECT_EQ(tracker.endpoints.size(), 0u);
}

TEST_F(K8sCollisionDetectionTest, RemoveNonexistentUidIsNoop) {
    CollisionTracker tracker;
    tracker.try_add(make_ep("uid-exists"));
    tracker.remove("uid-nonexistent");
    EXPECT_EQ(tracker.backend_id_to_uid.size(), 1u);
    EXPECT_EQ(tracker.endpoints.size(), 1u);
}

TEST_F(K8sCollisionDetectionTest, No1000EndpointCollisions) {
    // Verify no collisions when adding 1000 endpoints with sequential UIDs
    CollisionTracker tracker;
    for (int i = 0; i < 1000; ++i) {
        bool collision = tracker.try_add(make_ep("pod-" + std::to_string(i)));
        EXPECT_FALSE(collision) << "Unexpected collision at index " << i;
    }
    EXPECT_EQ(tracker.collision_count, 0u);
    EXPECT_EQ(tracker.backend_id_to_uid.size(), 1000u);
}

// =============================================================================
// Real-world K8s Response Tests
// =============================================================================

class K8sRealWorldTest : public ::testing::Test {
protected:
    std::vector<K8sEndpoint> parse_endpoint_slice(const std::string& json, uint16_t default_port = 8080) {
        std::vector<K8sEndpoint> endpoints;

        auto metadata = k8s_json::get_object(json, "metadata");
        uint32_t default_weight = K8S_DEFAULT_WEIGHT;
        uint32_t default_priority = K8S_DEFAULT_PRIORITY;

        if (metadata) {
            auto annotations = k8s_json::get_object(*metadata, "annotations");
            if (annotations) {
                auto weight_str = k8s_json::get_string(*annotations, "ranvier.io/weight");
                if (weight_str) {
                    try {
                        if (!weight_str->empty() && (*weight_str)[0] == '-') {
                            throw std::invalid_argument("negative value not allowed");
                        }
                        size_t pos = 0;
                        unsigned long parsed = std::stoul(*weight_str, &pos);
                        if (pos != weight_str->size()) {
                            throw std::invalid_argument("contains non-numeric characters");
                        }
                        if (parsed > K8S_MAX_WEIGHT) {
                            default_weight = K8S_MAX_WEIGHT;
                        } else {
                            default_weight = static_cast<uint32_t>(parsed);
                        }
                    } catch (const std::exception&) {}
                }

                auto priority_str = k8s_json::get_string(*annotations, "ranvier.io/priority");
                if (priority_str) {
                    try {
                        if (!priority_str->empty() && (*priority_str)[0] == '-') {
                            throw std::invalid_argument("negative value not allowed");
                        }
                        size_t pos = 0;
                        unsigned long parsed = std::stoul(*priority_str, &pos);
                        if (pos != priority_str->size()) {
                            throw std::invalid_argument("contains non-numeric characters");
                        }
                        if (parsed > K8S_MAX_PRIORITY) {
                            default_priority = K8S_MAX_PRIORITY;
                        } else {
                            default_priority = static_cast<uint32_t>(parsed);
                        }
                    } catch (const std::exception&) {}
                }
            }
        }

        uint16_t port = default_port;
        auto ports = k8s_json::get_array(json, "ports");
        for (const auto& p : ports) {
            auto port_num = k8s_json::get_int(p, "port");
            if (port_num) {
                port = static_cast<uint16_t>(*port_num);
                break;
            }
        }

        auto endpoint_items = k8s_json::get_array(json, "endpoints");

        for (const auto& ep : endpoint_items) {
            auto conditions = k8s_json::get_object(ep, "conditions");
            bool ready = true;
            if (conditions) {
                auto ready_val = k8s_json::get_bool(*conditions, "ready");
                if (ready_val) {
                    ready = *ready_val;
                }
            }

            std::string uid;
            auto target_ref = k8s_json::get_object(ep, "targetRef");
            if (target_ref) {
                auto ref_uid = k8s_json::get_string(*target_ref, "uid");
                if (ref_uid) {
                    uid = *ref_uid;
                }
            }

            auto addresses = k8s_json::get_array(ep, "addresses");
            for (const auto& addr : addresses) {
                std::string address = addr;
                if (address.size() >= 2 && address.front() == '"' && address.back() == '"') {
                    address = address.substr(1, address.size() - 2);
                }

                K8sEndpoint endpoint;
                endpoint.uid = uid.empty() ? address : uid + "-" + address;
                endpoint.address = address;
                endpoint.port = port;
                endpoint.ready = ready;
                endpoint.weight = default_weight;
                endpoint.priority = default_priority;

                endpoints.push_back(std::move(endpoint));
            }
        }

        return endpoints;
    }
};

TEST_F(K8sRealWorldTest, ParseTypicalGpuServiceEndpointSlice) {
    // Simulates a real EndpointSlice from a GPU inference service
    std::string json = R"({
        "apiVersion": "discovery.k8s.io/v1",
        "kind": "EndpointSlice",
        "metadata": {
            "name": "vllm-gpu-service-abc123",
            "namespace": "ml-inference",
            "labels": {
                "kubernetes.io/service-name": "vllm-gpu-service"
            },
            "annotations": {
                "ranvier.io/weight": "150",
                "ranvier.io/priority": "0"
            }
        },
        "addressType": "IPv4",
        "ports": [
            {"name": "http", "port": 8000, "protocol": "TCP"}
        ],
        "endpoints": [
            {
                "addresses": ["10.244.1.15"],
                "conditions": {"ready": true, "serving": true, "terminating": false},
                "nodeName": "gpu-node-1",
                "targetRef": {"kind": "Pod", "name": "vllm-0", "namespace": "ml-inference", "uid": "a1b2c3d4-e5f6-7890-abcd-ef1234567890"}
            },
            {
                "addresses": ["10.244.2.20"],
                "conditions": {"ready": true, "serving": true, "terminating": false},
                "nodeName": "gpu-node-2",
                "targetRef": {"kind": "Pod", "name": "vllm-1", "namespace": "ml-inference", "uid": "b2c3d4e5-f6a7-8901-bcde-f12345678901"}
            },
            {
                "addresses": ["10.244.3.25"],
                "conditions": {"ready": false, "serving": false, "terminating": true},
                "nodeName": "gpu-node-3",
                "targetRef": {"kind": "Pod", "name": "vllm-2", "namespace": "ml-inference", "uid": "c3d4e5f6-a7b8-9012-cdef-123456789012"}
            }
        ]
    })";

    auto endpoints = parse_endpoint_slice(json);
    ASSERT_EQ(endpoints.size(), 3u);

    // First endpoint
    EXPECT_EQ(endpoints[0].address, "10.244.1.15");
    EXPECT_EQ(endpoints[0].port, 8000);
    EXPECT_TRUE(endpoints[0].ready);
    EXPECT_EQ(endpoints[0].weight, 150u);
    EXPECT_EQ(endpoints[0].priority, 0u);

    // Second endpoint
    EXPECT_EQ(endpoints[1].address, "10.244.2.20");
    EXPECT_TRUE(endpoints[1].ready);

    // Third endpoint (terminating)
    EXPECT_EQ(endpoints[2].address, "10.244.3.25");
    EXPECT_FALSE(endpoints[2].ready);
}

// =============================================================================
// Port Validation Tests
// =============================================================================

// Replicate the parse_port function from k8s_discovery_service.cpp for testing
// (avoids Seastar dependency while testing the same logic)
namespace port_validation {

static std::optional<uint16_t> parse_port(const std::string& port_str) {
    if (port_str.empty()) {
        return std::nullopt;
    }

    // Reject strings with leading/trailing whitespace or non-digit characters
    for (size_t i = 0; i < port_str.size(); ++i) {
        char c = port_str[i];
        // Allow leading minus for negative number detection
        if (i == 0 && c == '-') {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }

    try {
        int port_int = std::stoi(port_str);
        if (port_int < 1 || port_int > 65535) {
            return std::nullopt;
        }
        return static_cast<uint16_t>(port_int);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

}  // namespace port_validation

class PortValidationTest : public ::testing::Test {};

// Valid port cases
TEST_F(PortValidationTest, ValidPortMinimum) {
    auto result = port_validation::parse_port("1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1);
}

TEST_F(PortValidationTest, ValidPortMaximum) {
    auto result = port_validation::parse_port("65535");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 65535);
}

TEST_F(PortValidationTest, ValidPortTypical) {
    auto result = port_validation::parse_port("8080");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 8080);
}

TEST_F(PortValidationTest, ValidPortHttps) {
    auto result = port_validation::parse_port("443");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 443);
}

TEST_F(PortValidationTest, ValidPortKubernetesApi) {
    auto result = port_validation::parse_port("6443");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 6443);
}

// Invalid port cases - empty string
TEST_F(PortValidationTest, InvalidPortEmpty) {
    auto result = port_validation::parse_port("");
    EXPECT_FALSE(result.has_value());
}

// Invalid port cases - non-numeric
TEST_F(PortValidationTest, InvalidPortAlphabetic) {
    auto result = port_validation::parse_port("abc");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortMixedAlphanumeric) {
    auto result = port_validation::parse_port("80a80");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortLeadingAlpha) {
    auto result = port_validation::parse_port("a8080");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortTrailingAlpha) {
    auto result = port_validation::parse_port("8080a");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortSpecialChars) {
    auto result = port_validation::parse_port("80:80");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortWhitespace) {
    auto result = port_validation::parse_port(" 8080");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortTrailingWhitespace) {
    auto result = port_validation::parse_port("8080 ");
    EXPECT_FALSE(result.has_value());
}

// Invalid port cases - out of range
TEST_F(PortValidationTest, InvalidPortZero) {
    auto result = port_validation::parse_port("0");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortNegative) {
    auto result = port_validation::parse_port("-1");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortNegativeLarge) {
    auto result = port_validation::parse_port("-8080");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortTooLarge) {
    auto result = port_validation::parse_port("65536");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortOverflow) {
    // This would cause uint16_t wrap-around if not validated
    auto result = port_validation::parse_port("70000");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortMassiveOverflow) {
    // Exceeds int range - should not throw, just return nullopt
    auto result = port_validation::parse_port("999999999999");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortIntMaxOverflow) {
    // Exceeds INT_MAX
    auto result = port_validation::parse_port("2147483648");
    EXPECT_FALSE(result.has_value());
}

// Edge cases
TEST_F(PortValidationTest, ValidPortLeadingZeros) {
    // "0443" should be parsed as 443 (octal not expected, stoi uses base 10)
    auto result = port_validation::parse_port("0443");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 443);
}

TEST_F(PortValidationTest, InvalidPortOnlyZeros) {
    auto result = port_validation::parse_port("0000");
    EXPECT_FALSE(result.has_value());  // 0 is not a valid port
}

TEST_F(PortValidationTest, InvalidPortFloat) {
    auto result = port_validation::parse_port("8080.5");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PortValidationTest, InvalidPortHex) {
    auto result = port_validation::parse_port("0x1F90");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Bounds Checking Tests (Rule #4) - OOM Prevention
// =============================================================================
//
// Tests for the three bounds introduced to prevent OOM from a compromised or
// misconfigured K8s API server:
//   1. K8S_MAX_RESPONSE_SIZE (16 MB) - caps response body accumulation
//   2. K8S_MAX_LINE_SIZE (1 MB) - caps watch buffer between newlines
//   3. K8S_MAX_ENDPOINTS (1000) - caps endpoints map size

// Replicate constants here (mirrors k8s_discovery_service.hpp, avoids Seastar include)
constexpr size_t K8S_MAX_RESPONSE_SIZE = 16 * 1024 * 1024;
constexpr size_t K8S_MAX_LINE_SIZE = 1 * 1024 * 1024;
constexpr size_t K8S_MAX_ENDPOINTS = 1000;
constexpr size_t K8S_MAX_TOKEN_SIZE = 1 * 1024 * 1024;

// --- Response Size Bounds ---
// Replicates the accumulation guard from k8s_get()

class K8sResponseSizeBoundsTest : public ::testing::Test {
protected:
    // Simulates the bounded response accumulation loop from k8s_get().
    // Returns true if the response was fully accumulated within the limit,
    // false if the limit was exceeded (would throw in production).
    static bool accumulate_response(const std::vector<std::string>& chunks,
                                    std::string& out) {
        out.clear();
        for (const auto& chunk : chunks) {
            if (out.size() + chunk.size() > K8S_MAX_RESPONSE_SIZE) {
                return false;  // Limit exceeded
            }
            out.append(chunk);
        }
        return true;
    }
};

TEST_F(K8sResponseSizeBoundsTest, ConstantValue) {
    EXPECT_EQ(K8S_MAX_RESPONSE_SIZE, 16u * 1024 * 1024);
}

TEST_F(K8sResponseSizeBoundsTest, SmallResponseAccepted) {
    std::string out;
    std::vector<std::string> chunks = {
        R"({"kind":"EndpointSliceList","items":[]})"
    };
    EXPECT_TRUE(accumulate_response(chunks, out));
    EXPECT_EQ(out, chunks[0]);
}

TEST_F(K8sResponseSizeBoundsTest, ResponseExactlyAtLimitAccepted) {
    std::string out;
    std::string chunk(K8S_MAX_RESPONSE_SIZE, 'x');
    std::vector<std::string> chunks = {chunk};
    EXPECT_TRUE(accumulate_response(chunks, out));
    EXPECT_EQ(out.size(), K8S_MAX_RESPONSE_SIZE);
}

TEST_F(K8sResponseSizeBoundsTest, ResponseOneOverLimitRejected) {
    std::string out;
    std::string chunk(K8S_MAX_RESPONSE_SIZE, 'x');
    // First chunk fills exactly to limit, second byte pushes over
    std::vector<std::string> chunks = {chunk, "x"};
    EXPECT_FALSE(accumulate_response(chunks, out));
}

TEST_F(K8sResponseSizeBoundsTest, LargeResponseInManySmallChunksRejected) {
    std::string out;
    // 16 MB + 1 byte delivered in 4KB chunks
    size_t chunk_size = 4096;
    size_t total_chunks = (K8S_MAX_RESPONSE_SIZE / chunk_size) + 1;
    std::vector<std::string> chunks;
    chunks.reserve(total_chunks);
    for (size_t i = 0; i < total_chunks; ++i) {
        chunks.emplace_back(chunk_size, 'a');
    }
    EXPECT_FALSE(accumulate_response(chunks, out));
}

TEST_F(K8sResponseSizeBoundsTest, SingleHugeChunkRejected) {
    std::string out;
    std::string huge(K8S_MAX_RESPONSE_SIZE + 1, 'z');
    std::vector<std::string> chunks = {huge};
    EXPECT_FALSE(accumulate_response(chunks, out));
}

// --- Watch Buffer (Line Size) Bounds ---
// Replicates the watch buffer guard from k8s_watch()

class K8sLineSizeBoundsTest : public ::testing::Test {
protected:
    // Simulates the bounded watch buffer logic from k8s_watch().
    // Feeds data chunks into a line buffer, extracts newline-delimited lines,
    // and checks if the buffer between newlines exceeds K8S_MAX_LINE_SIZE.
    //
    // Returns: pair<vector of extracted lines, bool exceeded_limit>
    static std::pair<std::vector<std::string>, bool>
    process_watch_stream(const std::vector<std::string>& chunks) {
        std::vector<std::string> lines;
        std::string buffer;
        bool exceeded = false;

        for (const auto& chunk : chunks) {
            buffer.append(chunk);

            // Extract complete lines
            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (!line.empty() && line != "\r") {
                    lines.push_back(std::move(line));
                }
            }

            // Check if remaining buffer (no newline yet) exceeds limit
            if (buffer.size() > K8S_MAX_LINE_SIZE) {
                exceeded = true;
                break;
            }
        }

        return {std::move(lines), exceeded};
    }
};

TEST_F(K8sLineSizeBoundsTest, ConstantValue) {
    EXPECT_EQ(K8S_MAX_LINE_SIZE, 1u * 1024 * 1024);
}

TEST_F(K8sLineSizeBoundsTest, NormalLinesAccepted) {
    auto [lines, exceeded] = process_watch_stream({
        R"({"type":"ADDED","object":{"metadata":{"uid":"pod-1"}}})" "\n",
        R"({"type":"MODIFIED","object":{"metadata":{"uid":"pod-2"}}})" "\n"
    });
    EXPECT_FALSE(exceeded);
    ASSERT_EQ(lines.size(), 2u);
}

TEST_F(K8sLineSizeBoundsTest, LineExactlyAtLimitAccepted) {
    // Line of exactly MAX_LINE_SIZE bytes followed by newline
    std::string line(K8S_MAX_LINE_SIZE, 'x');
    line += '\n';
    auto [lines, exceeded] = process_watch_stream({line});
    EXPECT_FALSE(exceeded);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].size(), K8S_MAX_LINE_SIZE);
}

TEST_F(K8sLineSizeBoundsTest, BufferOneOverLimitWithoutNewlineRejected) {
    // Data without any newline that exceeds MAX_LINE_SIZE
    std::string data(K8S_MAX_LINE_SIZE + 1, 'x');
    auto [lines, exceeded] = process_watch_stream({data});
    EXPECT_TRUE(exceeded);
    EXPECT_TRUE(lines.empty());
}

TEST_F(K8sLineSizeBoundsTest, SlowlorisStyleGradualGrowthRejected) {
    // Simulates Slowloris: many small chunks without newlines
    size_t chunk_size = 1024;
    size_t num_chunks = (K8S_MAX_LINE_SIZE / chunk_size) + 2;
    std::vector<std::string> chunks;
    chunks.reserve(num_chunks);
    for (size_t i = 0; i < num_chunks; ++i) {
        chunks.emplace_back(chunk_size, 'a');
    }
    auto [lines, exceeded] = process_watch_stream(chunks);
    EXPECT_TRUE(exceeded);
}

TEST_F(K8sLineSizeBoundsTest, NewlineResetsBuffer) {
    // Large data followed by newline, then more large data with newline
    // Should succeed because newlines reset the buffer
    std::string half(K8S_MAX_LINE_SIZE / 2, 'x');
    auto [lines, exceeded] = process_watch_stream({
        half + "\n",
        half + "\n"
    });
    EXPECT_FALSE(exceeded);
    ASSERT_EQ(lines.size(), 2u);
}

TEST_F(K8sLineSizeBoundsTest, LargeLineFollowedByOverflowDetected) {
    // First line is valid, second buffer overflows without newline
    std::string valid_line = R"({"type":"ADDED"})" "\n";
    std::string overflow(K8S_MAX_LINE_SIZE + 1, 'z');
    auto [lines, exceeded] = process_watch_stream({valid_line, overflow});
    EXPECT_TRUE(exceeded);
    ASSERT_EQ(lines.size(), 1u);  // First line was extracted before overflow
}

// --- Endpoints Map Bounds ---
// Replicates the endpoint insertion guard from handle_endpoint_added()

class K8sEndpointsLimitTest : public ::testing::Test {
protected:
    // Simulates the bounded endpoint insertion from handle_endpoint_added().
    // Uses a simple map to mirror _endpoints behavior.
    struct EndpointMap {
        std::map<std::string, K8sEndpoint> endpoints;
        uint64_t limit_exceeded_count = 0;

        // Returns true if the endpoint was inserted/updated, false if rejected
        bool try_add(const K8sEndpoint& ep) {
            // Mirror production logic: only check limit for genuinely new entries
            if (endpoints.find(ep.uid) == endpoints.end() &&
                endpoints.size() >= K8S_MAX_ENDPOINTS) {
                ++limit_exceeded_count;
                return false;  // Rejected
            }
            endpoints[ep.uid] = ep;
            return true;
        }

        bool try_remove(const std::string& uid) {
            return endpoints.erase(uid) > 0;
        }
    };

    static K8sEndpoint make_endpoint(const std::string& uid, const std::string& addr = "10.0.0.1") {
        K8sEndpoint ep;
        ep.uid = uid;
        ep.address = addr;
        ep.port = 8080;
        ep.ready = true;
        ep.weight = K8S_DEFAULT_WEIGHT;
        ep.priority = K8S_DEFAULT_PRIORITY;
        return ep;
    }
};

TEST_F(K8sEndpointsLimitTest, ConstantValue) {
    EXPECT_EQ(K8S_MAX_ENDPOINTS, 1000u);
}

TEST_F(K8sEndpointsLimitTest, SingleEndpointAccepted) {
    EndpointMap map;
    auto ep = make_endpoint("pod-1");
    EXPECT_TRUE(map.try_add(ep));
    EXPECT_EQ(map.endpoints.size(), 1u);
}

TEST_F(K8sEndpointsLimitTest, FillToExactLimitAllAccepted) {
    EndpointMap map;
    for (size_t i = 0; i < K8S_MAX_ENDPOINTS; ++i) {
        auto ep = make_endpoint("pod-" + std::to_string(i));
        EXPECT_TRUE(map.try_add(ep));
    }
    EXPECT_EQ(map.endpoints.size(), K8S_MAX_ENDPOINTS);
    EXPECT_EQ(map.limit_exceeded_count, 0u);
}

TEST_F(K8sEndpointsLimitTest, OneOverLimitRejected) {
    EndpointMap map;
    for (size_t i = 0; i < K8S_MAX_ENDPOINTS; ++i) {
        map.try_add(make_endpoint("pod-" + std::to_string(i)));
    }

    // This should be rejected
    auto overflow = make_endpoint("pod-overflow");
    EXPECT_FALSE(map.try_add(overflow));
    EXPECT_EQ(map.endpoints.size(), K8S_MAX_ENDPOINTS);
    EXPECT_EQ(map.limit_exceeded_count, 1u);
}

TEST_F(K8sEndpointsLimitTest, UpdateExistingEndpointAtLimitSucceeds) {
    EndpointMap map;
    for (size_t i = 0; i < K8S_MAX_ENDPOINTS; ++i) {
        map.try_add(make_endpoint("pod-" + std::to_string(i)));
    }

    // Updating an existing endpoint should succeed even at capacity
    auto updated = make_endpoint("pod-0", "10.0.0.99");
    updated.weight = 500;
    EXPECT_TRUE(map.try_add(updated));
    EXPECT_EQ(map.endpoints.size(), K8S_MAX_ENDPOINTS);
    EXPECT_EQ(map.endpoints["pod-0"].weight, 500u);
    EXPECT_EQ(map.limit_exceeded_count, 0u);
}

TEST_F(K8sEndpointsLimitTest, RemoveThenAddSucceeds) {
    EndpointMap map;
    for (size_t i = 0; i < K8S_MAX_ENDPOINTS; ++i) {
        map.try_add(make_endpoint("pod-" + std::to_string(i)));
    }

    // Remove one, then add a new one — should succeed
    EXPECT_TRUE(map.try_remove("pod-0"));
    EXPECT_EQ(map.endpoints.size(), K8S_MAX_ENDPOINTS - 1);

    auto new_ep = make_endpoint("pod-new");
    EXPECT_TRUE(map.try_add(new_ep));
    EXPECT_EQ(map.endpoints.size(), K8S_MAX_ENDPOINTS);
    EXPECT_EQ(map.limit_exceeded_count, 0u);
}

TEST_F(K8sEndpointsLimitTest, MultipleRejectionsCountedCorrectly) {
    EndpointMap map;
    for (size_t i = 0; i < K8S_MAX_ENDPOINTS; ++i) {
        map.try_add(make_endpoint("pod-" + std::to_string(i)));
    }

    // Try adding 5 more — all should be rejected
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(map.try_add(make_endpoint("overflow-" + std::to_string(i))));
    }
    EXPECT_EQ(map.endpoints.size(), K8S_MAX_ENDPOINTS);
    EXPECT_EQ(map.limit_exceeded_count, 5u);
}

// =============================================================================
// Token File Size Bounds Tests
// =============================================================================
//
// Tests for the token file size validation introduced to fix truncation of
// K8s projected tokens exceeding 4096 bytes (BACKLOG.md #13.5).
//
// The fix reads file size first (like load_ca_cert), then read_exactly(size),
// with a K8S_MAX_TOKEN_SIZE bound to prevent unbounded reads.

class K8sTokenSizeBoundsTest : public ::testing::Test {
protected:
    // Simulates the token size validation from load_service_account_token().
    // Returns: pair<bool accepted, std::string token_or_empty>
    // On rejection: accepted=false, token is empty.
    // On success: accepted=true, token is trimmed content.
    static std::pair<bool, std::string> validate_and_load_token(
            size_t file_size, const std::string& file_content) {
        // Mirror production logic: check size == 0
        if (file_size == 0) {
            return {false, {}};
        }

        // Mirror production logic: check size > K8S_MAX_TOKEN_SIZE
        if (file_size > K8S_MAX_TOKEN_SIZE) {
            return {false, {}};
        }

        // Simulate read_exactly(size) - in production this reads the actual file
        std::string token = file_content.substr(0, file_size);

        // Mirror production trimming logic
        token.erase(token.find_last_not_of(" \n\r\t") + 1);
        token.erase(0, token.find_first_not_of(" \n\r\t"));

        return {true, token};
    }
};

TEST_F(K8sTokenSizeBoundsTest, ConstantValue) {
    EXPECT_EQ(K8S_MAX_TOKEN_SIZE, 1u * 1024 * 1024);
}

TEST_F(K8sTokenSizeBoundsTest, TypicalSmallTokenAccepted) {
    // Typical K8s service account token (~900 bytes)
    std::string token(900, 'a');
    auto [accepted, result] = validate_and_load_token(token.size(), token);
    EXPECT_TRUE(accepted);
    EXPECT_EQ(result.size(), 900u);
}

TEST_F(K8sTokenSizeBoundsTest, TokenExceeding4KBAccepted) {
    // This is the key fix: tokens > 4096 bytes must now be accepted
    // (previously silently truncated at 4096)
    std::string token(8192, 'b');
    auto [accepted, result] = validate_and_load_token(token.size(), token);
    EXPECT_TRUE(accepted);
    EXPECT_EQ(result.size(), 8192u);
}

TEST_F(K8sTokenSizeBoundsTest, LargeProjectedTokenAccepted) {
    // K8s projected tokens with custom audiences can be ~16KB
    std::string token(16384, 'c');
    auto [accepted, result] = validate_and_load_token(token.size(), token);
    EXPECT_TRUE(accepted);
    EXPECT_EQ(result.size(), 16384u);
}

TEST_F(K8sTokenSizeBoundsTest, TokenAtExactMaxAccepted) {
    std::string token(K8S_MAX_TOKEN_SIZE, 'd');
    auto [accepted, result] = validate_and_load_token(token.size(), token);
    EXPECT_TRUE(accepted);
    EXPECT_EQ(result.size(), K8S_MAX_TOKEN_SIZE);
}

TEST_F(K8sTokenSizeBoundsTest, TokenOneOverMaxRejected) {
    std::string token(K8S_MAX_TOKEN_SIZE + 1, 'e');
    auto [accepted, result] = validate_and_load_token(token.size(), token);
    EXPECT_FALSE(accepted);
    EXPECT_TRUE(result.empty());
}

TEST_F(K8sTokenSizeBoundsTest, EmptyTokenFileRejected) {
    auto [accepted, result] = validate_and_load_token(0, "");
    EXPECT_FALSE(accepted);
    EXPECT_TRUE(result.empty());
}

TEST_F(K8sTokenSizeBoundsTest, TokenWithWhitespaceTrimmed) {
    std::string raw_token = "  \n  eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.payload.signature  \n\r\t  ";
    auto [accepted, result] = validate_and_load_token(raw_token.size(), raw_token);
    EXPECT_TRUE(accepted);
    EXPECT_EQ(result, "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.payload.signature");
}

TEST_F(K8sTokenSizeBoundsTest, TokenWithTrailingNewlineTrimmed) {
    // K8s token files typically have a trailing newline
    std::string raw_token = "eyJhbGciOiJSUzI1NiJ9.payload.sig\n";
    auto [accepted, result] = validate_and_load_token(raw_token.size(), raw_token);
    EXPECT_TRUE(accepted);
    EXPECT_EQ(result, "eyJhbGciOiJSUzI1NiJ9.payload.sig");
}

TEST_F(K8sTokenSizeBoundsTest, AllWhitespaceTokenBecomesEmpty) {
    std::string raw_token = "   \n\r\t   ";
    auto [accepted, result] = validate_and_load_token(raw_token.size(), raw_token);
    EXPECT_TRUE(accepted);
    EXPECT_TRUE(result.empty());
}

// =============================================================================
// HTTP Status Code Parsing Tests
// =============================================================================
//
// Tests for parse_http_status_code(), which replaces the brittle string search
// ("200 OK" / "200 ") with proper status line parsing.
// Mirrors the production implementation in k8s_discovery_service.cpp.

namespace http_status {

// Replicated from k8s_discovery_service.cpp to avoid Seastar dependencies.
static std::optional<int> parse_http_status_code(std::string_view headers) {
    auto line_end = headers.find("\r\n");
    std::string_view status_line = headers.substr(0, line_end);

    auto first_space = status_line.find(' ');
    if (first_space == std::string_view::npos) {
        return std::nullopt;
    }

    auto code_start = first_space + 1;
    while (code_start < status_line.size() && status_line[code_start] == ' ') {
        ++code_start;
    }

    auto second_space = status_line.find(' ', code_start);
    auto code_end = (second_space != std::string_view::npos)
                        ? second_space
                        : status_line.size();

    if (code_start >= code_end) {
        return std::nullopt;
    }

    int code = 0;
    auto [ptr, ec] = std::from_chars(
        status_line.data() + code_start,
        status_line.data() + code_end,
        code);

    if (ec != std::errc{} || ptr != status_line.data() + code_end) {
        return std::nullopt;
    }

    return code;
}

}  // namespace http_status

class HttpStatusParseTest : public ::testing::Test {};

// --- Standard status lines ---

TEST_F(HttpStatusParseTest, ParsesHttp11_200Ok) {
    auto code = http_status::parse_http_status_code("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 200);
}

TEST_F(HttpStatusParseTest, ParsesHttp10_200Ok) {
    auto code = http_status::parse_http_status_code("HTTP/1.0 200 OK\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 200);
}

TEST_F(HttpStatusParseTest, ParsesHttp2_200) {
    // HTTP/2 may omit the reason phrase
    auto code = http_status::parse_http_status_code("HTTP/2 200\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 200);
}

TEST_F(HttpStatusParseTest, Parses404NotFound) {
    auto code = http_status::parse_http_status_code("HTTP/1.1 404 Not Found\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 404);
}

TEST_F(HttpStatusParseTest, Parses403Forbidden) {
    auto code = http_status::parse_http_status_code("HTTP/1.1 403 Forbidden\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 403);
}

TEST_F(HttpStatusParseTest, Parses500InternalServerError) {
    auto code = http_status::parse_http_status_code("HTTP/1.1 500 Internal Server Error\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 500);
}

TEST_F(HttpStatusParseTest, Parses503ServiceUnavailable) {
    auto code = http_status::parse_http_status_code("HTTP/1.1 503 Service Unavailable\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 503);
}

TEST_F(HttpStatusParseTest, Parses410Gone) {
    // K8s sends 410 Gone when watch resourceVersion is too old
    auto code = http_status::parse_http_status_code("HTTP/1.1 410 Gone\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 410);
}

TEST_F(HttpStatusParseTest, Parses301MovedPermanently) {
    auto code = http_status::parse_http_status_code("HTTP/1.1 301 Moved Permanently\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 301);
}

// --- Status code without reason phrase (valid per HTTP spec) ---

TEST_F(HttpStatusParseTest, StatusCodeWithoutReasonPhrase) {
    auto code = http_status::parse_http_status_code("HTTP/1.1 200\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 200);
}

// --- Headers containing "200" in values should NOT confuse the parser ---

TEST_F(HttpStatusParseTest, DoesNotMatchFalse200InHeaders) {
    // The old brittle code would match "200 OK" appearing in a header value.
    // The new code only parses the first line.
    std::string headers =
        "HTTP/1.1 403 Forbidden\r\n"
        "X-Custom: 200 OK\r\n"
        "Content-Length: 200\r\n";
    auto code = http_status::parse_http_status_code(headers);
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 403);
}

TEST_F(HttpStatusParseTest, DoesNotMatchFalse200InBody) {
    // Even with "200" in what looks like body content after headers
    std::string headers = "HTTP/1.1 500 Internal Server Error\r\n"
                          "Content-Type: text/plain\r\n";
    auto code = http_status::parse_http_status_code(headers);
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 500);
}

// --- Full header block (multiple headers) ---

TEST_F(HttpStatusParseTest, FullHeaderBlockWithMultipleHeaders) {
    std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Cache-Control: no-cache, private\r\n";
    auto code = http_status::parse_http_status_code(headers);
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 200);
}

// --- Malformed / edge cases ---

TEST_F(HttpStatusParseTest, EmptyStringReturnsNullopt) {
    auto code = http_status::parse_http_status_code("");
    EXPECT_FALSE(code.has_value());
}

TEST_F(HttpStatusParseTest, NoSpaceInStatusLineReturnsNullopt) {
    auto code = http_status::parse_http_status_code("HTTP/1.1\r\n");
    EXPECT_FALSE(code.has_value());
}

TEST_F(HttpStatusParseTest, NonNumericStatusCodeReturnsNullopt) {
    auto code = http_status::parse_http_status_code("HTTP/1.1 abc OK\r\n");
    EXPECT_FALSE(code.has_value());
}

TEST_F(HttpStatusParseTest, PartialNumericStatusCodeReturnsNullopt) {
    auto code = http_status::parse_http_status_code("HTTP/1.1 20x OK\r\n");
    EXPECT_FALSE(code.has_value());
}

TEST_F(HttpStatusParseTest, MissingStatusCodeAfterSpaceReturnsNullopt) {
    auto code = http_status::parse_http_status_code("HTTP/1.1 \r\n");
    EXPECT_FALSE(code.has_value());
}

TEST_F(HttpStatusParseTest, GarbageInputReturnsNullopt) {
    auto code = http_status::parse_http_status_code("not an http response");
    EXPECT_FALSE(code.has_value());
}

TEST_F(HttpStatusParseTest, OnlyProtocolNoCodeReturnsNullopt) {
    auto code = http_status::parse_http_status_code("HTTP/1.1");
    EXPECT_FALSE(code.has_value());
}

TEST_F(HttpStatusParseTest, StatusLineWithoutCRLF) {
    // No \r\n — the function should still parse if the entire input is one status line
    auto code = http_status::parse_http_status_code("HTTP/1.1 200 OK");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 200);
}

TEST_F(HttpStatusParseTest, NegativeStatusCodeParsedAsInteger) {
    // The parser extracts the integer value; HTTP validity is the caller's concern.
    // "-200" is parseable as int, so the function returns -200.
    auto code = http_status::parse_http_status_code("HTTP/1.1 -200 OK\r\n");
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, -200);
    // The caller would reject this: (*code != 200)
}

// =============================================================================
// K8s Watch 410 Gone Detection Tests
// =============================================================================
//
// When a K8s watch receives a 410 Gone status, it means the resourceVersion
// is too old (compacted by etcd). The fix: parse the status code from the
// Status event, clear _resource_version, and trigger sync_endpoints() to
// re-list with a fresh resourceVersion before reconnecting.
//
// 410 can arrive in two forms:
// 1. Direct Status object: {"kind":"Status","code":410,"message":"..."}
// 2. ERROR watch event: {"type":"ERROR","object":{"kind":"Status","code":410,...}}

class K8sWatch410Test : public ::testing::Test {
protected:
    // Mirrors the Status detection logic from watch_endpoints()
    struct WatchEventResult {
        bool is_status_error = false;
        bool is_410_gone = false;
        int status_code = 0;
        std::string message;
    };

    // Check for direct Status objects (top-level kind == "Status")
    static WatchEventResult check_direct_status(const std::string& json) {
        WatchEventResult result;
        auto kind = k8s_json::get_string(json, "kind");
        if (!kind || *kind != "Status") {
            return result;
        }
        result.is_status_error = true;
        auto code = k8s_json::get_int(json, "code");
        result.status_code = static_cast<int>(code.value_or(0));
        auto msg = k8s_json::get_string(json, "message");
        result.message = msg.value_or("Unknown error");
        result.is_410_gone = (result.status_code == 410);
        return result;
    }

    // Check for ERROR watch events with embedded Status object
    static WatchEventResult check_error_event(const std::string& json) {
        WatchEventResult result;
        auto type = k8s_json::get_string(json, "type");
        if (!type || *type != "ERROR") {
            return result;
        }
        auto obj = k8s_json::get_object(json, "object");
        if (!obj) {
            return result;
        }
        auto kind = k8s_json::get_string(*obj, "kind");
        if (!kind || *kind != "Status") {
            return result;
        }
        result.is_status_error = true;
        auto code = k8s_json::get_int(*obj, "code");
        result.status_code = static_cast<int>(code.value_or(0));
        auto msg = k8s_json::get_string(*obj, "message");
        result.message = msg.value_or("Unknown error");
        result.is_410_gone = (result.status_code == 410);
        return result;
    }

    // Simulates the resource version management on 410 Gone.
    // Mirrors: clear _resource_version when 410 is detected.
    struct ResourceVersionTracker {
        std::string resource_version;

        void handle_410() {
            resource_version.clear();
        }

        bool needs_full_relist() const {
            return resource_version.empty();
        }
    };
};

// --- Direct Status 410 Gone ---

TEST_F(K8sWatch410Test, DirectStatus410Detected) {
    std::string json = R"({"kind":"Status","apiVersion":"v1","metadata":{},"status":"Failure","message":"too old resource version: 12345 (67890)","reason":"Gone","code":410})";
    auto result = check_direct_status(json);
    EXPECT_TRUE(result.is_status_error);
    EXPECT_TRUE(result.is_410_gone);
    EXPECT_EQ(result.status_code, 410);
    EXPECT_EQ(result.message, "too old resource version: 12345 (67890)");
}

TEST_F(K8sWatch410Test, DirectStatus403NotGone) {
    std::string json = R"({"kind":"Status","apiVersion":"v1","status":"Failure","message":"forbidden","code":403})";
    auto result = check_direct_status(json);
    EXPECT_TRUE(result.is_status_error);
    EXPECT_FALSE(result.is_410_gone);
    EXPECT_EQ(result.status_code, 403);
}

TEST_F(K8sWatch410Test, DirectStatus500NotGone) {
    std::string json = R"({"kind":"Status","status":"Failure","message":"internal error","code":500})";
    auto result = check_direct_status(json);
    EXPECT_TRUE(result.is_status_error);
    EXPECT_FALSE(result.is_410_gone);
    EXPECT_EQ(result.status_code, 500);
}

TEST_F(K8sWatch410Test, DirectStatusMissingCodeDefaultsToZero) {
    std::string json = R"({"kind":"Status","message":"some error"})";
    auto result = check_direct_status(json);
    EXPECT_TRUE(result.is_status_error);
    EXPECT_FALSE(result.is_410_gone);
    EXPECT_EQ(result.status_code, 0);
    EXPECT_EQ(result.message, "some error");
}

TEST_F(K8sWatch410Test, DirectStatusMissingMessageDefaultsToUnknown) {
    std::string json = R"({"kind":"Status","code":410})";
    auto result = check_direct_status(json);
    EXPECT_TRUE(result.is_status_error);
    EXPECT_TRUE(result.is_410_gone);
    EXPECT_EQ(result.message, "Unknown error");
}

// --- ERROR Watch Events ---

TEST_F(K8sWatch410Test, ErrorEvent410Detected) {
    std::string json = R"({"type":"ERROR","object":{"kind":"Status","apiVersion":"v1","metadata":{},"status":"Failure","message":"too old resource version: 100 (200)","reason":"Gone","code":410}})";
    auto result = check_error_event(json);
    EXPECT_TRUE(result.is_status_error);
    EXPECT_TRUE(result.is_410_gone);
    EXPECT_EQ(result.status_code, 410);
    EXPECT_EQ(result.message, "too old resource version: 100 (200)");
}

TEST_F(K8sWatch410Test, ErrorEvent403NotGone) {
    std::string json = R"({"type":"ERROR","object":{"kind":"Status","message":"forbidden","code":403}})";
    auto result = check_error_event(json);
    EXPECT_TRUE(result.is_status_error);
    EXPECT_FALSE(result.is_410_gone);
    EXPECT_EQ(result.status_code, 403);
}

TEST_F(K8sWatch410Test, ErrorEventMissingObjectNotDetected) {
    std::string json = R"({"type":"ERROR"})";
    auto result = check_error_event(json);
    EXPECT_FALSE(result.is_status_error);
}

TEST_F(K8sWatch410Test, ErrorEventNonStatusObjectNotDetected) {
    std::string json = R"({"type":"ERROR","object":{"kind":"EndpointSlice"}})";
    auto result = check_error_event(json);
    EXPECT_FALSE(result.is_status_error);
}

TEST_F(K8sWatch410Test, ErrorEventMissingCodeDefaultsToZero) {
    std::string json = R"({"type":"ERROR","object":{"kind":"Status","message":"unknown"}})";
    auto result = check_error_event(json);
    EXPECT_TRUE(result.is_status_error);
    EXPECT_FALSE(result.is_410_gone);
    EXPECT_EQ(result.status_code, 0);
}

// --- Non-error events should NOT match ---

TEST_F(K8sWatch410Test, AddedEventNotDetectedAsDirect) {
    std::string json = R"({"type":"ADDED","object":{"kind":"EndpointSlice","metadata":{"uid":"abc"}}})";
    auto result = check_direct_status(json);
    EXPECT_FALSE(result.is_status_error);
}

TEST_F(K8sWatch410Test, AddedEventNotDetectedAsError) {
    std::string json = R"({"type":"ADDED","object":{"kind":"EndpointSlice","metadata":{"uid":"abc"}}})";
    auto result = check_error_event(json);
    EXPECT_FALSE(result.is_status_error);
}

TEST_F(K8sWatch410Test, ModifiedEventNotDetected) {
    std::string json = R"({"type":"MODIFIED","object":{"kind":"EndpointSlice"}})";
    auto direct = check_direct_status(json);
    auto error = check_error_event(json);
    EXPECT_FALSE(direct.is_status_error);
    EXPECT_FALSE(error.is_status_error);
}

TEST_F(K8sWatch410Test, DeletedEventNotDetected) {
    std::string json = R"({"type":"DELETED","object":{"kind":"EndpointSlice"}})";
    auto direct = check_direct_status(json);
    auto error = check_error_event(json);
    EXPECT_FALSE(direct.is_status_error);
    EXPECT_FALSE(error.is_status_error);
}

// --- Resource Version Management ---

TEST_F(K8sWatch410Test, ResourceVersionClearedOn410) {
    ResourceVersionTracker tracker;
    tracker.resource_version = "12345";

    // Simulate receiving a 410 Gone
    tracker.handle_410();
    EXPECT_TRUE(tracker.resource_version.empty());
    EXPECT_TRUE(tracker.needs_full_relist());
}

TEST_F(K8sWatch410Test, ResourceVersionNotEmptyBeforeError) {
    ResourceVersionTracker tracker;
    tracker.resource_version = "12345";
    EXPECT_FALSE(tracker.needs_full_relist());
}

TEST_F(K8sWatch410Test, EmptyResourceVersionTriggersRelist) {
    ResourceVersionTracker tracker;
    EXPECT_TRUE(tracker.needs_full_relist());
}

TEST_F(K8sWatch410Test, FullReconnectFlowSimulation) {
    // Simulate the full reconnect flow:
    // 1. Watch starts with a resource version
    // 2. 410 Gone received - resource version cleared
    // 3. sync_endpoints() called - sets new resource version
    // 4. Watch reconnects with new resource version
    ResourceVersionTracker tracker;
    tracker.resource_version = "old-version-12345";

    // Step 1: Detect 410 in direct Status
    std::string status_json = R"({"kind":"Status","code":410,"message":"too old resource version"})";
    auto result = check_direct_status(status_json);
    EXPECT_TRUE(result.is_410_gone);

    // Step 2: Clear resource version
    tracker.handle_410();
    EXPECT_TRUE(tracker.needs_full_relist());

    // Step 3: sync_endpoints() would set a new version
    tracker.resource_version = "fresh-version-67890";
    EXPECT_FALSE(tracker.needs_full_relist());

    // Step 4: Watch reconnects - would use "fresh-version-67890"
    EXPECT_EQ(tracker.resource_version, "fresh-version-67890");
}

TEST_F(K8sWatch410Test, FullReconnectFlowWithErrorEvent) {
    // Same flow but with ERROR watch event instead of direct Status
    ResourceVersionTracker tracker;
    tracker.resource_version = "stale-version";

    std::string error_json = R"({"type":"ERROR","object":{"kind":"Status","code":410,"message":"expired"}})";
    auto result = check_error_event(error_json);
    EXPECT_TRUE(result.is_410_gone);

    tracker.handle_410();
    EXPECT_TRUE(tracker.needs_full_relist());

    tracker.resource_version = "new-version";
    EXPECT_FALSE(tracker.needs_full_relist());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
