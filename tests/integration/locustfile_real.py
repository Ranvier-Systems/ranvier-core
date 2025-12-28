#!/usr/bin/env python3
"""
Real vLLM Backend Load Testing for Ranvier Core

This load test measures the actual value proposition of prefix-aware routing:
1. Cache hit rate - requests routed to same backend for shared prefix
2. TTFT comparison - cache hit vs. cache miss latency
3. Tokens per second throughput
4. Usage statistics from SSE responses

Key Metrics:
- TTFT (Time To First Token): Latency until first token arrives
- Cache Hit Rate: Percentage of requests hitting warm KV cache
- Token Throughput: Tokens generated per second
- Routing Accuracy: Whether router correctly identified prefix locality

Usage:
    # With external vLLM endpoints:
    VLLM_ENDPOINT_1=http://server1:8000 VLLM_ENDPOINT_2=http://server2:8000 \
    make benchmark-real

    # With local vLLM containers (requires GPU):
    make benchmark-real-local
"""

import json
import logging
import os
import random
import re
import time
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple
from threading import Lock

import requests
from locust import HttpUser, task, between, events
from locust.runners import MasterRunner, WorkerRunner

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# ============================================================================
# Environment Configuration
# ============================================================================

RANVIER_NODES = [
    os.environ.get("RANVIER_NODE1", "http://172.29.2.1:8080"),
    os.environ.get("RANVIER_NODE2", "http://172.29.2.2:8080"),
    os.environ.get("RANVIER_NODE3", "http://172.29.2.3:8080"),
]

RANVIER_METRICS = [
    os.environ.get("RANVIER_METRICS1", "http://172.29.2.1:9180"),
    os.environ.get("RANVIER_METRICS2", "http://172.29.2.2:9180"),
    os.environ.get("RANVIER_METRICS3", "http://172.29.2.3:9180"),
]

# Support single-backend mode for single-GPU testing
SINGLE_BACKEND_MODE = os.environ.get("SINGLE_BACKEND_MODE", "false").lower() == "true"

if SINGLE_BACKEND_MODE:
    BACKENDS = [
        {
            "id": 1,
            "ip": os.environ.get("BACKEND1_IP", "172.29.1.10"),
            "port": int(os.environ.get("BACKEND1_PORT", "8000")),
        },
    ]
else:
    BACKENDS = [
        {
            "id": 1,
            "ip": os.environ.get("BACKEND1_IP", "172.29.1.10"),
            "port": int(os.environ.get("BACKEND1_PORT", "8000")),
        },
        {
            "id": 2,
            "ip": os.environ.get("BACKEND2_IP", "172.29.1.11"),
            "port": int(os.environ.get("BACKEND2_PORT", "8000")),
        },
    ]

# Benchmark mode: "prefix" for prefix-aware, "round_robin" for baseline
BENCHMARK_MODE = os.environ.get("BENCHMARK_MODE", "prefix")

# Prompt distribution: "mixed", "short", "medium", "long"
PROMPT_DISTRIBUTION = os.environ.get("PROMPT_DISTRIBUTION", "mixed")

# Ratio of requests that should share a prefix (0.0-1.0)
SHARED_PREFIX_RATIO = float(os.environ.get("SHARED_PREFIX_RATIO", "0.7"))

# Configurable thresholds
P99_LATENCY_THRESHOLD_MS = float(os.environ.get("P99_LATENCY_THRESHOLD_MS", "5000"))

# ============================================================================
# Prompt Templates with Shared Prefixes
# ============================================================================

# System prompts that will be shared across multiple requests
SYSTEM_PROMPTS = {
    "coding": """You are an expert software engineer. You write clean, efficient,
and well-documented code. You follow best practices and design patterns.
You always explain your reasoning step by step.""",

    "analysis": """You are a data analyst expert. You analyze data carefully,
identify patterns and trends, and provide actionable insights.
You use statistical methods and visualizations when appropriate.""",

    "writing": """You are a professional writer and editor. You craft clear,
engaging, and grammatically correct content. You adapt your style
to match the target audience and purpose.""",

    "math": """You are a mathematics tutor. You explain concepts clearly,
work through problems step by step, and provide multiple examples.
You check your work and verify your calculations.""",
}

