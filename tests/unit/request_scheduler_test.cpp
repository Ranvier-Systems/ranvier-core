// Ranvier Core - Request Scheduler Unit Tests
//
// Tests for BasicRequestScheduler: priority ordering, fair scheduling,
// tier capacity bounds, overflow counters, and LRU agent eviction.
//
// Uses a lightweight TestContext stub to avoid Seastar dependencies.

#include "request_scheduler.hpp"
#include <gtest/gtest.h>

using namespace ranvier;

// =============================================================================
// Lightweight test context (no Seastar dependencies)
// =============================================================================

struct TestContext {
    PriorityLevel priority = PriorityLevel::NORMAL;
    std::string user_agent;
    std::string agent_id;  // Required by scheduler for pause-aware dequeue
    std::chrono::steady_clock::time_point enqueue_time;
    std::string request_id;
};

using TestScheduler = BasicRequestScheduler<TestContext>;

// Helper: create a context with given priority and agent
static std::unique_ptr<TestContext> make_ctx(PriorityLevel p, std::string agent = "default") {
    auto ctx = std::make_unique<TestContext>();
    ctx->priority = p;
    ctx->user_agent = std::move(agent);
    return ctx;
}

// =============================================================================
// Construction
// =============================================================================

TEST(RequestSchedulerTest, DefaultConstruction) {
    TestScheduler sched;
    EXPECT_EQ(sched.total_queued(), 0u);
    EXPECT_EQ(sched.total_enqueued_count(), 0u);
    EXPECT_EQ(sched.total_dequeued_count(), 0u);
    EXPECT_EQ(sched.agents_tracked(), 0u);
    auto depths = sched.queue_depths();
    for (auto d : depths) EXPECT_EQ(d, 0u);
}

TEST(RequestSchedulerTest, CustomCapacities) {
    SchedulerSettings settings;
    settings.tier_capacity = {2, 2, 2, 2};
    TestScheduler sched(settings);

    // Fill each tier to capacity
    for (int tier = 0; tier < 4; ++tier) {
        auto p = static_cast<PriorityLevel>(tier);
        EXPECT_TRUE(sched.enqueue(make_ctx(p, "a")));
        EXPECT_TRUE(sched.enqueue(make_ctx(p, "b")));
        EXPECT_FALSE(sched.enqueue(make_ctx(p, "c")));  // Overflow
    }
    EXPECT_EQ(sched.total_queued(), 8u);
}

// =============================================================================
// Priority ordering
// =============================================================================

TEST(RequestSchedulerTest, CriticalAlwaysDequeuesFirst) {
    TestScheduler sched;
    sched.enqueue(make_ctx(PriorityLevel::LOW, "low"));
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "normal"));
    sched.enqueue(make_ctx(PriorityLevel::CRITICAL, "critical"));
    sched.enqueue(make_ctx(PriorityLevel::HIGH, "high"));

    auto r = sched.dequeue();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->priority, PriorityLevel::CRITICAL);
}

TEST(RequestSchedulerTest, TierPriorityOrder) {
    TestScheduler sched;
    // Enqueue one per tier in reverse order
    sched.enqueue(make_ctx(PriorityLevel::LOW, "low"));
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "normal"));
    sched.enqueue(make_ctx(PriorityLevel::HIGH, "high"));
    sched.enqueue(make_ctx(PriorityLevel::CRITICAL, "critical"));

    // Dequeue order should be CRITICAL → HIGH → NORMAL → LOW
    PriorityLevel expected[] = {
        PriorityLevel::CRITICAL,
        PriorityLevel::HIGH,
        PriorityLevel::NORMAL,
        PriorityLevel::LOW,
    };
    for (auto exp : expected) {
        auto r = sched.dequeue();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ((*r)->priority, exp);
    }

    // Queue is now empty
    EXPECT_FALSE(sched.dequeue().has_value());
}

// =============================================================================
// Empty queue
// =============================================================================

TEST(RequestSchedulerTest, DequeueFromEmptyReturnsNullopt) {
    TestScheduler sched;
    EXPECT_FALSE(sched.dequeue().has_value());
}

