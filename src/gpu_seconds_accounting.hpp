// Ranvier Core - Capacity-Weighted Uptime Accounting (pure)
//
// Pure, reactor-free helpers for the per-backend backend_gpu_seconds_total
// counter: gpu_count integrated over the time a backend has been live. The
// arithmetic lives here, free of Seastar and of any clock, so it is unit-tested
// directly; the router/scrape wiring stays a thin layer that captures
// timestamps and stores the running accumulator.
//
// Units are GPU-seconds (a backend declaring gpu_count=8 that stays live for
// 10s contributes 80). Observe-only — nothing here influences routing.

#pragma once

#include <vector>

namespace ranvier {

// One live segment in a backend's history: it presented `gpu_count` GPUs for
// `seconds` of (steady-clock) live time. Time spent unhealthy/deregistered is
// simply not represented as a segment.
struct GpuSecondsSegment {
    double gpu_count = 0.0;
    double seconds   = 0.0;
};

// Total GPU-seconds contributed across a sequence of live segments: the sum of
// gpu_count x seconds. Non-positive gpu_count or seconds contribute nothing —
// a steady clock never runs backwards, so a negative duration would signal a
// caller bug rather than real elapsed time, and is dropped rather than
// subtracted (the counter must stay monotonic).
inline double gpu_seconds_total(const std::vector<GpuSecondsSegment>& segments) {
    double total = 0.0;
    for (const auto& seg : segments) {
        if (seg.gpu_count > 0.0 && seg.seconds > 0.0) {
            total += seg.gpu_count * seg.seconds;
        }
    }
    return total;
}

// Scrape-time / finalize value: the already-accumulated GPU-seconds plus the
// in-progress segment that has not yet been folded in. The in-progress term is
// counted only while the backend is live; `in_progress_seconds` is the elapsed
// live time since the current segment began.
//
// Pure and read-only by construction: it takes the accumulator by value and
// returns a number, touching no clock and mutating no state. A scrape can call
// it as often as it likes without advancing the stored accumulator (no double
// counting across scrapes). The same call also expresses a transition's
// "finalize the in-progress segment" step — the caller stores the result back
// into the accumulator and resets the segment start.
inline double gpu_seconds_with_in_progress(double accumulated,
                                           bool live,
                                           double in_progress_gpu_count,
                                           double in_progress_seconds) {
    double total = accumulated;
    if (live && in_progress_gpu_count > 0.0 && in_progress_seconds > 0.0) {
        total += in_progress_gpu_count * in_progress_seconds;
    }
    return total;
}

}  // namespace ranvier