# Short prompts (< 100 tokens)
SHORT_PROMPTS = [
    "Write a Python function to calculate factorial.",
    "Explain what a hash table is in one paragraph.",
    "What is the Big O complexity of quicksort?",
    "Fix this code: def add(a,b) return a+b",
    "What is the difference between == and === in JavaScript?",
]

# Medium prompts (100-500 tokens) - include shared prefix
MEDIUM_PROMPTS = [
    ("coding", "I'm building a REST API in Python using FastAPI. I need to implement user authentication with JWT tokens. Please show me how to create the login endpoint and middleware for protected routes."),
    ("coding", "I'm building a REST API in Python using FastAPI. I need to add rate limiting to prevent abuse. Show me how to implement a token bucket algorithm."),
    ("coding", "I'm building a REST API in Python using FastAPI. I need to set up database connections with SQLAlchemy. Show me the best practices for connection pooling."),
    ("analysis", "I have a dataset of customer transactions with columns: customer_id, timestamp, amount, category. I want to identify customers who are likely to churn. What features should I engineer?"),
    ("analysis", "I have a dataset of customer transactions with columns: customer_id, timestamp, amount, category. I want to segment customers into groups. What clustering algorithm would you recommend?"),
    ("analysis", "I have a dataset of customer transactions with columns: customer_id, timestamp, amount, category. I want to detect fraudulent transactions. What anomaly detection approach should I use?"),
]

# Long prompts (500+ tokens) - include shared prefix
LONG_PROMPTS = [
    ("coding", """I'm working on a microservices architecture for an e-commerce platform.
The system has the following services:
- User Service: handles authentication and user profiles
- Product Service: manages product catalog and inventory
- Order Service: processes orders and payments
- Notification Service: sends emails and push notifications

Currently, services communicate synchronously via REST APIs. I'm experiencing issues with:
1. Cascading failures when one service is down
2. High latency for operations that span multiple services
3. Difficulty maintaining data consistency across services

Please help me design a better communication pattern using message queues.
Include specific technology recommendations and implementation details."""),

    ("coding", """I'm working on a microservices architecture for an e-commerce platform.
The system has the following services:
- User Service: handles authentication and user profiles
- Product Service: manages product catalog and inventory
- Order Service: processes orders and payments
- Notification Service: sends emails and push notifications

I need to implement distributed tracing to debug issues across services.
What tools and patterns should I use? Please provide a detailed implementation plan."""),

    ("analysis", """I'm analyzing website traffic data for an e-commerce site. The data includes:
- Session data: session_id, user_id, start_time, end_time, device_type, browser
- Page views: session_id, page_url, timestamp, time_on_page
- Events: session_id, event_type, event_data, timestamp
- Purchases: session_id, order_id, products, total_amount

I want to build a model to predict which sessions will result in a purchase.
Please walk me through:
1. Data preprocessing and feature engineering
2. Model selection and training approach
3. Evaluation metrics and validation strategy
4. How to deploy and monitor the model in production"""),
]

# ============================================================================
# Metrics Collection
# ============================================================================

@dataclass
class RequestMetrics:
    """Metrics collected for each request."""
    request_id: str
    prompt_prefix_hash: str
    backend_id: Optional[str] = None
    ttft_ms: Optional[float] = None
    total_time_ms: Optional[float] = None
    prompt_tokens: int = 0
    completion_tokens: int = 0
    is_cache_hit: bool = False
    routing_mode: str = "unknown"