// =============================================================================
// Fair scheduling within a tier
// =============================================================================

TEST(RequestSchedulerTest, FairSchedulingAlternatesAgents) {
    TestScheduler sched;
    // Enqueue: 3 from agent A, 3 from agent B, all NORMAL
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "agent-A"));
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "agent-A"));
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "agent-A"));
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "agent-B"));
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "agent-B"));
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "agent-B"));

    // First dequeue: agent-A or agent-B (both unseen, takes first found = agent-A)
    auto r1 = sched.dequeue();
    ASSERT_TRUE(r1.has_value());
    auto first_agent = (*r1)->user_agent;

    // Second dequeue: should pick the OTHER agent (fair scheduling)
    auto r2 = sched.dequeue();
    ASSERT_TRUE(r2.has_value());
    EXPECT_NE((*r2)->user_agent, first_agent);

    // Third dequeue: should alternate back
    auto r3 = sched.dequeue();
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ((*r3)->user_agent, first_agent);
}

TEST(RequestSchedulerTest, CriticalSkipsFairScheduling) {
    TestScheduler sched;
    // CRITICAL always pops front, no fair scheduling scan
    sched.enqueue(make_ctx(PriorityLevel::CRITICAL, "agent-A"));
    sched.enqueue(make_ctx(PriorityLevel::CRITICAL, "agent-A"));
    sched.enqueue(make_ctx(PriorityLevel::CRITICAL, "agent-B"));

    // All three should dequeue in FIFO order, not alternating
    auto r1 = sched.dequeue();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ((*r1)->user_agent, "agent-A");

    auto r2 = sched.dequeue();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ((*r2)->user_agent, "agent-A");

    auto r3 = sched.dequeue();
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ((*r3)->user_agent, "agent-B");
}

// =============================================================================
// Tier capacity and overflow
// =============================================================================

TEST(RequestSchedulerTest, OverflowDropsTrackedPerTier) {
    SchedulerSettings settings;
    settings.tier_capacity = {1, 1, 1, 1};
    TestScheduler sched(settings);

    // Fill each tier
    EXPECT_TRUE(sched.enqueue(make_ctx(PriorityLevel::CRITICAL)));
    EXPECT_TRUE(sched.enqueue(make_ctx(PriorityLevel::HIGH)));
    EXPECT_TRUE(sched.enqueue(make_ctx(PriorityLevel::NORMAL)));
    EXPECT_TRUE(sched.enqueue(make_ctx(PriorityLevel::LOW)));

    // Overflow each tier
    EXPECT_FALSE(sched.enqueue(make_ctx(PriorityLevel::CRITICAL)));
    EXPECT_FALSE(sched.enqueue(make_ctx(PriorityLevel::HIGH)));
    EXPECT_FALSE(sched.enqueue(make_ctx(PriorityLevel::HIGH)));
    EXPECT_FALSE(sched.enqueue(make_ctx(PriorityLevel::LOW)));

    auto drops = sched.overflow_drops();
    EXPECT_EQ(drops[0], 1u);  // CRITICAL
    EXPECT_EQ(drops[1], 2u);  // HIGH
    EXPECT_EQ(drops[2], 0u);  // NORMAL — no overflow
    EXPECT_EQ(drops[3], 1u);  // LOW
}

TEST(RequestSchedulerTest, ZeroCapacityUsesFallbackDefault) {
    SchedulerSettings settings;
    settings.tier_capacity = {0, 0, 0, 0};
    TestScheduler sched(settings);

    // Should use DEFAULT_TIER_CAPACITY (512), so enqueueing a few should succeed
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(sched.enqueue(make_ctx(PriorityLevel::NORMAL)));
    }
    EXPECT_EQ(sched.total_queued(), 100u);
}

// =============================================================================
// Counters
// =============================================================================

