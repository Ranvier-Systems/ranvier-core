// Ranvier Core - StreamParser Unit Tests
//
// Tests for the chunked HTTP response parser used in the streaming proxy path.
// Validates zero-copy patterns, edge case handling, and error detection.

#include "stream_parser.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace ranvier;

// =============================================================================
// StreamParser Basic Tests
// =============================================================================

class StreamParserTest : public ::testing::Test {
protected:
    StreamParser parser;

    // Helper to create a temporary_buffer from string
    seastar::temporary_buffer<char> make_buffer(const std::string& s) {
        return seastar::temporary_buffer<char>(s.data(), s.size());
    }
};

TEST_F(StreamParserTest, DefaultConstructorCreatesHeaderState) {
    EXPECT_EQ(parser.state(), StreamParser::State::Headers);
    EXPECT_EQ(parser.buffer_size(), 0u);
}

TEST_F(StreamParserTest, ResetClearsState) {
    // Push some data
    parser.push(make_buffer("HTTP/1.1 200 OK\r\n\r\n"));

    parser.reset();

    EXPECT_EQ(parser.state(), StreamParser::State::Headers);
    EXPECT_EQ(parser.buffer_size(), 0u);
}

TEST_F(StreamParserTest, ParsesSimpleChunkedResponse) {
    // Complete HTTP response with chunked encoding
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_EQ(result.data, "hello");
    EXPECT_TRUE(result.done);
    EXPECT_FALSE(result.has_error);
}

TEST_F(StreamParserTest, DetectsNon200Status) {
    std::string response =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_FALSE(result.header_snoop_success);  // Not 200 OK
    EXPECT_TRUE(result.done);
}

TEST_F(StreamParserTest, HandlesMultipleChunks) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "6\r\n"
        "chunk1\r\n"
        "6\r\n"
        "chunk2\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_EQ(result.data, "chunk1chunk2");
    EXPECT_TRUE(result.done);
}

TEST_F(StreamParserTest, HandlesIncrementalParsing) {
    // Send headers first
    auto result1 = parser.push(make_buffer(
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"));
    EXPECT_TRUE(result1.header_snoop_success);
    EXPECT_FALSE(result1.done);
    EXPECT_TRUE(result1.data.empty());

    // Send chunk size
    auto result2 = parser.push(make_buffer("5\r\n"));
    EXPECT_FALSE(result2.done);

    // Send chunk data
    auto result3 = parser.push(make_buffer("hello\r\n"));
    EXPECT_EQ(result3.data, "hello");
    EXPECT_FALSE(result3.done);

    // Send terminal chunk
    auto result4 = parser.push(make_buffer("0\r\n\r\n"));
    EXPECT_TRUE(result4.done);
}

// =============================================================================
// Edge Cases and Robustness Tests
// =============================================================================

TEST_F(StreamParserTest, HandlesEmptyChunkSize) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "\r\n"  // Empty chunk size line (treat as 0)
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.done);  // Empty is treated as terminal chunk
}

TEST_F(StreamParserTest, HandlesChunkExtensions) {
    // RFC 7230: chunk-ext = *( ";" chunk-ext-name [ "=" chunk-ext-val ] )
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5;ext=value\r\n"  // Chunk size with extension
        "hello\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_EQ(result.data, "hello");
    EXPECT_TRUE(result.done);
}

TEST_F(StreamParserTest, RejectsOversizedHeaders) {
    // Create headers larger than max_header_size (16KB)
    std::string oversized_headers = "HTTP/1.1 200 OK\r\n";
    oversized_headers += std::string(20 * 1024, 'X');  // 20KB of garbage

    auto result = parser.push(make_buffer(oversized_headers));

    EXPECT_TRUE(result.has_error);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(StreamParserTest, RejectsOversizedChunk) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "FFFFFFFF\r\n";  // ~4GB chunk size

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.has_error);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(StreamParserTest, RejectsInvalidChunkSizeHex) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "ZZZ\r\n";  // Invalid hex

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.has_error);
}

TEST_F(StreamParserTest, ErrorStateIsPersistent) {
    // First, trigger an error
    std::string oversized_headers = "HTTP/1.1 200 OK\r\n";
    oversized_headers += std::string(20 * 1024, 'X');
    parser.push(make_buffer(oversized_headers));

    // Subsequent pushes should immediately return error
    auto result = parser.push(make_buffer("more data"));

    EXPECT_TRUE(result.has_error);
    EXPECT_EQ(parser.state(), StreamParser::State::Error);
}

TEST_F(StreamParserTest, HandlesSSEFormat) {
    // Server-Sent Events format used by LLM streaming APIs
    // Chunk size 17 hex = 23 bytes = strlen("data: {\"text\": \"Hi!\"}\n\n")
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "17\r\n"
        "data: {\"text\": \"Hi!\"}\n\n\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_EQ(result.data, "data: {\"text\": \"Hi!\"}\n\n");
    EXPECT_TRUE(result.done);
}

// =============================================================================
// Buffer Management Tests
// =============================================================================

TEST_F(StreamParserTest, BufferSizeReportsUnreadData) {
    // Push partial headers
    parser.push(make_buffer("HTTP/1.1 200 OK\r\n"));

    // Buffer should contain unread data
    EXPECT_GT(parser.buffer_size(), 0u);
}

TEST_F(StreamParserTest, BufferSizeAfterTerminalChunk) {
    // Per RFC 7230, chunked-body ends with: last-chunk trailer-part CRLF
    // After parsing "0\r\n", the trailing "\r\n" remains in buffer
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n"
        "\r\n";

    parser.push(make_buffer(response));

    // Trailing CRLF after terminal chunk remains (2 bytes)
    // This is correct per HTTP spec - it's part of message framing
    EXPECT_EQ(parser.buffer_size(), 2u);
}

