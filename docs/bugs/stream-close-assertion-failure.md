# Bug: Seastar output_stream assertion failure under load

## Summary
Ranvier crashes with assertion failure in Seastar's `output_stream` destructor during high-concurrency stress testing with large prefixes.

## Environment
- 8x A100 40GB GPUs
- 8 vLLM backends (Llama-3.1-8B-Instruct)
- 3 Ranvier nodes (--smp 2)
- 10 concurrent Locust users
- Large prefix stress test (2000-8000 tokens)

## Error
```
/usr/local/include/seastar/core/iostream.hh:500: seastar::output_stream<CharType>::~output_stream() [with CharType = char]: Assertion `!_in_batch && "Was this stream properly closed?"` failed.
terminate called without an active exception
Aborting on shard 0, in scheduling group main.
```

Exit code: 139 (SIGABRT)

## Last successful log entries before crash
```
INFO  2025-12-31 04:09:12,662 [shard 0:main] ranvier.proxy - [req-0-000004eaf84d-000045] Request received from direct (25469 bytes)
INFO  2025-12-31 04:09:12,670 [shard 0:main] ranvier.proxy - [req-0-000004eaf84d-000045] Routing to backend 1 at 172.17.0.1:8000
```

The crash occurred after routing was determined but before connection was established or logged.

## Likely cause
The `output_stream` is being destroyed (possibly due to an exception or early return) without being properly closed/flushed first. This can happen when:

1. An exception is thrown during request processing and the stream cleanup path doesn't properly close the stream
2. A timeout or cancellation destroys the stream while it's still in batch mode
3. Backend connection failure triggers cleanup that doesn't await stream close

## Reproduction steps
1. Start 8 vLLM backends with Llama-3.1-8B-Instruct
2. Start 3 Ranvier nodes with `--smp 2`
3. Run large-prefix stress test:
   ```bash
   BENCHMARK_MODE=round_robin \
   PROMPT_DISTRIBUTION=large-prefix \
   LARGE_PREFIX_MIN_TOKENS=2000 \
   LARGE_PREFIX_MAX_TOKENS=8000 \
   locust -f tests/integration/locustfile_real.py \
     --headless --users 10 --spawn-rate 2 --run-time 5m
   ```
4. Crash occurs within a few minutes

## Investigation areas

### 1. Check proxy.cc stream handling
Look for code paths where `output_stream` might be destroyed without `close().get()`:
- Exception handlers
- Early returns
- Timeout/cancellation paths

### 2. Check connection error handling
The crash happened during connection establishment. Look at:
- `Connection established to backend` code path
- What happens if connection fails after stream is created

### 3. Add RAII wrapper or scope guard
Ensure streams are always closed, even on exception:
```cpp
auto close_stream = defer([&stream] {
    stream.close().get();
});
```

## Workaround
Reduce concurrent users (--users 5 instead of 10) to lower the probability of triggering the race condition.

## Related files
- `src/proxy.cc` - Main proxy request handling
- `src/backend_connection.cc` - Backend connection management
