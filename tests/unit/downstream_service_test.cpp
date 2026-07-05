// Ranvier Core - Downstream Service Lifecycle Seam Unit Tests
//
// Reactor-free tests over the process-wide downstream-service factory slot: the
// unset default (no service), installing a factory, and resetting it back to
// none. Uses the seastar stubs in tests/stubs so this test does NOT need a
// running reactor — start()/stop() futures are constructed but never driven.
//
// What this test does NOT cover (out of scope here; needs a reactor):
//   - Application::startup() invoking the factory on shard 0 and awaiting start()
//   - stop() running first at teardown, before core services
// Those verify in the dev container / integration build.

#include "downstream_service.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace ranvier;

namespace {

// Test double, distinguishable and flag-bearing, so a caller can prove the
// installed factory produced THIS type. start()/stop() return ready stub futures
// and are never driven here.
class FakeDownstreamService final : public DownstreamService {
public:
    seastar::future<> start() override { return seastar::make_ready_future<>(); }
    seastar::future<> stop() override { return seastar::make_ready_future<>(); }
};

}  // namespace

// =============================================================================
// Factory slot: unset => none; installed => that factory; reset => none again
// =============================================================================

TEST(DownstreamServiceFactory, DefaultsToNoneWhenUnset) {
    // Ensure a clean slate (another test may have installed one).
    set_downstream_service_factory(nullptr);
    // Unset means "run nothing" — an empty std::function, not a default service.
    EXPECT_FALSE(static_cast<bool>(get_downstream_service_factory()));
}

TEST(DownstreamServiceFactory, InstalledFactoryProducesService) {
    set_downstream_service_factory([]() -> std::unique_ptr<DownstreamService> {
        return std::make_unique<FakeDownstreamService>();
    });

    auto factory = get_downstream_service_factory();
    ASSERT_TRUE(static_cast<bool>(factory));
    auto svc = factory();
    ASSERT_NE(svc.get(), nullptr);
    EXPECT_NE(dynamic_cast<FakeDownstreamService*>(svc.get()), nullptr);

    // Reset so later tests / the process see "none" again.
    set_downstream_service_factory(nullptr);
    EXPECT_FALSE(static_cast<bool>(get_downstream_service_factory()));
}

TEST(DownstreamServiceFactory, ResetToNullClearsInstalledFactory) {
    set_downstream_service_factory([]() -> std::unique_ptr<DownstreamService> {
        return std::make_unique<FakeDownstreamService>();
    });
    ASSERT_TRUE(static_cast<bool>(get_downstream_service_factory()));

    set_downstream_service_factory(nullptr);
    EXPECT_FALSE(static_cast<bool>(get_downstream_service_factory()));
}