// =============================================================================
// Content-Length (Non-Chunked) Response Tests
// =============================================================================

TEST_F(StreamParserTest, ParsesContentLengthResponse) {
    // Ollama-style response: Content-Length instead of chunked encoding
    std::string body = R"({"id":"chatcmpl-1","choices":[{"message":{"content":"Hello!"}}]})";
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_EQ(result.data, body);
    EXPECT_TRUE(result.done);
    EXPECT_FALSE(result.has_error);
}

TEST_F(StreamParserTest, ParsesContentLengthIncrementally) {
    // Body arrives in two TCP segments
    std::string body = R"({"id":"chatcmpl-1","choices":[{"message":{"content":"Hi"}}]})";
    std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n";

    auto result1 = parser.push(make_buffer(headers + body.substr(0, 10)));
    EXPECT_TRUE(result1.header_snoop_success);
    EXPECT_FALSE(result1.done);  // Not enough body data yet

    auto result2 = parser.push(make_buffer(body.substr(10)));
    EXPECT_EQ(result2.data, body);
    EXPECT_TRUE(result2.done);
}

TEST_F(StreamParserTest, DetectsNon200ContentLengthResponse) {
    std::string body = R"({"error":"model not found"})";
    std::string response =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    auto result = parser.push(make_buffer(response));

    EXPECT_FALSE(result.header_snoop_success);
    EXPECT_EQ(result.data, body);
    EXPECT_TRUE(result.done);
}

TEST_F(StreamParserTest, ContentLengthLargeJsonBody) {
    // Simulate a larger Ollama response that would have triggered
    // "Chunk size line too long" before the fix
    std::string body(500, 'x');  // 500-byte body
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_EQ(result.data, body);
    EXPECT_TRUE(result.done);
    EXPECT_FALSE(result.has_error);
}

TEST_F(StreamParserTest, LowercaseContentLengthHeader) {
    // Some servers send lowercase header names
    std::string body = R"({"ok":true})";
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "content-length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_EQ(result.data, body);
    EXPECT_TRUE(result.done);
}

TEST_F(StreamParserTest, MixedCaseTransferEncodingHeader) {
    // RFC 7230: header names are case-insensitive
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-encoding: Chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_EQ(result.data, "hello");
    EXPECT_TRUE(result.done);
}

TEST_F(StreamParserTest, LowercaseTransferEncodingHeader) {
    // Some servers send lowercase header names
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "transfer-encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_EQ(result.data, "hello");
    EXPECT_TRUE(result.done);
}

// =============================================================================
// is_streaming() Accessor Tests
// =============================================================================

TEST_F(StreamParserTest, IsStreamingReturnsTrueForChunkedResponse) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";

    parser.push(make_buffer(response));

    EXPECT_TRUE(parser.is_streaming());
    EXPECT_EQ(parser.state(), StreamParser::State::ChunkSize);
}

TEST_F(StreamParserTest, IsStreamingReturnsFalseForContentLengthResponse) {
    std::string body = R"({"id":"1","choices":[{"message":{"content":"Hi"}}]})";
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    parser.push(make_buffer(response));

    EXPECT_FALSE(parser.is_streaming());
}

TEST_F(StreamParserTest, IsStreamingReturnsFalseBeforeHeadersParsed) {
    // Before any data is pushed, state is Headers — not streaming
    EXPECT_FALSE(parser.is_streaming());
}

TEST_F(StreamParserTest, IsStreamingReturnsFalseForPartialHeaders) {
    // Push incomplete headers — state remains Headers
    parser.push(make_buffer("HTTP/1.1 200 OK\r\n"));
    EXPECT_FALSE(parser.is_streaming());
}

TEST_F(StreamParserTest, IsStreamingReturnsTrueAfterChunkDataParsed) {
    // After parsing a chunk, state is ChunkData or ChunkSize — still streaming
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n";

    parser.push(make_buffer(response));

    // After parsing the chunk data, state returns to ChunkSize
    EXPECT_TRUE(parser.is_streaming());
    EXPECT_EQ(parser.state(), StreamParser::State::ChunkSize);
}

TEST_F(StreamParserTest, IsStreamingReturnsFalseForIncrementalContentLength) {
    // Headers parsed but body incomplete — state is ContentBody, not streaming
    std::string body = R"({"id":"1","choices":[{"message":{"content":"Hello!"}}]})";
    std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n";

    parser.push(make_buffer(headers + body.substr(0, 5)));

    EXPECT_FALSE(parser.is_streaming());
    EXPECT_EQ(parser.state(), StreamParser::State::ContentBody);
}

// =============================================================================
// Config Tests
// =============================================================================

TEST(StreamParserConfigTest, ConfigValuesAreReasonable) {
    EXPECT_GE(StreamParserConfig::max_header_size, 8 * 1024);  // At least 8KB
    EXPECT_LE(StreamParserConfig::max_header_size, 64 * 1024);  // At most 64KB

    EXPECT_GE(StreamParserConfig::max_chunk_size, 64 * 1024);  // At least 64KB
    EXPECT_LE(StreamParserConfig::max_chunk_size, 16 * 1024 * 1024);  // At most 16MB

    EXPECT_GT(StreamParserConfig::compaction_threshold, 0.0);
    EXPECT_LT(StreamParserConfig::compaction_threshold, 1.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
