// Ranvier Core - Connection Pool Unit Tests
//
// Tests for connection pool management logic: per-host limits, global limits,
// idle timeout eviction, max-age TTL enforcement, and half-open detection.
//
// Uses TestClock for deterministic timing (same pattern as circuit_breaker_test
// and rate_limiter_test). Uses SyncClosePolicy to avoid Seastar reactor dependency
// when evicting default-constructed bundles with null streams.
//
// Note: Half-open detection positive tests (verifying that in.eof() == true
// triggers eviction) require a running Seastar reactor to manipulate stream
// state and belong in integration tests. Unit tests verify that non-half-open
// connections survive cleanup correctly.

#include "connection_pool.hpp"
#include "test_clock.hpp"
#include <gtest/gtest.h>
#include <chrono>

using namespace ranvier;

// Test-specific type aliases: deterministic clock + sync close (no reactor needed)
using TestBundle = BasicConnectionBundle<TestClock>;
using TestPool = BasicConnectionPool<TestClock, SyncClosePolicy>;

// =============================================================================
// ConnectionPoolConfig Tests
// =============================================================================

class ConnectionPoolConfigTest : public ::testing::Test {};

TEST_F(ConnectionPoolConfigTest, DefaultConfigValues) {
    ConnectionPoolConfig config;
    EXPECT_EQ(config.max_connections_per_host, 10u);
    EXPECT_EQ(config.idle_timeout, std::chrono::seconds(60));
    EXPECT_EQ(config.max_total_connections, 100u);
    EXPECT_EQ(config.reaper_interval, std::chrono::seconds(15));
    EXPECT_TRUE(config.tcp_keepalive);
    EXPECT_EQ(config.keepalive_idle, std::chrono::seconds(30));
    EXPECT_EQ(config.keepalive_interval, std::chrono::seconds(10));
    EXPECT_EQ(config.keepalive_count, 3u);
    EXPECT_EQ(config.max_connection_age, std::chrono::seconds(300));
}

TEST_F(ConnectionPoolConfigTest, ConfigCanBeCustomized) {
    ConnectionPoolConfig config;
    config.max_connections_per_host = 20;
    config.idle_timeout = std::chrono::seconds(120);
    config.max_total_connections = 200;
    config.max_connection_age = std::chrono::seconds(600);

    EXPECT_EQ(config.max_connections_per_host, 20u);
    EXPECT_EQ(config.idle_timeout, std::chrono::seconds(120));
    EXPECT_EQ(config.max_total_connections, 200u);
    EXPECT_EQ(config.max_connection_age, std::chrono::seconds(600));
}

// =============================================================================
// ConnectionBundle Timestamp Tests (TestClock - deterministic)
// =============================================================================

class ConnectionBundleTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
    }
};

TEST_F(ConnectionBundleTest, DefaultConstructorSetsTimestamps) {
    TestBundle bundle;
    auto now = TestClock::now();
    EXPECT_EQ(bundle.created_at, now);
    EXPECT_EQ(bundle.last_used, now);
    EXPECT_TRUE(bundle.is_valid);
}

TEST_F(ConnectionBundleTest, TouchUpdatesLastUsed) {
    TestBundle bundle;
    auto creation_time = TestClock::now();

    TestClock::advance(std::chrono::seconds(10));
    bundle.touch();

    EXPECT_EQ(bundle.created_at, creation_time);
    EXPECT_EQ(bundle.last_used, TestClock::now());
}

TEST_F(ConnectionBundleTest, IsExpiredChecksIdleTime) {
    TestBundle bundle;
    auto timeout = std::chrono::seconds(60);

    // Just created - not expired
    EXPECT_FALSE(bundle.is_expired(timeout));

    // Advance to just before timeout
    TestClock::advance(std::chrono::seconds(60));
    EXPECT_FALSE(bundle.is_expired(timeout));

    // Advance past timeout
    TestClock::advance(std::chrono::milliseconds(1));
    EXPECT_TRUE(bundle.is_expired(timeout));
}

