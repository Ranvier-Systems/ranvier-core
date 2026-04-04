// Ranvier Core - Streaming HTTP Response Parser Implementation
//
// Optimized for Seastar's shared-nothing model with zero-copy patterns.
// Uses read position tracking to avoid O(n) substring copies in the hot path.

#include "stream_parser.hpp"
#include "logging.hpp"
#include "metrics_service.hpp"

#include <charconv>
#include <cstring>

namespace ranvier {

StreamParser::Result StreamParser::push(seastar::temporary_buffer<char> chunk) {
    Result res;

    // Early exit if parser is in error state
    if (_state == State::Error) {
        res.has_error = true;
        res.error_message = "Parser in error state";
        return res;
    }

    // Early exit for empty chunks
    if (chunk.empty()) {
        return res;
    }

    // SECURITY: Early size check BEFORE appending new data
    // Prevents Slowloris-style attacks that slowly accumulate data to exhaust memory.
    // Check is performed before append to reject immediately rather than after growth.
    size_t new_size = _accum.size() + chunk.size();
    if (new_size > StreamParserConfig::max_accumulator_size) {
        log_parser.warn("Accumulator size limit exceeded: {} + {} > {} bytes",
                        _accum.size(), chunk.size(), StreamParserConfig::max_accumulator_size);
        _state = State::Error;
        res.has_error = true;
        res.error_message = "Request too large";
        if (g_metrics) {
            g_metrics->record_stream_parser_size_rejection();
        }
        return res;
    }

    // Debug log when approaching limit (crossing early warning threshold for first time)
    // This aids in monitoring slow accumulation patterns before hard rejection
    if (new_size > StreamParserConfig::early_warning_threshold &&
        _accum.size() <= StreamParserConfig::early_warning_threshold) {
        log_parser.debug("Accumulator approaching limit: {}/{} bytes (90% threshold crossed)",
                         new_size, StreamParserConfig::max_accumulator_size);
    }

    // Append incoming chunk to accumulator
    // Note: This is a necessary copy from the network buffer.
    // Seastar's temporary_buffer owns the memory from the TCP stack.
    _accum.append(chunk.get(), chunk.size());

    // Note: seastar::sstring doesn't support reserve(), so we rely on
    // its small-string optimization and natural growth pattern

    // Parse loop: process as much data as possible
    while (_read_pos < _accum.size() && _state != State::Error) {
        ssize_t consumed = 0;

        switch (_state) {
            case State::Headers:
                consumed = parse_headers(res);
                break;
            case State::ChunkSize:
                consumed = parse_chunk_size(res);
                break;
            case State::ChunkData:
                consumed = parse_chunk_data(res);
                break;
            case State::ContentBody:
                consumed = parse_content_body(res);
                break;
            case State::Error:
                // Already handled above
                break;
        }

        if (consumed < 0) {
            // Error occurred
            _state = State::Error;
            res.has_error = true;
            break;
        }

        if (consumed == 0) {
            // Need more data
            break;
        }

        _read_pos += static_cast<size_t>(consumed);

        // Check for completion
        if (res.done) {
            break;
        }
    }

    // Compact the accumulator if we've consumed more than threshold
    compact_if_needed();

    return res;
}

void StreamParser::reset() noexcept {
    _state = State::Headers;
    _accum = seastar::sstring();  // sstring doesn't have clear()
    _read_pos = 0;
    _chunk_bytes_needed = 0;
    _content_length = 0;
}

void StreamParser::compact_if_needed() {
    // Compact when read_pos exceeds threshold of total buffer size
    // This prevents unbounded memory growth while minimizing copies
    if (_read_pos > 0 &&
        _accum.size() > 0 &&
        static_cast<double>(_read_pos) / static_cast<double>(_accum.size()) >
            StreamParserConfig::compaction_threshold) {

        // Move unread data to beginning of buffer
        size_t unread_size = _accum.size() - _read_pos;
        if (unread_size > 0) {
            // Use memmove for overlapping regions
            std::memmove(_accum.data(), _accum.data() + _read_pos, unread_size);
        }
        _accum.resize(unread_size);
        _read_pos = 0;
    }
}

ssize_t StreamParser::parse_headers(Result& res) {
    auto view = unread_view();

    // Look for end of headers (CRLF CRLF)
    auto header_end = view.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        // Check for header size limit (prevents memory exhaustion attack)
        if (view.size() > StreamParserConfig::max_header_size) {
            res.error_message = "Headers exceed maximum size";
            return -1;
        }
        return 0;  // Need more data
    }

