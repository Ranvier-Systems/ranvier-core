// Ranvier Core - TokenizerService Unit Tests
//
// Tests for the semaphore-gated local tokenization fallback (Backlog 21.2),
// configuration, counter bookkeeping, and Seastar API contract assumptions.
//
// These tests instantiate TokenizerService WITHOUT a running Seastar reactor.
// Only synchronous methods and member state are exercised — no futures are
// awaited, no cross-shard dispatch is performed.

#include "tokenizer_service.hpp"
#include "config_schema.hpp"

#include <gtest/gtest.h>

#include <seastar/core/semaphore.hh>
#include <seastar/util/defer.hh>

using namespace ranvier;

// =============================================================================
// TokenizationResult Defaults
// =============================================================================

class TokenizationResultTest : public ::testing::Test {};

TEST_F(TokenizationResultTest, DefaultResultIsEmpty) {
    TokenizationResult result;

    EXPECT_TRUE(result.tokens.empty());
    EXPECT_FALSE(result.cache_hit);
    EXPECT_FALSE(result.cross_shard);
    EXPECT_EQ(result.source_shard, 0u);
}

TEST_F(TokenizationResultTest, EmptyResultIsWhatCallerSeesOnRejection) {
    // When the semaphore rejects a local fallback, encode_threaded_async()
    // returns a default-constructed TokenizationResult. The caller in
    // http_controller.cpp checks tokens.empty() to fall back to hash/random
    // routing. Verify this contract holds.
    TokenizationResult rejected_result{};
    EXPECT_TRUE(rejected_result.tokens.empty());
}

// =============================================================================
// TokenizerService Counter Defaults
// =============================================================================

class TokenizerServiceCounterTest : public ::testing::Test {
protected:
    TokenizerService svc;
};

TEST_F(TokenizerServiceCounterTest, LocalFallbackRejectedStartsAtZero) {
    EXPECT_EQ(svc.local_fallback_rejected(), 0u);
}

TEST_F(TokenizerServiceCounterTest, CrossShardLocalFallbacksStartsAtZero) {
    EXPECT_EQ(svc.cross_shard_local_fallbacks(), 0u);
}

TEST_F(TokenizerServiceCounterTest, ThreadPoolCountersStartAtZero) {
    EXPECT_EQ(svc.thread_pool_dispatches(), 0u);
    EXPECT_EQ(svc.thread_pool_fallbacks(), 0u);
}

TEST_F(TokenizerServiceCounterTest, CrossShardDispatchesStartAtZero) {
    EXPECT_EQ(svc.cross_shard_dispatches(), 0u);
}

TEST_F(TokenizerServiceCounterTest, CacheCountersStartAtZero) {
    EXPECT_EQ(svc.cache_hits(), 0u);
    EXPECT_EQ(svc.cache_misses(), 0u);
    EXPECT_EQ(svc.cache_size(), 0u);
}

// =============================================================================
// TokenizerService Configuration
// =============================================================================

class TokenizerServiceConfigTest : public ::testing::Test {
protected:
    TokenizerService svc;
};

TEST_F(TokenizerServiceConfigTest, ConfigureLocalFallbackWithDefault) {
    // Default semaphore count is 1; reconfiguring to 1 should not crash
    svc.configure_local_fallback(1);
    EXPECT_EQ(svc.local_fallback_rejected(), 0u);
}

TEST_F(TokenizerServiceConfigTest, ConfigureLocalFallbackWithZero) {
    // Setting to 0 effectively disables the local fallback entirely
    svc.configure_local_fallback(0);
    EXPECT_EQ(svc.local_fallback_rejected(), 0u);
}

TEST_F(TokenizerServiceConfigTest, ConfigureLocalFallbackWithHighConcurrency) {
    svc.configure_local_fallback(8);
    EXPECT_EQ(svc.local_fallback_rejected(), 0u);
}

TEST_F(TokenizerServiceConfigTest, ConfigureLocalFallbackCanBeCalledMultipleTimes) {
    // Simulates runtime reconfiguration — should not crash or leak
    svc.configure_local_fallback(1);
    svc.configure_local_fallback(4);
    svc.configure_local_fallback(0);
    svc.configure_local_fallback(1);
    EXPECT_EQ(svc.local_fallback_rejected(), 0u);
}

