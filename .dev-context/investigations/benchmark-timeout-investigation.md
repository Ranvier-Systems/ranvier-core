# Investigation: High Incomplete (Timeout) Rate in Benchmarks

**Date:** 2026-02-08
**Issue:** 30-37% incomplete request rates in benchmark runs, constant regardless of duration

## Data Points

| Model | Users | Duration | Incomplete (RR) | Incomplete (Prefix) |
|-------|-------|----------|------------------|---------------------|
| 8B    | 30    | 10m      | 36.4%            | 33.1%               |
| 8B    | 30    | 30m      | 36.8%            | 29.9%               |
| 8B    | 20    | 10m      | 37.0%            | 32.4%               |
| 13B   | 30    | 30m      | 39.7%            | 34.4%               |

Key observation: Rate is **constant** across 10m and 30m runs, confirming these are
**mid-run timeouts**, not end-of-test in-flight draining artifacts.

---

## 1. What Counts as "Incomplete"?

**Source:** `locustfile_real.py:2317-2363`

There are three categories of request outcomes:

| Category | Condition | Meaning |
|----------|-----------|---------|
| **Successful** | `ttft_ms is not None` | Got HTTP 200, parsed first SSE token, TTFT recorded |
| **Incomplete** | `ttft_ms is None AND got_http_ok = True` | Got HTTP 200, but stream terminated before any `data:` line was parsed |
| **Failed** | `ttft_ms is None AND got_http_ok = False` | HTTP error, ConnectTimeout, ReadTimeout, or other exception |

### How "Incomplete" Happens

A request is classified as **incomplete** when:

1. The HTTP connection succeeds (status 200) — `got_http_ok = True` is set at line 3525
2. But the streaming loop (`for line in resp.iter_lines()`) exits **before** a `data:` SSE line is received — so `ttft` stays `None`
3. When `record_request()` is called with `ttft_ms=None` and `got_http_ok=True`, it increments `incomplete_requests`

### The Three Timeout Mechanisms

| Timeout | Default | Scope | What it guards |
|---------|---------|-------|----------------|
| `CONNECT_TIMEOUT_SECONDS` | **30s** | TCP connection + headers | Time to establish connection to Ranvier and receive HTTP response headers |
| `READ_TIMEOUT_SECONDS` | **120s** | Per-socket-read | Max wait for each chunk from `iter_lines()`. If no data for 120s, `ReadTimeout` fires |
| `STREAMING_TIMEOUT_SECONDS` | **300s** (5 min) | Overall response | Total wall-clock time for the entire streaming response. Checked inside the `iter_lines()` loop |

### Why Incomplete is NOT the Same as Streaming Timeout

Important distinction: A **streaming timeout** (STREAMING_TIMEOUT) fires an `exception` event and does NOT set `got_http_ok` — the request is recorded before the timeout check at line 3563 as a failed request (metrics are recorded with `got_http_ok=True` but `ttft_ms=None`).

Wait — actually, looking more carefully at the code flow:

1. Line 3525: `metrics.got_http_ok = True` — set immediately after HTTP 200 check
2. Line 3528-3538: Streaming timeout check is **inside** the `iter_lines()` loop
3. Line 3554-3564: If `stream_timed_out`, the request fires an error event and calls `record_request(metrics)` — at this point `got_http_ok=True` but `ttft_ms=None`, so it counts as **incomplete**

So streaming timeouts DO count as incomplete (not failed), because `got_http_ok` was already set to `True` before the timeout check.

However, `ReadTimeout` exceptions (line 3643) do NOT set `got_http_ok=True` because the exception may fire during the initial response or during streaming. If it fires before line 3525 (before getting HTTP 200 headers), `got_http_ok` stays `False` → counted as **failed**. If `ReadTimeout` fires during `iter_lines()` (after line 3525), `got_http_ok` would already be `True` → counted as **incomplete**.

### Root Cause Analysis: What's Actually Triggering the Incompletes?

Given the 30-37% rate is constant regardless of run duration, the incompletes are NOT
end-of-test artifacts. They're happening mid-run. The most likely triggers:

#### Hypothesis A: `READ_TIMEOUT` (120s) During Prefill

With stress distribution (70% large/xlarge prompts = 2K-8K token prefixes):
- Prefill of 8K tokens on 8B model: ~1-5s on A100 (well under 120s)
- But with 30 users across 8 GPUs (~3.75 concurrent per GPU), vLLM queues requests
- If a request is queued behind 3-4 other large prompts, the total wait could approach
  the `READ_TIMEOUT` of 120s — but this seems unlikely to hit 30% of the time

#### Hypothesis B: vLLM Queuing and Back-Pressure