TEST_F(ConnectionBundleTest, IsTooOldChecksCreationTime) {
    TestBundle bundle;
    auto max_age = std::chrono::seconds(300);

    // Just created - not too old
    EXPECT_FALSE(bundle.is_too_old(max_age));

    // Touch doesn't affect creation time
    TestClock::advance(std::chrono::seconds(200));
    bundle.touch();
    EXPECT_FALSE(bundle.is_too_old(max_age));

    // Advance past max age from creation
    TestClock::advance(std::chrono::seconds(101));
    EXPECT_TRUE(bundle.is_too_old(max_age));
}

TEST_F(ConnectionBundleTest, TouchResetsIdleButNotAge) {
    TestBundle bundle;

    // Advance 50 seconds and touch
    TestClock::advance(std::chrono::seconds(50));
    bundle.touch();

    // Not idle-expired (just touched)
    EXPECT_FALSE(bundle.is_expired(std::chrono::seconds(60)));

    // But age still counts from creation
    TestClock::advance(std::chrono::seconds(260));
    EXPECT_TRUE(bundle.is_too_old(std::chrono::seconds(300)));

    // Touch again - still too old (age is immutable)
    bundle.touch();
    EXPECT_TRUE(bundle.is_too_old(std::chrono::seconds(300)));
}

TEST_F(ConnectionBundleTest, DefaultConstructedStreamNotHalfOpen) {
    // Default-constructed input_stream has eof() == false
    // This means default-constructed bundles are NOT considered half-open
    TestBundle bundle;
    EXPECT_FALSE(bundle.is_half_open());
}

// =============================================================================
// Per-Host Connection Limit Tests
// =============================================================================

class ConnectionPoolPerHostLimitTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
        config.max_connections_per_host = 3;
        config.max_total_connections = 100;
        config.idle_timeout = std::chrono::seconds(60);
        config.max_connection_age = std::chrono::seconds(300);
        config.reaper_interval = std::chrono::seconds(0);  // Disable timer for tests
    }

    ConnectionPoolConfig config;

    // Helper to create a test bundle for a given port
    TestBundle make_bundle(uint16_t port) {
        TestBundle bundle;
        bundle.addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, port));
        return bundle;
    }
};

TEST_F(ConnectionPoolPerHostLimitTest, AcceptsUpToPerHostLimit) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    for (int i = 0; i < 3; ++i) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
    }

    auto s = pool.stats();
    EXPECT_EQ(s.total_idle_connections, 3u);
    EXPECT_EQ(s.num_backends, 1u);
}

TEST_F(ConnectionPoolPerHostLimitTest, EvictsOldestWhenPerHostLimitExceeded) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    // Fill pool to per-host limit
    for (int i = 0; i < 3; ++i) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
    }
    EXPECT_EQ(pool.stats().total_idle_connections, 3u);

    // Put one more - oldest should be evicted, count stays at 3
    TestBundle extra;
    extra.addr = addr;
    pool.put(std::move(extra));

    EXPECT_EQ(pool.stats().total_idle_connections, 3u);
}

TEST_F(ConnectionPoolPerHostLimitTest, DifferentHostsHaveIndependentLimits) {
    TestPool pool(config);
    auto addr1 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));
    auto addr2 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8081));

    // Fill both hosts to their per-host limit
    for (int i = 0; i < 3; ++i) {
        TestBundle b1;
        b1.addr = addr1;
        pool.put(std::move(b1));

        TestBundle b2;
        b2.addr = addr2;
        pool.put(std::move(b2));
    }

    auto s = pool.stats();
    EXPECT_EQ(s.total_idle_connections, 6u);
    EXPECT_EQ(s.num_backends, 2u);
}

