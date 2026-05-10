// Ranvier Core - Seastar Contract Test: Cross-Shard Exception Propagation
//
// Characterises what exception type the initiating shard observes when a
// lambda passed to `seastar::smp::submit_to` throws `std::bad_alloc` on the
// target shard. The two outcomes the test distinguishes are:
//
//   * "propagated cleanly as bad_alloc" — typed `catch (const std::bad_alloc&)`
//     fires at the outer site. Any production handler that catches
//     `std::bad_alloc` (or its `std::exception` base) gets the original type.
//   * "repackaged as <type>"            — Seastar's cross-shard
//     future-marshalling has substituted a different exception (e.g.
//     `seastar::broken_promise`). A handler that only matches `std::bad_alloc`
//     by name will silently miss the OOM.
//
// The test always passes once the reactor preconditions hold; the printed
// "[result] …" line is the artefact callers consume.
//
// Originally added under the request-lifecycle crash audit (finding M4) to
// resolve a question that could not be settled by code-reading: whether the
// outer `try/catch` around `TokenizerService::encode_cached_async` (in
// `HttpController::handle_proxy`'s tokenisation block) sees `std::bad_alloc`
// in its original type when the cross-shard tokenizer lambda allocates and
// fails. The test is kept as a long-lived contract check on Seastar — if
// future Seastar versions change exception transport, this is where we'll
// see it first.
//
// Departure from convention: every other Seastar-linked unit test in this
// directory keeps the reactor unbooted (see the header comment in
// `tokenizer_service_test.cpp`). This test cannot — `smp::submit_to` is the
// unit under observation and only exists once the reactor is up with
// `smp::count >= 2`. The reactor is therefore booted from `main()` via
// `seastar::app_template::run` with a fixed minimal argv (`--smp 2`,
// `--memory 256M`, `--overprovisioned`); the gtest body then asserts on
// observations captured into static globals.
//
// Run: `./cross_shard_exception_propagation_test` (no extra flags required).
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

    // Mirrors the cross-shard call shape used by
    // `TokenizerService::encode_cached_async` — same return type, same
    // single-throw failure mode (no in-lambda try/catch around the throw).
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
            // The rethrow-and-catch shape mirrors the production tokenisation
            // outer catch in `HttpController::handle_proxy`, with one
            // addition: an explicit `catch(const std::bad_alloc&)` ahead of
            // the generic `std::exception` branch so we can distinguish
            // "propagated cleanly" from "repackaged as another std::exception
            // subclass" (e.g. `seastar::broken_promise`, which the production
            // handler would still catch but for the wrong reason — masking
            // the OOM).
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

TEST(CrossShardSubmitTo, BadAllocSurfaceType) {
    ASSERT_TRUE(g_obs.reactor_started.load(std::memory_order_acquire))
        << "Seastar reactor never started — main() did not invoke app.run()";
    ASSERT_TRUE(g_obs.reactor_had_two_shards.load(std::memory_order_acquire))
        << "Test requires smp::count >= 2; the cross-shard path was not exercised";
    ASSERT_TRUE(g_obs.lambda_executed.load(std::memory_order_acquire))
        << "Cross-shard lambda body never ran — submit_to dropped the work";
    ASSERT_TRUE(g_obs.any_exception_caught)
        << "Outer catch saw no exception at all — submit_to silently swallowed the throw";

    // Diagnostic — the test passes once the preconditions hold; the printed
    // line is the artefact callers consume.
    if (g_obs.caught_as_bad_alloc) {
        std::cout
            << "\n[result] propagated cleanly as bad_alloc"
            << "\n  typeid (mangled)   : " << g_obs.typeid_mangled
            << "\n  typeid (demangled) : " << g_obs.typeid_demangled
            << "\n  what()             : " << g_obs.what_message
            << "\n";
        SUCCEED() << "propagated cleanly as bad_alloc";
    } else if (g_obs.caught_as_std_exception_only) {
        std::cout
            << "\n[result] repackaged as " << g_obs.typeid_demangled
            << " (still std::exception, but no longer std::bad_alloc)"
            << "\n  typeid (mangled)   : " << g_obs.typeid_mangled
            << "\n  typeid (demangled) : " << g_obs.typeid_demangled
            << "\n  what()             : " << g_obs.what_message
            << "\n";
        SUCCEED() << "repackaged as " << g_obs.typeid_demangled;
    } else {
        std::cout
            << "\n[result] repackaged as non-std type"
            << "\n  typeid (mangled)   : " << g_obs.typeid_mangled
            << "\n  typeid (demangled) : " << g_obs.typeid_demangled
            << "\n";
        SUCCEED() << "repackaged as non-std type (" << g_obs.typeid_mangled << ")";
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
    opts.name = "cross_shard_exception_propagation";
    seastar::app_template app(std::move(opts));

    char arg0[]            = "cross_shard_exception_propagation_test";
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
