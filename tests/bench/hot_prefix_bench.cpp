// Hot-prefix telemetry hot-path microbenchmark (BACKLOG §21 P1).
//
// Measures the per-prefix-routed-request cost that cache-topology telemetry adds
// when enabled: one StreamSummary::touch() plus, on the ART-hit path, one
// hash_prefix() (the miss path already paid for that hash). Reactor-free and
// Docker-free — the targeted complement to the end-to-end A/B gate, which can't
// resolve a sub-microsecond per-request delta buried under millisecond request
// latency. Build Release for meaningful numbers: `make bench-hot-prefix`.
//
// Not a correctness test (no asserts) — it prints ns/op for a human / CI trend.

#include "parse_utils.hpp"    // hash_prefix (inline FNV-1a)
#include "stream_summary.hpp" // StreamSummary

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace ranvier;

namespace {

using Clock = std::chrono::steady_clock;

// Keep the optimizer honest — measured results feed this volatile sink.
volatile uint64_t g_sink = 0;

// Run `body(i)` for `iters` ops (plus a 10% warmup) and return ns/op.
template <typename F>
double bench(size_t iters, F&& body) {
    const size_t warmup = iters / 10;
    for (size_t i = 0; i < warmup; ++i) body(i);
    auto t0 = Clock::now();
    for (size_t i = 0; i < iters; ++i) body(i);
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count()
         / static_cast<double>(iters);
}

void row(const char* label, double ns_per_op) {
    std::printf("  %-40s %10.2f\n", label, ns_per_op);
}

}  // namespace

int main() {
    constexpr size_t kIters = 5'000'000;

    std::printf("Hot-prefix telemetry hot-path microbenchmark (BACKLOG §21)\n");
    std::printf("Build Release for meaningful numbers.\n\n");
    std::printf("  %-40s %10s\n", "operation", "ns/op");
    std::printf("  ----------------------------------------------------\n");

    // StreamSummary::touch() — hit-heavy: a small hot set, ~all increments (the
    // warm steady state a real workload sits in).
    {
        StreamSummary s;  // capacity = StreamSummary::kDefaultCapacity (128)
        for (uint64_t k = 0; k < 64; ++k) s.touch(k);
        double ns = bench(kIters, [&](size_t i) { s.touch(i & 63u); });
        g_sink += s.total();
        row("StreamSummary::touch (hit-heavy)", ns);
    }

    // StreamSummary::touch() — churn: distinct keys once full, so every op is a
    // miss that evicts (the O(K) min-scan worst case).
    {
        StreamSummary s;
        for (uint64_t k = 0; k < StreamSummary::kDefaultCapacity; ++k) s.touch(k);
        double ns = bench(kIters,
            [&](size_t i) { s.touch(StreamSummary::kDefaultCapacity + i); });
        g_sink += s.total();
        row("StreamSummary::touch (churn/evict)", ns);
    }

    // hash_prefix() over representative routing-prefix lengths (tokens). This is
    // the cost newly paid on the ART-hit path when tracking is on.
    {
        std::mt19937 rng(12345);
        for (size_t prefix_len : {size_t{16}, size_t{64}, size_t{256}, size_t{512}}) {
            std::vector<int32_t> tokens(prefix_len);
            for (auto& t : tokens) t = static_cast<int32_t>(rng() & 0x7fffffff);
            uint64_t acc = 0;
            // Fewer iters — hashing longer prefixes is proportionally pricier.
            double ns = bench(kIters / 10,
                [&](size_t) { acc ^= hash_prefix(tokens.data(), tokens.size(), 1); });
            g_sink += acc;
            char label[48];
            std::snprintf(label, sizeof(label), "hash_prefix (%zu tokens)", prefix_len);
            row(label, ns);
        }
    }

    std::printf("\n  Per-request tax when telemetry ON ~= touch(hit) + hash_prefix(hit path).\n");
    std::printf("  Compare against the ~µs-scale ART lookup + tokenization it sits beside.\n");
    std::printf("  (sink=%llu)\n", static_cast<unsigned long long>(g_sink));
    return 0;
}