TEST_F(ConnectionPoolPerHostLimitTest, PerHostLimitOfOneWorks) {
    config.max_connections_per_host = 1;
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    TestBundle b1;
    b1.addr = addr;
    pool.put(std::move(b1));
    EXPECT_EQ(pool.stats().total_idle_connections, 1u);

    // Second put evicts the first
    TestBundle b2;
    b2.addr = addr;
    pool.put(std::move(b2));
    EXPECT_EQ(pool.stats().total_idle_connections, 1u);
}

// =============================================================================
// Global Connection Limit Tests
// =============================================================================

class ConnectionPoolGlobalLimitTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
        config.max_connections_per_host = 5;
        config.max_total_connections = 6;
        config.idle_timeout = std::chrono::seconds(60);
        config.max_connection_age = std::chrono::seconds(300);
        config.reaper_interval = std::chrono::seconds(0);
    }

    ConnectionPoolConfig config;
};

TEST_F(ConnectionPoolGlobalLimitTest, EvictsGloballyOldestWhenTotalLimitReached) {
    TestPool pool(config);
    auto addr1 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));
    auto addr2 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8081));

    // Put 3 connections for host 1 (created at t=0,1,2s)
    for (int i = 0; i < 3; ++i) {
        TestBundle bundle;
        bundle.addr = addr1;
        pool.put(std::move(bundle));
        TestClock::advance(std::chrono::seconds(1));
    }

    // Put 3 connections for host 2 (created at t=3,4,5s)
    for (int i = 0; i < 3; ++i) {
        TestBundle bundle;
        bundle.addr = addr2;
        pool.put(std::move(bundle));
        TestClock::advance(std::chrono::seconds(1));
    }

    EXPECT_EQ(pool.stats().total_idle_connections, 6u);

    // Put one more - should evict the globally oldest (the first connection from host 1)
    TestBundle extra;
    extra.addr = addr2;
    pool.put(std::move(extra));

    // Total should still be at the limit (one evicted, one added)
    EXPECT_EQ(pool.stats().total_idle_connections, 6u);
}

TEST_F(ConnectionPoolGlobalLimitTest, GlobalLimitWorksAcrossManyHosts) {
    config.max_total_connections = 4;
    config.max_connections_per_host = 2;
    TestPool pool(config);

    // Put 2 connections each for 2 hosts = 4 total (at limit)
    for (uint16_t port = 8080; port <= 8081; ++port) {
        auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, port));
        for (int i = 0; i < 2; ++i) {
            TestBundle bundle;
            bundle.addr = addr;
            pool.put(std::move(bundle));
            TestClock::advance(std::chrono::seconds(1));
        }
    }
    EXPECT_EQ(pool.stats().total_idle_connections, 4u);

    // Add one more for a third host - global eviction kicks in
    auto addr3 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8082));
    TestBundle extra;
    extra.addr = addr3;
    pool.put(std::move(extra));

    EXPECT_EQ(pool.stats().total_idle_connections, 4u);
    EXPECT_EQ(pool.stats().num_backends, 3u);
}

// =============================================================================
// Idle Timeout Eviction Tests (TestClock - deterministic)
// =============================================================================

class ConnectionPoolIdleTimeoutTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
        config.max_connections_per_host = 10;
        config.max_total_connections = 100;
        config.idle_timeout = std::chrono::seconds(30);
        config.max_connection_age = std::chrono::seconds(300);
        config.reaper_interval = std::chrono::seconds(0);
    }

    ConnectionPoolConfig config;
};

TEST_F(ConnectionPoolIdleTimeoutTest, FreshConnectionsSurviveCleanup) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    TestBundle bundle;
    bundle.addr = addr;
    pool.put(std::move(bundle));

    // Cleanup immediately - nothing should be evicted
    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 0u);
    EXPECT_EQ(pool.stats().total_idle_connections, 1u);
}