At 30 users, 8 GPUs: requests queue in vLLM's scheduler. When many large prompts are
in-flight, vLLM may:
- Delay responding to new requests (waiting for KV cache space)
- Not send any SSE data until prefill completes
- If multiple requests are queued, the first SSE `data:` line may take 30-60s

The `iter_lines()` call blocks waiting for vLLM's first data chunk. During this wait:
- If `READ_TIMEOUT` (120s) is hit → `ReadTimeout` exception → **could be incomplete or failed depending on timing**
- If `STREAMING_TIMEOUT` (300s) is hit → stream break → **incomplete**

#### Hypothesis C: `iter_lines()` Socket Interrupted by Locust Stop

When Locust's `--run-time` expires and `--stop-timeout` (default 90s) elapses, Locust
forcibly interrupts users. If a user is blocked in `iter_lines()`:
- The socket is closed
- `iter_lines()` returns empty (loop ends)
- `ttft` is still `None` → **incomplete**

BUT: This can't explain a 30% constant rate since it would only affect requests
in-flight at the moment `--run-time` expires. At most, this is `users * 1` requests
out of potentially thousands.

#### Hypothesis D: vLLM Drops Requests Under Load (Most Likely)

With `max_tokens=100` and stress prompts (4K-8K prefix tokens), each request needs:
- 4K-8K tokens of KV cache for prefill
- 100 tokens for generation
- Total: ~4.1K-8.1K tokens per request

With 3.75 concurrent requests per GPU, each GPU needs 15K-30K tokens of KV cache
simultaneously. If vLLM's `--gpu-memory-utilization 0.85` can't support this many
concurrent sequences, it will:

1. **Queue aggressively** — delaying first token delivery
2. **Preempt sequences** — evicting KV cache to make room, causing re-computation
3. **Drop to continuous batching** — serializing requests, massively increasing latency

When a request is queued long enough that no SSE data arrives before the user fires
another request (or Locust cycles the user), the connection may be interrupted.

**Actually, the most concrete mechanism is this:** Locust users have a `wait_time` (line to be verified). When the wait time fires, Locust expects the user to make a new request. If the previous request's `iter_lines()` is still blocking, the behavior depends on the implementation — but Locust's user model means that slow requests can lead to socket interruptions.

---

## 2. Is the Timeout Threshold Appropriate?

### Current Values vs Expected Request Duration

For stress distribution with 8B model on A100:

| Phase | Small (100 tok) | Large (2K-4K tok) | XLarge (4K-8K tok) |
|-------|-----------------|--------------------|--------------------|
| Prefill (no queue) | <100ms | 0.5-2s | 1-5s |
| Generation (100 tok) | 2-5s | 2-5s | 2-5s |
| **Total (no queue)** | **2-5s** | **2.5-7s** | **3-10s** |
| With 3-4 queued ahead | +6-40s | +6-40s | +6-40s |
| **Total (queued)** | **8-45s** | **8-47s** | **9-50s** |

The timeouts are:
- `CONNECT_TIMEOUT` (30s): **Appropriate** — connection to Ranvier should be fast
- `READ_TIMEOUT` (120s): **Generous** — 2 minutes per chunk, even deeply queued requests should deliver within this
- `STREAMING_TIMEOUT` (300s): **Very generous** — 5 minutes total, far exceeds expected response times

**Conclusion: The timeouts are NOT the bottleneck.** The defaults are generous enough
that timeouts alone shouldn't cause 30% incomplete rates. The incompletes are likely
caused by something other than explicit timeout triggers.

---

## 3. Is 30 Users Too Many?

### Load Analysis

| Metric | Value |
|--------|-------|
| Users | 30 |
| GPUs | 8 |
| Users per GPU | 3.75 |
| Avg prompt size (stress dist) | ~4K-6K tokens |
| KV cache per concurrent request | ~4K-8K tokens |
| Total concurrent KV cache per GPU | ~15K-30K tokens |
| `max_tokens` per request | 100 |

With vLLM's `--gpu-memory-utilization 0.85` on A100 (80GB → 68GB usable):
- Model weights (8B): ~16GB
- Available for KV cache: ~52GB
- KV cache capacity depends on model's head dim, layers, etc.
- For Llama 3.1 8B: ~50K-100K tokens of KV cache in 52GB
- 3.75 concurrent at 8K tokens = ~30K tokens → well within capacity

**So memory isn't the constraint.** The constraint is likely **vLLM's scheduler
throughput** — with 3.75 concurrent large prompts, the GPU is compute-bound during
prefill, and requests get queued.

### Recommendation

20 users (2.5/GPU) shows a similar incomplete rate (37%), suggesting even 2.5
concurrent users per GPU leads to significant queuing with 4K-8K prefix prompts.