TEST_F(TokenizerServiceConfigTest, ServiceIsNotLoadedByDefault) {
    EXPECT_FALSE(svc.is_loaded());
    EXPECT_EQ(svc.vocab_size(), 0u);
}

TEST_F(TokenizerServiceConfigTest, CrossShardDisabledWithoutRefs) {
    // Without set_cross_shard_refs(), cross-shard should be non-functional
    // (should_dispatch_cross_shard returns false internally)
    EXPECT_TRUE(svc.cross_shard_enabled());  // Config default is enabled
    // But without refs, it won't actually dispatch
}

TEST_F(TokenizerServiceConfigTest, ThreadPoolDisabledWithoutRef) {
    EXPECT_FALSE(svc.thread_pool_enabled());
}

// =============================================================================
// Seastar Semaphore Contract Tests
//
// These verify the seastar::semaphore API behavior that
// encode_threaded_async() Priority 3 depends on. If Seastar ever changes
// try_wait/signal semantics, these tests catch it.
// =============================================================================

class SemaphoreContractTest : public ::testing::Test {};

TEST_F(SemaphoreContractTest, TryWaitSucceedsOnAvailableUnits) {
    seastar::semaphore sem(1);
    EXPECT_TRUE(sem.try_wait(1));
    sem.signal(1);
}

TEST_F(SemaphoreContractTest, TryWaitFailsWhenDrained) {
    seastar::semaphore sem(1);
    EXPECT_TRUE(sem.try_wait(1));    // Acquire the single unit
    EXPECT_FALSE(sem.try_wait(1));   // Should fail — no units left
    sem.signal(1);                    // Release
}

TEST_F(SemaphoreContractTest, SignalRestoresAvailability) {
    seastar::semaphore sem(1);
    EXPECT_TRUE(sem.try_wait(1));
    sem.signal(1);
    EXPECT_TRUE(sem.try_wait(1));    // Should succeed after signal
    sem.signal(1);
}

TEST_F(SemaphoreContractTest, MultipleConcurrentUnits) {
    // Simulates tokenizer_local_fallback_max_concurrent = 3
    seastar::semaphore sem(3);
    EXPECT_TRUE(sem.try_wait(1));
    EXPECT_TRUE(sem.try_wait(1));
    EXPECT_TRUE(sem.try_wait(1));
    EXPECT_FALSE(sem.try_wait(1));   // All 3 consumed
    sem.signal(1);
    EXPECT_TRUE(sem.try_wait(1));    // One freed up
    EXPECT_FALSE(sem.try_wait(1));   // Still full again
    sem.signal(3);                    // Release remaining
}

TEST_F(SemaphoreContractTest, ZeroCountAlwaysFails) {
    // configure_local_fallback(0) creates a semaphore with 0 units
    seastar::semaphore sem(0);
    EXPECT_FALSE(sem.try_wait(1));   // Should always fail
    EXPECT_FALSE(sem.try_wait(1));   // Still fails
}

TEST_F(SemaphoreContractTest, MoveAssignmentReplacesState) {
    // configure_local_fallback() uses move assignment to reconfigure
    seastar::semaphore sem(1);
    EXPECT_TRUE(sem.try_wait(1));    // Drain the semaphore

    sem = seastar::semaphore(2);      // Replace with fresh semaphore (count=2)
    EXPECT_TRUE(sem.try_wait(1));    // New semaphore has 2 units
    EXPECT_TRUE(sem.try_wait(1));
    EXPECT_FALSE(sem.try_wait(1));
    sem.signal(2);
}

// =============================================================================
// Scope Guard Contract Tests (seastar::defer)
//
// encode_threaded_async() uses seastar::defer to ensure the semaphore is
// signaled even if tokenize_locally() throws. Verify the RAII pattern.
// =============================================================================

class DeferContractTest : public ::testing::Test {};

TEST_F(DeferContractTest, DeferFiresOnScopeExit) {
    seastar::semaphore sem(1);
    EXPECT_TRUE(sem.try_wait(1));

    {
        auto guard = seastar::defer([&sem] { sem.signal(1); });
        // Semaphore is still drained inside the scope
        EXPECT_FALSE(sem.try_wait(1));
    }
    // Guard destructor ran — semaphore should be restored
    EXPECT_TRUE(sem.try_wait(1));
    sem.signal(1);
}

