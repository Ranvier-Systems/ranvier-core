// Ranvier Core - Test Clock for Deterministic Time Control
//
// Provides a manually-controllable clock for unit testing time-sensitive
// components (rate limiter, circuit breaker, quorum liveness).
// Satisfies the C++ Clock concept so it can be used as a drop-in
// template parameter replacing std::chrono::steady_clock.
//
// Usage:
//   TestClock::reset();                            // Reset to epoch
//   TestClock::advance(std::chrono::seconds(5));   // Advance by 5 seconds
//   auto now = TestClock::now();                   // Get current test time

#pragma once

#include <chrono>

namespace ranvier {

struct TestClock {
    using duration = std::chrono::steady_clock::duration;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<TestClock, duration>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept { return _now; }

    // Reset clock to epoch (call in test SetUp)
    static void reset() { _now = time_point(duration::zero()); }

    // Advance clock by a duration
    static void advance(duration d) { _now += d; }

    // Set clock to an explicit time point
    static void set(time_point tp) { _now = tp; }

private:
    inline static time_point _now{duration::zero()};
};

}  // namespace ranvier
