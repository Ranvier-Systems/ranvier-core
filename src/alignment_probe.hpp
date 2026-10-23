// Ranvier Core - Backend Alignment Probe (pure logic)
//
// The native KV-event index answers for a request only when Ranvier hashes
// the token ids the backend stores (BACKLOG sections 29 and 30). This header
// holds the Seastar-free half of the per-backend probe that checks it: the
// fixed probe conversation, the POST /tokenize request and response handling,
// and the verdict. AlignmentProber (alignment_prober.hpp) does the I/O.
//
// The verdict compares Ranvier's token ids for the probe conversation (its
// routing path: RequestRewriter + the configured ChatTemplate + its tokenizer)
// with the ids the backend's /tokenize returns for the same messages, up to
// the routing prefix: the longest prefix routing could hash for this
// conversation, capped at the native index depth and aligned down to the
// block size. A divergence past that prefix (a generation prompt Ranvier's
// template does not render, for example) cannot change a routing key, so it
// still reads aligned.

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace ranvier::alignment_probe {

enum class Verdict : uint8_t {
    ALIGNED,      // Every routing-prefix token matches the backend's
    MISALIGNED,   // A routing-prefix token differs: the stream can never match a key
    UNSUPPORTED,  // No /tokenize (404/405/501): alignment unknown, not wrong
    FAILED,       // Transport or parse failure: no verdict, retried
};

inline std::string_view verdict_name(Verdict v) {
    switch (v) {
        case Verdict::ALIGNED:     return "aligned";
        case Verdict::MISALIGNED:  return "misaligned";
        case Verdict::UNSUPPORTED: return "unsupported";
        case Verdict::FAILED:      return "failed";
    }
    return "failed";
}

struct ProbeMessage {
    std::string_view role;
    std::string_view content;
};

// The probe conversation. The system message spans several 16-token blocks
// and carries what broke alignment before: a newline straight after the first
// word, whitespace the templates trim, a code fence, and non-ASCII text.
inline constexpr std::array<ProbeMessage, 2> kProbeMessages{{
    {"system",
     "You are\na careful reviewer for a C++20 network service built on a shared-nothing, "
     "thread-per-core runtime. Review only what is asked.  \n\n"
     "House rules:\n"
     "- Keep answers short and cite file:line where you can.\n"
     "- Prefer value semantics; never block the reactor.\n"
     "- R\xC3\xA9sum\xC3\xA9-style bullet lists are fine, na\xC3\xAFve guesses are not. "
     "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xA7\xE3\x82\x82\xE7\xAD\x94\xE3\x81\x88"
     "\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80\x82\n\n"
     "```cpp\n"
     "seastar::future<> f(std::vector<int> v) {\n"
     "    co_await seastar::yield();\n"
     "    co_return;\n"
     "}\n"
     "```\n  "},
    {"user", "Is this coroutine safe to call from a timer callback?"},
}};

// Rule #4 bounds for anything the backend sends back.
inline constexpr size_t kMaxProbeTokens = 8192;
inline constexpr size_t kMaxResponseBytes = 64 * 1024;

// {"messages": [...], "add_generation_prompt": true}: the body of a chat
// request as Ranvier's routing path sees it, and the body of the backend's
// POST /tokenize. Ranvier ignores add_generation_prompt; vLLM's chat path
// applies it, so the backend's ids are the ones its KV cache stores.
inline std::string build_chat_body() {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("messages");
    w.StartArray();
    for (const auto& m : kProbeMessages) {
        w.StartObject();
        w.Key("role");
        w.String(m.role.data(), static_cast<rapidjson::SizeType>(m.role.size()));
        w.Key("content");
        w.String(m.content.data(), static_cast<rapidjson::SizeType>(m.content.size()));
        w.EndObject();
    }
    w.EndArray();
    w.Key("add_generation_prompt");
    w.Bool(true);
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

inline std::string build_tokenize_request(std::string_view host, std::string_view body) {
    std::string req;
    req.reserve(160 + host.size() + body.size());
    req.append("POST /tokenize HTTP/1.1\r\nHost: ");
    req.append(host);
    req.append("\r\nContent-Type: application/json\r\nContent-Length: ");
    req.append(std::to_string(body.size()));
    req.append("\r\nConnection: close\r\n\r\n");
    req.append(body);
    return req;
}

struct HttpResponse {
    int status = 0;
    std::string body;
};

namespace detail {

inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Chunked transfer coding (RFC 9112 section 7.1); trailers ignored.
inline std::optional<std::string> dechunk(std::string_view in) {
    std::string out;
    while (true) {
        auto eol = in.find("\r\n");
        if (eol == std::string_view::npos) return std::nullopt;
        auto size_field = in.substr(0, eol);
        if (auto semi = size_field.find(';'); semi != std::string_view::npos) {
            size_field = size_field.substr(0, semi);
        }
        size_field = trim(size_field);
        size_t size = 0;
        auto [ptr, ec] = std::from_chars(size_field.data(),
                                         size_field.data() + size_field.size(), size, 16);
        if (ec != std::errc{} || ptr != size_field.data() + size_field.size() ||
            size_field.empty()) {
            return std::nullopt;
        }
        in.remove_prefix(eol + 2);
        if (size == 0) return out;
        if (size > kMaxResponseBytes || out.size() + size > kMaxResponseBytes ||
            in.size() < size + 2) {
            return std::nullopt;
        }
        out.append(in.data(), size);
        in.remove_prefix(size);
        if (in.substr(0, 2) != "\r\n") return std::nullopt;
        in.remove_prefix(2);
    }
}

}  // namespace detail

// Parses a complete HTTP/1.1 response read to EOF (the request asked for
// Connection: close). Honors Content-Length and chunked bodies.
inline std::optional<HttpResponse> parse_http_response(std::string_view raw) {
    auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string_view::npos) return std::nullopt;
    auto head = raw.substr(0, header_end);
    auto body = raw.substr(header_end + 4);

    auto line_end = head.find("\r\n");
    auto status_line = head.substr(0, line_end);
    if (status_line.substr(0, 5) != "HTTP/") return std::nullopt;
    auto sp = status_line.find(' ');
    if (sp == std::string_view::npos || status_line.size() < sp + 4) return std::nullopt;
    int status = 0;
    auto code = status_line.substr(sp + 1, 3);
    auto [ptr, ec] = std::from_chars(code.data(), code.data() + code.size(), status);
    if (ec != std::errc{} || ptr != code.data() + code.size() || status < 100 || status > 599) {
        return std::nullopt;
    }

    bool chunked = false;
    std::optional<size_t> content_length;
    std::string_view headers = line_end == std::string_view::npos
                                   ? std::string_view{}
                                   : head.substr(line_end + 2);
    while (!headers.empty()) {
        auto eol = headers.find("\r\n");
        auto line = headers.substr(0, eol);
        headers = eol == std::string_view::npos ? std::string_view{} : headers.substr(eol + 2);
        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        auto name = detail::trim(line.substr(0, colon));
        auto value = detail::trim(line.substr(colon + 1));
        if (detail::iequals(name, "transfer-encoding")) {
            // The final coding decides framing ("gzip, chunked").
            auto comma = value.rfind(',');
            auto last = detail::trim(comma == std::string_view::npos
                                         ? value : value.substr(comma + 1));
            chunked = detail::iequals(last, "chunked");
        } else if (detail::iequals(name, "content-length")) {
            size_t n = 0;
            auto [p, e] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (e != std::errc{} || p != value.data() + value.size()) return std::nullopt;
            content_length = n;
        }
    }

    HttpResponse resp;
    resp.status = status;
    if (chunked) {
        auto decoded = detail::dechunk(body);
        if (!decoded) return std::nullopt;
        resp.body = std::move(*decoded);
    } else if (content_length) {
        if (body.size() < *content_length) return std::nullopt;  // truncated
        resp.body.assign(body.data(), *content_length);
    } else {
        resp.body.assign(body.data(), body.size());
    }
    return resp;
}

// vLLM: {"count": N, "max_model_len": M, "tokens": [...], "token_strs": null}.
// Rejects anything that is not a non-empty array of non-negative int32 ids.
inline std::optional<std::vector<int32_t>> parse_tokenize_response(std::string_view body) {
    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());
    if (doc.HasParseError() || !doc.IsObject()) return std::nullopt;
    auto it = doc.FindMember("tokens");
    if (it == doc.MemberEnd() || !it->value.IsArray()) return std::nullopt;
    const auto& arr = it->value;
    if (arr.Empty() || arr.Size() > kMaxProbeTokens) return std::nullopt;
    std::vector<int32_t> ids;
    ids.reserve(arr.Size());
    for (const auto& v : arr.GetArray()) {
        if (!v.IsInt64()) return std::nullopt;
        int64_t id = v.GetInt64();
        if (id < 0 || id > std::numeric_limits<int32_t>::max()) return std::nullopt;
        ids.push_back(static_cast<int32_t>(id));
    }
    return ids;
}

