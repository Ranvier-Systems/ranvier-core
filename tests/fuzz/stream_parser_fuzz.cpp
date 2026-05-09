// libFuzzer harness for StreamParser::push.
//
// Targets audit findings:
//   H10 — chunk trailer length / overflow in parse_chunk_data
//   M11 — HTTP status snoop assumes one-shot first chunk
//   M12 — Content-Length parse without upper bound
//
// The harness splits the fuzzer input into pseudo-random chunks and feeds
// them to a single StreamParser, mimicking a backend response that arrives
// across multiple TCP reads. The split lets the fuzzer probe split-buffer
// behaviour (M11) by varying where the boundaries land.
//
// See tests/fuzz/README.md for build and run instructions.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <seastar/core/temporary_buffer.hh>

#include "stream_parser.hpp"

namespace {

// Pull a chunk size in [1, 64] from the tail of the buffer. Using the
// tail keeps the head bytes (which usually carry HTTP-shaped data) under
// the fuzzer's primary mutation pressure.
size_t pop_chunk_size(const uint8_t*& data, size_t& size) {
    if (size == 0) {
        return 0;
    }
    size_t take = (data[size - 1] % 64) + 1;
    --size;
    return take;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ranvier::StreamParser parser;

    while (size > 0) {
        size_t take = pop_chunk_size(data, size);
        if (take == 0 || take > size) {
            take = size;
        }
        seastar::temporary_buffer<char> buf(
            reinterpret_cast<const char*>(data), take);
        auto res = parser.push(std::move(buf));
        data += take;
        size -= take;
        if (res.has_error || res.done) {
            // Reset so subsequent inputs don't pile up parser state across
            // the boundary. The parser must be safe to push() into after
            // an error path, but we exercise reset() here too.
            parser.reset();
        }
    }
    return 0;
}