@dataclass
class BenchmarkStats:
    """Aggregated benchmark statistics."""
    total_requests: int = 0
    successful_requests: int = 0
    failed_requests: int = 0

    # Cache hit tracking
    cache_hits: int = 0
    cache_misses: int = 0

    # TTFT by cache status
    ttft_cache_hit: List[float] = field(default_factory=list)
    ttft_cache_miss: List[float] = field(default_factory=list)

    # Token throughput
    total_prompt_tokens: int = 0
    total_completion_tokens: int = 0
    total_generation_time_s: float = 0.0

    # Routing tracking: prefix_hash -> backend_id
    prefix_to_backend: Dict[str, str] = field(default_factory=dict)

    # Lock for thread safety
    _lock: Lock = field(default_factory=Lock)

    def record_request(self, metrics: RequestMetrics):
        """Record metrics from a completed request."""
        with self._lock:
            self.total_requests += 1

            if metrics.ttft_ms is None:
                self.failed_requests += 1
                return

            self.successful_requests += 1
            self.total_prompt_tokens += metrics.prompt_tokens
            self.total_completion_tokens += metrics.completion_tokens

            if metrics.total_time_ms:
                self.total_generation_time_s += metrics.total_time_ms / 1000.0

            # Track cache hits based on prefix routing
            if metrics.prompt_prefix_hash:
                expected_backend = self.prefix_to_backend.get(metrics.prompt_prefix_hash)

                if expected_backend is None:
                    # First request with this prefix - cache miss
                    self.prefix_to_backend[metrics.prompt_prefix_hash] = metrics.backend_id
                    self.cache_misses += 1
                    self.ttft_cache_miss.append(metrics.ttft_ms)
                    metrics.is_cache_hit = False
                elif expected_backend == metrics.backend_id:
                    # Same backend - cache hit
                    self.cache_hits += 1
                    self.ttft_cache_hit.append(metrics.ttft_ms)
                    metrics.is_cache_hit = True
                else:
                    # Different backend - cache miss (routing failure)
                    self.cache_misses += 1
                    self.ttft_cache_miss.append(metrics.ttft_ms)
                    metrics.is_cache_hit = False

    def get_summary(self) -> dict:
        """Generate summary statistics."""
        with self._lock:
            cache_hit_rate = 0.0
            if self.cache_hits + self.cache_misses > 0:
                cache_hit_rate = self.cache_hits / (self.cache_hits + self.cache_misses) * 100

            ttft_hit_p50 = self._percentile(self.ttft_cache_hit, 0.50)
            ttft_hit_p99 = self._percentile(self.ttft_cache_hit, 0.99)
            ttft_miss_p50 = self._percentile(self.ttft_cache_miss, 0.50)
            ttft_miss_p99 = self._percentile(self.ttft_cache_miss, 0.99)

            tokens_per_sec = 0.0
            if self.total_generation_time_s > 0:
                tokens_per_sec = self.total_completion_tokens / self.total_generation_time_s

            return {
                "total_requests": self.total_requests,
                "successful_requests": self.successful_requests,
                "failed_requests": self.failed_requests,
                "cache_hits": self.cache_hits,
                "cache_misses": self.cache_misses,
                "cache_hit_rate_pct": cache_hit_rate,
                "ttft_cache_hit_p50_ms": ttft_hit_p50,
                "ttft_cache_hit_p99_ms": ttft_hit_p99,
                "ttft_cache_miss_p50_ms": ttft_miss_p50,
                "ttft_cache_miss_p99_ms": ttft_miss_p99,
                "ttft_improvement_pct": self._calculate_improvement(ttft_miss_p50, ttft_hit_p50),
                "total_prompt_tokens": self.total_prompt_tokens,
                "total_completion_tokens": self.total_completion_tokens,
                "tokens_per_second": tokens_per_sec,
                "unique_prefixes": len(self.prefix_to_backend),
            }

    def _percentile(self, data: List[float], p: float) -> Optional[float]:
        """Calculate percentile of data."""
        if not data:
            return None
        sorted_data = sorted(data)
        idx = int(len(sorted_data) * p)
        return sorted_data[min(idx, len(sorted_data) - 1)]

    def _calculate_improvement(self, baseline: Optional[float], improved: Optional[float]) -> Optional[float]:
        """Calculate percentage improvement (positive = faster)."""
        if baseline is None or improved is None or baseline == 0:
            return None
        return ((baseline - improved) / baseline) * 100


