// Ranvier Core - Backend Alignment Probe Unit Tests (BACKLOG section 30)
//
// The pure half of the probe: request building, HTTP and /tokenize response
// parsing, the routing-prefix computation, and the verdict.

#include "alignment_probe.hpp"
#include "chat_template.hpp"
#include "request_rewriter.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace ranvier;
using namespace ranvier::alignment_probe;

namespace {

std::vector<int32_t> iota_ids(size_t n, int32_t start = 1) {
    std::vector<int32_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = start + static_cast<int32_t>(i);
    return v;
}

std::string http(std::string_view status_line, std::string_view headers, std::string_view body) {
    std::string s(status_line);
    s += "\r\n";
    s += headers;
    s += "\r\n";
    s += body;
    return s;
}

}  // namespace

// --- Request building ---------------------------------------------------------

TEST(AlignmentProbeRequest, ChatBodyCarriesTheProbeMessagesAndGenerationPrompt) {
    std::string body = build_chat_body();
    rapidjson::Document doc;
    doc.Parse(body.c_str());
    ASSERT_FALSE(doc.HasParseError());
    ASSERT_TRUE(doc["messages"].IsArray());
    ASSERT_EQ(doc["messages"].Size(), kProbeMessages.size());
    EXPECT_STREQ(doc["messages"][0]["role"].GetString(), "system");
    EXPECT_EQ(std::string_view(doc["messages"][0]["content"].GetString()),
              kProbeMessages[0].content);
    EXPECT_TRUE(doc["add_generation_prompt"].GetBool());
}

TEST(AlignmentProbeRequest, RoutingPathSeesASystemPrefix) {
    // The probe must exercise the system-prefix boundary under every template.
    for (auto fmt : {ChatTemplateFormat::none, ChatTemplateFormat::llama3,
                     ChatTemplateFormat::chatml, ChatTemplateFormat::mistral,
                     ChatTemplateFormat::llama2}) {
        auto ex = RequestRewriter::extract_text_with_boundary_info(
            build_chat_body(), false, ChatTemplate(fmt));
        ASSERT_TRUE(ex.has_value()) << chat_template_format_name(fmt);
        EXPECT_TRUE(ex->has_system_prefix) << chat_template_format_name(fmt);
        EXPECT_GT(ex->system_prefix_end, 200u) << chat_template_format_name(fmt);
        EXPECT_LE(ex->system_prefix_end, ex->text.size());
    }
}

TEST(AlignmentProbeRequest, TokenizeRequestIsWellFormed) {
    std::string body = R"({"messages":[]})";
    std::string req = build_tokenize_request("10.0.0.5:8000", body);
    EXPECT_EQ(req.rfind("POST /tokenize HTTP/1.1\r\n", 0), 0u);
    EXPECT_NE(req.find("\r\nHost: 10.0.0.5:8000\r\n"), std::string::npos);
    EXPECT_NE(req.find("\r\nContent-Length: " + std::to_string(body.size()) + "\r\n"),
              std::string::npos);
    EXPECT_NE(req.find("\r\nConnection: close\r\n\r\n"), std::string::npos);
    EXPECT_EQ(req.substr(req.size() - body.size()), body);
}

// --- HTTP response parsing ---------------------------------------------------

TEST(AlignmentProbeHttp, ContentLengthBody) {
    auto r = parse_http_response(http("HTTP/1.1 200 OK", "Content-Length: 5\r\n", "hello"));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->body, "hello");
}

TEST(AlignmentProbeHttp, ContentLengthTruncatedIsRejected) {
    EXPECT_FALSE(parse_http_response(http("HTTP/1.1 200 OK", "Content-Length: 9\r\n", "hello"))
                     .has_value());
}

TEST(AlignmentProbeHttp, ChunkedBody) {
    auto r = parse_http_response(http("HTTP/1.1 200 OK",
                                      "transfer-encoding: chunked\r\n",
                                      "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\n"));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->body, "hello world");
}

TEST(AlignmentProbeHttp, MalformedChunkIsRejected) {
    EXPECT_FALSE(parse_http_response(http("HTTP/1.1 200 OK", "Transfer-Encoding: chunked\r\n",
                                          "zz\r\nhello\r\n0\r\n\r\n"))
                     .has_value());
    EXPECT_FALSE(parse_http_response(http("HTTP/1.1 200 OK", "Transfer-Encoding: chunked\r\n",
                                          "5\r\nhel"))
                     .has_value());
}

TEST(AlignmentProbeHttp, NoBodyFramingReadsToEof) {
    auto r = parse_http_response(http("HTTP/1.1 404 Not Found", "", "{\"detail\":\"x\"}"));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404);
    EXPECT_EQ(r->body, "{\"detail\":\"x\"}");
}

