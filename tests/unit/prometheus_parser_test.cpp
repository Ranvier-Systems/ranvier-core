// Ranvier Core - Prometheus Parser Unit Tests
//
// Tests for the lightweight Prometheus text format metric extraction
// used to scrape vLLM /metrics endpoints.

#include "prometheus_parser.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

using namespace ranvier;

// =============================================================================
// Basic Extraction
// =============================================================================

class PrometheusParserTest : public ::testing::Test {};

TEST_F(PrometheusParserTest, SimpleGauge) {
    const char* body = "vllm:num_requests_running 42\n";
    auto result = extract_prometheus_metric(body, "vllm:num_requests_running");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 42.0);
}

TEST_F(PrometheusParserTest, FloatingPointValue) {
    const char* body = "vllm:gpu_cache_usage_perc 0.753\n";
    auto result = extract_prometheus_metric(body, "vllm:gpu_cache_usage_perc");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 0.753);
}

TEST_F(PrometheusParserTest, LargeCounter) {
    const char* body = "vllm:prompt_tokens_total 123456789\n";
    auto result = extract_prometheus_metric(body, "vllm:prompt_tokens_total");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 123456789.0);
}

TEST_F(PrometheusParserTest, ZeroValue) {
    const char* body = "vllm:num_requests_waiting 0\n";
    auto result = extract_prometheus_metric(body, "vllm:num_requests_waiting");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 0.0);
}

TEST_F(PrometheusParserTest, ScientificNotation) {
    const char* body = "gpu_memory_used_bytes 2.8e+10\n";
    auto result = extract_prometheus_metric(body, "gpu_memory_used_bytes");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 2.8e+10);
}

// =============================================================================
// Labels (metric_name{labels} value)
// =============================================================================

TEST_F(PrometheusParserTest, MetricWithLabels) {
    const char* body =
        "vllm:num_requests_running{model=\"llama-3-70b\"} 8\n";
    auto result = extract_prometheus_metric(body, "vllm:num_requests_running");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 8.0);
}

TEST_F(PrometheusParserTest, MetricWithMultipleLabels) {
    const char* body =
        "vllm:avg_prompt_throughput_toks_per_s{model=\"llama\",gpu=\"0\"} 1250.3\n";
    auto result = extract_prometheus_metric(body, "vllm:avg_prompt_throughput_toks_per_s");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 1250.3);
}

// =============================================================================
// Comment and TYPE Lines
// =============================================================================

TEST_F(PrometheusParserTest, SkipsComments) {
    const char* body =
        "# HELP vllm:num_requests_running Number of running requests\n"
        "# TYPE vllm:num_requests_running gauge\n"
        "vllm:num_requests_running 5\n";
    auto result = extract_prometheus_metric(body, "vllm:num_requests_running");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 5.0);
}

TEST_F(PrometheusParserTest, SkipsEmptyLines) {
    const char* body =
        "\n"
        "\n"
        "vllm:num_requests_running 7\n"
        "\n";
    auto result = extract_prometheus_metric(body, "vllm:num_requests_running");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 7.0);
}

// =============================================================================
// Multiple Metrics (returns first match)
// =============================================================================

TEST_F(PrometheusParserTest, MultipleMetricsExtractsCorrectOne) {
    const char* body =
        "vllm:num_requests_running 3\n"
        "vllm:num_requests_waiting 12\n"
        "vllm:gpu_cache_usage_perc 0.65\n";

    auto running = extract_prometheus_metric(body, "vllm:num_requests_running");
    auto waiting = extract_prometheus_metric(body, "vllm:num_requests_waiting");
    auto cache = extract_prometheus_metric(body, "vllm:gpu_cache_usage_perc");

    ASSERT_TRUE(running.has_value());
    EXPECT_DOUBLE_EQ(*running, 3.0);

    ASSERT_TRUE(waiting.has_value());
    EXPECT_DOUBLE_EQ(*waiting, 12.0);

    ASSERT_TRUE(cache.has_value());
    EXPECT_DOUBLE_EQ(*cache, 0.65);
}

TEST_F(PrometheusParserTest, ReturnsFirstMatchWhenDuplicated) {
    const char* body =
        "vllm:num_requests_running{gpu=\"0\"} 3\n"
        "vllm:num_requests_running{gpu=\"1\"} 7\n";
    auto result = extract_prometheus_metric(body, "vllm:num_requests_running");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 3.0);  // First match
}

// =============================================================================
// Not Found
// =============================================================================

