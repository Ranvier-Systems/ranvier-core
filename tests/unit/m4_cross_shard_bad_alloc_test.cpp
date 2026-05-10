// Ranvier Core - M4 Cross-Shard `std::bad_alloc` Propagation Diagnostic
//
// Backlog §18 / docs/audits/request-lifecycle-crash-audit.md M4
// (Phase 2: Tokenization). The audit could not decide statically whether a
// `std::bad_alloc` thrown inside the lambda passed to `seastar::smp::submit_to`
// at `src/tokenizer_service.cpp:256` is propagated cleanly to the outer
// `try/catch` at `src/http_controller.cpp:1373` or repacked into a different
// exception type (e.g. `seastar::broken_promise`) by the cross-shard
// future-marshalling machinery.
//
// This test reproduces the cross-shard call shape with a lambda returning the
// same `std::pair<std::vector<int32_t>, bool>` type, makes that lambda throw
// `std::bad_alloc` directly (the simplest seam — a real OOM is not required
// to characterise Seastar's exception transport), and prints which catch
// branch at the outer site fires. The assertion always passes once the
// reactor preconditions hold; the diagnostic line is the artefact the
// developer needs to drive the audit's decision rule:
//
//   * "PROPAGATED CLEANLY as bad_alloc" → mark M4 MITIGATED in the audit doc
//     and flip the BACKLOG entry, mirroring the M10 closure addendum.
//   * "REPACKAGED as <type>"            → add a `try/catch` inside the
//     submitted lambda that returns `{{}, false}`, per the audit's fix.
//
// Departure from convention: every other Seastar-linked unit test in this
// directory keeps the reactor unbooted (see the header comment in
// `tokenizer_service_test.cpp`). This test cannot — `smp::submit_to` is
// the unit under observation and only exists once the reactor is up with
// `smp::count >= 2`. The reactor is therefore booted from `main()` via
// `seastar::app_template::run` with a fixed minimal argv (`--smp 2`,
// `--memory 256M`, `--overprovisioned`); the gtest body then asserts on
// observations captured into static globals.
//
// Run: `./m4_cross_shard_bad_alloc_test` (no extra flags required).
//      The diagnostic line is printed to stdout by the test body.

#include <gtest/gtest.h>

#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#if __has_include(<cxxabi.h>)
#  include <cxxabi.h>
#  define RANVIER_HAVE_CXXABI 1
#else
#  define RANVIER_HAVE_CXXABI 0
#endif

namespace {

// All cross-shard observations are written into process-global state so the
// gtest body (which runs after `app.run` returns) can assert on them. The
// `std::atomic<bool>` flags below are written from the target shard; the
// `std::string` fields are only written from the initiating shard inside the
// `then_wrapped` continuation, so per-shard allocator ownership is not
// crossed.
struct OuterCatchObservation {
    std::atomic<bool> reactor_started{false};
    std::atomic<bool> reactor_had_two_shards{false};
    std::atomic<bool> lambda_executed{false};
    bool any_exception_caught = false;
    bool caught_as_bad_alloc = false;          // matched catch(const std::bad_alloc&)
    bool caught_as_std_exception_only = false; // matched catch(const std::exception&), not bad_alloc
    bool caught_via_ellipsis = false;          // matched catch(...) only
    std::string typeid_mangled;
    std::string typeid_demangled;
    std::string what_message;
};

OuterCatchObservation g_obs;

std::string demangle(const char* mangled) {
#if RANVIER_HAVE_CXXABI
    int status = 0;
    char* out = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (status == 0 && out != nullptr) {
        std::string s(out);
        std::free(out);
        return s;
    }
#endif
    return mangled ? std::string(mangled) : std::string{};
}

seastar::future<> run_experiment() {
    g_obs.reactor_started.store(true, std::memory_order_release);

    if (seastar::smp::count < 2) {
        // Without a second shard there is no cross-shard `submit_to` to
        // observe; let the gtest assertion surface the misconfiguration.
        return seastar::make_ready_future<>();
    }
    g_obs.reactor_had_two_shards.store(true, std::memory_order_release);

    const unsigned target_shard =
        (seastar::this_shard_id() + 1) % seastar::smp::count;

    // Mirrors `src/tokenizer_service.cpp:256` — same return type as the
    // tokenizer cross-shard lambda. The body throws `std::bad_alloc` before
    // any in-lambda try/catch can intercept it, which is the exact M4
    // failure mode (the audit notes the lambda has no `try/catch` around
    // its allocations at lines 264, 280, 290).
    auto fut = seastar::smp::submit_to(
        target_shard,
        []() -> std::pair<std::vector<int32_t>, bool> {
            g_obs.lambda_executed.store(true, std::memory_order_release);
            throw std::bad_alloc{};
        });

    return std::move(fut).then_wrapped(
        [](seastar::future<std::pair<std::vector<int32_t>, bool>> f) {
            // Use `failed()` + `get_exception()` rather than `get()`/`get0()`
            // so the test compiles against any reasonably recent Seastar.
            //
            // The rethrow-and-catch shape below mirrors
            // `src/http_controller.cpp:1373-1387`, with one addition: an
            // explicit `catch(const std::bad_alloc&)` ahead of the generic
            // `std::exception` branch so we can distinguish "propagated
            // cleanly" from "repackaged as another std::exception subclass"
            // (e.g. `seastar::broken_promise`, which the production handler
            // would still catch but for the wrong reason — masking the OOM).
            if (!f.failed()) {
                f.ignore_ready_future();
                return;
            }
            std::exception_ptr ep = f.get_exception();
            try {
                std::rethrow_exception(ep);
            } catch (const std::bad_alloc& e) {
                g_obs.any_exception_caught = true;
                g_obs.caught_as_bad_alloc = true;
                g_obs.typeid_mangled = typeid(e).name();
                g_obs.typeid_demangled = demangle(typeid(e).name());
                g_obs.what_message = e.what();
            } catch (const std::exception& e) {
                g_obs.any_exception_caught = true;
                g_obs.caught_as_std_exception_only = true;
                g_obs.typeid_mangled = typeid(e).name();
                g_obs.typeid_demangled = demangle(typeid(e).name());
                g_obs.what_message = e.what();
            } catch (...) {
                g_obs.any_exception_caught = true;
                g_obs.caught_via_ellipsis = true;
                g_obs.typeid_mangled = "<non-std::exception>";
            }
        });
}

}  // namespace

