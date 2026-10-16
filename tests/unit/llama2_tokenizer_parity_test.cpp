// Gated FFI parity test for the llama2 chat template (BACKLOG section 29).
//
// The native KV-event index answers for a request only when Ranvier hashes the
// token IDs the backend stores. This replays reference IDs captured from the
// backend by
//   tests/tokenizer_parity/llama2_tokenizer_parity.py --emit-fixture
// through Ranvier's real path: RequestRewriter::extract_text_with_boundary_info
// with ChatTemplate(llama2), then tokenizers::Tokenizer::FromBlobJSON + Encode.
// For every case it checks two things:
//   1. the full encoding equals the backend's IDs;
//   2. the encoding of the rendered system prefix (boundary Strategy 4) is a
//      token-for-token prefix of the full encoding, so the routing boundary
//      lands on a token the backend also has at that position.
//
// Opt-in at build time (RANVIER_BUILD_LLAMA2_PARITY_TEST=ON, default OFF): the
// model's tokenizer.json is not vendored (Llama 2 licence). Skips at run time
// unless both env vars point at real files:
//
//   RANVIER_LLAMA2_TOKENIZER_JSON  the model's tokenizer.json
//   RANVIER_LLAMA2_PARITY_FIXTURE  llama2_reference_tokens.json
//
// So it is safe on any machine: absent the artifacts it reports SKIPPED.

#include "chat_template.hpp"
#include "request_rewriter.hpp"

#include <tokenizers_cpp.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ranvier;

namespace {

std::string slurp(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    ok = true;
    return ss.str();
}

// {"messages": <the case's messages>} — the request body Ranvier would see.
std::string request_body(const rapidjson::Value& messages) {
    rapidjson::Document body;
    body.SetObject();
    rapidjson::Value copy(messages, body.GetAllocator());
    body.AddMember("messages", copy, body.GetAllocator());
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    body.Accept(writer);
    return std::string(buf.GetString(), buf.GetSize());
}

size_t first_divergence(const std::vector<int32_t>& a, const std::vector<int32_t>& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return i;
    }
    return n;
}

}  // namespace

TEST(Llama2TokenizerParity, RanvierPathReproducesBackendIds) {
    const char* tok_path = std::getenv("RANVIER_LLAMA2_TOKENIZER_JSON");
    const char* fix_path = std::getenv("RANVIER_LLAMA2_PARITY_FIXTURE");
    if (tok_path == nullptr || fix_path == nullptr) {
        GTEST_SKIP() << "set RANVIER_LLAMA2_TOKENIZER_JSON and "
                        "RANVIER_LLAMA2_PARITY_FIXTURE to run (see "
                        "tests/tokenizer_parity/README.md)";
    }

    bool ok = false;
    const std::string tok_json = slurp(tok_path, ok);
    ASSERT_TRUE(ok) << "cannot read tokenizer.json: " << tok_path;
    const std::string fixture = slurp(fix_path, ok);
    ASSERT_TRUE(ok) << "cannot read fixture: " << fix_path;

    auto tokenizer = tokenizers::Tokenizer::FromBlobJSON(tok_json);
    ASSERT_TRUE(tokenizer != nullptr) << "FromBlobJSON returned null";

    rapidjson::Document doc;
    doc.Parse(fixture.c_str());
    ASSERT_FALSE(doc.HasParseError()) << "fixture is not valid JSON";
    ASSERT_TRUE(doc.HasMember("cases") && doc["cases"].IsArray());
    const auto& cases = doc["cases"];
    ASSERT_GT(cases.Size(), 0u) << "fixture has no cases";

    const ChatTemplate tpl(ChatTemplateFormat::llama2);
    for (rapidjson::SizeType i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        ASSERT_TRUE(c.HasMember("messages") && c["messages"].IsArray());
        ASSERT_TRUE(c.HasMember("token_ids") && c["token_ids"].IsArray());
        const std::string name = c.HasMember("name") && c["name"].IsString()
                                     ? c["name"].GetString()
                                     : "case #" + std::to_string(i);

        std::vector<int32_t> expected;
        expected.reserve(c["token_ids"].Size());
        for (const auto& t : c["token_ids"].GetArray()) {
            expected.push_back(static_cast<int32_t>(t.GetInt64()));
        }

        auto extraction = RequestRewriter::extract_text_with_boundary_info(
            request_body(c["messages"]), /*need_formatted_messages=*/false, tpl);
        ASSERT_TRUE(extraction.has_value()) << name << ": nothing extracted";

        const std::vector<int32_t> got = tokenizer->Encode(extraction->text);
        EXPECT_EQ(got, expected)
            << name << ": diverged at token " << first_divergence(got, expected)
            << " (rendered: " << extraction->text << ")";

        if (extraction->has_system_prefix) {
            const std::vector<int32_t> prefix =
                tokenizer->Encode(extraction->text.substr(0, extraction->system_prefix_end));
            ASSERT_LE(prefix.size(), got.size()) << name;
            EXPECT_TRUE(std::equal(prefix.begin(), prefix.end(), got.begin()))
                << name << ": system-prefix tokens are not a prefix of the full encoding "
                << "(diverged at token " << first_divergence(prefix, got)
                << "); the Strategy 4 boundary would not match the backend's blocks";
        }
    }
}
