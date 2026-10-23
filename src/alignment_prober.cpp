// Ranvier Core - Backend Alignment Prober (BACKLOG section 30)
//
// See alignment_prober.hpp. The I/O mirrors HealthService::scrape_vllm_metrics:
// one raw HTTP/1.1 request on a fresh connection, one deadline over connect,
// write and read, a bounded read to EOF, both streams closed on every path.

#include "alignment_prober.hpp"

#include "logging.hpp"
#include "request_rewriter.hpp"
#include "router_service.hpp"
#include "tokenizer_service.hpp"

#include <absl/container/flat_hash_set.h>

#include <fmt/format.h>

#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/reactor.hh>  // seastar::connect
#include <seastar/core/with_timeout.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>

using namespace seastar;

namespace ranvier {

using alignment_probe::Result;
using alignment_probe::Verdict;

AlignmentProber::AlignmentProber(Config cfg, sharded<TokenizerService>& tokenizer)
    : _cfg(std::move(cfg)),
      _tokenizer(tokenizer),
      _chat_body(alignment_probe::build_chat_body()) {
    _state.reserve(MAX_TRACKED_BACKENDS);
}

std::optional<Result> AlignmentProber::verdict(BackendId id) const {
    auto it = _state.find(id);
    if (it == _state.end() || !it->second.has_verdict) return std::nullopt;
    return it->second.result;
}

future<> AlignmentProber::run_cycle(std::vector<std::pair<BackendId, socket_address>> backends) {
    ++_cycle;

    // Prune backends that left the registry, so a reused id starts unprobed.
    if (!_state.empty()) {
        absl::flat_hash_set<BackendId> listed;
        listed.reserve(backends.size());
        for (const auto& [id, addr] : backends) listed.insert(id);
        for (auto it = _state.begin(); it != _state.end();) {
            if (!listed.contains(it->first)) {
                _state.erase(it++);
            } else {
                ++it;
            }
        }
    }

    std::vector<Candidate> due;
    for (const auto& [id, addr] : backends) {
        auto stream = RouterService::native_stream_probe_state(id);
        if (!stream.fresh) continue;  // no live stream: nothing a verdict would change
        auto it = _state.find(id);
        if (it != _state.end()) {
            const auto& st = it->second;
            if (st.has_verdict && st.epoch == stream.complete_epoch &&
                st.consecutive_failures == 0) {
                continue;  // verdict current for this epoch
            }
            if (st.consecutive_failures > 0 && _cycle < st.next_attempt_cycle) {
                continue;  // backing off
            }
        }
        due.push_back(Candidate{id, addr, stream.complete_epoch});
    }
    if (due.empty() || _ranvier_disabled) co_return;

    if (!co_await ensure_ranvier_side()) co_return;  // logged; retried next cycle

    // Rule #2: bounded concurrency, not a sequential await loop. Rule #16: the
    // callable is a plain lambda returning probe_one()'s future, not a lambda
    // coroutine, so nothing it captures can be freed at a suspension point.
    co_await max_concurrent_for_each(due, MAX_CONCURRENT_PROBES,
                                     [this](Candidate c) { return probe_one(std::move(c)); });
}

future<bool> AlignmentProber::ensure_ranvier_side() {
    if (_ranvier) co_return true;

    auto extraction = RequestRewriter::extract_text_with_boundary_info(
        _chat_body, /*need_formatted_messages=*/false, _cfg.chat_template);
    if (!extraction || extraction->text.empty()) {
        // Deterministic for a given template: retrying cannot help.
        _ranvier_disabled = true;
        log_health.warn("Alignment probe: the routing path extracted no text from the probe "
                        "conversation (chat template {}); probing disabled until restart",
                        chat_template_format_name(_cfg.chat_template.format()));
        co_return false;
    }

    // Same tokenizer and path as routing. Rule #17: never the blocking encode()
    // on the reactor. No outer with_timeout: encode_threaded_async bounds its own
    // waits and keeps reading its string_view after suspending, so abandoning it
    // on a timeout would free the text under it (Rule #21). The strings live in
    // this frame until both awaits complete.
    std::string full_text = std::move(extraction->text);
    std::string prefix_text;
    if (extraction->has_system_prefix && extraction->system_prefix_end <= full_text.size()) {
        prefix_text = full_text.substr(0, extraction->system_prefix_end);
    }
    try {
        auto full = co_await _tokenizer.local().encode_threaded_async(full_text);
        size_t boundary_tokens = 0;
        if (!prefix_text.empty()) {
            auto prefix = co_await _tokenizer.local().encode_threaded_async(prefix_text);
            boundary_tokens = prefix.tokens.size();
        }
        RanvierSide side;
        side.routing_prefix = alignment_probe::routing_prefix_tokens(
            full.tokens.size(), boundary_tokens, _cfg.prefix_token_length,
            _cfg.indexed_depth, _cfg.block_alignment);
        side.ids = std::move(full.tokens);
        if (side.ids.empty() || side.routing_prefix == 0) {
            if (_ranvier_failures++ % MAX_BACKOFF_CYCLES == 0) {
                log_health.warn("Alignment probe: the tokenizer returned no ids for the "
                                "probe conversation; retrying next cycle");
            }
            co_return false;
        }
        log_health.info("Alignment probe ready: {} probe tokens, routing prefix {} "
                        "(boundary {}, template {})",
                        side.ids.size(), side.routing_prefix, boundary_tokens,
                        chat_template_format_name(_cfg.chat_template.format()));
        _ranvier = std::move(side);
        co_return true;
    } catch (const std::exception& e) {
        // Rule #9: warn with context, throttled; the next cycle retries.
        if (_ranvier_failures++ % MAX_BACKOFF_CYCLES == 0) {
            log_health.warn("Alignment probe: tokenizing the probe conversation failed: {}",
                            e.what());
        }
        co_return false;
    }
}

future<> AlignmentProber::probe_one(Candidate c) {
    Result r;
    std::string error;
    try {
        auto resp = co_await post_tokenize(c.addr);
        r = alignment_probe::classify_response(resp, _ranvier->ids, _ranvier->routing_prefix);
        if (r.verdict == Verdict::FAILED) {
            error = resp ? fmt::format("HTTP {} or unparsable /tokenize body", resp->status)
                         : std::string("unparsable HTTP response");
        }
    } catch (const std::exception& e) {
        r.verdict = Verdict::FAILED;
        r.routing_prefix = _ranvier->routing_prefix;
        error = e.what();
    }
    // record() logs every outcome at the right level (warn for failures,
    // Rule #9), throttled by the backoff.
    record(c, r, error);
}

future<std::optional<alignment_probe::HttpResponse>>
AlignmentProber::post_tokenize(socket_address addr) {
    auto deadline = lowres_clock::now() + _cfg.timeout;
    auto conn = co_await with_timeout(deadline, seastar::connect(addr));
    auto out = conn.output();
    auto in = conn.input();

    std::exception_ptr ep;
    std::string raw;
    try {
        auto request = alignment_probe::build_tokenize_request(
            fmt::format("{}:{}", addr.addr(), addr.port()), _chat_body);
        co_await out.write(request);
        co_await out.flush();
        // Rule #4: bounded read; a response past the cap is not a probe answer.
        while (raw.size() <= alignment_probe::kMaxResponseBytes) {
            auto buf = co_await with_timeout(deadline, in.read());
            if (buf.empty()) break;  // EOF (Connection: close)
            raw.append(buf.get(), buf.size());
        }
    } catch (...) {
        ep = std::current_exception();
    }
    // Close both streams on every path (co_await is not allowed in a handler).
    // Rule #9: warn; a probe runs about once per engine start, so this is rare.
    try { co_await out.close(); } catch (const std::exception& e) {
        log_health.warn("Alignment probe {}: output stream close failed: {}", addr, e.what());
    }
    try { co_await in.close(); } catch (const std::exception& e) {
        log_health.warn("Alignment probe {}: input stream close failed: {}", addr, e.what());
    }
    if (ep) std::rethrow_exception(ep);
    if (raw.size() > alignment_probe::kMaxResponseBytes) co_return std::nullopt;
    co_return alignment_probe::parse_http_response(raw);
}

void AlignmentProber::record(const Candidate& c, Result r, std::string_view error) {
    auto it = _state.find(c.id);
    if (it == _state.end()) {
        if (_state.size() >= MAX_TRACKED_BACKENDS) {
            if (_state_overflow_drops++ == 0) {
                log_health.warn("Alignment probe: tracking cap of {} backends reached; "
                                "backend {} stays unverified", MAX_TRACKED_BACKENDS, c.id);
            }
            return;
        }
        it = _state.emplace(c.id, BackendState{}).first;
    }
    auto& st = it->second;
    const auto template_name = chat_template_format_name(_cfg.chat_template.format());

    if (r.verdict == Verdict::FAILED) {
        ++_stats.failed;
        ++st.consecutive_failures;
        uint32_t backoff = 1u << std::min<uint32_t>(st.consecutive_failures - 1, 3);
        backoff = std::min(backoff, MAX_BACKOFF_CYCLES);
        st.next_attempt_cycle = _cycle + backoff;
        // Rule #9 at warn, throttled: the first failure, then once per
        // MAX_BACKOFF_CYCLES attempts while it keeps failing.
        if (st.consecutive_failures == 1 ||
            st.consecutive_failures % MAX_BACKOFF_CYCLES == 0) {
            log_health.warn("Alignment probe: backend {} at {} failed ({} in a row): {}; "
                            "retrying in {} health cycles",
                            c.id, c.addr, st.consecutive_failures, error, backoff);
        }
        return;  // any earlier verdict stays as it was
    }

    const bool changed = !st.has_verdict || st.epoch != c.epoch ||
                         st.result.verdict != r.verdict ||
                         st.result.first_divergence != r.first_divergence;
    st.has_verdict = true;
    st.epoch = c.epoch;
    st.result = r;
    st.consecutive_failures = 0;
    st.next_attempt_cycle = 0;

    switch (r.verdict) {
        case Verdict::ALIGNED:
            ++_stats.aligned;
            if (changed) {
                log_health.info("Alignment probe: backend {} aligned: first {} routing-prefix "
                                "tokens identical ({} of {} probe tokens match; template {})",
                                c.id, r.routing_prefix, r.first_divergence,
                                _ranvier->ids.size(), template_name);
            }
            break;
        case Verdict::MISALIGNED:
            ++_stats.misaligned;
            if (changed) {
                log_health.warn(
                    "Alignment probe: backend {} MISALIGNED: Ranvier and the backend first "
                    "differ at token {} (Ranvier id {}, backend id {}), inside the {}-token "
                    "routing prefix. Its native KV events can never match a routing key. "
                    "Check the chat template ({}) and tokenizer ({}) against the served "
                    "model (BACKLOG section 30)",
                    c.id, r.first_divergence, r.ranvier_id, r.backend_id, r.routing_prefix,
                    template_name, _cfg.tokenizer_path);
            }
            break;
        case Verdict::UNSUPPORTED:
            ++_stats.unsupported;
            if (changed) {
                log_health.info("Alignment probe: backend {} has no /tokenize (HTTP {}); "
                                "its stream's alignment is unverified",
                                c.id, r.http_status);
            }
            break;
        case Verdict::FAILED:
            break;  // handled above
    }
}

}  // namespace ranvier