TEST(M4CrossShardBadAlloc, OuterCatchExceptionTypeDiagnostic) {
    ASSERT_TRUE(g_obs.reactor_started.load(std::memory_order_acquire))
        << "Seastar reactor never started — main() did not invoke app.run()";
    ASSERT_TRUE(g_obs.reactor_had_two_shards.load(std::memory_order_acquire))
        << "Test requires smp::count >= 2; the cross-shard path was not exercised";
    ASSERT_TRUE(g_obs.lambda_executed.load(std::memory_order_acquire))
        << "Cross-shard lambda body never ran — submit_to dropped the work";
    ASSERT_TRUE(g_obs.any_exception_caught)
        << "Outer catch saw no exception at all — submit_to silently swallowed the throw";

    // Diagnostic — the test passes once the preconditions hold; the printed
    // line is the artefact the developer feeds into the M4 decision rule.
    if (g_obs.caught_as_bad_alloc) {
        std::cout
            << "\n[M4 result] PROPAGATED CLEANLY as bad_alloc"
            << "\n  typeid (mangled)   : " << g_obs.typeid_mangled
            << "\n  typeid (demangled) : " << g_obs.typeid_demangled
            << "\n  what()             : " << g_obs.what_message
            << "\n  decision rule      : mark M4 MITIGATED in the audit doc"
               " (mirror the M10 'Investigation closure (2026-05-10)' addendum)"
            << "\n";
        SUCCEED() << "PROPAGATED CLEANLY as bad_alloc";
    } else if (g_obs.caught_as_std_exception_only) {
        std::cout
            << "\n[M4 result] REPACKAGED as " << g_obs.typeid_demangled
            << " (still std::exception, but no longer std::bad_alloc)"
            << "\n  typeid (mangled)   : " << g_obs.typeid_mangled
            << "\n  typeid (demangled) : " << g_obs.typeid_demangled
            << "\n  what()             : " << g_obs.what_message
            << "\n  decision rule      : add try/catch inside the submitted"
               " lambda (return {{}, false}), per the M4 audit fix"
            << "\n";
        SUCCEED() << "REPACKAGED as " << g_obs.typeid_demangled;
    } else {
        std::cout
            << "\n[M4 result] REPACKAGED as non-std type"
            << "\n  typeid (mangled)   : " << g_obs.typeid_mangled
            << "\n  typeid (demangled) : " << g_obs.typeid_demangled
            << "\n  decision rule      : add try/catch inside the submitted"
               " lambda (return {{}, false}), per the M4 audit fix"
            << "\n";
        SUCCEED() << "REPACKAGED as non-std type (" << g_obs.typeid_mangled << ")";
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    // `gtest_discover_tests` runs the binary with `--gtest_list_tests` at
    // build time to enumerate cases. Avoid bringing the reactor up in that
    // mode — listing should be cheap and must not require the per-shard
    // memory/cpu the reactor wants.
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && std::string(argv[i]) == "--gtest_list_tests") {
            return RUN_ALL_TESTS();
        }
    }

    // Drive the Seastar reactor with a fixed minimal config so the test does
    // not race the user-supplied gtest args. `--smp 2` is required for an
    // actual cross-shard `submit_to`; `--memory 256M` keeps the per-shard
    // budget low enough for CI containers; `--overprovisioned` and
    // `--lock-memory 0` relax the CPU-pinning and mlock requirements that
    // otherwise need root in containers.
    seastar::app_template::seastar_options opts;
    opts.name = "m4_cross_shard_bad_alloc_diag";
    seastar::app_template app(std::move(opts));

    char arg0[]            = "m4_cross_shard_bad_alloc_test";
    char arg_smp_flag[]    = "--smp";
    char arg_smp_val[]     = "2";
    char arg_mem_flag[]    = "--memory";
    char arg_mem_val[]     = "256M";
    char arg_lock_flag[]   = "--lock-memory";
    char arg_lock_val[]    = "0";
    char arg_overprov[]    = "--overprovisioned";
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
                  << "; the gtest assertions will surface the failure mode.\n";
    }

    return RUN_ALL_TESTS();
}
