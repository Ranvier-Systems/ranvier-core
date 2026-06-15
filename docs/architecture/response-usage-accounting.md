# Response-Side Usage Accounting (actuals, not estimates) — Scope

**Status:** scoping memo. **Phase 1 implemented** (ledger/attribution actuals +
`tokens_estimated` flag + cost recompute; capture-when-present) **plus the
`stream_options.include_usage` injection** (§3.2 option a, pass-through, gated by
`cost_estimation.inject_stream_usage`, default off). The Phase-2 span
output-tokens (§3.4) and the strip variant (§3.2 option b) are not yet done.
**Date:** 2026-06-15
**Context:** follow-up to the usage-ledger sink (§20.2 P1.5) and the GenAI
trace span (§20.2 P1.6), both of which currently record **pre-flight
estimates**, not the engine's authoritative counts.

## 1. Problem

Two shipped features carry token/cost figures that are estimates:

- **Usage-ledger sink (P1.5)** and the **`request_attribution` SQLite row**:
  `input_tokens` / `output_tokens` / `cost_units` come from
  `ProxyContext.estimated_*` — input from tokenization/char heuristics, output
  from the request's `max_tokens` (or a multiplier), cost from
  `estimate_request_cost()`. For metering / billing / attribution, estimated
  output tokens can be wildly off (a request that stops early, or runs to a
  smaller completion than `max_tokens`), so any downstream bill is wrong.
- **GenAI span (P1.6)**: sets exact `gen_ai.usage.input_tokens` (`tokens.size()`)
  but **no** `gen_ai.usage.output_tokens` — deliberately omitted because the
  span ends at dispatch handoff, before the response streams.

Meanwhile the OpenAI-compatible response carries the engine's authoritative
`usage {prompt_tokens, completion_tokens, total_tokens}`, which Ranvier proxies
straight through and never reads. This memo scopes capturing those actuals and
feeding them to the consumers above.

## 2. Current state (findings)

- **Nothing parses response `usage`** anywhere on the proxy path (grep: the only
  `*_tokens` reads are vLLM `/metrics` scraping and request-side `max_tokens`).
- **`StreamParser`** (`src/stream_parser.hpp`) already snoops the backend stream:
  it parses the HTTP status line, tracks `done`, distinguishes streaming
  (chunked) vs non-streaming (Content-Length), and returns clean `data` to the
  client. It is the natural hook to also snoop `usage` — same pattern as the
  status-line snoop, no extra parse pass.
- **The terminal block** (`http_controller.cpp`, the P1.5 site) builds both the
  `LogRequestOp` and the `UsageEvent` from `ctx->estimated_*` at request
  completion — which runs **after** streaming finishes, so it can use actuals
  captured during the stream.
- **Streaming caveat (central):** OpenAI/vLLM emit `usage` in a *streaming*
  response only when the request sets `stream_options: {include_usage: true}`;
  the usage then rides a final SSE chunk. Non-streaming responses always include
  `usage`. So for streaming requests that didn't ask for usage, there is nothing
  to parse unless Ranvier injects `include_usage`.

## 3. Design

### 3.1 Capture the actuals

- **Non-streaming** (`stream:false`): parse the response body's top-level
  `usage` object (bounded — it's small).
- **Streaming**: snoop the terminal SSE chunk's `usage` in `StreamParser`
  (extend `Result` with `usage_present` + `prompt_tokens` / `completion_tokens`,
  populated when the parser sees a `usage` object in the data it's already
  scanning). The clean data still flows to the client unchanged.

### 3.2 Streaming-usage acquisition — the key decision

To get actuals for streaming requests that didn't request usage, Ranvier must
inject `stream_options.include_usage: true` into the forwarded request body.
Options:

- **(a) Inject + pass through:** client sees one extra, well-formed usage SSE
  chunk. Minimal code; tiny client-visible change.
- **(b) Inject + strip:** Ranvier consumes the usage chunk for accounting and
  removes it before forwarding, so client behaviour is unchanged. Cleanest for
  clients; adds stream-rewriting in `StreamParser`.
- **(c) No inject:** parse usage only when the client already set
  `include_usage`; otherwise fall back to estimates. Zero behaviour change, but
  most streaming requests get no actuals.

Recommendation: gate injection behind a config flag; **Phase 1 ships (c)+(a)**
(parse-when-present, optional inject-and-pass-through) to avoid the strip
complexity, with **(b) as a follow-up** if the extra chunk bothers clients.

### 3.3 Thread to consumers + fall back honestly

- Add `ctx->actual_input_tokens`, `ctx->actual_output_tokens`,
  `ctx->actual_usage_present` to `ProxyContext`, populated during streaming /
  at non-streaming completion.
- In the terminal block: if `actual_usage_present`, use actuals for the
  `LogRequestOp` + `UsageEvent` and **recompute `cost_units` from the actual
  tokens** (reuse the existing cost model); else fall back to `estimated_*`.
- Add a **`bool tokens_estimated`** (or a `usage_source` enum) to `UsageEvent`
  (forward-compat append) and the `request_attribution` row (additive SQLite
  migration), so a billing consumer can tell actual from estimated. This is the
  billing-honesty bit — silently mixing the two is worse than either.

### 3.4 The span (Phase 2)

Adding `gen_ai.usage.output_tokens` needs the `request_span` to live until
completion (it currently ends at dispatch handoff). Options: **(A)** move the
span handle into `ProxyContext` and end it at stream completion (lifecycle
restructure); **(B)** emit a completion-time child span / span event carrying
the output usage; **(C)** leave the span input-exact-only and let output usage
live on the ledger. Recommend Phase 2 = (A) or (B); **Phase 1 leaves the span
untouched.**

## 4. Phasing

- **Phase 1 — ledger/attribution actuals** (billing-critical, tractable): §3.1
  capture, §3.3 threading + estimate fallback + estimated flag + cost recompute.
  No span change. This is the bulk of the value.
- **Phase 2 — span output tokens** (GenAI observability fidelity): §3.4.

## 5. Hard Rules touchpoints

- **#1 / hot path:** usage parsing happens at/after stream end (off the
  TTFT-critical path); `StreamParser` already parses every chunk, so this is an
  incremental snoop, not a new pass.
- **#4 bounded:** cap the usage-parse buffer / only scan the terminal chunk.
- **#9:** a usage parse failure falls back to estimates and logs at warn — never
  fails the request.
- **#7:** the SQLite migration is additive (`CREATE`-only, default for old
  rows); persistence stays dumb (the service layer decides actual-vs-estimate).

## 6. Open questions for the reviewer

1. **Streaming injection:** inject `include_usage`? If so, Phase-1 pass-through
   (a, rec) or strip (b)?
2. **Default posture:** capture actuals on by default (changes recorded values
   to be *correct*) — non-streaming always, streaming behind a flag (rec)? Or
   keep estimates the default and make actuals opt-in?
3. **Estimated/actual flag:** add to `UsageEvent` + the `request_attribution`
   row via additive migration (rec: yes)?
4. **Cost recompute** from actual tokens (rec: yes)?
5. **Span output (Phase 2):** worth the span-lifecycle change, or is ledger-only
   accounting sufficient and the span stays input-exact-only?
6. **Phasing:** ledger-first (Phase 1) then span (Phase 2), as above?

Answer these and I'll turn it into the Phase-1 implementation PR.
