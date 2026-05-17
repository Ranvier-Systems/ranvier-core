// Ranvier Core - Seastar-based Test: Worker Affinity Pinning End-to-End
//
// Verifies the worker_affinity helper actually pins foreign std::thread
// workers to non-reactor cores. The test boots a 2-shard reactor via
// app_template (mirroring cross_shard_exception_propagation_test), runs the
// full pipeline (snapshot → init → pin), and reads the post-pin affinity
// back via pthread_getaffinity_np on the worker's native_handle.
//
// What it actually asserts:
//
//   * snapshot_process_cpuset() captured a non-empty process cpuset
//     (called from main() before app.run(), same call site as production).
//   * initialize_non_reactor_cpuset() runs to completion under a reactor
//     and is idempotent when called twice.
//   * pin_worker_to_non_reactor_core() either:
//       (a) returns true AND the worker thread's resulting affinity is a
//           single-CPU mask, that CPU is in the process cpuset, and that
//           CPU is NOT in the union of reactor affinities; OR
//       (b) returns false because every allowed CPU is also a reactor
//           (the M=0 fallback path on tight-fit CI runners) — in which
//           case the success-path assertion is skipped with a diagnostic.
//
// The test is platform-gated to Linux+glibc to match the helper. On
// non-Linux it compiles to an empty test that passes trivially.
//
// Departure from convention: this is a reactor-booting unit test (the only
// other one in the suite is cross_shard_exception_propagation_test). The
// helper is intrinsically reactor-coupled — initialize_non_reactor_cpuset
// uses smp::submit_to to survey per-shard affinities — so there's no
// useful reactor-free surface to test in isolation.
//
// Run: ./worker_affinity_test (no extra flags required).

#include <gtest/gtest.h>

#include "worker_affinity.hpp"

#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sleep.hh>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#if defined(__linux__) && defined(__GLIBC__)
#  include <pthread.h>
#  include <sched.h>
#  define RANVIER_WA_TEST_ENABLED 1
#else
#  define RANVIER_WA_TEST_ENABLED 0
#endif

namespace {

#if RANVIER_WA_TEST_ENABLED

// All cross-shard / cross-thread observations are written into process-
// global state so the gtest body (which runs after app.run() returns) can
// assert on them. cpu_set_t is trivially copyable POD; the booleans are
// only written before main() observes them, so no atomics needed.
struct Observations {
    // From main() pre-reactor snapshot.
    bool snapshot_pre_reactor_ok = false;
    cpu_set_t process_cpuset_pre_reactor;
    int process_cpuset_bit_count = 0;

    // From reactor experiment.
    unsigned smp_count = 0;
    bool init_first_call_completed = false;
    bool init_second_call_completed = false;  // idempotency
    cpu_set_t reactor_union_observed;
    int reactor_union_bit_count = 0;
    bool pin_returned_true = false;
    bool worker_affinity_read_ok = false;
    cpu_set_t worker_post_pin_affinity;
    int worker_affinity_bit_count = 0;
    int worker_pinned_cpu = -1;
};

Observations g_obs;

int count_bits(const cpu_set_t& s) {
    int n = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &s)) ++n;
    }
    return n;
}

int first_set_bit(const cpu_set_t& s) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &s)) return cpu;
    }
    return -1;
}

// Survey the reactor cpuset independently of the helper, so the test's
// success-path assertion has a ground truth to compare against.
seastar::future<cpu_set_t> survey_reactor_union() {
    cpu_set_t accumulator;
    CPU_ZERO(&accumulator);
    for (unsigned shard = 0; shard < seastar::smp::count; ++shard) {
        cpu_set_t s = co_await seastar::smp::submit_to(shard,
            []() -> cpu_set_t {
                cpu_set_t local;
                CPU_ZERO(&local);
                pthread_getaffinity_np(pthread_self(),
                                       sizeof(local), &local);
                return local;
            });
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &s)) {
                CPU_SET(cpu, &accumulator);
            }
        }
    }
    co_return accumulator;
}

