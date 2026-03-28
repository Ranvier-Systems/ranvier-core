// Ranvier Core - Local Discovery Unit Tests
//
// Tests pure parsing and detection logic from LocalDiscoveryService.
// These functions are replicated here to avoid Seastar dependencies,
// following the same pattern as k8s_discovery_test.cpp.

#include "config.hpp"
#include "types.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <rapidjson/document.h>

using namespace ranvier;

// =============================================================================
// Replicated pure functions from local_discovery.cpp
// (Avoids linking Seastar — same approach as k8s_discovery_test.cpp)
// =============================================================================

namespace local_ports {
    constexpr uint16_t OLLAMA     = 11434;
    constexpr uint16_t VLLM      = 8080;
    constexpr uint16_t LMSTUDIO  = 1234;
    constexpr uint16_t LLAMACPP  = 8000;
    constexpr uint16_t TEXTGENUI = 5000;
    constexpr uint16_t LOCALAI   = 3000;
}  // namespace local_ports

struct TestDiscoveredBackend {
    BackendId id = 0;
    uint16_t port = 0;
    std::string server_type;
    std::vector<std::string> available_models;
    uint32_t consecutive_failures = 0;
    static constexpr size_t MAX_MODELS = 128;
};

// Replicated from LocalDiscoveryService::detect_server_type
// Takes pre-parsed model list instead of re-parsing JSON
std::string detect_server_type(uint16_t port, const std::string& body,
        const std::vector<std::string>& models) {
    if (body.find("ollama") != std::string::npos) {
        return "ollama";
    }

    // Check if any model ID contains ":" (like "llama3:8b") — likely Ollama
    for (const auto& model : models) {
        if (model.find(':') != std::string::npos) {
            return "ollama";
        }
    }

    if (port == local_ports::VLLM && (body.find("vllm") != std::string::npos ||
                                       body.find("model_permission") != std::string::npos)) {
        return "vllm";
    }

    switch (port) {
        case local_ports::OLLAMA:     return "ollama";
        case local_ports::LMSTUDIO:   return "lmstudio";
        case local_ports::LLAMACPP:   return "llamacpp";
        case local_ports::TEXTGENUI:  return "textgenui";
        case local_ports::LOCALAI:    return "localai";
        default:                      return "unknown";
    }
}

// Replicated from LocalDiscoveryService::parse_models_response
TestDiscoveredBackend parse_models_response(uint16_t port, const std::string& body) {
    TestDiscoveredBackend backend;
    backend.port = port;

    rapidjson::Document doc;
    doc.Parse(body.c_str(), body.size());

    if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("data") && doc["data"].IsArray()) {
        const auto& data = doc["data"];
        size_t model_count = 0;
        for (rapidjson::SizeType i = 0; i < data.Size() && model_count < TestDiscoveredBackend::MAX_MODELS; ++i) {
            if (data[i].IsObject() && data[i].HasMember("id") && data[i]["id"].IsString()) {
                const char* model_id = data[i]["id"].GetString();
                if (model_id) {
                    backend.available_models.emplace_back(model_id);
                    ++model_count;
                }
            }
        }
    }

    backend.server_type = detect_server_type(port, body, backend.available_models);
    return backend;
}

// =============================================================================
// detect_server_type tests
// =============================================================================

class DetectServerTypeTest : public ::testing::Test {};

TEST_F(DetectServerTypeTest, OllamaByBodyMarker) {
    std::string body = R"({"data": [{"id": "llama3"}], "source": "ollama"})";
    EXPECT_EQ(detect_server_type(9999, body, {"llama3"}), "ollama");
}

TEST_F(DetectServerTypeTest, OllamaByColonInModelId) {
    std::string body = R"({"data": [{"id": "llama3:8b"}]})";
    EXPECT_EQ(detect_server_type(9999, body, {"llama3:8b"}), "ollama");
}

TEST_F(DetectServerTypeTest, OllamaByPort) {
    std::string body = R"({"data": [{"id": "some-model"}]})";
    EXPECT_EQ(detect_server_type(local_ports::OLLAMA, body, {"some-model"}), "ollama");
}

TEST_F(DetectServerTypeTest, VllmByPortAndMarker) {
    std::string body = R"({"data": [{"id": "meta-llama/Llama-2-7b", "model_permission": []}]})";
    EXPECT_EQ(detect_server_type(local_ports::VLLM, body, {"meta-llama/Llama-2-7b"}), "vllm");
}

TEST_F(DetectServerTypeTest, VllmByPortAndVllmString) {
    std::string body = R"({"data": [{"id": "model"}], "engine": "vllm"})";
    EXPECT_EQ(detect_server_type(local_ports::VLLM, body, {"model"}), "vllm");
}

TEST_F(DetectServerTypeTest, LmStudioByPort) {
    std::string body = R"({"data": [{"id": "some-model"}]})";
    EXPECT_EQ(detect_server_type(local_ports::LMSTUDIO, body, {"some-model"}), "lmstudio");
}

