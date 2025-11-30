#pragma once

#include <optional>
#include <string>

#include <seastar/core/sstring.hh>
#include <seastar/core/temporary_buffer.hh>

namespace ranvier {

class StreamParser {
    enum class State { Headers, ChunkSize, ChunkData };
    State _state = State::Headers;
    seastar::sstring _accum;
    size_t _chunk_bytes_needed = 0;

public:
    struct Result {
        seastar::sstring data; // Clean data to send to user
        bool header_snoop_success = false; // Did we see 200 OK?
        bool done = false; // Is the stream finished?
    };

    Result push(seastar::temporary_buffer<char> chunk);
};

} // namespace ranvier