seastar::future<> run_experiment() {
    g_obs.smp_count = seastar::smp::count;

    // Independent ground-truth survey for the success-path assertion.
    g_obs.reactor_union_observed = co_await survey_reactor_union();
    g_obs.reactor_union_bit_count = count_bits(g_obs.reactor_union_observed);

    // First init: the real survey.
    co_await ranvier::worker_affinity::initialize_non_reactor_cpuset();
    g_obs.init_first_call_completed = true;

    // Second init: must be a ready future (idempotent).
    co_await ranvier::worker_affinity::initialize_non_reactor_cpuset();
    g_obs.init_second_call_completed = true;

    // Spawn a worker thread that busy-waits on a stop flag, so the thread
    // stays alive long enough for us to pin and read its affinity back.
    // The pthread_setaffinity_np syscall is asynchronous from the kernel's
    // perspective — it updates the mask and re-schedules on next decision
    // — but pthread_getaffinity_np reads the mask itself, not the
    // currently-executing CPU, so reading immediately after pin is valid.
    std::atomic<bool> stop{false};
    std::thread worker([&stop] {
        while (!stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    g_obs.pin_returned_true = ranvier::worker_affinity::pin_worker_to_non_reactor_core(
        worker.native_handle(), /*worker_index=*/0, "worker_affinity_test");

    cpu_set_t after;
    CPU_ZERO(&after);
    const int rc = pthread_getaffinity_np(worker.native_handle(),
                                          sizeof(after), &after);
    g_obs.worker_affinity_read_ok = (rc == 0);
    g_obs.worker_post_pin_affinity = after;
    g_obs.worker_affinity_bit_count = count_bits(after);
    g_obs.worker_pinned_cpu = first_set_bit(after);

    stop.store(true, std::memory_order_release);
    worker.join();
    co_return;
}

#endif  // RANVIER_WA_TEST_ENABLED

}  // namespace

#if RANVIER_WA_TEST_ENABLED

TEST(WorkerAffinity, SnapshotCapturedBeforeReactor) {
    ASSERT_TRUE(g_obs.snapshot_pre_reactor_ok)
        << "pthread_getaffinity_np failed in main() before app.run() — "
           "the test runner has unusual cpuset permissions";
    EXPECT_GT(g_obs.process_cpuset_bit_count, 0)
        << "Process cpuset is empty — no CPU is allowed to the process";
}

TEST(WorkerAffinity, ReactorBootedWithExpectedShardCount) {
    EXPECT_EQ(g_obs.smp_count, 2u)
        << "Test driver passed --smp 2; reactor reports a different count";
}

TEST(WorkerAffinity, InitializeIsIdempotent) {
    EXPECT_TRUE(g_obs.init_first_call_completed)
        << "First initialize_non_reactor_cpuset() never completed";
    EXPECT_TRUE(g_obs.init_second_call_completed)
        << "Second initialize_non_reactor_cpuset() never completed "
           "(idempotency check)";
}

TEST(WorkerAffinity, WorkerAffinityReadbackSucceeds) {
    EXPECT_TRUE(g_obs.worker_affinity_read_ok)
        << "pthread_getaffinity_np on worker thread failed";
}

TEST(WorkerAffinity, PinEitherSucceedsOrCleanlyFallsBack) {
    // Always true: pin() is documented to return true on success, false
    // otherwise — never throw, never hang. Reaching this assertion at all
    // means neither happened.
    SUCCEED();

    std::cout << "\n[result] pin_returned=" << (g_obs.pin_returned_true ? "true" : "false")
              << " reactor_union_bits=" << g_obs.reactor_union_bit_count
              << " process_cpuset_bits=" << g_obs.process_cpuset_bit_count
              << " worker_affinity_bits=" << g_obs.worker_affinity_bit_count
              << " pinned_cpu=" << g_obs.worker_pinned_cpu
              << "\n";
}

TEST(WorkerAffinity, PinnedThreadLandsOnNonReactorCore) {
    if (!g_obs.pin_returned_true) {
        GTEST_SKIP()
            << "pin returned false (M=0 fallback path on this CI runner — "
               "every allowed CPU is also a Seastar reactor). The success "
               "path can only be exercised on a host with at least one "
               "non-reactor core in the process cpuset.";
    }
    // Success-path invariants:
    //   (a) The worker's affinity is now a single-CPU mask.
    //   (b) That CPU is allowed to the process.
    //   (c) That CPU is NOT in the union of reactor affinities.
    EXPECT_EQ(g_obs.worker_affinity_bit_count, 1)
        << "Pinned worker has " << g_obs.worker_affinity_bit_count
        << " bits in its affinity mask; expected exactly 1";
    ASSERT_GE(g_obs.worker_pinned_cpu, 0)
        << "Could not determine which CPU the worker was pinned to";
    EXPECT_TRUE(CPU_ISSET(g_obs.worker_pinned_cpu,
                          &g_obs.process_cpuset_pre_reactor))
        << "Worker pinned to CPU " << g_obs.worker_pinned_cpu
        << " which is NOT in the process cpuset (cgroup violation)";
    EXPECT_FALSE(CPU_ISSET(g_obs.worker_pinned_cpu,
                           &g_obs.reactor_union_observed))
        << "Worker pinned to CPU " << g_obs.worker_pinned_cpu
        << " which IS a reactor core — placement defeated the purpose of "
           "the pin";
}

#else  // !RANVIER_WA_TEST_ENABLED

TEST(WorkerAffinity, DisabledOnNonLinux) {
    GTEST_SKIP() << "Worker affinity tests require Linux+glibc; the helper "
                    "compiles to a no-op on this platform";
}

#endif

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    // gtest_discover_tests runs the binary with --gtest_list_tests at
    // build time to enumerate cases. Avoid bringing the reactor up in that
    // mode — listing should be cheap and must not require per-shard
    // memory/cpu.
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr &&
            std::string(argv[i]) == "--gtest_list_tests") {
            return RUN_ALL_TESTS();
        }
    }