TEST_F(DetectServerTypeTest, LlamaCppByPort) {
    std::string body = R"({"data": [{"id": "some-model"}]})";
    EXPECT_EQ(detect_server_type(local_ports::LLAMACPP, body, {"some-model"}), "llamacpp");
}

TEST_F(DetectServerTypeTest, TextGenUIByPort) {
    std::string body = R"({"data": [{"id": "some-model"}]})";
    EXPECT_EQ(detect_server_type(local_ports::TEXTGENUI, body, {"some-model"}), "textgenui");
}

TEST_F(DetectServerTypeTest, LocalAIByPort) {
    std::string body = R"({"data": [{"id": "some-model"}]})";
    EXPECT_EQ(detect_server_type(local_ports::LOCALAI, body, {"some-model"}), "localai");
}

TEST_F(DetectServerTypeTest, UnknownPortReturnsUnknown) {
    std::string body = R"({"data": [{"id": "some-model"}]})";
    EXPECT_EQ(detect_server_type(4444, body, {"some-model"}), "unknown");
}

TEST_F(DetectServerTypeTest, OllamaBodyMarkerTakesPrecedenceOverPort) {
    // Even on vLLM's port, "ollama" in body wins
    std::string body = R"({"data": [{"id": "model"}], "source": "ollama"})";
    EXPECT_EQ(detect_server_type(local_ports::VLLM, body, {"model"}), "ollama");
}

TEST_F(DetectServerTypeTest, ColonModelIdTakesPrecedenceOverPort) {
    // Colon in model ID → ollama, even on LM Studio's port
    std::string body = R"({"data": [{"id": "mistral:7b"}]})";
    EXPECT_EQ(detect_server_type(local_ports::LMSTUDIO, body, {"mistral:7b"}), "ollama");
}

TEST_F(DetectServerTypeTest, InvalidJsonFallsBackToPort) {
    std::string body = "not json at all";
    EXPECT_EQ(detect_server_type(local_ports::LLAMACPP, body, {}), "llamacpp");
}

TEST_F(DetectServerTypeTest, EmptyBodyFallsBackToPort) {
    EXPECT_EQ(detect_server_type(local_ports::OLLAMA, "", {}), "ollama");
    EXPECT_EQ(detect_server_type(9999, "", {}), "unknown");
}

TEST_F(DetectServerTypeTest, VllmPortWithoutMarkersReturnsUnknown) {
    // Port 8080 without vllm markers falls through to default (8080 not in switch)
    std::string body = R"({"data": [{"id": "model"}]})";
    EXPECT_EQ(detect_server_type(local_ports::VLLM, body, {"model"}), "unknown");
}

// =============================================================================
// parse_models_response tests
// =============================================================================

class ParseModelsResponseTest : public ::testing::Test {};

TEST_F(ParseModelsResponseTest, SingleModel) {
    std::string body = R"({"data": [{"id": "llama3"}]})";
    auto result = parse_models_response(local_ports::OLLAMA, body);
    ASSERT_EQ(result.available_models.size(), 1u);
    EXPECT_EQ(result.available_models[0], "llama3");
    EXPECT_EQ(result.port, local_ports::OLLAMA);
    EXPECT_EQ(result.server_type, "ollama");
}

TEST_F(ParseModelsResponseTest, MultipleModels) {
    std::string body = R"({"data": [
        {"id": "llama3"},
        {"id": "codellama"},
        {"id": "mistral"}
    ]})";
    auto result = parse_models_response(local_ports::LMSTUDIO, body);
    ASSERT_EQ(result.available_models.size(), 3u);
    EXPECT_EQ(result.available_models[0], "llama3");
    EXPECT_EQ(result.available_models[1], "codellama");
    EXPECT_EQ(result.available_models[2], "mistral");
}

TEST_F(ParseModelsResponseTest, EmptyDataArray) {
    std::string body = R"({"data": []})";
    auto result = parse_models_response(local_ports::LMSTUDIO, body);
    EXPECT_TRUE(result.available_models.empty());
    EXPECT_EQ(result.server_type, "lmstudio");
}

TEST_F(ParseModelsResponseTest, MissingDataField) {
    std::string body = R"({"models": [{"id": "foo"}]})";
    auto result = parse_models_response(local_ports::LMSTUDIO, body);
    EXPECT_TRUE(result.available_models.empty());
}

TEST_F(ParseModelsResponseTest, InvalidJson) {
    std::string body = "{invalid json";
    auto result = parse_models_response(local_ports::LMSTUDIO, body);
    EXPECT_TRUE(result.available_models.empty());
}

TEST_F(ParseModelsResponseTest, EmptyBody) {
    auto result = parse_models_response(local_ports::LMSTUDIO, "");
    EXPECT_TRUE(result.available_models.empty());
}