TEST(AlignmentProbeHttp, GarbageIsRejected) {
    EXPECT_FALSE(parse_http_response("").has_value());
    EXPECT_FALSE(parse_http_response("HTTP/1.1 200 OK\r\n").has_value());  // no header end
    EXPECT_FALSE(parse_http_response("SMTP 220 hi\r\n\r\n").has_value());
    EXPECT_FALSE(parse_http_response("HTTP/1.1 2x0 OK\r\n\r\n").has_value());
    EXPECT_FALSE(parse_http_response("HTTP/1.1 999 Nope\r\n\r\n").has_value());
}

// --- /tokenize body parsing --------------------------------------------------

TEST(AlignmentProbeTokenize, ParsesVllmShape) {
    auto ids = parse_tokenize_response(
        R"({"count":4,"max_model_len":8192,"tokens":[1,518,25580,29962],"token_strs":null})");
    ASSERT_TRUE(ids.has_value());
    EXPECT_EQ(*ids, (std::vector<int32_t>{1, 518, 25580, 29962}));
}

TEST(AlignmentProbeTokenize, RejectsBadShapes) {
    EXPECT_FALSE(parse_tokenize_response("not json").has_value());
    EXPECT_FALSE(parse_tokenize_response("[1,2,3]").has_value());
    EXPECT_FALSE(parse_tokenize_response(R"({"count":0})").has_value());
    EXPECT_FALSE(parse_tokenize_response(R"({"tokens":[]})").has_value());
    EXPECT_FALSE(parse_tokenize_response(R"({"tokens":[1,"2"]})").has_value());
    EXPECT_FALSE(parse_tokenize_response(R"({"tokens":[1,2.5]})").has_value());
    EXPECT_FALSE(parse_tokenize_response(R"({"tokens":[1,-3]})").has_value());
    EXPECT_FALSE(parse_tokenize_response(R"({"tokens":[1,4294967296]})").has_value());
}

TEST(AlignmentProbeTokenize, RejectsOversizeArrays) {
    std::string body = R"({"tokens":[)";
    for (size_t i = 0; i <= kMaxProbeTokens; ++i) {
        if (i) body += ',';
        body += '1';
    }
    body += "]}";
    EXPECT_FALSE(parse_tokenize_response(body).has_value());
}

// --- Routing prefix ----------------------------------------------------------

TEST(AlignmentProbePrefix, BoundaryLongerThanPrefixLengthWins) {
    // 150-token system prefix, 128-token routing key: routing may hash at
    // either, so the probe must cover 150, aligned down to 144.
    EXPECT_EQ(routing_prefix_tokens(190, 150, 128, 2048, 16), 144u);
}

TEST(AlignmentProbePrefix, PrefixLengthWinsWhenBoundaryIsShort) {
    EXPECT_EQ(routing_prefix_tokens(190, 40, 128, 2048, 16), 128u);
    EXPECT_EQ(routing_prefix_tokens(190, 0, 128, 2048, 16), 128u);
}

TEST(AlignmentProbePrefix, CappedByIndexDepthAndTokenCount) {
    EXPECT_EQ(routing_prefix_tokens(190, 150, 128, 64, 16), 64u);
    EXPECT_EQ(routing_prefix_tokens(100, 0, 128, 2048, 16), 96u);
}

TEST(AlignmentProbePrefix, ShortPrefixFallsBackToUnalignedLength) {
    // hash_prefix hashes the raw count when it aligns down to zero.
    EXPECT_EQ(routing_prefix_tokens(10, 0, 128, 2048, 16), 10u);
}

// --- Verdict -------------------------------------------------------------------

TEST(AlignmentProbeVerdict, IdenticalIsAligned) {
    auto ids = iota_ids(190);
    auto r = classify(ids, ids, 144);
    EXPECT_EQ(r.verdict, Verdict::ALIGNED);
    EXPECT_EQ(r.first_divergence, 190u);
    EXPECT_EQ(r.ranvier_id, -1);
    EXPECT_EQ(r.backend_id, -1);
}

TEST(AlignmentProbeVerdict, DivergencePastThePrefixIsAligned) {
    // A generation prompt the template does not render: the backend has
    // extra tokens after the conversation.
    auto ranvier = iota_ids(190);
    auto backend = ranvier;
    backend.insert(backend.end(), {9001, 9002, 9003});
    auto r = classify(ranvier, backend, 144);
    EXPECT_EQ(r.verdict, Verdict::ALIGNED);
    EXPECT_EQ(r.first_divergence, 190u);
    EXPECT_EQ(r.backend_id, 9001);

    backend = ranvier;
    backend[150] = 7;  // inside the last, partial block: never hashed
    EXPECT_EQ(classify(ranvier, backend, 144).verdict, Verdict::ALIGNED);
}

