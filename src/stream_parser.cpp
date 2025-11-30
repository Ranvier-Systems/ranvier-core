#include "stream_parser.hpp"

namespace ranvier {

StreamParser::Result StreamParser::push(seastar::temporary_buffer<char> chunk) {
    Result res;
    _accum.append(chunk.get(), chunk.size());

    while (!_accum.empty()) {
        if (_state == State::Headers) {
            auto header_end = _accum.find("\r\n\r\n");
            if (header_end == seastar::sstring::npos) return res; // Need more data

            seastar::sstring headers = _accum.substr(0, header_end);
            res.header_snoop_success = (headers.find(" 200 ") != seastar::sstring::npos);

            _accum = _accum.substr(header_end + 4);
            _state = State::ChunkSize;
        } else if (_state == State::ChunkSize) {
            auto line_end = _accum.find("\r\n");
            if (line_end == seastar::sstring::npos) return res;

            seastar::sstring size_str = _accum.substr(0, line_end);
            _accum = _accum.substr(line_end + 2);

            try {
                _chunk_bytes_needed = std::stoul(size_str, nullptr, 16);
            } catch(...) { _chunk_bytes_needed = 0; }

            if (_chunk_bytes_needed == 0) {
                res.done = true;
                return res;
            }
            _state = State::ChunkData;
        } else if (_state == State::ChunkData) {
            if (_accum.size() < _chunk_bytes_needed + 2) return res;

            res.data += _accum.substr(0, _chunk_bytes_needed);
            _accum = _accum.substr(_chunk_bytes_needed + 2); // Skip data + \r\n
            _state = State::ChunkSize;
        }
    }
    return res;
}

} // namespace ranvier
