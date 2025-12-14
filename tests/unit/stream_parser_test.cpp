/**
 * Unit tests for StreamParser
 *
 * Tests the HTTP chunked transfer encoding parser used for streaming
 * LLM responses from backends to clients.
 *
 * The parser handles:
 * - HTTP response headers (detecting 200 OK)
 * - Chunked transfer encoding size parsing
 * - Chunk data extraction
 * - Stream completion detection
 */

#include <gtest/gtest.h>
#include "stream_parser.hpp"

#include <string>

using namespace ranvier;

class StreamParserTest : public ::testing::Test {
protected:
    StreamParser parser;

    // Helper to create a temporary_buffer from string
    seastar::temporary_buffer<char> make_buffer(const std::string& data) {
        return seastar::temporary_buffer<char>(data.c_str(), data.size());
    }
};

// =============================================================================
// Header Parsing Tests
// =============================================================================

TEST_F(StreamParserTest, ParsesSuccessHeaderCorrectly) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_TRUE(result.done);
    EXPECT_EQ(result.data, "hello");
}

TEST_F(StreamParserTest, DetectsNon200StatusCode) {
    std::string response =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: text/plain\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "error\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_FALSE(result.header_snoop_success);
    EXPECT_TRUE(result.done);
}

TEST_F(StreamParserTest, Detects404StatusCode) {
    std::string response =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_FALSE(result.header_snoop_success);
}

TEST_F(StreamParserTest, IncompleteHeadersWaitsForMore) {
    // Send partial headers
    std::string partial = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";

    auto result = parser.push(make_buffer(partial));

    // Should not have completed header parsing
    EXPECT_FALSE(result.header_snoop_success);
    EXPECT_FALSE(result.done);
    EXPECT_TRUE(result.data.empty());
}

TEST_F(StreamParserTest, HeadersCompletedInSecondPush) {
    // First push: partial headers
    parser.push(make_buffer("HTTP/1.1 200 OK\r\n"));

    // Second push: complete headers and a chunk
    auto result = parser.push(make_buffer(
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n"));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_TRUE(result.done);
    EXPECT_EQ(result.data, "hello");
}

// =============================================================================
// Chunked Encoding Tests
// =============================================================================

TEST_F(StreamParserTest, ParsesSingleChunk) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "b\r\n"
        "Hello World\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.done);
    EXPECT_EQ(result.data, "Hello World");
}

TEST_F(StreamParserTest, ParsesMultipleChunks) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "5\r\n"
        "Hello\r\n"
        "1\r\n"
        " \r\n"
        "5\r\n"
        "World\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.done);
    EXPECT_EQ(result.data, "Hello World");
}

TEST_F(StreamParserTest, ParsesHexChunkSize) {
    // 'a' in hex = 10 in decimal
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "a\r\n"
        "0123456789\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.done);
    EXPECT_EQ(result.data, "0123456789");
}

TEST_F(StreamParserTest, ParsesUppercaseHexChunkSize) {
    // 'F' in hex = 15 in decimal
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "F\r\n"
        "123456789012345\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.done);
    EXPECT_EQ(result.data, "123456789012345");
}

TEST_F(StreamParserTest, ZeroChunkEndsStream) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.done);
    EXPECT_TRUE(result.data.empty());
}

// =============================================================================
// Incremental Parsing Tests (Streaming Simulation)
// =============================================================================

TEST_F(StreamParserTest, IncrementalHeaderThenChunks) {
    // Push headers
    auto r1 = parser.push(make_buffer("HTTP/1.1 200 OK\r\n\r\n"));
    EXPECT_TRUE(r1.header_snoop_success);
    EXPECT_FALSE(r1.done);

    // Push first chunk
    auto r2 = parser.push(make_buffer("5\r\nhello\r\n"));
    EXPECT_FALSE(r2.done);
    EXPECT_EQ(r2.data, "hello");

    // Push second chunk
    auto r3 = parser.push(make_buffer("5\r\nworld\r\n"));
    EXPECT_FALSE(r3.done);
    EXPECT_EQ(r3.data, "world");

    // Push terminator
    auto r4 = parser.push(make_buffer("0\r\n\r\n"));
    EXPECT_TRUE(r4.done);
}