TEST(AlignmentProbeVerdict, DivergenceAtThePrefixEdge) {
    auto ranvier = iota_ids(190);
    auto backend = ranvier;
    backend[143] = 7;  // last token of the routing prefix
    auto r = classify(ranvier, backend, 144);
    EXPECT_EQ(r.verdict, Verdict::MISALIGNED);
    EXPECT_EQ(r.first_divergence, 143u);
    EXPECT_EQ(r.ranvier_id, 144);
    EXPECT_EQ(r.backend_id, 7);

    backend = ranvier;
    backend[144] = 7;  // first token past it
    EXPECT_EQ(classify(ranvier, backend, 144).verdict, Verdict::ALIGNED);
}

TEST(AlignmentProbeVerdict, BackendShorterThanThePrefixIsMisaligned) {
    auto ranvier = iota_ids(190);
    std::vector<int32_t> backend(ranvier.begin(), ranvier.begin() + 100);
    auto r = classify(ranvier, backend, 144);
    EXPECT_EQ(r.verdict, Verdict::MISALIGNED);
    EXPECT_EQ(r.first_divergence, 100u);
    EXPECT_EQ(r.backend_id, -1);
}

TEST(AlignmentProbeVerdict, TheSection29MismatchIsMisalignedAtTokenOne) {
    // 2026-10-16: vLLM (CodeLlama, Llama-2 template) started
    // <s> [INST] <<SYS>>; Ranvier (GPT-2, template none) started "You are".
    std::vector<int32_t> backend{1, 518, 25580, 29962, 3532, 14816, 29903, 6778,
                                 13, 3492, 526, 263};
    std::vector<int32_t> ranvier{1639, 389, 257, 7613, 8796, 13, 198, 5303,
                                 13, 3492, 526, 263};
    auto r = classify(ranvier, backend, 12);
    EXPECT_EQ(r.verdict, Verdict::MISALIGNED);
    EXPECT_EQ(r.first_divergence, 0u);
    EXPECT_EQ(r.ranvier_id, 1639);
    EXPECT_EQ(r.backend_id, 1);
}

TEST(AlignmentProbeVerdict, NothingToJudgeIsFailed) {
    auto ids = iota_ids(20);
    EXPECT_EQ(classify(ids, ids, 0).verdict, Verdict::FAILED);
    EXPECT_EQ(classify({}, ids, 16).verdict, Verdict::FAILED);
    EXPECT_EQ(classify(ids, {}, 16).verdict, Verdict::FAILED);
}

// --- Response classification --------------------------------------------------

TEST(AlignmentProbeClassify, StatusCodes) {
    auto ids = iota_ids(32);
    EXPECT_EQ(classify_response(std::nullopt, ids, 32).verdict, Verdict::FAILED);
    for (int s : {404, 405, 501}) {
        auto r = classify_response(HttpResponse{s, ""}, ids, 32);
        EXPECT_EQ(r.verdict, Verdict::UNSUPPORTED) << s;
        EXPECT_EQ(r.http_status, s);
    }
    for (int s : {400, 500, 503}) {
        EXPECT_EQ(classify_response(HttpResponse{s, ""}, ids, 32).verdict, Verdict::FAILED) << s;
    }
    EXPECT_EQ(classify_response(HttpResponse{200, "garbage"}, ids, 32).verdict,
              Verdict::FAILED);
}

TEST(AlignmentProbeClassify, OkBodyIsJudged) {
    std::vector<int32_t> ids{1, 2, 3, 4};
    auto aligned = classify_response(HttpResponse{200, R"({"tokens":[1,2,3,4]})"}, ids, 4);
    EXPECT_EQ(aligned.verdict, Verdict::ALIGNED);
    EXPECT_EQ(aligned.http_status, 200);
    EXPECT_EQ(aligned.backend_tokens, 4u);
    auto misaligned = classify_response(HttpResponse{200, R"({"tokens":[1,2,9,4]})"}, ids, 4);
    EXPECT_EQ(misaligned.verdict, Verdict::MISALIGNED);
    EXPECT_EQ(misaligned.first_divergence, 2u);
}

TEST(AlignmentProbeVerdict, Names) {
    EXPECT_EQ(verdict_name(Verdict::ALIGNED), "aligned");
    EXPECT_EQ(verdict_name(Verdict::MISALIGNED), "misaligned");
    EXPECT_EQ(verdict_name(Verdict::UNSUPPORTED), "unsupported");
    EXPECT_EQ(verdict_name(Verdict::FAILED), "failed");
}
