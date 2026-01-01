# Bug: Persistence state corruption after crash prevents restart

## Summary
After Ranvier crashes with the output_stream assertion failure, the persisted state becomes corrupted, preventing the server from restarting. The server fails during `load_persisted_state()` with a Seastar error about "wrapped_iovecs are re-populated live".

## Relationship to other bugs
This is a secondary issue caused by the primary crash bug documented in `stream-close-assertion-failure.md`. When Ranvier crashes mid-operation, the SQLite database (`/tmp/ranvier.db`) is left in an inconsistent state.

## Environment
- Same as stream-close-assertion-failure.md
- Occurs after crash during high-concurrency stress testing

## Error on restart
```
INFO  2025-12-31 04:37:32,729 [shard 0:main] ranvier - Restoring state from persistence:
INFO  2025-12-31 04:37:32,729 [shard 0:main] ranvier -   Backends: 8
INFO  2025-12-31 04:37:32,729 [shard 0:main] ranvier -   Routes:   50
INFO  2025-12-31 04:37:32,729 [shard 0:main] ranvier -   - Backend 1 -> 172.17.0.1:8000 (weight=100, priority=0)
...
ERROR 2025-12-31 04:37:32,729 [shard 0:main] seastar - wrapped_iovecs are re-populated live, at: 0x6db448 0x6db750 ...
```

The server successfully reads the backend count and route count from the database, but crashes during restoration with a Seastar internal error.

## Stack trace (partial)
```
seastar::continuation<..., load_persisted_state()::{lambda(auto:1&, auto:2&)#1}...>
seastar::internal::do_with_state<std::tuple<std::vector<ranvier::BackendRecord...>, std::vector<ranvier::RouteRecord...>>>
```

The crash occurs in the `load_persisted_state()` function while processing the restored backend and route records.

## Workaround
Delete the container to clear the persisted state:
```bash
docker compose -f docker-compose.benchmark-real.yml down ranvier1
docker rm ranvier-bench1
docker compose -f docker-compose.benchmark-real.yml up -d ranvier1
```

Or force recreate:
```bash
docker compose -f docker-compose.benchmark-real.yml up -d --force-recreate ranvier1
```

## Root causes to investigate

### 1. SQLite transaction handling during crash
- Is the database left in an inconsistent state after SIGABRT?
- Are transactions properly used for atomic updates?
- Consider using WAL mode for better crash recovery

### 2. Data validation on restore
- The restored data (8 backends, 50 routes) appears valid in logs
- Crash happens during processing, not reading
- May be corrupted route data that passes initial validation

### 3. Memory/buffer state during restoration
- "wrapped_iovecs are re-populated live" suggests buffer reuse issue
- May be related to async I/O operations during restore
- Check if file handles are properly managed

## Recommendations

### Short-term
1. Add integrity checks before restoration
2. Implement graceful fallback to fresh state on corruption
3. Log more details about what's being restored when crash occurs

### Long-term
1. Use SQLite WAL mode for crash resilience
2. Add checksum/versioning to persisted state
3. Implement state recovery/repair tooling
4. Consider separating backend config from route cache (backends are critical, routes can be relearned)

## Related files
- `src/persistence.cc` - State persistence logic
- `src/main.cc` - Startup and state restoration
- Database: `/tmp/ranvier.db` (SQLite)