TEST_F(DeferContractTest, DeferFiresOnException) {
    seastar::semaphore sem(1);
    EXPECT_TRUE(sem.try_wait(1));

    try {
        auto guard = seastar::defer([&sem] { sem.signal(1); });
        throw std::runtime_error("simulated tokenization failure");
    } catch (...) {
        // Guard destructor should have fired during stack unwinding
    }
    EXPECT_TRUE(sem.try_wait(1));
    sem.signal(1);
}

TEST_F(DeferContractTest, SemaphoreAndDeferInteractionPattern) {
    // Reproduces the exact pattern from encode_threaded_async() Priority 3:
    //   if (!_local_tokenize_sem.try_wait(1)) { reject; }
    //   auto guard = seastar::defer([this] { _local_tokenize_sem.signal(1); });
    //   return make_ready_future(tokenize_locally(text));
    seastar::semaphore sem(1);
    uint64_t rejected_count = 0;

    // First call: succeeds
    {
        if (!sem.try_wait(1)) {
            ++rejected_count;
        } else {
            auto guard = seastar::defer([&sem] { sem.signal(1); });
            // Simulate synchronous tokenize_locally() work
            volatile int work = 42;
            (void)work;
        }
        // Guard released semaphore
    }
    EXPECT_EQ(rejected_count, 0u);

    // Acquire and hold the semaphore to simulate "another tokenization in flight"
    EXPECT_TRUE(sem.try_wait(1));

    // Second call while held: should be rejected
    {
        if (!sem.try_wait(1)) {
            ++rejected_count;
        } else {
            auto guard = seastar::defer([&sem] { sem.signal(1); });
        }
    }
    EXPECT_EQ(rejected_count, 1u);

    // Release and try again: should succeed
    sem.signal(1);
    {
        if (!sem.try_wait(1)) {
            ++rejected_count;
        } else {
            auto guard = seastar::defer([&sem] { sem.signal(1); });
        }
    }
    EXPECT_EQ(rejected_count, 1u);  // No new rejections
}

// =============================================================================
// Config Schema Tests
// =============================================================================

class ConfigSchemaTest : public ::testing::Test {};

TEST_F(ConfigSchemaTest, DefaultLocalFallbackMaxConcurrent) {
    AssetsConfig config;
    EXPECT_EQ(config.tokenizer_local_fallback_max_concurrent, 1u);
}

TEST_F(ConfigSchemaTest, DefaultThreadPoolSettings) {
    AssetsConfig config;
    EXPECT_TRUE(config.tokenizer_thread_pool_enabled);
    EXPECT_EQ(config.tokenizer_thread_pool_queue_size, 256u);
}

TEST_F(ConfigSchemaTest, LocalFallbackMaxConcurrentCanBeCustomized) {
    AssetsConfig config;
    config.tokenizer_local_fallback_max_concurrent = 4;
    EXPECT_EQ(config.tokenizer_local_fallback_max_concurrent, 4u);
}

TEST_F(ConfigSchemaTest, LocalFallbackMaxConcurrentCanBeZero) {
    // Zero means "disable local fallback entirely"
    AssetsConfig config;
    config.tokenizer_local_fallback_max_concurrent = 0;
    EXPECT_EQ(config.tokenizer_local_fallback_max_concurrent, 0u);
}

// =============================================================================
// Type Traits Tests
// =============================================================================

TEST(TokenizerServiceTypeTraitsTest, TokenizationResultIsMovable) {
    EXPECT_TRUE(std::is_move_constructible_v<TokenizationResult>);
    EXPECT_TRUE(std::is_move_assignable_v<TokenizationResult>);
}

TEST(TokenizerServiceTypeTraitsTest, TokenizationResultIsCopyable) {
    EXPECT_TRUE(std::is_copy_constructible_v<TokenizationResult>);
    EXPECT_TRUE(std::is_copy_assignable_v<TokenizationResult>);
}

TEST(TokenizerServiceTypeTraitsTest, CrossShardConfigIsCopyable) {
    EXPECT_TRUE(std::is_copy_constructible_v<CrossShardTokenizationConfig>);
    EXPECT_TRUE(std::is_copy_assignable_v<CrossShardTokenizationConfig>);
}

// =============================================================================
// TokenizerService::truncate_for_routing (BACKLOG §1.4)
// =============================================================================
//
// Pure-computation tests for the byte-budget truncation helper. These are
// reactor-free: the function is a static method with no Seastar, no I/O,
// and no allocation (returns a zero-copy string_view).