# Global stats object
_benchmark_stats = BenchmarkStats()
_initial_sync_errors: Dict[str, float] = {}
_backends_registered = False

# ============================================================================
# Helper Functions
# ============================================================================

def get_metric_value(metrics_url: str, metric_name: str) -> Optional[float]:
    """Extract a specific metric value from Prometheus endpoint."""
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return None

        for line in resp.text.split("\n"):
            if line.startswith("#"):
                continue
            if metric_name in line:
                match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                if match:
                    return float(match.group(1))
        return None
    except requests.exceptions.RequestException as e:
        logger.warning(f"Failed to fetch metrics from {metrics_url}: {e}")
        return None


def register_backends_on_all_nodes():
    """Register backends on all Ranvier nodes."""
    global _backends_registered
    if _backends_registered:
        return

    logger.info("Registering backends on all Ranvier nodes...")

    for node_url in RANVIER_NODES:
        for backend in BACKENDS:
            url = (
                f"{node_url}/admin/backends"
                f"?id={backend['id']}"
                f"&ip={backend['ip']}"
                f"&port={backend['port']}"
            )
            try:
                resp = requests.post(url, timeout=10)
                if resp.status_code == 200:
                    logger.info(f"Registered backend {backend['id']} on {node_url}")
                else:
                    logger.warning(
                        f"Failed to register backend {backend['id']} on {node_url}: "
                        f"{resp.status_code} - {resp.text}"
                    )
            except requests.exceptions.RequestException as e:
                logger.error(f"Error registering backend {backend['id']} on {node_url}: {e}")

    _backends_registered = True
    logger.info("Backend registration complete")


def capture_initial_sync_errors():
    """Capture initial sync error counts from all nodes."""
    global _initial_sync_errors
    _initial_sync_errors = {}

    for i, metrics_url in enumerate(RANVIER_METRICS):
        value = get_metric_value(metrics_url, "router_cluster_sync_errors")
        node_name = f"node{i + 1}"
        _initial_sync_errors[node_name] = value if value is not None else 0.0
        logger.info(f"Initial sync errors on {node_name}: {_initial_sync_errors[node_name]}")


def check_sync_errors() -> Tuple[bool, str]:
    """Check if any sync errors occurred during the test run."""
    errors_found = []

    for i, metrics_url in enumerate(RANVIER_METRICS):
        node_name = f"node{i + 1}"
        current_value = get_metric_value(metrics_url, "router_cluster_sync_errors")

        if current_value is None:
            continue

        initial_value = _initial_sync_errors.get(node_name, 0.0)
        delta = current_value - initial_value

        if delta > 0:
            errors_found.append(f"{node_name}: {delta} new sync errors")

    if errors_found:
        return False, f"Sync errors detected: {', '.join(errors_found)}"

    return True, "No sync errors detected"


def hash_prompt_prefix(messages: List[dict], token_limit: int = 100) -> str:
    """Generate a hash of the prompt prefix for cache tracking.

    This approximates what the router does - extracting the first N tokens
    to determine routing affinity.
    """
    # Combine all message content
    full_text = ""
    for msg in messages:
        role = msg.get("role", "")
        content = msg.get("content", "")
        full_text += f"{role}: {content}\n"

    # Take first ~400 characters as approximation of first 100 tokens
    prefix = full_text[:400]

    # Simple hash
    return str(hash(prefix))