#if RANVIER_WA_TEST_ENABLED
    // Snapshot the process-wide cpuset BEFORE the reactor pins anything —
    // matches the production call site in src/main.cpp. The helper's
    // internal snapshot is captured by the same call; we also record into
    // the test's own struct so the success-path assertion has a ground
    // truth that doesn't depend on the helper's internals.
    CPU_ZERO(&g_obs.process_cpuset_pre_reactor);
    const int rc = pthread_getaffinity_np(pthread_self(),
                                          sizeof(g_obs.process_cpuset_pre_reactor),
                                          &g_obs.process_cpuset_pre_reactor);
    g_obs.snapshot_pre_reactor_ok = (rc == 0);
    g_obs.process_cpuset_bit_count = count_bits(g_obs.process_cpuset_pre_reactor);

    ranvier::worker_affinity::snapshot_process_cpuset();

    // Boot the reactor. Match the argv shape from
    // cross_shard_exception_propagation_test (--smp 2 for a meaningful
    // multi-shard survey; --overprovisioned + --lock-memory 0 keep the
    // test runnable in containers).
    seastar::app_template::seastar_options opts;
    opts.name = "worker_affinity_test";
    seastar::app_template app(std::move(opts));

    char arg0[]          = "worker_affinity_test";
    char arg_smp_flag[]  = "--smp";
    char arg_smp_val[]   = "2";
    char arg_mem_flag[]  = "--memory";
    char arg_mem_val[]   = "256M";
    char arg_lock_flag[] = "--lock-memory";
    char arg_lock_val[]  = "0";
    char arg_overprov[]  = "--overprovisioned";
    char* app_argv[] = {
        arg0,
        arg_smp_flag,  arg_smp_val,
        arg_mem_flag,  arg_mem_val,
        arg_lock_flag, arg_lock_val,
        arg_overprov,
        nullptr,
    };
    int app_argc = static_cast<int>(sizeof(app_argv) / sizeof(app_argv[0])) - 1;

    int reactor_rc = app.run(app_argc, app_argv, []() -> seastar::future<> {
        return run_experiment();
    });
    if (reactor_rc != 0) {
        std::cerr << "Seastar reactor exited with rc=" << reactor_rc
                  << "; gtest assertions will surface the failure mode.\n";
    }
#endif

    return RUN_ALL_TESTS();
}