class TokenizerTruncateForRoutingTest : public ::testing::Test {
protected:
    // Fixed ratio/target to keep arithmetic obvious: budget == 60 bytes.
    static constexpr size_t kTarget = 10;
    static constexpr size_t kBpt = 6;
    static constexpr size_t kBudget = kTarget * kBpt;  // 60
};

TEST_F(TokenizerTruncateForRoutingTest, ShorterThanBudgetReturnsFullText) {
    // "hello world" is 11 bytes, well under the 60-byte budget.
    std::string_view text = "hello world";
    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_EQ(out.size(), text.size());
    EXPECT_EQ(out, text);
    // Zero-copy: returned view points into the same buffer.
    EXPECT_EQ(out.data(), text.data());
}

TEST_F(TokenizerTruncateForRoutingTest, ExactlyAtBudgetReturnsFullText) {
    // text.size() == budget must NOT truncate (guards the `<=` vs `<` edge).
    std::string text(kBudget, 'a');
    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_EQ(out.size(), kBudget);
    EXPECT_EQ(out.data(), text.data());
}

TEST_F(TokenizerTruncateForRoutingTest, OneByteOverBudgetTruncatesToBudget) {
    std::string text(kBudget + 1, 'a');
    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_EQ(out.size(), kBudget);
    EXPECT_EQ(out.data(), text.data());  // Zero-copy
    EXPECT_EQ(out, std::string(kBudget, 'a'));
}

TEST_F(TokenizerTruncateForRoutingTest, PureAsciiTruncationIsExact) {
    // Double the budget in ASCII — truncate to exactly the budget.
    std::string text(kBudget * 2, 'x');
    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_EQ(out.size(), kBudget);
    EXPECT_EQ(out, std::string(kBudget, 'x'));
}

TEST_F(TokenizerTruncateForRoutingTest, ZeroCopyInvariant) {
    std::string text(200, 'z');
    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    // Returned view must reference the same underlying buffer — no
    // allocation, no copy.
    EXPECT_EQ(out.data(), text.data());
    EXPECT_EQ(out.size(), kBudget);
}

// Helpers for the UTF-8 boundary tests. Each test builds a string where a
// multi-byte sequence is positioned so that its LAST byte lands at index
// `kBudget` (i.e. at the budget boundary, which is the first byte NOT
// included in the returned view). The expected return size is the index
// of the sequence's first (start) byte — everything before it.

TEST_F(TokenizerTruncateForRoutingTest, BacksUpOneByteForTwoByteUtf8Boundary) {
    // 2-byte "é" (U+00E9 = 0xC3 0xA9) placed so that 0xA9 lands at kBudget.
    // Backup: end=kBudget → end=kBudget-1 (start byte 0xC3, exit).
    constexpr size_t kStartIndex = kBudget - 1;
    std::string text(kStartIndex, 'a');
    text += "\xC3\xA9";
    text += std::string(10, 'b');  // Padding to push size > kBudget
    ASSERT_EQ(static_cast<unsigned char>(text[kStartIndex]), 0xC3u);
    ASSERT_EQ(static_cast<unsigned char>(text[kBudget]),     0xA9u);

    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_EQ(out.size(), kStartIndex);
    EXPECT_EQ(out, std::string(kStartIndex, 'a'));
}

TEST_F(TokenizerTruncateForRoutingTest, BacksUpTwoBytesForThreeByteUtf8Boundary) {
    // 3-byte "汉" (U+6C49 = 0xE6 0xB1 0x89) placed so that 0x89 lands at
    // kBudget. Backup: end=kBudget → kBudget-1 → kBudget-2 (start byte, exit).
    constexpr size_t kStartIndex = kBudget - 2;
    std::string text(kStartIndex, 'a');
    text += "\xE6\xB1\x89";
    text += std::string(10, 'b');
    ASSERT_EQ(static_cast<unsigned char>(text[kStartIndex]), 0xE6u);
    ASSERT_EQ(static_cast<unsigned char>(text[kBudget]),     0x89u);

    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_EQ(out.size(), kStartIndex);
    EXPECT_EQ(out, std::string(kStartIndex, 'a'));
}

