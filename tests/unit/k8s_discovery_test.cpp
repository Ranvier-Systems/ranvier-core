// Ranvier Core - K8s Discovery Service Unit Tests
//
// Tests for K8s endpoint parsing and JSON helper functions.
// These tests don't require Seastar runtime or actual K8s API access.

#include <gtest/gtest.h>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
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

    BackendId to_backend_id() const {
        uint32_t hash = 0;
        for (char c : uid) {
            hash = hash * 31 + static_cast<uint32_t>(c);
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
