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
    auto result1 = parser.push(make_buffer("HTTP/1.1 200 OK\r\n\r\n"));
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
        "\r\n"
        "FFFFFFFF\r\n";  // ~4GB chunk size

    auto result = parser.push(make_buffer(response));

    EXPECT_TRUE(result.has_error);
    EXPECT_FALSE(result.error_message.empty());
}

TEST_F(StreamParserTest, RejectsInvalidChunkSizeHex) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
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
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "\r\n"
        "1a\r\n"
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

TEST_F(StreamParserTest, BufferSizeZeroAfterConsumingAll) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "0\r\n"
        "\r\n";

    parser.push(make_buffer(response));

    // All data should be consumed
    EXPECT_EQ(parser.buffer_size(), 0u);
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