TEST_F(ConnectionPoolIdleTimeoutTest, ExpiredConnectionsEvicted) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    // Put 3 connections
    for (int i = 0; i < 3; ++i) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
    }
    EXPECT_EQ(pool.stats().total_idle_connections, 3u);

    // Advance past idle timeout
    TestClock::advance(std::chrono::seconds(31));

    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 3u);
    EXPECT_EQ(pool.stats().total_idle_connections, 0u);
    EXPECT_EQ(pool.stats().dead_connections_reaped, 3u);
}

TEST_F(ConnectionPoolIdleTimeoutTest, MixOfFreshAndExpiredConnections) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    // Put 2 old connections
    for (int i = 0; i < 2; ++i) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
    }

    // Advance 25 seconds (not yet expired)
    TestClock::advance(std::chrono::seconds(25));

    // Put 2 fresh connections
    for (int i = 0; i < 2; ++i) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
    }

    EXPECT_EQ(pool.stats().total_idle_connections, 4u);

    // Advance 6 more seconds - old connections now at 31s (expired),
    // new connections at 6s (not expired)
    TestClock::advance(std::chrono::seconds(6));

    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 2u);
    EXPECT_EQ(pool.stats().total_idle_connections, 2u);
}

TEST_F(ConnectionPoolIdleTimeoutTest, ConnectionAtExactTimeout) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    TestBundle bundle;
    bundle.addr = addr;
    pool.put(std::move(bundle));

    // Advance exactly to timeout boundary (not expired - uses >)
    TestClock::advance(std::chrono::seconds(30));
    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 0u);

    // 1ms past timeout - now expired
    TestClock::advance(std::chrono::milliseconds(1));
    closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 1u);
}

TEST_F(ConnectionPoolIdleTimeoutTest, CleanupAcrossMultipleHosts) {
    TestPool pool(config);
    auto addr1 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));
    auto addr2 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8081));

    // Put connections for two hosts
    for (auto& addr : {addr1, addr2}) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
    }
    EXPECT_EQ(pool.stats().total_idle_connections, 2u);
    EXPECT_EQ(pool.stats().num_backends, 2u);

    // Expire all
    TestClock::advance(std::chrono::seconds(31));
    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 2u);
    EXPECT_EQ(pool.stats().total_idle_connections, 0u);
}

// =============================================================================
// Max-Age TTL Enforcement Tests (TestClock - deterministic)
// =============================================================================

class ConnectionPoolMaxAgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
        config.max_connections_per_host = 10;
        config.max_total_connections = 100;
        config.idle_timeout = std::chrono::seconds(60);
        config.max_connection_age = std::chrono::seconds(120);
        config.reaper_interval = std::chrono::seconds(0);
    }

    ConnectionPoolConfig config;
};

TEST_F(ConnectionPoolMaxAgeTest, ConnectionEvictedAfterMaxAge) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    TestBundle bundle;
    bundle.addr = addr;
    pool.put(std::move(bundle));

    // Advance past max age (but keep touching so not idle-expired)
    // We can't touch inside the pool, but we can just advance past max_age
    // The cleanup checks is_too_old which uses created_at (immutable)
    TestClock::advance(std::chrono::seconds(121));

    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 1u);
    EXPECT_EQ(pool.stats().connections_reaped_max_age, 1u);
    EXPECT_EQ(pool.stats().dead_connections_reaped, 1u);
}

TEST_F(ConnectionPoolMaxAgeTest, MaxAgeTrackedSeparatelyFromIdleTimeout) {
    // Set idle_timeout > max_connection_age to verify max-age takes precedence
    config.idle_timeout = std::chrono::seconds(600);  // 10 min idle
    config.max_connection_age = std::chrono::seconds(120);  // 2 min max age
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    TestBundle bundle;
    bundle.addr = addr;
    pool.put(std::move(bundle));

    // At 121s: not idle-expired (600s timeout) but max-age exceeded (120s)
    TestClock::advance(std::chrono::seconds(121));

    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 1u);
    EXPECT_EQ(pool.stats().connections_reaped_max_age, 1u);
}