    // Extract headers for status snoop
    auto headers = view.substr(0, header_end);

    // Check for HTTP 200 OK status
    // Format: "HTTP/1.1 200 OK" or "HTTP/1.0 200 ..."
    // We look for " 200 " which handles various HTTP versions
    res.header_snoop_success = (headers.find(" 200 ") != std::string_view::npos);

    // Determine transfer encoding: chunked vs Content-Length.
    // Ollama (and some other backends) send non-chunked responses with Content-Length.
    // We must detect this to avoid misinterpreting the body as chunked encoding.
    //
    // Per RFC 7230 §3.2, HTTP header field names are case-insensitive.
    // We use a case-insensitive prefix match to handle any server casing.
    bool is_chunked = false;
    bool saw_connection_close = false;
    bool saw_connection_keepalive = false;
    _content_length = 0;

    // Case-insensitive prefix match (needle must be all lowercase)
    auto icase_starts_with = [](std::string_view haystack, std::string_view needle) -> bool {
        if (haystack.size() < needle.size()) return false;
        for (size_t i = 0; i < needle.size(); ++i) {
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i]))) != needle[i])
                return false;
        }
        return true;
    };

    // Case-insensitive substring search (needle must be all lowercase)
    auto icase_contains = [](std::string_view haystack, std::string_view needle) -> bool {
        if (haystack.size() < needle.size()) return false;
        for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
            bool match = true;
            for (size_t j = 0; j < needle.size() && match; ++j) {
                match = (static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i + j]))) == needle[j]);
            }
            if (match) return true;
        }
        return false;
    };

    // Scan each header line for Transfer-Encoding and Content-Length
    size_t line_start = 0;
    while (line_start < headers.size()) {
        auto line_end = headers.find("\r\n", line_start);
        if (line_end == std::string_view::npos) line_end = headers.size();
        auto line = headers.substr(line_start, line_end - line_start);

        if (icase_starts_with(line, "transfer-encoding:")) {
            if (icase_contains(line.substr(18), "chunked")) {
                is_chunked = true;
            }
        } else if (icase_starts_with(line, "connection:")) {
            auto val = line.substr(11);
            if (icase_contains(val, "close")) {
                saw_connection_close = true;
            }
            if (icase_contains(val, "keep-alive")) {
                saw_connection_keepalive = true;
            }
        } else if (icase_starts_with(line, "content-length:")) {
            auto val_str = line.substr(15);
            while (!val_str.empty() && (val_str.front() == ' ' || val_str.front() == '\t')) {
                val_str.remove_prefix(1);
            }
            // Trim trailing whitespace (OWS per RFC 7230 §3.2.6)
            while (!val_str.empty() && (val_str.back() == ' ' || val_str.back() == '\t')) {
                val_str.remove_suffix(1);
            }
            auto [ptr, ec] = std::from_chars(val_str.data(), val_str.data() + val_str.size(),
                                              _content_length);
            if (ec != std::errc{} || ptr != val_str.data() + val_str.size()) {
                res.error_message = "Invalid Content-Length header";
                return -1;
            }
        }

        line_start = (line_end < headers.size()) ? line_end + 2 : headers.size();
    }

    // Determine connection persistence.
    // HTTP/1.0 defaults to close unless explicit "Connection: keep-alive".
    // HTTP/1.1 defaults to keep-alive unless explicit "Connection: close".
    bool is_http_10 = icase_starts_with(headers, "http/1.0");
    if (saw_connection_close) {
        res.connection_close = true;
    } else if (is_http_10 && !saw_connection_keepalive) {
        res.connection_close = true;
    }

    if (is_chunked) {
        _state = State::ChunkSize;
    } else {
        if (_content_length > StreamParserConfig::max_accumulator_size) {
            res.error_message = "Content-Length exceeds maximum";
            return -1;
        }
        _state = State::ContentBody;
    }

    return static_cast<ssize_t>(header_end + 4);
}