TEST_F(PrometheusParserTest, MetricNotFound) {
    const char* body = "vllm:num_requests_running 42\n";
    auto result = extract_prometheus_metric(body, "nonexistent_metric");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PrometheusParserTest, EmptyBody) {
    auto result = extract_prometheus_metric("", "vllm:num_requests_running");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PrometheusParserTest, OnlyComments) {
    const char* body =
        "# HELP some_metric A help line\n"
        "# TYPE some_metric gauge\n";
    auto result = extract_prometheus_metric(body, "some_metric");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Prefix Mismatch (must not match partial metric names)
// =============================================================================

TEST_F(PrometheusParserTest, DoesNotMatchPrefix) {
    // "vllm:num_requests_running_total" should NOT match "vllm:num_requests_running"
    const char* body = "vllm:num_requests_running_total 99\n";
    auto result = extract_prometheus_metric(body, "vllm:num_requests_running");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PrometheusParserTest, DoesNotMatchSuffix) {
    const char* body = "vllm:num_requests_running 10\n";
    // Searching for a longer name should not match
    auto result = extract_prometheus_metric(body, "vllm:num_requests_running_total");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Special Prometheus Values
// =============================================================================

TEST_F(PrometheusParserTest, PositiveInfinity) {
    const char* body = "some_metric +Inf\n";
    auto result = extract_prometheus_metric(body, "some_metric");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::isinf(*result));
    EXPECT_GT(*result, 0);
}

TEST_F(PrometheusParserTest, NegativeInfinity) {
    const char* body = "some_metric -Inf\n";
    auto result = extract_prometheus_metric(body, "some_metric");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::isinf(*result));
    EXPECT_LT(*result, 0);
}

TEST_F(PrometheusParserTest, NaN) {
    const char* body = "some_metric NaN\n";
    auto result = extract_prometheus_metric(body, "some_metric");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::isnan(*result));
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(PrometheusParserTest, NoTrailingNewline) {
    // Last line without \n should still be parsed
    const char* body = "vllm:num_requests_running 15";
    auto result = extract_prometheus_metric(body, "vllm:num_requests_running");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, 15.0);
}

TEST_F(PrometheusParserTest, NegativeValue) {
    const char* body = "some_gauge -3.14\n";
    auto result = extract_prometheus_metric(body, "some_gauge");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result, -3.14);
}

TEST_F(PrometheusParserTest, RealisticVllmOutput) {
    // Simulates a realistic (abbreviated) vLLM /metrics response
    const char* body =
        "# HELP vllm:num_requests_running Number of requests currently running on GPU.\n"
        "# TYPE vllm:num_requests_running gauge\n"
        "vllm:num_requests_running 8\n"
        "# HELP vllm:num_requests_waiting Number of requests waiting to be processed.\n"
        "# TYPE vllm:num_requests_waiting gauge\n"
        "vllm:num_requests_waiting 3\n"
        "# HELP vllm:gpu_cache_usage_perc GPU KV-cache usage. 1 means 100 percent usage.\n"
        "# TYPE vllm:gpu_cache_usage_perc gauge\n"
        "vllm:gpu_cache_usage_perc 0.654\n"
        "# HELP vllm:avg_prompt_throughput_toks_per_s Average prompt throughput.\n"
        "# TYPE vllm:avg_prompt_throughput_toks_per_s gauge\n"
        "vllm:avg_prompt_throughput_toks_per_s 1250.3\n"
        "# HELP vllm:avg_generation_throughput_toks_per_s Average generation throughput.\n"
        "# TYPE vllm:avg_generation_throughput_toks_per_s gauge\n"
        "vllm:avg_generation_throughput_toks_per_s 85.2\n"
        "# HELP vllm:prompt_tokens_total Total prompt tokens processed.\n"
        "# TYPE vllm:prompt_tokens_total counter\n"
        "vllm:prompt_tokens_total 9876543\n"
        "# HELP vllm:generation_tokens_total Total generation tokens produced.\n"
        "# TYPE vllm:generation_tokens_total counter\n"
        "vllm:generation_tokens_total 1234567\n"
        "# HELP gpu_memory_used_bytes GPU memory used.\n"
        "# TYPE gpu_memory_used_bytes gauge\n"
        "gpu_memory_used_bytes 30496071680\n"
        "# HELP gpu_memory_total_bytes GPU memory total.\n"
        "# TYPE gpu_memory_total_bytes gauge\n"
        "gpu_memory_total_bytes 42949672960\n";

    EXPECT_DOUBLE_EQ(*extract_prometheus_metric(body, "vllm:num_requests_running"), 8.0);
    EXPECT_DOUBLE_EQ(*extract_prometheus_metric(body, "vllm:num_requests_waiting"), 3.0);
    EXPECT_DOUBLE_EQ(*extract_prometheus_metric(body, "vllm:gpu_cache_usage_perc"), 0.654);
    EXPECT_DOUBLE_EQ(*extract_prometheus_metric(body, "vllm:avg_prompt_throughput_toks_per_s"), 1250.3);
    EXPECT_DOUBLE_EQ(*extract_prometheus_metric(body, "vllm:avg_generation_throughput_toks_per_s"), 85.2);
    EXPECT_DOUBLE_EQ(*extract_prometheus_metric(body, "vllm:prompt_tokens_total"), 9876543.0);
    EXPECT_DOUBLE_EQ(*extract_prometheus_metric(body, "vllm:generation_tokens_total"), 1234567.0);
    EXPECT_DOUBLE_EQ(*extract_prometheus_metric(body, "gpu_memory_used_bytes"), 30496071680.0);
    EXPECT_DOUBLE_EQ(*extract_prometheus_metric(body, "gpu_memory_total_bytes"), 42949672960.0);

    // Verify a metric that doesn't exist returns nullopt
    EXPECT_FALSE(extract_prometheus_metric(body, "nonexistent_metric").has_value());
}