TEST_F(ConnectionPoolMaxAgeTest, IdleExpiresBeforeMaxAge) {
    // When idle_timeout < max_connection_age, idle wins
    config.idle_timeout = std::chrono::seconds(30);
    config.max_connection_age = std::chrono::seconds(300);
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    TestBundle bundle;
    bundle.addr = addr;
    pool.put(std::move(bundle));

    // At 31s: idle-expired (30s) but not max-age (300s)
    TestClock::advance(std::chrono::seconds(31));

    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 1u);
    // Should NOT count as max-age reap (it was idle-expired)
    EXPECT_EQ(pool.stats().connections_reaped_max_age, 0u);
    EXPECT_EQ(pool.stats().dead_connections_reaped, 1u);
}

TEST_F(ConnectionPoolMaxAgeTest, MaxAgeCounterAccumulates) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    // Create and expire connections in multiple rounds
    for (int round = 0; round < 3; ++round) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));

        TestClock::advance(std::chrono::seconds(121));
        pool.cleanup_expired();
    }

    EXPECT_EQ(pool.stats().connections_reaped_max_age, 3u);
    EXPECT_EQ(pool.stats().dead_connections_reaped, 3u);
}

// =============================================================================
// Half-Open Connection Detection Tests
// =============================================================================

class ConnectionPoolHalfOpenTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
        config.max_connections_per_host = 10;
        config.max_total_connections = 100;
        config.idle_timeout = std::chrono::seconds(60);
        config.max_connection_age = std::chrono::seconds(300);
        config.reaper_interval = std::chrono::seconds(0);
    }

    ConnectionPoolConfig config;
};

TEST_F(ConnectionPoolHalfOpenTest, NonHalfOpenConnectionsSurviveCleanup) {
    // Default-constructed bundles have is_half_open() == false
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    for (int i = 0; i < 5; ++i) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
    }

    // Cleanup should find nothing to evict (not expired, not too old, not half-open)
    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 0u);
    EXPECT_EQ(pool.stats().total_idle_connections, 5u);
}

TEST_F(ConnectionPoolHalfOpenTest, NonHalfOpenConnectionAcceptedByPut) {
    // Verify that put() accepts non-half-open connections
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    TestBundle bundle;
    bundle.addr = addr;
    pool.put(std::move(bundle));

    EXPECT_EQ(pool.stats().total_idle_connections, 1u);
}

TEST_F(ConnectionPoolHalfOpenTest, InvalidConnectionRejectedByPut) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    TestBundle bundle;
    bundle.addr = addr;
    bundle.is_valid = false;
    pool.put(std::move(bundle));

    // Invalid connection should not be added to pool
    EXPECT_EQ(pool.stats().total_idle_connections, 0u);
}

// Note: Positive half-open detection (verifying that connections with
// in.eof() == true are evicted) requires a running Seastar reactor to
// create streams in EOF state. That test belongs in integration tests.

// =============================================================================
// Stats Tracking Tests
// =============================================================================

class ConnectionPoolStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
        config.max_connections_per_host = 5;
        config.max_total_connections = 20;
        config.idle_timeout = std::chrono::seconds(30);
        config.max_connection_age = std::chrono::seconds(300);
        config.reaper_interval = std::chrono::seconds(0);
    }

    ConnectionPoolConfig config;
};

TEST_F(ConnectionPoolStatsTest, InitialStatsAreZero) {
    TestPool pool(config);
    auto s = pool.stats();

    EXPECT_EQ(s.total_idle_connections, 0u);
    EXPECT_EQ(s.num_backends, 0u);
    EXPECT_EQ(s.max_per_host, 5u);
    EXPECT_EQ(s.max_total, 20u);
    EXPECT_EQ(s.dead_connections_reaped, 0u);
    EXPECT_EQ(s.connections_reaped_max_age, 0u);
}