TEST(RequestSchedulerTest, EnqueueDequeueCountersAccurate) {
    TestScheduler sched;
    sched.enqueue(make_ctx(PriorityLevel::NORMAL));
    sched.enqueue(make_ctx(PriorityLevel::HIGH));
    sched.enqueue(make_ctx(PriorityLevel::LOW));
    EXPECT_EQ(sched.total_enqueued_count(), 3u);
    EXPECT_EQ(sched.total_dequeued_count(), 0u);

    sched.dequeue();
    sched.dequeue();
    EXPECT_EQ(sched.total_enqueued_count(), 3u);
    EXPECT_EQ(sched.total_dequeued_count(), 2u);
    EXPECT_EQ(sched.total_queued(), 1u);
}

TEST(RequestSchedulerTest, QueueDepthsPerTier) {
    TestScheduler sched;
    sched.enqueue(make_ctx(PriorityLevel::CRITICAL));
    sched.enqueue(make_ctx(PriorityLevel::CRITICAL));
    sched.enqueue(make_ctx(PriorityLevel::HIGH));
    sched.enqueue(make_ctx(PriorityLevel::LOW));
    sched.enqueue(make_ctx(PriorityLevel::LOW));
    sched.enqueue(make_ctx(PriorityLevel::LOW));

    auto depths = sched.queue_depths();
    EXPECT_EQ(depths[0], 2u);  // CRITICAL
    EXPECT_EQ(depths[1], 1u);  // HIGH
    EXPECT_EQ(depths[2], 0u);  // NORMAL
    EXPECT_EQ(depths[3], 3u);  // LOW
}

// =============================================================================
// Agent tracking and LRU eviction
// =============================================================================

TEST(RequestSchedulerTest, AgentsTrackedAfterDequeue) {
    TestScheduler sched;
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "agent-X"));
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "agent-Y"));
    EXPECT_EQ(sched.agents_tracked(), 0u);  // Not tracked until dequeued

    sched.dequeue();
    EXPECT_EQ(sched.agents_tracked(), 1u);

    sched.dequeue();
    EXPECT_EQ(sched.agents_tracked(), 2u);
}

TEST(RequestSchedulerTest, UnknownAgentUsedWhenUserAgentEmpty) {
    TestScheduler sched;
    auto ctx = std::make_unique<TestContext>();
    ctx->priority = PriorityLevel::NORMAL;
    // user_agent left empty
    sched.enqueue(std::move(ctx));
    sched.dequeue();

    EXPECT_EQ(sched.agents_tracked(), 1u);
}

TEST(RequestSchedulerTest, EnqueueSetsEnqueueTime) {
    TestScheduler sched;
    auto before = std::chrono::steady_clock::now();
    sched.enqueue(make_ctx(PriorityLevel::NORMAL));

    // Dequeue and verify enqueue_time was set
    auto result = sched.dequeue();
    ASSERT_TRUE(result.has_value());
    EXPECT_GE((*result)->enqueue_time, before);
    EXPECT_LE((*result)->enqueue_time, std::chrono::steady_clock::now());
}

// =============================================================================
// Invalid priority fallback
// =============================================================================

TEST(RequestSchedulerTest, InvalidPriorityFallsBackToNormal) {
    TestScheduler sched;
    auto ctx = std::make_unique<TestContext>();
    ctx->priority = static_cast<PriorityLevel>(99);  // Invalid tier
    EXPECT_TRUE(sched.enqueue(std::move(ctx)));

    // Should be in the NORMAL queue (index 2)
    auto depths = sched.queue_depths();
    EXPECT_EQ(depths[2], 1u);
}

TEST(RequestSchedulerTest, AgentTrackingNeverExceedsLimit) {
    // Use a small capacity scheduler to test LRU eviction
    SchedulerSettings settings;
    settings.tier_capacity = {512, 512, 512, 512};
    TestScheduler sched(settings);

    // Enqueue and dequeue 300 distinct agents (exceeds MAX_AGENT_TRACKING = 256)
    for (int i = 0; i < 300; ++i) {
        sched.enqueue(make_ctx(PriorityLevel::NORMAL, "agent-" + std::to_string(i)));
    }
    for (int i = 0; i < 300; ++i) {
        sched.dequeue();
    }

    // Agent tracking should be capped at 256
    EXPECT_LE(sched.agents_tracked(), 256u);
}

