#include "worker_affinity.hpp"

#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/log.hh>

#include <mutex>
#include <vector>

#if defined(__linux__) && defined(__GLIBC__)
#include <pthread.h>
#include <sched.h>
#endif

namespace ranvier::worker_affinity {

namespace {

seastar::logger log_affinity("worker_affinity");

#if defined(__linux__) && defined(__GLIBC__)

// Snapshot of the process-wide allowed cpuset, captured in main() before
// Seastar pins the reactor threads.
cpu_set_t g_process_cpuset;
bool g_snapshot_taken = false;

// Non-reactor cpuset, computed once on first initialize_non_reactor_cpuset()
// call. After init completes, these are read-only and safe to access from
// any shard without synchronisation (the future chain that awaits init
// imposes happens-before on subsequent pin calls).
cpu_set_t g_non_reactor_cpuset;
std::vector<int> g_non_reactor_cores;  // Ordered list of non-reactor CPU ids.
bool g_init_done = false;

// One-shot flag for log spam suppression when pin() is called on a system
// with no non-reactor cores.
std::once_flag g_empty_warn_flag;

// Cap on cpu_set_t scan range. CPU_SETSIZE is typically 1024; the kernel
// ignores bits beyond what it knows about, so scanning the full range is
// safe and avoids needing the host CPU count.
constexpr int kCpuScanMax = CPU_SETSIZE;

#endif

}  // namespace

void snapshot_process_cpuset() {
#if defined(__linux__) && defined(__GLIBC__)
    if (g_snapshot_taken) {
        log_affinity.warn("snapshot_process_cpuset() called more than once — ignoring");
        return;
    }
    CPU_ZERO(&g_process_cpuset);
    const int rc = pthread_getaffinity_np(pthread_self(),
                                          sizeof(g_process_cpuset),
                                          &g_process_cpuset);
    if (rc != 0) {
        // Leave g_snapshot_taken false — initialize_non_reactor_cpuset() will
        // see "snapshot unavailable" and short-circuit to a no-op.
        log_affinity.warn("pthread_getaffinity_np failed in snapshot (errno={}); "
                          "worker affinity pinning will be disabled", rc);
        CPU_ZERO(&g_process_cpuset);
        return;
    }
    g_snapshot_taken = true;
#else
    log_affinity.info("Worker affinity pinning unavailable on this platform "
                      "(not Linux+glibc) — workers will run with inherited "
                      "affinity");
#endif
}

seastar::future<> initialize_non_reactor_cpuset() {
#if defined(__linux__) && defined(__GLIBC__)
    if (g_init_done) {
        return seastar::make_ready_future<>();
    }
    if (!g_snapshot_taken) {
        // No snapshot (e.g. tests that don't call main(), or a failed
        // getaffinity in snapshot_process_cpuset). Mark init done with an
        // empty cpuset so subsequent pin calls short-circuit.
        CPU_ZERO(&g_non_reactor_cpuset);
        g_non_reactor_cores.clear();
        g_init_done = true;
        log_affinity.warn("No process cpuset snapshot available; worker "
                          "affinity pinning is a no-op for this run");
        return seastar::make_ready_future<>();
    }

    // Survey each shard's reactor affinity by submitting a per-shard
    // pthread_self() affinity read. cpu_set_t is trivially copyable, so the
    // per-shard return value is value-copied back to the caller's shard with
    // no foreign_malloc / heap-cross-shard concerns. when_all_succeed gives
    // us a vector of bitmaps to union.
    std::vector<seastar::future<cpu_set_t>> per_shard;
    per_shard.reserve(seastar::smp::count);
    for (unsigned shard = 0; shard < seastar::smp::count; ++shard) {
        per_shard.push_back(seastar::smp::submit_to(shard, []() -> cpu_set_t {
            cpu_set_t local;
            CPU_ZERO(&local);
            const int rc = pthread_getaffinity_np(pthread_self(),
                                                 sizeof(local), &local);
            if (rc != 0) {
                log_affinity.warn(
                    "pthread_getaffinity_np failed on shard {} (errno={}); "
                    "treating reactor cpuset as full to be safe",
                    seastar::this_shard_id(), rc);
                for (int cpu = 0; cpu < kCpuScanMax; ++cpu) {
                    CPU_SET(cpu, &local);
                }
            }
            return local;
        }));
    }
    return seastar::when_all_succeed(per_shard.begin(), per_shard.end())
        .then([](std::vector<cpu_set_t> reactor_sets) {
            cpu_set_t reactor_union;
            CPU_ZERO(&reactor_union);
            for (const auto& s : reactor_sets) {
                for (int cpu = 0; cpu < kCpuScanMax; ++cpu) {
                    if (CPU_ISSET(cpu, &s)) {
                        CPU_SET(cpu, &reactor_union);
                    }
                }
            }

            CPU_ZERO(&g_non_reactor_cpuset);
            g_non_reactor_cores.clear();
            for (int cpu = 0; cpu < kCpuScanMax; ++cpu) {
                if (CPU_ISSET(cpu, &g_process_cpuset) &&
                    !CPU_ISSET(cpu, &reactor_union)) {
                    CPU_SET(cpu, &g_non_reactor_cpuset);
                    g_non_reactor_cores.push_back(cpu);
                }
            }
            g_init_done = true;

            if (g_non_reactor_cores.empty()) {
                log_affinity.warn(
                    "No non-reactor CPUs available — every allowed core is "
                    "a Seastar reactor. Worker threads (persistence, "
                    "tokenizer pool) will run with inherited affinity and "
                    "may compete with reactors. Consider running with "
                    "--smp=N-K to leave headroom.");
            } else {
                log_affinity.info(
                    "Foreign worker affinity initialised: {} non-reactor "
                    "core(s) available for pinning",
                    g_non_reactor_cores.size());
            }
        });
#else
    return seastar::make_ready_future<>();
#endif
}

bool pin_worker_to_non_reactor_core(
    [[maybe_unused]] std::thread::native_handle_type thread_handle,
    [[maybe_unused]] unsigned worker_index,
    [[maybe_unused]] const char* owner_label) {
#if defined(__linux__) && defined(__GLIBC__)
    if (!g_init_done) {
        // Tests that bypass startup will hit this; production callers should
        // have awaited initialize_non_reactor_cpuset() before spawning.
        return false;
    }
    if (g_non_reactor_cores.empty()) {
        std::call_once(g_empty_warn_flag, [owner_label] {
            log_affinity.warn(
                "Foreign worker affinity pin requested by {} but non-reactor "
                "cpuset is empty — leaving workers on inherited affinity "
                "(one-shot warning)",
                owner_label);
        });
        return false;
    }

    const int target_cpu =
        g_non_reactor_cores[worker_index % g_non_reactor_cores.size()];

    cpu_set_t target_set;
    CPU_ZERO(&target_set);
    CPU_SET(target_cpu, &target_set);

    const int rc = pthread_setaffinity_np(thread_handle,
                                          sizeof(target_set),
                                          &target_set);
    if (rc != 0) {
        log_affinity.warn("pthread_setaffinity_np failed for {} worker "
                          "(index={}, target_cpu={}, errno={})",
                          owner_label, worker_index, target_cpu, rc);
        return false;
    }
    log_affinity.info("Pinned {} worker (index={}) to CPU {}",
                      owner_label, worker_index, target_cpu);
    return true;
#else
    return false;
#endif
}

}  // namespace ranvier::worker_affinity