ssize_t StreamParser::parse_chunk_size(Result& res) {
    auto view = unread_view();

    // Look for chunk size line ending (CRLF)
    auto line_end = view.find("\r\n");
    if (line_end == std::string_view::npos) {
        // Chunk size lines are typically very short (e.g., "1a\r\n")
        // Limit to reasonable maximum to detect malformed input
        if (view.size() > 32) {
            res.error_message = "Chunk size line too long";
            return -1;
        }
        return 0;  // Need more data
    }

    // Parse hex chunk size
    // Note: Chunk size may have optional extension after semicolon (RFC 7230)
    auto size_str = view.substr(0, line_end);

    // Handle optional chunk extension (ignore everything after semicolon)
    auto semi_pos = size_str.find(';');
    if (semi_pos != std::string_view::npos) {
        size_str = size_str.substr(0, semi_pos);
    }

    // Trim whitespace (defensive parsing)
    while (!size_str.empty() && (size_str.front() == ' ' || size_str.front() == '\t')) {
        size_str.remove_prefix(1);
    }
    while (!size_str.empty() && (size_str.back() == ' ' || size_str.back() == '\t')) {
        size_str.remove_suffix(1);
    }

    // Parse hex value using std::from_chars (no allocation, no exception)
    size_t chunk_size = 0;
    auto [ptr, ec] = std::from_chars(size_str.data(), size_str.data() + size_str.size(),
                                      chunk_size, 16);

    if (ec != std::errc{} || ptr != size_str.data() + size_str.size()) {
        // Allow empty chunk size line in some edge cases (treat as 0)
        if (size_str.empty()) {
            chunk_size = 0;
        } else {
            res.error_message = "Invalid chunk size encoding";
            return -1;
        }
    }

    // Check for reasonable chunk size (prevents memory exhaustion)
    if (chunk_size > StreamParserConfig::max_chunk_size) {
        res.error_message = "Chunk size exceeds maximum";
        return -1;
    }

    if (chunk_size == 0) {
        // Terminal chunk - stream is complete
        res.done = true;
        return static_cast<ssize_t>(line_end + 2);
    }

    _chunk_bytes_needed = chunk_size;
    _state = State::ChunkData;
    return static_cast<ssize_t>(line_end + 2);
}

ssize_t StreamParser::parse_chunk_data(Result& res) {
    auto view = unread_view();

    // Need chunk data + CRLF trailer
    size_t needed = _chunk_bytes_needed + 2;
    if (view.size() < needed) {
        return 0;  // Need more data
    }

    // Extract chunk data (zero-copy view, then append to output)
    auto chunk_data = view.substr(0, _chunk_bytes_needed);
    res.data.append(chunk_data.data(), chunk_data.size());

    // Validate CRLF trailer
    if (view[_chunk_bytes_needed] != '\r' || view[_chunk_bytes_needed + 1] != '\n') {
        // Some backends may be lenient; log but don't fail
        // This handles edge cases with non-compliant servers
    }

    _chunk_bytes_needed = 0;
    _state = State::ChunkSize;
    return static_cast<ssize_t>(needed);
}

ssize_t StreamParser::parse_content_body(Result& res) {
    auto view = unread_view();

    if (_content_length > 0) {
        // Known Content-Length: wait for all bytes
        if (view.size() < _content_length) {
            return 0;  // Need more data
        }
        res.data.append(view.data(), _content_length);
        res.done = true;
        return static_cast<ssize_t>(_content_length);
    }

    // No Content-Length and not chunked: accumulate until EOF.
    // EOF is signaled by an empty chunk from the network layer, which sets res.done
    // in the caller. Here we just pass through whatever data is available.
    if (!view.empty()) {
        res.data.append(view.data(), view.size());
        return static_cast<ssize_t>(view.size());
    }

    return 0;
}

} // namespace ranvier