TEST(RequestSchedulerTest, ExistingAgentUpdatedInPlace) {
    TestScheduler sched;
    // Dequeue the same agent twice — should update, not add a second entry
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "repeat"));
    sched.dequeue();
    EXPECT_EQ(sched.agents_tracked(), 1u);

    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "repeat"));
    sched.dequeue();
    EXPECT_EQ(sched.agents_tracked(), 1u);  // Still 1, not 2
}

TEST(RequestSchedulerTest, QueueDepthSingleTierAccessor) {
    TestScheduler sched;
    sched.enqueue(make_ctx(PriorityLevel::HIGH));
    sched.enqueue(make_ctx(PriorityLevel::HIGH));
    sched.enqueue(make_ctx(PriorityLevel::LOW));

    EXPECT_EQ(sched.queue_depth(0), 0u);  // CRITICAL
    EXPECT_EQ(sched.queue_depth(1), 2u);  // HIGH
    EXPECT_EQ(sched.queue_depth(2), 0u);  // NORMAL
    EXPECT_EQ(sched.queue_depth(3), 1u);  // LOW
    EXPECT_EQ(sched.queue_depth(99), 0u); // Out of bounds
}

// =============================================================================
// Pause-aware dequeue
// =============================================================================

// Helper: create a context with both user_agent and agent_id set
static std::unique_ptr<TestContext> make_agent_ctx(PriorityLevel p,
                                                   std::string agent_id,
                                                   std::string user_agent = "") {
    auto ctx = std::make_unique<TestContext>();
    ctx->priority = p;
    ctx->agent_id = agent_id;
    ctx->user_agent = user_agent.empty() ? agent_id : std::move(user_agent);
    return ctx;
}

TEST(RequestSchedulerTest, PausedAgentSkippedDuringDequeue) {
    TestScheduler sched;
    sched.set_pause_check([](std::string_view aid) {
        return aid == "paused-agent";
    });

    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "paused-agent"));
    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "active-agent"));

    auto r = sched.dequeue();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->agent_id, "active-agent");

    // Paused agent's request is still in the queue
    EXPECT_EQ(sched.total_queued(), 1u);
}

TEST(RequestSchedulerTest, CriticalTierRespectsPauseCheck) {
    TestScheduler sched;
    sched.set_pause_check([](std::string_view) {
        return true;  // Everything is "paused"
    });

    sched.enqueue(make_agent_ctx(PriorityLevel::CRITICAL, "paused-agent"));

    // Admin pause is authoritative — even CRITICAL tier respects it
    auto r = sched.dequeue();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(sched.total_queued(), 1u);
    EXPECT_GT(sched.paused_skips(), 0u);
}

TEST(RequestSchedulerTest, CriticalTierDequeuesNonPausedAgent) {
    TestScheduler sched;
    sched.set_pause_check([](std::string_view aid) {
        return aid == "paused-agent";
    });

    sched.enqueue(make_agent_ctx(PriorityLevel::CRITICAL, "paused-agent"));
    sched.enqueue(make_agent_ctx(PriorityLevel::CRITICAL, "active-agent"));

    // Non-paused CRITICAL request still dequeues immediately
    auto r = sched.dequeue();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->agent_id, "active-agent");
    // Paused request remains in queue
    EXPECT_EQ(sched.total_queued(), 1u);
}

TEST(RequestSchedulerTest, AllPausedTierSkippedToNextTier) {
    TestScheduler sched;
    sched.set_pause_check([](std::string_view aid) {
        return aid == "paused-agent";
    });

    // HIGH tier: only paused agent
    sched.enqueue(make_agent_ctx(PriorityLevel::HIGH, "paused-agent"));
    // NORMAL tier: active agent
    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "active-agent"));

    // Should skip HIGH (all paused) and dequeue from NORMAL
    auto r = sched.dequeue();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->priority, PriorityLevel::NORMAL);
    EXPECT_EQ((*r)->agent_id, "active-agent");
}