The 13B model at 30 users (39.7% incomplete) is worse than 8B (36.8%), which makes
sense since 13B is slower for prefill.

---

## 4. What Can We Tune?

### A. Reduce `max_tokens` (Quick Win)

**Current:** `max_tokens = 100` (hardcoded in locustfile_real.py:3444, 3460)

Each request generates up to 100 output tokens. This keeps the GPU busy for 2-5s per
request during generation, on top of prefill. Reducing to 50 or even 20 would:
- Reduce per-request duration by 50-80%
- Free up GPU faster for queued requests
- Directly reduce queuing depth

**Recommendation:** Make `max_tokens` configurable via environment variable (e.g.,
`MAX_OUTPUT_TOKENS`, default 100). For stress testing focused on TTFT, `max_tokens=20`
would be sufficient since TTFT is measured before generation.

### B. Increase `--stop-timeout` (Partial)

**Current default:** 90s (bench.sh:50)

This won't help mid-run incompletes, only end-of-test draining. Since the rate is
constant regardless of duration, this isn't the primary issue.

### C. Add Environment Variable Overrides for Timeouts in bench.sh

**Current:** bench.sh does NOT pass `STREAMING_TIMEOUT_SECONDS`, `CONNECT_TIMEOUT_SECONDS`,
or `READ_TIMEOUT_SECONDS` to the locust container. They use defaults.

**Recommendation:** Add `--streaming-timeout`, `--connect-timeout`, and `--read-timeout`
flags to bench.sh, passed as `-e` env vars to the docker compose run command.

### D. Reduce Prompt Size (Address Root Cause)

The stress distribution is intentionally aggressive (70% large/xlarge = 2K-8K tokens).
For benchmarking KV cache benefits, this is necessary. But for baseline measurements:

- `--prompt-dist mixed` uses 30% short, 50% medium, 20% long (much lighter)
- Or reduce `LARGE_PREFIX_MAX_TOKENS` from 8000 to 4000

### E. Reduce Users (Address Root Cause)

At 30 users / 8 GPUs, the system is heavily loaded. Recommendations:
- **8B model:** 10-20 users (1.25-2.5/GPU) for cleaner measurements
- **13B model:** 8-15 users (1-1.875/GPU)
- Monitor incomplete rate and find the sweet spot

### F. Investigate vLLM Queuing Behavior

Add logging or metrics to understand:
- vLLM queue depth over time (`/metrics` endpoint has `vllm:num_requests_waiting`)
- Whether vLLM is preempting (evicting KV cache) — check `vllm:num_preemptions_total`
- If `--max-num-seqs` in vLLM limits concurrency (default is 256, but effective
  concurrency depends on memory)

### G. The Real Fix: Understand What "Incomplete" Actually Is

The most important finding is that the **definition of incomplete may be masking
different failure modes**. An incomplete request is any request where:
1. HTTP 200 was received
2. But `ttft` was never recorded (no `data:` SSE line parsed)

This could be caused by:
- **Streaming timeout** (5 min overall)
- **Read timeout** during streaming (120s per chunk)
- **Socket interruption** (Locust stopping the user)
- **vLLM sending empty response** (200 OK but no SSE data)
- **Connection reset** during streaming

**Recommendation:** Add sub-categorization of incomplete requests:
- `incomplete_streaming_timeout` — explicit STREAMING_TIMEOUT hit
- `incomplete_read_timeout` — ReadTimeout during iter_lines() after HTTP 200
- `incomplete_no_data` — iter_lines() loop ended without any data lines
- `incomplete_connection_reset` — socket error during streaming

This would immediately clarify whether the 30% is dominated by one specific mechanism.

---

## 5. Summary of Recommendations (Priority Order)

| Priority | Action | Impact | Effort |
|----------|--------|--------|--------|
| **1** | Sub-categorize incomplete reasons (add logging) | Diagnosis | Low |
| **2** | Make `max_tokens` configurable via env var | Reduce GPU time per request | Low |
| **3** | Add timeout flags to bench.sh | User convenience | Low |
| **4** | Default to fewer users for stress dist | Reduce overload | Low |
| **5** | Log vLLM queue metrics during benchmark | Diagnosis | Medium |
| **6** | Reduce default `LARGE_PREFIX_MAX_TOKENS` | Reduce load | Low |

### Quick Experiment

To test whether GPU saturation is the root cause, run:
```bash
# Reduce users to 10 (1.25/GPU) and see if incomplete rate drops
./scripts/bench.sh --duration 10m --users 10 --prompt-dist stress

# Or keep users but reduce prompt size
./scripts/bench.sh --duration 10m --users 30 --prompt-dist mixed
```

If incomplete rate drops significantly with fewer users or lighter prompts, the root
cause is confirmed as GPU saturation / vLLM queuing under stress load.