TEST_F(StreamParserTest, PartialChunkSizeWaitsForNewline) {
    // Headers first
    parser.push(make_buffer("HTTP/1.1 200 OK\r\n\r\n"));

    // Partial chunk size (no newline yet)
    auto r1 = parser.push(make_buffer("1"));
    EXPECT_FALSE(r1.done);
    EXPECT_TRUE(r1.data.empty());

    // Complete chunk size and data
    auto r2 = parser.push(make_buffer("0\r\n0123456789abcdef\r\n0\r\n\r\n"));
    EXPECT_TRUE(r2.done);
    EXPECT_EQ(r2.data, "0123456789abcdef");
}

TEST_F(StreamParserTest, PartialChunkDataWaitsForMore) {
    // Complete headers and chunk size
    parser.push(make_buffer("HTTP/1.1 200 OK\r\n\r\na\r\n"));

    // Partial chunk data (need 10 bytes, only got 5)
    auto r1 = parser.push(make_buffer("hello"));
    EXPECT_FALSE(r1.done);
    EXPECT_TRUE(r1.data.empty());

    // Rest of chunk data + terminator
    auto r2 = parser.push(make_buffer("world\r\n0\r\n\r\n"));
    EXPECT_TRUE(r2.done);
    EXPECT_EQ(r2.data, "helloworld");
}

TEST_F(StreamParserTest, ByteByByteStreaming) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "3\r\n"
        "abc\r\n"
        "0\r\n"
        "\r\n";

    StreamParser::Result last_result;
    for (char c : response) {
        last_result = parser.push(make_buffer(std::string(1, c)));
    }

    EXPECT_TRUE(last_result.done);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(StreamParserTest, EmptyChunkDataBetweenChunks) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.done);
    EXPECT_EQ(result.data, "hello");
}

TEST_F(StreamParserTest, LargeChunkSize) {
    // Create a response with a larger chunk (100 bytes)
    std::string data(100, 'x');
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "64\r\n" +  // 64 hex = 100 decimal
        data + "\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.done);
    EXPECT_EQ(result.data.size(), 100u);
    EXPECT_EQ(result.data, data);
}

TEST_F(StreamParserTest, ChunkWithNewlinesInData) {
    // Chunk data contains \r\n but that's part of the data
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "7\r\n"
        "a\r\nb\r\nc\r\n"  // 7 bytes: a, \r, \n, b, \r, \n, c
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.done);
    EXPECT_EQ(result.data, "a\r\nb\r\nc");
}

TEST_F(StreamParserTest, InvalidChunkSizeDefaultsToZero) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "xyz\r\n";  // Invalid hex, should be treated as 0

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.done);  // Invalid chunk size = 0 = end of stream
}

TEST_F(StreamParserTest, ResponseWithExtraHeaderFields) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "X-Custom-Header: value\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "4\r\n"
        "test\r\n"
        "0\r\n"
        "\r\n";

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.header_snoop_success);
    EXPECT_TRUE(result.done);
    EXPECT_EQ(result.data, "test");
}

// =============================================================================
// Real-world LLM Response Simulation
// =============================================================================

TEST_F(StreamParserTest, SimulatedLLMStreamingResponse) {
    // Simulate a typical LLM streaming response
    // Headers
    parser.push(make_buffer(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"));

    std::string accumulated;

    // First token: "Hello"
    auto r1 = parser.push(make_buffer("1a\r\ndata: {\"token\": \"Hello\"}\n\n\r\n"));
    accumulated += r1.data;

    // Second token: " world"
    auto r2 = parser.push(make_buffer("1b\r\ndata: {\"token\": \" world\"}\n\n\r\n"));
    accumulated += r2.data;

    // End token
    auto r3 = parser.push(make_buffer("f\r\ndata: [DONE]\n\n\r\n"));
    accumulated += r3.data;

    // Terminator
    auto r4 = parser.push(make_buffer("0\r\n\r\n"));

    EXPECT_TRUE(r4.done);
    EXPECT_TRUE(accumulated.find("Hello") != std::string::npos);
    EXPECT_TRUE(accumulated.find("world") != std::string::npos);
}

// =============================================================================
// State Consistency Tests
// =============================================================================

TEST_F(StreamParserTest, MultiplePushesAccumulateData) {
    parser.push(make_buffer("HTTP/1.1 200 OK\r\n\r\n"));

    std::string total_data;

    // Push multiple small chunks
    auto r1 = parser.push(make_buffer("1\r\na\r\n"));
    total_data += r1.data;

    auto r2 = parser.push(make_buffer("1\r\nb\r\n"));
    total_data += r2.data;

    auto r3 = parser.push(make_buffer("1\r\nc\r\n"));
    total_data += r3.data;

    parser.push(make_buffer("0\r\n\r\n"));

    EXPECT_EQ(total_data, "abc");
}