TEST(RequestSchedulerTest, PausedCriticalFallsThroughToLowerTier) {
    TestScheduler sched;
    sched.set_pause_check([](std::string_view aid) {
        return aid == "paused-agent";
    });

    // CRITICAL tier: only paused agent
    sched.enqueue(make_agent_ctx(PriorityLevel::CRITICAL, "paused-agent"));
    // HIGH tier: active agent
    sched.enqueue(make_agent_ctx(PriorityLevel::HIGH, "active-agent"));

    // Should skip CRITICAL (all paused) and dequeue from HIGH
    auto r = sched.dequeue();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->priority, PriorityLevel::HIGH);
    EXPECT_EQ((*r)->agent_id, "active-agent");
    // Paused CRITICAL request stays queued
    EXPECT_EQ(sched.total_queued(), 1u);
}

TEST(RequestSchedulerTest, AllPausedReturnsNullopt) {
    TestScheduler sched;
    sched.set_pause_check([](std::string_view) {
        return true;  // Everything paused
    });

    sched.enqueue(make_agent_ctx(PriorityLevel::HIGH, "agent-a"));
    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-b"));

    // All paused → nullopt (pause applies to all tiers including CRITICAL)
    EXPECT_FALSE(sched.dequeue().has_value());
    // Requests remain in queue
    EXPECT_EQ(sched.total_queued(), 2u);
}

TEST(RequestSchedulerTest, ResumedAgentDrainsNaturally) {
    bool paused = true;
    TestScheduler sched;
    sched.set_pause_check([&paused](std::string_view) {
        return paused;
    });

    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-a"));

    // While paused: nothing dequeues
    EXPECT_FALSE(sched.dequeue().has_value());

    // Resume
    paused = false;

    // Now it dequeues
    auto r = sched.dequeue();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->agent_id, "agent-a");
}

TEST(RequestSchedulerTest, PausedSkipsCounterIncrements) {
    TestScheduler sched;
    sched.set_pause_check([](std::string_view aid) {
        return aid == "paused-agent";
    });

    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "paused-agent"));
    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "active-agent"));

    EXPECT_EQ(sched.paused_skips(), 0u);
    sched.dequeue();
    EXPECT_GT(sched.paused_skips(), 0u);
}

TEST(RequestSchedulerTest, NoPauseCheckSetBehavesAsBeforeChange) {
    TestScheduler sched;
    // No set_pause_check() call — should dequeue normally

    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-a"));
    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-b"));

    auto r1 = sched.dequeue();
    ASSERT_TRUE(r1.has_value());
    auto r2 = sched.dequeue();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(sched.total_queued(), 0u);
    EXPECT_EQ(sched.paused_skips(), 0u);
}

TEST(RequestSchedulerTest, EmptyAgentIdNeverPauseChecked) {
    TestScheduler sched;
    bool pause_called = false;
    sched.set_pause_check([&pause_called](std::string_view) {
        pause_called = true;
        return true;
    });

    // Context with empty agent_id (unidentified agent)
    sched.enqueue(make_ctx(PriorityLevel::NORMAL, "some-user-agent"));

    auto r = sched.dequeue();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(pause_called);
}

TEST(RequestSchedulerTest, DrainOneIgnoresPauseCheck) {
    TestScheduler sched;
    sched.set_pause_check([](std::string_view) {
        return true;  // Everything paused
    });

    sched.enqueue(make_agent_ctx(PriorityLevel::CRITICAL, "agent-a"));
    sched.enqueue(make_agent_ctx(PriorityLevel::HIGH, "agent-b"));

    // dequeue() returns nullopt (all paused)
    EXPECT_FALSE(sched.dequeue().has_value());

    // drain_one() ignores pause — used for shutdown cleanup
    auto r1 = sched.drain_one();
    ASSERT_TRUE(r1.has_value());
    auto r2 = sched.drain_one();
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(sched.drain_one().has_value());  // Empty
}