def parse_sse_usage(response_text: str) -> Tuple[int, int]:
    """Parse usage statistics from SSE response chunks.

    vLLM includes usage info in the final chunk:
    {"usage": {"prompt_tokens": 123, "completion_tokens": 45, "total_tokens": 168}}
    """
    prompt_tokens = 0
    completion_tokens = 0

    for line in response_text.split("\n"):
        if line.startswith("data: ") and line != "data: [DONE]":
            try:
                data = json.loads(line[6:])
                if "usage" in data:
                    usage = data["usage"]
                    prompt_tokens = usage.get("prompt_tokens", 0)
                    completion_tokens = usage.get("completion_tokens", 0)
            except (json.JSONDecodeError, KeyError):
                pass

    return prompt_tokens, completion_tokens


def get_backend_from_response(headers: dict) -> Optional[str]:
    """Extract backend ID from response headers.

    Ranvier adds X-Backend-ID header to responses.
    """
    return headers.get("X-Backend-ID") or headers.get("x-backend-id")


# ============================================================================
# Prompt Generation
# ============================================================================

def generate_prompt() -> Tuple[List[dict], str]:
    """Generate a prompt based on configured distribution.

    Returns:
        Tuple of (messages list, prefix_hash for cache tracking)
    """
    distribution = PROMPT_DISTRIBUTION.lower()

    if distribution == "short":
        return _generate_short_prompt()
    elif distribution == "medium":
        return _generate_medium_prompt()
    elif distribution == "long":
        return _generate_long_prompt()
    else:  # mixed
        # Weighted distribution: 30% short, 50% medium, 20% long
        r = random.random()
        if r < 0.3:
            return _generate_short_prompt()
        elif r < 0.8:
            return _generate_medium_prompt()
        else:
            return _generate_long_prompt()


def _generate_short_prompt() -> Tuple[List[dict], str]:
    """Generate a short prompt without shared prefix."""
    prompt = random.choice(SHORT_PROMPTS)
    messages = [{"role": "user", "content": prompt}]
    return messages, hash_prompt_prefix(messages)


def _generate_medium_prompt() -> Tuple[List[dict], str]:
    """Generate a medium prompt with potential shared prefix."""
    # Decide if this should share a prefix with other requests
    if random.random() < SHARED_PREFIX_RATIO:
        # Use a shared system prompt
        category, user_prompt = random.choice(MEDIUM_PROMPTS)
        system_prompt = SYSTEM_PROMPTS[category]
        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ]
    else:
        # Unique prompt
        prompt = random.choice(SHORT_PROMPTS)
        messages = [{"role": "user", "content": prompt + f" (request {random.randint(1000, 9999)})"}]

    return messages, hash_prompt_prefix(messages)


def _generate_long_prompt() -> Tuple[List[dict], str]:
    """Generate a long prompt with potential shared prefix."""
    if random.random() < SHARED_PREFIX_RATIO:
        category, user_prompt = random.choice(LONG_PROMPTS)
        system_prompt = SYSTEM_PROMPTS[category]
        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ]
    else:
        # Unique long prompt
        messages = [{"role": "user", "content": random.choice(LONG_PROMPTS)[1]}]

    return messages, hash_prompt_prefix(messages)


# ============================================================================
# Event Handlers
# ============================================================================

@events.test_start.add_listener
def on_test_start(environment, **kwargs):
    """Called when the test starts."""
    if isinstance(environment.runner, WorkerRunner):
        return

    logger.info("=" * 70)
    logger.info("Starting Ranvier Real Backend Load Test")
    logger.info("=" * 70)
    logger.info(f"Benchmark Mode: {BENCHMARK_MODE}")
    logger.info(f"Prompt Distribution: {PROMPT_DISTRIBUTION}")
    logger.info(f"Shared Prefix Ratio: {SHARED_PREFIX_RATIO}")
    logger.info(f"P99 Latency Threshold: {P99_LATENCY_THRESHOLD_MS}ms")
    logger.info(f"Backends: {BACKENDS}")
    logger.info("=" * 70)

    # Register backends
    register_backends_on_all_nodes()

    # Wait for backends to be ready
    logger.info("Waiting for backends to warm up...")
    time.sleep(5)

    # Capture initial sync errors
    capture_initial_sync_errors()

    logger.info("Load test initialization complete")