TEST_F(ConnectionPoolStatsTest, StatsTrackPutAndCleanup) {
    TestPool pool(config);
    auto addr1 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));
    auto addr2 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8081));

    // Put connections
    for (int i = 0; i < 3; ++i) {
        TestBundle b1;
        b1.addr = addr1;
        pool.put(std::move(b1));
    }
    for (int i = 0; i < 2; ++i) {
        TestBundle b2;
        b2.addr = addr2;
        pool.put(std::move(b2));
    }

    auto s = pool.stats();
    EXPECT_EQ(s.total_idle_connections, 5u);
    EXPECT_EQ(s.num_backends, 2u);

    // Expire and cleanup
    TestClock::advance(std::chrono::seconds(31));
    pool.cleanup_expired();

    s = pool.stats();
    EXPECT_EQ(s.total_idle_connections, 0u);
    EXPECT_EQ(s.dead_connections_reaped, 5u);
}

TEST_F(ConnectionPoolStatsTest, StatsReflectConfigValues) {
    config.max_connections_per_host = 42;
    config.max_total_connections = 1000;
    TestPool pool(config);

    auto s = pool.stats();
    EXPECT_EQ(s.max_per_host, 42u);
    EXPECT_EQ(s.max_total, 1000u);
}

// =============================================================================
// clear_pool Tests (Backend Removal)
// =============================================================================

class ConnectionPoolClearPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
        config.max_connections_per_host = 10;
        config.max_total_connections = 100;
        config.idle_timeout = std::chrono::seconds(60);
        config.max_connection_age = std::chrono::seconds(300);
        config.reaper_interval = std::chrono::seconds(0);
    }

    ConnectionPoolConfig config;
};

TEST_F(ConnectionPoolClearPoolTest, ClearsAllConnectionsForBackend) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    for (int i = 0; i < 5; ++i) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
    }
    EXPECT_EQ(pool.stats().total_idle_connections, 5u);

    size_t closed = pool.clear_pool(addr);
    EXPECT_EQ(closed, 5u);
    EXPECT_EQ(pool.stats().total_idle_connections, 0u);
    EXPECT_EQ(pool.stats().num_backends, 0u);
}

TEST_F(ConnectionPoolClearPoolTest, ClearNonExistentBackendReturnsZero) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 9999));

    size_t closed = pool.clear_pool(addr);
    EXPECT_EQ(closed, 0u);
}

TEST_F(ConnectionPoolClearPoolTest, ClearOneBackendPreservesOthers) {
    TestPool pool(config);
    auto addr1 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));
    auto addr2 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8081));

    for (int i = 0; i < 3; ++i) {
        TestBundle b1;
        b1.addr = addr1;
        pool.put(std::move(b1));
    }
    for (int i = 0; i < 2; ++i) {
        TestBundle b2;
        b2.addr = addr2;
        pool.put(std::move(b2));
    }
    EXPECT_EQ(pool.stats().total_idle_connections, 5u);
    EXPECT_EQ(pool.stats().num_backends, 2u);

    // Clear only host 1
    size_t closed = pool.clear_pool(addr1);
    EXPECT_EQ(closed, 3u);
    EXPECT_EQ(pool.stats().total_idle_connections, 2u);
    EXPECT_EQ(pool.stats().num_backends, 1u);
}

// =============================================================================
// Map Entry Cleanup Tests (Rule #4: Bounded Container)
// =============================================================================

class ConnectionPoolMapCleanupTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
        config.max_connections_per_host = 10;
        config.max_total_connections = 100;
        config.idle_timeout = std::chrono::seconds(30);
        config.max_connection_age = std::chrono::seconds(300);
        config.reaper_interval = std::chrono::seconds(0);
    }

    ConnectionPoolConfig config;
};