// =============================================================================
// Per-agent queue depth limit
// =============================================================================

TEST(RequestSchedulerTest, PerAgentQueueLimitEnforced) {
    SchedulerSettings settings;
    settings.max_per_agent_queued = 3;
    TestScheduler sched(settings);

    EXPECT_TRUE(sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-a")));
    EXPECT_TRUE(sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-a")));
    EXPECT_TRUE(sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-a")));
    // 4th should be rejected
    EXPECT_FALSE(sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-a")));
    EXPECT_EQ(sched.per_agent_drops(), 1u);

    // Different agent is unaffected
    EXPECT_TRUE(sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-b")));
    EXPECT_EQ(sched.per_agent_drops(), 1u);
}

TEST(RequestSchedulerTest, PerAgentLimitCountsAcrossTiers) {
    SchedulerSettings settings;
    settings.max_per_agent_queued = 2;
    TestScheduler sched(settings);

    EXPECT_TRUE(sched.enqueue(make_agent_ctx(PriorityLevel::HIGH, "agent-a")));
    EXPECT_TRUE(sched.enqueue(make_agent_ctx(PriorityLevel::LOW, "agent-a")));
    // 3rd in a different tier still exceeds the per-agent limit
    EXPECT_FALSE(sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-a")));
    EXPECT_EQ(sched.per_agent_drops(), 1u);
}

TEST(RequestSchedulerTest, PerAgentLimitNotAppliedToEmptyAgentId) {
    SchedulerSettings settings;
    settings.max_per_agent_queued = 1;
    TestScheduler sched(settings);

    // Empty agent_id bypasses per-agent check
    EXPECT_TRUE(sched.enqueue(make_ctx(PriorityLevel::NORMAL, "ua1")));
    EXPECT_TRUE(sched.enqueue(make_ctx(PriorityLevel::NORMAL, "ua2")));
    EXPECT_EQ(sched.per_agent_drops(), 0u);
}

TEST(RequestSchedulerTest, PerAgentLimitFreesAfterDequeue) {
    SchedulerSettings settings;
    settings.max_per_agent_queued = 1;
    TestScheduler sched(settings);

    EXPECT_TRUE(sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-a")));
    EXPECT_FALSE(sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-a")));

    // Dequeue frees the slot
    sched.dequeue();
    EXPECT_TRUE(sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "agent-a")));
}

// =============================================================================
// queue_depths_by_agent()
// =============================================================================

TEST(RequestSchedulerTest, QueueDepthsByAgentAccurate) {
    TestScheduler sched;
    sched.enqueue(make_agent_ctx(PriorityLevel::HIGH, "cursor-ide"));
    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "cursor-ide"));
    sched.enqueue(make_agent_ctx(PriorityLevel::NORMAL, "claude-code"));
    sched.enqueue(make_ctx(PriorityLevel::LOW, "raw-ua"));  // empty agent_id

    auto depths = sched.queue_depths_by_agent();
    EXPECT_EQ(depths.size(), 2u);  // Only entries with non-empty agent_id
    EXPECT_EQ(depths["cursor-ide"], 2u);
    EXPECT_EQ(depths["claude-code"], 1u);
}

TEST(RequestSchedulerTest, QueueDepthsByAgentEmptyWhenQueueEmpty) {
    TestScheduler sched;
    auto depths = sched.queue_depths_by_agent();
    EXPECT_TRUE(depths.empty());
}

// =============================================================================
// PriorityLevel utilities
// =============================================================================

TEST(PriorityLevelTest, ToStringCoversAllLevels) {
    EXPECT_STREQ(priority_level_to_string(PriorityLevel::CRITICAL), "critical");
    EXPECT_STREQ(priority_level_to_string(PriorityLevel::HIGH), "high");
    EXPECT_STREQ(priority_level_to_string(PriorityLevel::NORMAL), "normal");
    EXPECT_STREQ(priority_level_to_string(PriorityLevel::LOW), "low");
}