@events.test_stop.add_listener
def on_test_stop(environment, **kwargs):
    """Called when the test stops."""
    if isinstance(environment.runner, WorkerRunner):
        return

    logger.info("=" * 70)
    logger.info("Benchmark Results Summary")
    logger.info("=" * 70)

    # Get aggregated stats
    summary = _benchmark_stats.get_summary()

    # Print cache hit statistics
    logger.info(f"Cache Statistics:")
    logger.info(f"  Cache Hits: {summary['cache_hits']}")
    logger.info(f"  Cache Misses: {summary['cache_misses']}")
    logger.info(f"  Cache Hit Rate: {summary['cache_hit_rate_pct']:.1f}%")
    logger.info(f"  Unique Prefixes: {summary['unique_prefixes']}")

    # Print TTFT comparison
    logger.info(f"\nTTFT Comparison:")
    if summary['ttft_cache_hit_p50_ms']:
        logger.info(f"  Cache Hit P50: {summary['ttft_cache_hit_p50_ms']:.1f}ms")
        logger.info(f"  Cache Hit P99: {summary['ttft_cache_hit_p99_ms']:.1f}ms")
    if summary['ttft_cache_miss_p50_ms']:
        logger.info(f"  Cache Miss P50: {summary['ttft_cache_miss_p50_ms']:.1f}ms")
        logger.info(f"  Cache Miss P99: {summary['ttft_cache_miss_p99_ms']:.1f}ms")
    if summary['ttft_improvement_pct']:
        logger.info(f"  TTFT Improvement: {summary['ttft_improvement_pct']:.1f}%")

    # Print throughput
    logger.info(f"\nThroughput:")
    logger.info(f"  Total Prompt Tokens: {summary['total_prompt_tokens']}")
    logger.info(f"  Total Completion Tokens: {summary['total_completion_tokens']}")
    logger.info(f"  Tokens/Second: {summary['tokens_per_second']:.1f}")

    # Check sync errors
    sync_passed, sync_msg = check_sync_errors()
    logger.info(f"\nCluster Health: {sync_msg}")

    # Validation
    logger.info("=" * 70)
    validation_passed = True

    # Check P99 TTFT (use overall TTFT from Locust stats)
    stats = environment.stats
    ttft_stats = stats.get("TTFT (Time To First Token)", "GET")

    if ttft_stats and ttft_stats.num_requests > 0:
        p99_latency = ttft_stats.get_response_time_percentile(0.99)
        if p99_latency is not None and p99_latency > P99_LATENCY_THRESHOLD_MS:
            validation_passed = False
            logger.error(f"FAIL: P99 TTFT {p99_latency:.1f}ms exceeds {P99_LATENCY_THRESHOLD_MS}ms threshold")
        else:
            logger.info(f"PASS: P99 TTFT {p99_latency:.1f}ms within threshold")

    if not sync_passed:
        validation_passed = False
        logger.error(f"FAIL: {sync_msg}")

    if validation_passed:
        logger.info("BENCHMARK PASSED")
    else:
        logger.error("BENCHMARK FAILED")
        environment.process_exit_code = 1

    logger.info("=" * 70)

    # Output machine-readable summary
    logger.info("BENCHMARK_STATS_JSON:" + json.dumps(summary))


# ============================================================================
# Locust User Class
# ============================================================================

