// Gated FFI parity test for Kimi tokenization.
//
// Verifies that tokenizers-cpp -- the exact path RouterService/TokenizerService
// use (tokenizers::Tokenizer::FromBlobJSON + Encode) -- reproduces the
// authoritative Kimi token IDs captured by
//   tests/tokenizer_parity/kimi_tokenizer_parity.py --emit-fixture
//
// This moves the Python harness's parity check into Ranvier's real FFI path.
// It is opt-in at build time (RANVIER_BUILD_KIMI_PARITY_TEST=ON, default OFF)
// because it needs a converted Kimi tokenizer.json that is not vendored in the
// repo, and it SKIPS at run time unless both env vars point at real files:
//
//   RANVIER_KIMI_TOKENIZER_JSON  path to the converted fast tokenizer.json
//   RANVIER_KIMI_PARITY_FIXTURE  path to kimi_reference_tokens.json
//
// So it is safe on any machine: absent the artifacts it reports SKIPPED, never
// a false failure.

#include <tokenizers_cpp.h>

#include <rapidjson/document.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

}  // namespace

TEST(KimiTokenizerParity, FastTokenizerReproducesAuthoritativeIds) {
    const char* tok_path = std::getenv("RANVIER_KIMI_TOKENIZER_JSON");
    const char* fix_path = std::getenv("RANVIER_KIMI_PARITY_FIXTURE");
    if (tok_path == nullptr || fix_path == nullptr) {
        GTEST_SKIP() << "set RANVIER_KIMI_TOKENIZER_JSON and "
                        "RANVIER_KIMI_PARITY_FIXTURE to run (see "
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

    for (rapidjson::SizeType i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        ASSERT_TRUE(c.HasMember("rendered") && c["rendered"].IsString());
        ASSERT_TRUE(c.HasMember("token_ids") && c["token_ids"].IsArray());

        const std::string rendered(c["rendered"].GetString(),
                                   c["rendered"].GetStringLength());
        std::vector<int32_t> expected;
        expected.reserve(c["token_ids"].Size());
        for (const auto& t : c["token_ids"].GetArray()) {
            expected.push_back(static_cast<int32_t>(t.GetInt64()));
        }

        const std::vector<int32_t> got = tokenizer->Encode(rendered);
        EXPECT_EQ(got, expected)
            << "tokenizers-cpp diverged from the authoritative IDs for case #"
            << i << " (rendered: " << rendered << ")";
    }
}
