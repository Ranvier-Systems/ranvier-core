// Ranvier Core - Request hot-path timeout helper
//
// Wraps seastar::with_timeout with a domain-tagged exception type so callers
// on the request hot path can identify which phase exceeded its deadline
// (tokenize, thread-pool dispatch, backend chunk read, ...). Closes BACKLOG
// §18 cross-cutting ticket: H3 (reactor-blocking FFI fallback), M5 (thread
// pool future with no timeout), tightens M15 (chunk-read site).

#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/timed_out_error.hh>
#include <seastar/core/with_timeout.hh>

#include <chrono>
#include <utility>

namespace ranvier {

// Domain-tagged timeout for request hot-path operations.
//
// Derives from seastar::timed_out_error so existing
// `catch (const seastar::timed_out_error&)` handlers continue to match
// without modification. New code that wants to distinguish phases can catch
// this type and inspect label().
//
// `label` must point to a static C-string; the lifetime is not extended.
class request_timeout_error : public seastar::timed_out_error {
public:
    explicit request_timeout_error(const char* label) noexcept : _label(label) {}

    const char* what() const noexcept override { return _label; }
    const char* label() const noexcept { return _label; }

private:
    const char* _label;
};

// Wrap a hot-path future with a deadline. On timeout, the returned future
// fails with request_timeout_error(label) instead of a bare
// seastar::timed_out_error so call-site logging / metrics can identify the
// phase. If the inner future fails for any other reason, that exception
// propagates unchanged.
template <typename Clock, typename Duration, typename T>
inline seastar::future<T> with_request_timeout(
    std::chrono::time_point<Clock, Duration> deadline,
    seastar::future<T> fut,
    const char* label) {
    return seastar::with_timeout(deadline, std::move(fut))
        .handle_exception_type([label](const seastar::timed_out_error&) {
            return seastar::make_exception_future<T>(
                request_timeout_error(label));
        });
}

// Convenience overload: deadline = lowres_clock::now() + duration.
template <typename Rep, typename Period, typename T>
inline seastar::future<T> with_request_timeout(
    std::chrono::duration<Rep, Period> timeout,
    seastar::future<T> fut,
    const char* label) {
    return with_request_timeout(
        seastar::lowres_clock::now() + timeout, std::move(fut), label);
}

}  // namespace ranvier