class RealBackendUser(HttpUser):
    """Simulates users sending chat completion requests to real vLLM backends."""

    wait_time = between(0.5, 2)

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._request_count = 0

    @task
    def chat_completion(self):
        """Send a chat completion request and collect detailed metrics."""
        # Round-robin across Ranvier nodes
        node_index = self._request_count % len(RANVIER_NODES)
        self._request_count += 1

        target_url = RANVIER_NODES[node_index]

        # Generate prompt
        messages, prefix_hash = generate_prompt()

        request_body = {
            "model": os.environ.get("VLLM_MODEL", "default"),
            "messages": messages,
            "stream": True,
            "max_tokens": 100,  # Limit output for benchmarking
        }

        # Initialize metrics
        metrics = RequestMetrics(
            request_id=f"req-{self._request_count}",
            prompt_prefix_hash=prefix_hash,
            routing_mode=BENCHMARK_MODE,
        )

        start_time = time.perf_counter()
        ttft = None
        response_text = ""

        try:
            resp = requests.post(
                f"{target_url}/v1/chat/completions",
                json=request_body,
                headers={"Content-Type": "application/json"},
                stream=True,
                timeout=60,
            )

            # Get backend ID from response headers
            metrics.backend_id = get_backend_from_response(dict(resp.headers))

            if resp.status_code != 200:
                events.request.fire(
                    request_type="POST",
                    name=f"/v1/chat/completions (node{node_index + 1})",
                    response_time=(time.perf_counter() - start_time) * 1000,
                    response_length=0,
                    exception=Exception(f"Status: {resp.status_code}"),
                    context={},
                )
                _benchmark_stats.record_request(metrics)
                return

            # Process streaming response
            for line in resp.iter_lines():
                if line:
                    decoded = line.decode("utf-8")
                    response_text += decoded + "\n"

                    if decoded.startswith("data: "):
                        if ttft is None:
                            ttft = (time.perf_counter() - start_time) * 1000

                        if decoded == "data: [DONE]":
                            break

            total_time = (time.perf_counter() - start_time) * 1000

            # Parse usage stats
            prompt_tokens, completion_tokens = parse_sse_usage(response_text)
            metrics.prompt_tokens = prompt_tokens
            metrics.completion_tokens = completion_tokens
            metrics.ttft_ms = ttft
            metrics.total_time_ms = total_time

            # Record to global stats
            _benchmark_stats.record_request(metrics)

            # Fire Locust events
            events.request.fire(
                request_type="POST",
                name=f"/v1/chat/completions (node{node_index + 1})",
                response_time=total_time,
                response_length=len(response_text),
                exception=None,
                context={},
            )

            if ttft is not None:
                # Record TTFT
                events.request.fire(
                    request_type="GET",
                    name="TTFT (Time To First Token)",
                    response_time=ttft,
                    response_length=0,
                    exception=None,
                    context={},
                )

                # Record cache-specific TTFT
                cache_status = "hit" if metrics.is_cache_hit else "miss"
                events.request.fire(
                    request_type="GET",
                    name=f"TTFT (Cache {cache_status.upper()})",
                    response_time=ttft,
                    response_length=0,
                    exception=None,
                    context={},
                )

            if completion_tokens > 0:
                # Record tokens per second for this request
                gen_time_s = (total_time - (ttft or 0)) / 1000.0
                if gen_time_s > 0:
                    tps = completion_tokens / gen_time_s
                    events.request.fire(
                        request_type="METRIC",
                        name="Tokens/Second",
                        response_time=tps,  # Using response_time to record the metric
                        response_length=completion_tokens,
                        exception=None,
                        context={},
                    )

        except Exception as e:
            total_time = (time.perf_counter() - start_time) * 1000
            events.request.fire(
                request_type="POST",
                name=f"/v1/chat/completions (node{node_index + 1})",
                response_time=total_time,
                response_length=0,
                exception=e,
                context={},
            )
            _benchmark_stats.record_request(metrics)
            logger.warning(f"Request to node{node_index + 1} failed: {e}")