// The longest prefix routing could hash for the probe: the system-prefix
// boundary when one was found (routing does not cap it), otherwise
// prefix_token_length; whichever is larger, so either routing choice is
// covered. Native lookups happen only within indexed_depth, and hash_prefix
// aligns down to the block size (parse_utils.hpp).
inline size_t routing_prefix_tokens(size_t ranvier_tokens, size_t boundary_tokens,
                                    size_t prefix_token_length, size_t indexed_depth,
                                    uint32_t block_alignment) {
    size_t p = std::max(boundary_tokens, std::min(ranvier_tokens, prefix_token_length));
    p = std::min({p, ranvier_tokens, indexed_depth});
    if (block_alignment == 0) return p;
    size_t aligned = (p / block_alignment) * block_alignment;
    return aligned == 0 ? p : aligned;
}

struct Result {
    Verdict verdict = Verdict::FAILED;
    size_t routing_prefix = 0;    // Tokens that must match
    size_t first_divergence = 0;  // First differing index (= common length if none)
    int64_t ranvier_id = -1;      // Ids at first_divergence; -1 when past the end
    int64_t backend_id = -1;
    size_t backend_tokens = 0;
    int http_status = 0;
};

inline Result classify(const std::vector<int32_t>& ranvier,
                       const std::vector<int32_t>& backend, size_t routing_prefix) {
    Result r;
    r.routing_prefix = routing_prefix;
    r.backend_tokens = backend.size();
    if (routing_prefix == 0 || ranvier.empty() || backend.empty()) {
        r.verdict = Verdict::FAILED;  // nothing to judge
        return r;
    }
    const size_t n = std::min(ranvier.size(), backend.size());
    size_t d = 0;
    while (d < n && ranvier[d] == backend[d]) ++d;
    r.first_divergence = d;
    if (d < ranvier.size()) r.ranvier_id = ranvier[d];
    if (d < backend.size()) r.backend_id = backend[d];
    r.verdict = d >= routing_prefix ? Verdict::ALIGNED : Verdict::MISALIGNED;
    return r;
}

inline Result classify_response(const std::optional<HttpResponse>& resp,
                                const std::vector<int32_t>& ranvier, size_t routing_prefix) {
    Result r;
    r.routing_prefix = routing_prefix;
    if (!resp) return r;  // FAILED: unparsable HTTP
    r.http_status = resp->status;
    if (resp->status == 404 || resp->status == 405 || resp->status == 501) {
        r.verdict = Verdict::UNSUPPORTED;
        return r;
    }
    if (resp->status != 200) return r;  // FAILED
    auto ids = parse_tokenize_response(resp->body);
    if (!ids) return r;  // FAILED
    Result c = classify(ranvier, *ids, routing_prefix);
    c.http_status = resp->status;
    return c;
}

}  // namespace ranvier::alignment_probe