TEST_F(ParseModelsResponseTest, ModelEntryMissingId) {
    std::string body = R"({"data": [{"name": "llama3"}, {"id": "valid-model"}]})";
    auto result = parse_models_response(local_ports::LMSTUDIO, body);
    ASSERT_EQ(result.available_models.size(), 1u);
    EXPECT_EQ(result.available_models[0], "valid-model");
}

TEST_F(ParseModelsResponseTest, ModelIdNotString) {
    std::string body = R"({"data": [{"id": 42}, {"id": "valid"}]})";
    auto result = parse_models_response(local_ports::LMSTUDIO, body);
    ASSERT_EQ(result.available_models.size(), 1u);
    EXPECT_EQ(result.available_models[0], "valid");
}

TEST_F(ParseModelsResponseTest, DataNotArray) {
    std::string body = R"({"data": "not-an-array"})";
    auto result = parse_models_response(local_ports::LMSTUDIO, body);
    EXPECT_TRUE(result.available_models.empty());
}

TEST_F(ParseModelsResponseTest, ExtraFieldsIgnored) {
    std::string body = R"({"data": [{"id": "model-1", "created": 1234, "owned_by": "system"}], "object": "list"})";
    auto result = parse_models_response(local_ports::LMSTUDIO, body);
    ASSERT_EQ(result.available_models.size(), 1u);
    EXPECT_EQ(result.available_models[0], "model-1");
}

TEST_F(ParseModelsResponseTest, MaxModelsEnforced) {
    // Build a response with more than MAX_MODELS entries
    std::string body = R"({"data": [)";
    for (size_t i = 0; i < TestDiscoveredBackend::MAX_MODELS + 10; ++i) {
        if (i > 0) body += ",";
        body += R"({"id": "model-)" + std::to_string(i) + R"("})";
    }
    body += "]}";
    auto result = parse_models_response(local_ports::LMSTUDIO, body);
    EXPECT_EQ(result.available_models.size(), TestDiscoveredBackend::MAX_MODELS);
}

TEST_F(ParseModelsResponseTest, OllamaStyleModelIds) {
    std::string body = R"({"data": [
        {"id": "llama3:8b"},
        {"id": "codellama:13b-instruct"},
        {"id": "mistral:latest"}
    ]})";
    auto result = parse_models_response(9999, body);
    ASSERT_EQ(result.available_models.size(), 3u);
    // Colon in model IDs should trigger "ollama" detection
    EXPECT_EQ(result.server_type, "ollama");
}

TEST_F(ParseModelsResponseTest, RealWorldOllamaResponse) {
    std::string body = R"({
        "object": "list",
        "data": [
            {"id": "llama3:8b", "object": "model", "created": 1700000000, "owned_by": "library"},
            {"id": "codellama:7b", "object": "model", "created": 1700000000, "owned_by": "library"}
        ]
    })";
    auto result = parse_models_response(local_ports::OLLAMA, body);
    ASSERT_EQ(result.available_models.size(), 2u);
    EXPECT_EQ(result.available_models[0], "llama3:8b");
    EXPECT_EQ(result.available_models[1], "codellama:7b");
    EXPECT_EQ(result.server_type, "ollama");
}

TEST_F(ParseModelsResponseTest, RealWorldVllmResponse) {
    std::string body = R"({
        "object": "list",
        "data": [
            {"id": "meta-llama/Llama-2-7b-chat-hf", "object": "model", "created": 1700000000,
             "owned_by": "vllm", "model_permission": []}
        ]
    })";
    auto result = parse_models_response(local_ports::VLLM, body);
    ASSERT_EQ(result.available_models.size(), 1u);
    EXPECT_EQ(result.available_models[0], "meta-llama/Llama-2-7b-chat-hf");
    EXPECT_EQ(result.server_type, "vllm");
}

// =============================================================================
// DiscoveredBackend struct tests
// =============================================================================

class DiscoveredBackendTest : public ::testing::Test {};

TEST_F(DiscoveredBackendTest, DefaultValues) {
    TestDiscoveredBackend b;
    EXPECT_EQ(b.id, 0);
    EXPECT_EQ(b.port, 0);
    EXPECT_TRUE(b.server_type.empty());
    EXPECT_TRUE(b.available_models.empty());
    EXPECT_EQ(b.consecutive_failures, 0u);
}

TEST_F(DiscoveredBackendTest, MaxModelsConstant) {
    EXPECT_EQ(TestDiscoveredBackend::MAX_MODELS, 128u);
}

// =============================================================================
// Well-known port constants
// =============================================================================

class LocalPortsTest : public ::testing::Test {};

TEST_F(LocalPortsTest, WellKnownPortValues) {
    EXPECT_EQ(local_ports::OLLAMA, 11434);
    EXPECT_EQ(local_ports::VLLM, 8080);
    EXPECT_EQ(local_ports::LMSTUDIO, 1234);
    EXPECT_EQ(local_ports::LLAMACPP, 8000);
    EXPECT_EQ(local_ports::TEXTGENUI, 5000);
    EXPECT_EQ(local_ports::LOCALAI, 3000);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