TEST_F(ConnectionPoolMapCleanupTest, EmptyPoolEntriesRemovedAfterCleanup) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    // Add connections
    TestBundle bundle;
    bundle.addr = addr;
    pool.put(std::move(bundle));
    EXPECT_EQ(pool.stats().num_backends, 1u);

    // Expire and cleanup
    TestClock::advance(std::chrono::seconds(31));
    pool.cleanup_expired();

    // Map entry should be removed (Rule #4: don't leave empty deques in map)
    EXPECT_EQ(pool.stats().num_backends, 0u);
}

TEST_F(ConnectionPoolMapCleanupTest, NonEmptyPoolEntriesPreserved) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    // Add 2 connections at different times
    TestBundle b1;
    b1.addr = addr;
    pool.put(std::move(b1));

    TestClock::advance(std::chrono::seconds(25));

    TestBundle b2;
    b2.addr = addr;
    pool.put(std::move(b2));

    // Advance so first is expired (31s) but second is fresh (6s)
    TestClock::advance(std::chrono::seconds(6));
    pool.cleanup_expired();

    // One connection remains, map entry preserved
    EXPECT_EQ(pool.stats().num_backends, 1u);
    EXPECT_EQ(pool.stats().total_idle_connections, 1u);
}

TEST_F(ConnectionPoolMapCleanupTest, MultipleEmptyPoolsCleanedUp) {
    TestPool pool(config);

    // Create connections for 5 different backends
    for (uint16_t port = 8080; port <= 8084; ++port) {
        auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, port));
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
    }
    EXPECT_EQ(pool.stats().num_backends, 5u);

    // Expire all and cleanup
    TestClock::advance(std::chrono::seconds(31));
    size_t closed = pool.cleanup_expired();

    EXPECT_EQ(closed, 5u);
    EXPECT_EQ(pool.stats().num_backends, 0u);
    EXPECT_EQ(pool.stats().total_idle_connections, 0u);
}

// =============================================================================
// Eviction Order Tests
// =============================================================================

class ConnectionPoolEvictionOrderTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
        config.max_connections_per_host = 3;
        config.max_total_connections = 100;
        config.idle_timeout = std::chrono::seconds(60);
        config.max_connection_age = std::chrono::seconds(300);
        config.reaper_interval = std::chrono::seconds(0);
    }

    ConnectionPoolConfig config;
};

TEST_F(ConnectionPoolEvictionOrderTest, PerHostEvictsOldest) {
    // Per-host eviction uses deque: oldest is at front, newest at back
    // When limit is exceeded, front (oldest) is evicted
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    // Fill to capacity (3 connections)
    for (int i = 0; i < 3; ++i) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
        TestClock::advance(std::chrono::seconds(1));
    }

    // Put a 4th - should evict the first (oldest at front of deque)
    TestBundle b4;
    b4.addr = addr;
    pool.put(std::move(b4));

    // Pool still has 3 connections
    EXPECT_EQ(pool.stats().total_idle_connections, 3u);
}

TEST_F(ConnectionPoolEvictionOrderTest, GlobalEvictsOldestAcrossAllPools) {
    config.max_total_connections = 4;
    TestPool pool(config);

    auto addr1 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));
    auto addr2 = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8081));

    // Put 2 old connections for host 1 (t=0, t=1)
    for (int i = 0; i < 2; ++i) {
        TestBundle bundle;
        bundle.addr = addr1;
        pool.put(std::move(bundle));
        TestClock::advance(std::chrono::seconds(1));
    }

    // Put 2 newer connections for host 2 (t=2, t=3)
    for (int i = 0; i < 2; ++i) {
        TestBundle bundle;
        bundle.addr = addr2;
        pool.put(std::move(bundle));
        TestClock::advance(std::chrono::seconds(1));
    }

    EXPECT_EQ(pool.stats().total_idle_connections, 4u);

    // Put one more for host 2 - global eviction should remove oldest from host 1
    TestBundle extra;
    extra.addr = addr2;
    pool.put(std::move(extra));

    EXPECT_EQ(pool.stats().total_idle_connections, 4u);
}

// =============================================================================
// Mixed Scenario Tests
// =============================================================================

class ConnectionPoolMixedTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
        config.max_connections_per_host = 5;
        config.max_total_connections = 20;
        config.idle_timeout = std::chrono::seconds(30);
        config.max_connection_age = std::chrono::seconds(120);
        config.reaper_interval = std::chrono::seconds(0);
    }

    ConnectionPoolConfig config;
};

TEST_F(ConnectionPoolMixedTest, CleanupPrioritizesIdleOverMaxAge) {
    // When a connection is both idle-expired AND max-age expired,
    // cleanup_expired() checks idle first (cheaper check).
    // Result: it's counted as idle-expired, not max-age
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    TestBundle bundle;
    bundle.addr = addr;
    pool.put(std::move(bundle));

    // Advance past both idle (30s) and max-age (120s)
    TestClock::advance(std::chrono::seconds(121));

    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 1u);
    // Counted as idle-expired (checked first in cleanup_expired)
    EXPECT_EQ(pool.stats().connections_reaped_max_age, 0u);
    EXPECT_EQ(pool.stats().dead_connections_reaped, 1u);
}

TEST_F(ConnectionPoolMixedTest, MultipleCleanupRoundsAccumulate) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    // Round 1: idle-expire 2 connections
    for (int i = 0; i < 2; ++i) {
        TestBundle bundle;
        bundle.addr = addr;
        pool.put(std::move(bundle));
    }
    TestClock::advance(std::chrono::seconds(31));
    pool.cleanup_expired();

    // Round 2: max-age expire 1 connection (idle_timeout < advance < max_age
    // won't work here because idle would trigger first; so advance past max_age
    // with idle_timeout > max_age to trigger max-age first)
    config.idle_timeout = std::chrono::seconds(600);
    // We can't change config on existing pool, so just test accumulation
    // of dead_connections_reaped counter
    TestBundle bundle;
    bundle.addr = addr;
    pool.put(std::move(bundle));
    TestClock::advance(std::chrono::seconds(31));
    pool.cleanup_expired();

    EXPECT_EQ(pool.stats().dead_connections_reaped, 3u);
}

TEST_F(ConnectionPoolMixedTest, PutAfterClearPoolWorks) {
    TestPool pool(config);
    auto addr = seastar::socket_address(seastar::ipv4_addr(0x7f000001, 8080));

    // Put, clear, put again
    TestBundle b1;
    b1.addr = addr;
    pool.put(std::move(b1));
    EXPECT_EQ(pool.stats().total_idle_connections, 1u);

    pool.clear_pool(addr);
    EXPECT_EQ(pool.stats().total_idle_connections, 0u);

    TestBundle b2;
    b2.addr = addr;
    pool.put(std::move(b2));
    EXPECT_EQ(pool.stats().total_idle_connections, 1u);
    EXPECT_EQ(pool.stats().num_backends, 1u);
}

TEST_F(ConnectionPoolMixedTest, EmptyCleanupIsNoop) {
    TestPool pool(config);
    size_t closed = pool.cleanup_expired();
    EXPECT_EQ(closed, 0u);
}

// =============================================================================
// Backward Compatibility Type Alias Tests
// =============================================================================

TEST(ConnectionPoolTypeAliasTest, ConnectionBundleAliasExists) {
    // Verify backward-compatible alias compiles
    static_assert(std::is_same_v<ConnectionBundle, BasicConnectionBundle<std::chrono::steady_clock>>,
                  "ConnectionBundle should be alias for BasicConnectionBundle<steady_clock>");
}

TEST(ConnectionPoolTypeAliasTest, ConnectionPoolAliasExists) {
    // Verify backward-compatible alias compiles (default ClosePolicy is AsyncClosePolicy)
    static_assert(std::is_same_v<ConnectionPool, BasicConnectionPool<std::chrono::steady_clock, AsyncClosePolicy>>,
                  "ConnectionPool should be alias for BasicConnectionPool<steady_clock, AsyncClosePolicy>");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
