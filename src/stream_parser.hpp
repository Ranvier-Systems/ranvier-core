// Ranvier Core - Streaming HTTP Response Parser
//
// Zero-copy optimized parser for chunked HTTP responses from LLM backends.
// Designed for the Seastar async model with minimal allocation in the hot path.
//
// Key design decisions:
// - Uses read_pos offset instead of substr() to avoid O(n) copies
// - Reserves output buffer capacity based on typical LLM response patterns
// - Handles malformed chunked encoding gracefully (robustness)
// - Thread-safe: Each parser instance is used on a single shard
//
// Data flow: Backend TCP chunks -> push() -> Result.data for client streaming

#pragma once

#include <optional>
#include <string>
#include <limits>

#include <seastar/core/sstring.hh>
#include <seastar/core/temporary_buffer.hh>

namespace ranvier {

// Configuration constants for buffer management
struct StreamParserConfig {
    // Maximum header size before rejecting (prevents memory exhaustion)
    static constexpr size_t max_header_size = 16 * 1024;  // 16 KB

    // Maximum single chunk size (vLLM typically sends small SSE chunks)
    static constexpr size_t max_chunk_size = 1024 * 1024;  // 1 MB

    // Maximum total accumulator size before rejecting (prevents Slowloris-style attacks)
    // This is the hard upper limit checked BEFORE appending new data.
    // 1000 concurrent connections at max size = 1GB memory consumption.
    static constexpr size_t max_accumulator_size = 1024 * 1024;  // 1 MB

    // Threshold for early warning when accumulator is approaching limit (90% of max)
    // Log debug message when first crossing this threshold to aid in monitoring
    static constexpr size_t early_warning_threshold = (max_accumulator_size * 9) / 10;  // 921.6 KB

    // Initial output buffer reservation (typical SSE event size)
    static constexpr size_t initial_output_reserve = 4096;

    // Accumulator compaction threshold (compact when read_pos > this fraction)
    static constexpr double compaction_threshold = 0.5;
};

class StreamParser {
public:
    enum class State {
        Headers,    // Parsing HTTP response headers
        ChunkSize,  // Parsing chunked encoding size line
        ChunkData,  // Reading chunk data bytes
        Error       // Parser encountered unrecoverable error
    };

    struct Result {
        seastar::sstring data;              // Clean data to send to client
        bool header_snoop_success = false;  // Did we see HTTP 200 OK?
        bool done = false;                  // Is the stream finished (0-length chunk)?
        bool has_error = false;             // Did parsing encounter an error?
        std::string error_message;          // Error description if has_error is true
    };

    // Push a chunk of data from the backend and parse it
    // Returns parsed data ready for streaming to client
    //
    // The chunk is consumed by this call (moved in).
    // Multiple calls build up state until full chunks are available.
    Result push(seastar::temporary_buffer<char> chunk);

    // Reset parser state for connection reuse
    void reset() noexcept;

    // Get current parser state (for debugging/metrics)
    State state() const noexcept { return _state; }

    // Get accumulated buffer size (for backpressure monitoring)
    size_t buffer_size() const noexcept { return _accum.size() - _read_pos; }

private:
    State _state = State::Headers;
    seastar::sstring _accum;
    size_t _read_pos = 0;           // Read offset into _accum (avoids substr copies)
    size_t _chunk_bytes_needed = 0;

    // Helper to get unread portion of accumulator as string_view (zero-copy)
    std::string_view unread_view() const noexcept {
        return std::string_view(_accum.data() + _read_pos, _accum.size() - _read_pos);
    }

    // Compact the accumulator by removing already-read data
    // Called when read_pos exceeds threshold to prevent unbounded growth
    void compact_if_needed();

    // Parse helpers returning bytes consumed or 0 if need more data
    // Returns negative on error
    ssize_t parse_headers(Result& res);
    ssize_t parse_chunk_size(Result& res);
    ssize_t parse_chunk_data(Result& res);
};

} // namespace ranvier