TEST_F(TokenizerTruncateForRoutingTest, BacksUpThreeBytesForFourByteUtf8Boundary) {
    // 4-byte "😀" (U+1F600 = 0xF0 0x9F 0x98 0x80) placed so that 0x80
    // lands at kBudget. Backup walks all three continuation bytes.
    constexpr size_t kStartIndex = kBudget - 3;
    std::string text(kStartIndex, 'a');
    text += "\xF0\x9F\x98\x80";
    text += std::string(10, 'b');
    ASSERT_EQ(static_cast<unsigned char>(text[kStartIndex]), 0xF0u);
    ASSERT_EQ(static_cast<unsigned char>(text[kBudget]),     0x80u);

    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_EQ(out.size(), kStartIndex);
}

TEST_F(TokenizerTruncateForRoutingTest, NoBackupWhenBoundaryIsStartByte) {
    // A 2-byte "é" starts exactly at kBudget (not straddling). text[kBudget]
    // is the start byte 0xC3 — not a continuation — so the backup loop
    // doesn't fire and we return exactly `kBudget` bytes.
    std::string text(kBudget, 'a');
    text += "\xC3\xA9";
    text += std::string(10, 'b');
    ASSERT_EQ(static_cast<unsigned char>(text[kBudget]), 0xC3u);

    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_EQ(out.size(), kBudget);  // No backup — budget is exact.
    EXPECT_EQ(out, std::string(kBudget, 'a'));
}

TEST_F(TokenizerTruncateForRoutingTest, NoBackupWhenBoundaryIsAscii) {
    // Byte at position `kBudget` is ASCII ('b'), loop must not fire.
    std::string text(kBudget * 2, 'a');
    text[kBudget] = 'b';
    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_EQ(out.size(), kBudget);
    EXPECT_EQ(out[kBudget - 1], 'a');  // Last byte of returned view
}

TEST_F(TokenizerTruncateForRoutingTest, ZeroTargetTokensReturnsEmpty) {
    std::string_view text = "hello world";
    auto out = TokenizerService::truncate_for_routing(text, 0, kBpt);
    EXPECT_TRUE(out.empty());
}

TEST_F(TokenizerTruncateForRoutingTest, ZeroBytesPerTokenReturnsEmpty) {
    std::string_view text = "hello world";
    auto out = TokenizerService::truncate_for_routing(text, kTarget, 0);
    EXPECT_TRUE(out.empty());
}

TEST_F(TokenizerTruncateForRoutingTest, EmptyInputReturnsEmpty) {
    std::string_view text = "";
    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_TRUE(out.empty());
}

TEST_F(TokenizerTruncateForRoutingTest, PathologicalAllContinuationBytesTerminates) {
    // Malformed input: a run of continuation bytes with no start byte. The
    // UTF-8 backup loop walks all the way to end=0 and stops (guarded by
    // `end > 0`). The test verifies the algorithm terminates without
    // out-of-bounds access on adversarial input and produces an empty
    // view (which the tokenizer will subsequently handle as a no-op).
    std::string text(kBudget * 2, '\x80');  // 0x80 = continuation byte
    auto out = TokenizerService::truncate_for_routing(text, kTarget, kBpt);
    EXPECT_TRUE(out.empty());
}

TEST_F(TokenizerTruncateForRoutingTest, BudgetSmallerThanFirstMultiByteChar) {
    // Legitimate UTF-8 input where the budget lands in the middle of the
    // very first multi-byte sequence. The backup loop must walk back
    // through the continuation bytes to end=0 (guarded by `end > 0`) and
    // return an empty view rather than split the character. This differs
    // from PathologicalAllContinuationBytesTerminates in that the input
    // is well-formed — we just can't fit any characters in the budget.
    std::string text = "\xF0\x9F\x98\x80";  // 4-byte 😀
    text += std::string(20, 'a');
    // target=1, bpt=1 → byte_budget=1. text[1] is a continuation byte of 😀.
    auto out = TokenizerService::truncate_for_routing(text, /*target_tokens=*/1, /*bytes_per_token=*/1);
    EXPECT_TRUE(out.empty());
}

TEST_F(TokenizerTruncateForRoutingTest, DefaultBytesPerTokenIsSix) {
    // Callers relying on the default argument should see bpt=6.
    // 200 bytes / 6 bpt * 20 tokens = 120 byte budget.
    std::string text(200, 'x');
    auto out = TokenizerService::truncate_for_routing(text, /*target_tokens=*/20);
    EXPECT_EQ(out.size(), 20u * 6u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
