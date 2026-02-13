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

Environment Variables:
    Backend Configuration:
        NUM_BACKENDS          - Number of vLLM backends (default: 2)
        BACKEND_BASE_IP       - Base IP for sequential port config (e.g., 172.17.0.1)
        BACKEND_PORT_START    - Starting port for sequential config (default: 8000)
        BACKEND{N}_IP         - Per-backend IP override (default: 172.29.1.{9+N})
        BACKEND{N}_PORT       - Per-backend port override (default: 8000)
        SKIP_BACKEND_REGISTRATION - Skip auto-registration if backends already registered
        SINGLE_BACKEND_MODE   - Legacy: set to "true" for single backend

    Ranvier Node Configuration:
        NUM_RANVIER_NODES     - Number of Ranvier router nodes (default: 3)
        RANVIER_BASE_IP       - Base IP for sequential port config (e.g., localhost)
        RANVIER_PORT_START    - Starting port for sequential config (default: 8080)
        RANVIER_METRICS_PORT_START - Starting metrics port for sequential config (default: 9180)
        RANVIER_NODE{N}       - Full URL for node N (e.g., http://host:8080)
        RANVIER_NODE{N}_IP    - IP address for node N (default: 172.29.2.{N})
        RANVIER_NODE{N}_PORT  - Port for node N (default: 8080)
        RANVIER_METRICS{N}    - Full metrics URL for node N
        RANVIER_METRICS{N}_PORT - Metrics port for node N (default: 9180)

    Benchmark Configuration:
        BENCHMARK_MODE        - "prefix" (default), "hash", or "random"
        PROMPT_DISTRIBUTION   - "mixed", "short", "medium", "long", "large-prefix", "stress", "file"
        SHARED_PREFIX_RATIO   - Ratio of requests sharing prefixes (default: 0.7)
        P99_LATENCY_THRESHOLD_MS - P99 TTFT threshold in ms (default: 5000)

    Generation Configuration:
        MAX_OUTPUT_TOKENS         - Max tokens to generate per request (default: 100)
                                    Lower values (e.g., 20) reduce GPU contention and
                                    incomplete rates without affecting TTFT measurements.

    Timeout Configuration:
        CONNECT_TIMEOUT_SECONDS   - TCP connection + headers timeout (default: 30)
        READ_TIMEOUT_SECONDS      - Socket read timeout per chunk (default: 120)
                                    Applies to each iter_lines() read; prevents indefinite blocking
        STREAMING_TIMEOUT_SECONDS - Overall streaming timeout (default: 300)
                                    Total time allowed for streaming response

    Custom Prompt File Configuration:
        PROMPT_FILE           - Path to JSONL file containing prompts (optional)
        PROMPT_SAMPLING       - Sampling strategy: "random" (default), "sequential", "weighted"

        Supported prompt file formats:
        - ShareGPT: {"conversations": [{"from": "human", "value": "..."}, {"from": "gpt", "value": "..."}]}
        - OpenAI:   {"messages": [{"role": "user", "content": "..."}, {"role": "assistant", "content": "..."}]}
        - Simple:   {"prompt": "user prompt text here"}

    Large Prefix Stress Testing:
        LARGE_PREFIX_MIN_TOKENS - Minimum prefix size (default: 2000)
        LARGE_PREFIX_MAX_TOKENS - Maximum prefix size (default: 8000)
        NUM_LARGE_PREFIXES      - Number of unique prefixes to generate (default: 5)

    Client-Side Tokenization:
        CLIENT_TOKENIZE         - Enable client-side tokenization (default: false)
                                  When enabled:
                                  1. Uses /v1/completions endpoint (instead of /v1/chat/completions)
                                  2. Pre-tokenizes prompts and sends prompt_token_ids
                                  3. Ranvier uses client tokens for routing (no server tokenization)
                                  4. vLLM skips tokenization (uses prompt_token_ids directly)
                                  This provides maximum efficiency by eliminating all tokenization
                                  on both Ranvier and vLLM.
        TOKENIZER_PATH          - Path to tokenizer JSON file (default: assets/gpt2.json)
                                  If file doesn't exist, falls back to loading from VLLM_MODEL

Usage:
    # 8 backends on same host with sequential ports (simplest for multi-GPU):
    NUM_BACKENDS=8 BACKEND_BASE_IP=172.17.0.1 BACKEND_PORT_START=8000 \
    locust -f tests/integration/locustfile_real.py --headless ...

    # 3 Ranvier nodes on same host with sequential ports:
    NUM_RANVIER_NODES=3 RANVIER_BASE_IP=localhost RANVIER_PORT_START=8081 \
    locust -f tests/integration/locustfile_real.py --headless ...

    # Skip registration if backends already registered via curl:
    SKIP_BACKEND_REGISTRATION=true NUM_RANVIER_NODES=1 RANVIER_NODE1=http://localhost:8081 \
    locust -f tests/integration/locustfile_real.py --headless ...

    # With 4 backends on different hosts:
    NUM_BACKENDS=4 \
    BACKEND1_IP=10.0.0.1 BACKEND2_IP=10.0.0.2 \
    BACKEND3_IP=10.0.0.3 BACKEND4_IP=10.0.0.4 \
    make benchmark-real

    # Stress test with large prefixes:
    PROMPT_DISTRIBUTION=stress NUM_BACKENDS=4 make benchmark-real

    # Use included sample prompts:
    PROMPT_FILE=tests/integration/data/prompts/sample_prompts.jsonl PROMPT_DISTRIBUTION=file \
    locust -f tests/integration/locustfile_real.py --headless ...

    # Use realistic prompts for benchmarking:
    PROMPT_FILE=tests/integration/data/prompts/realistic_prompts.jsonl PROMPT_DISTRIBUTION=file \
    locust -f tests/integration/locustfile_real.py --headless ...

    # Use custom production prompts with sequential sampling:
    PROMPT_FILE=/data/prod_prompts.jsonl PROMPT_DISTRIBUTION=file PROMPT_SAMPLING=sequential \
    locust -f tests/integration/locustfile_real.py --headless ...

    # Validate prompt file before benchmarking:
    python tests/integration/prompt_loader.py validate my_prompts.jsonl
    python tests/integration/prompt_loader.py stats my_prompts.jsonl

Official Datasets (ShareGPT format, commonly used for LLM benchmarks):
    - ShareGPT: huggingface.co/datasets/anon8231489123/ShareGPT_Vicuna_unfiltered
    - LMSYS-Chat-1M: huggingface.co/datasets/lmsys/lmsys-chat-1m
    - WildChat: huggingface.co/datasets/allenai/WildChat-1M
    - OpenOrca: huggingface.co/datasets/Open-Orca/OpenOrca

    Download example:
        pip install huggingface_hub
        huggingface-cli download lmsys/lmsys-chat-1m --include "*.jsonl" --local-dir ./data
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
from requests.exceptions import ReadTimeout, ConnectTimeout, Timeout
from locust import HttpUser, task, between, events
from locust.runners import MasterRunner, WorkerRunner

# Optional: tokenizers library for client-side tokenization
# Install with: pip install tokenizers
_tokenizer = None
_client_tokenize_enabled = False

def _init_client_tokenizer():
    """Initialize client-side tokenizer if CLIENT_TOKENIZE is enabled."""
    global _tokenizer, _client_tokenize_enabled

    client_tokenize = os.environ.get("CLIENT_TOKENIZE", "false").lower() in ("true", "1", "yes")
    if not client_tokenize:
        return False

    try:
        from tokenizers import Tokenizer
        tokenizer_path = os.environ.get("TOKENIZER_PATH", "assets/gpt2.json")

        # Try to load from file
        if os.path.exists(tokenizer_path):
            _tokenizer = Tokenizer.from_file(tokenizer_path)
            logger.info(f"Client tokenizer loaded from: {tokenizer_path}")
        else:
            # Try to load from HuggingFace model name
            model_name = os.environ.get("VLLM_MODEL", "gpt2")
            _tokenizer = Tokenizer.from_pretrained(model_name)
            logger.info(f"Client tokenizer loaded from model: {model_name}")

        _client_tokenize_enabled = True
        return True
    except ImportError:
        logger.warning("CLIENT_TOKENIZE=true but 'tokenizers' package not installed. "
                      "Install with: pip install tokenizers")
        return False
    except Exception as e:
        logger.warning(f"Failed to initialize client tokenizer: {e}")
        return False

def tokenize_messages(messages: List[dict]) -> Optional[List[int]]:
    """Tokenize chat messages and return token IDs.

    Returns None if client tokenization is disabled or fails.
    """
    if not _client_tokenize_enabled or _tokenizer is None:
        return None

    try:
        # Convert messages to a single string (chat template style)
        # This matches how vLLM/HuggingFace typically process chat messages
        text_parts = []
        for msg in messages:
            role = msg.get("role", "user")
            content = msg.get("content", "")
            text_parts.append(f"<|{role}|>\n{content}")
        text = "\n".join(text_parts)

        # Tokenize
        encoding = _tokenizer.encode(text)
        return encoding.ids
    except Exception as e:
        logger.debug(f"Client tokenization failed: {e}")
        return None


def tokenize_system_messages(messages: List[dict]) -> Optional[int]:
    """Tokenize only the system messages and return the token count.

    This is used to calculate prefix_token_count for routing hints.
    The prefix_token_count tells Ranvier how many tokens constitute the
    "shared prefix" (system messages) for prefix-aware routing.

    IMPORTANT: The tokenization format must match what Ranvier's extract_text()
    produces: raw content concatenated with "\n" separators, followed by a
    trailing "\n" to match the BPE boundary alignment fix.

    Returns None if:
    - Client tokenization is disabled
    - No system messages found
    - Tokenization fails
    """
    if not _client_tokenize_enabled or _tokenizer is None:
        return None

    try:
        # Extract system message content only (matching Ranvier's extract_system_messages)
        system_parts = []
        for msg in messages:
            if msg.get("role") == "system":
                content = msg.get("content", "")
                if content:
                    system_parts.append(content)

        if not system_parts:
            return None

        # Format must match Ranvier's extract_text() + "\n" for BPE boundary alignment
        # - Multiple system messages: "content1\ncontent2\n"
        # - Single system message: "content\n"
        system_text = "\n".join(system_parts) + "\n"
        encoding = _tokenizer.encode(system_text)
        return len(encoding.ids)
    except Exception as e:
        logger.debug(f"System message tokenization failed: {e}")
        return None


def messages_to_prompt(messages: List[dict]) -> str:
    """Convert chat messages to a prompt string for /v1/completions endpoint.

    Uses the same format as tokenize_messages for consistency.
    """
    text_parts = []
    for msg in messages:
        role = msg.get("role", "user")
        content = msg.get("content", "")
        text_parts.append(f"<|{role}|>\n{content}")
    return "\n".join(text_parts)

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# ============================================================================
# Environment Configuration
# ============================================================================

# Skip backend registration if backends are already registered manually
# Use this when you've already registered backends via curl or admin API
SKIP_BACKEND_REGISTRATION = os.environ.get("SKIP_BACKEND_REGISTRATION", "false").lower() in ("true", "1", "yes")

# Support single-backend mode for single-GPU testing (legacy, prefer NUM_BACKENDS=1)
SINGLE_BACKEND_MODE = os.environ.get("SINGLE_BACKEND_MODE", "false").lower() == "true"

# Configurable backend count (1-16 backends supported)
# Set NUM_BACKENDS to control how many backends to use
if SINGLE_BACKEND_MODE:
    NUM_BACKENDS = 1
else:
    NUM_BACKENDS = int(os.environ.get("NUM_BACKENDS", "2"))

# Simplified backend config for sequential ports on same host (common for multi-GPU)
# If BACKEND_BASE_IP is set, all backends use that IP with sequential ports starting from BACKEND_PORT_START
# Example: BACKEND_BASE_IP=172.17.0.1 BACKEND_PORT_START=8000 NUM_BACKENDS=8
#   -> Backend 1: 172.17.0.1:8000, Backend 2: 172.17.0.1:8001, ..., Backend 8: 172.17.0.1:8007
BACKEND_BASE_IP = os.environ.get("BACKEND_BASE_IP")
BACKEND_PORT_START = int(os.environ.get("BACKEND_PORT_START", "8000"))

# Default backend IP patterns (used when BACKEND_BASE_IP is not set)
DEFAULT_BACKEND_IP_PATTERN = "172.29.1.{}"  # {} is replaced with 10 + backend_index
DEFAULT_BACKEND_PORT = 8000


def _build_backends_list(num_backends: int) -> List[dict]:
    """Build the backends list dynamically based on environment configuration.

    Configuration modes (in order of precedence):
    1. BACKEND_BASE_IP + BACKEND_PORT_START: Sequential ports on same host
       Example: BACKEND_BASE_IP=172.17.0.1 BACKEND_PORT_START=8000 NUM_BACKENDS=8
         -> 172.17.0.1:8000, 172.17.0.1:8001, ..., 172.17.0.1:8007

    2. Per-backend overrides: BACKEND{N}_IP and BACKEND{N}_PORT
       Example: BACKEND1_IP=10.0.0.1 BACKEND2_IP=10.0.0.2 NUM_BACKENDS=2

    3. Defaults: Different IPs (172.29.1.10, .11, ...), same port (8000)
       Example: NUM_BACKENDS=4
         -> 172.29.1.10:8000, 172.29.1.11:8000, 172.29.1.12:8000, 172.29.1.13:8000
    """
    backends = []

    for i in range(1, num_backends + 1):
        # Check for per-backend override first
        backend_ip = os.environ.get(f"BACKEND{i}_IP")
        backend_port = os.environ.get(f"BACKEND{i}_PORT")

        if backend_ip is not None:
            # Per-backend override takes precedence
            ip = backend_ip
            port = int(backend_port) if backend_port else DEFAULT_BACKEND_PORT
        elif BACKEND_BASE_IP:
            # Use simplified sequential port config
            ip = BACKEND_BASE_IP
            port = BACKEND_PORT_START + (i - 1)
        else:
            # Fall back to default pattern (different IPs, same port)
            ip = DEFAULT_BACKEND_IP_PATTERN.format(9 + i)  # 172.29.1.10, .11, .12, etc.
            port = DEFAULT_BACKEND_PORT

        backends.append({"id": i, "ip": ip, "port": port})

    return backends


BACKENDS = _build_backends_list(NUM_BACKENDS)

# Configurable Ranvier node count
# Set NUM_RANVIER_NODES to control how many router nodes to use
NUM_RANVIER_NODES = int(os.environ.get("NUM_RANVIER_NODES", "3"))

# Simplified Ranvier config for sequential ports on same host (common for multi-node testing)
# If RANVIER_BASE_IP is set, all nodes use that IP with sequential ports starting from RANVIER_PORT_START
# Example: RANVIER_BASE_IP=localhost RANVIER_PORT_START=8081 NUM_RANVIER_NODES=3
#   -> Node 1: localhost:8081, Node 2: localhost:8082, Node 3: localhost:8083
#   -> Metrics 1: localhost:9181, Metrics 2: localhost:9182, Metrics 3: localhost:9183
RANVIER_BASE_IP = os.environ.get("RANVIER_BASE_IP")
RANVIER_PORT_START = int(os.environ.get("RANVIER_PORT_START", "8080"))
RANVIER_METRICS_PORT_START = int(os.environ.get("RANVIER_METRICS_PORT_START", "9180"))

# Default Ranvier node patterns (used when RANVIER_BASE_IP is not set)
DEFAULT_RANVIER_IP_PATTERN = "172.29.2.{}"  # {} is replaced with node_index
DEFAULT_RANVIER_PORT = 8080
DEFAULT_RANVIER_METRICS_PORT = 9180


def _build_ranvier_nodes_list(num_nodes: int) -> List[str]:
    """Build the Ranvier nodes list dynamically.

    Configuration modes (in order of precedence):
    1. RANVIER_NODE{N}: Full URL override (e.g., http://host:8080)

    2. RANVIER_BASE_IP + RANVIER_PORT_START: Sequential ports on same host
       Example: RANVIER_BASE_IP=localhost RANVIER_PORT_START=8081 NUM_RANVIER_NODES=3
         -> http://localhost:8081, http://localhost:8082, http://localhost:8083

    3. Per-node overrides: RANVIER_NODE{N}_IP and RANVIER_NODE{N}_PORT
       Example: RANVIER_NODE1_IP=10.0.0.1 RANVIER_NODE2_IP=10.0.0.2

    4. Defaults: Different IPs (172.29.2.1, .2, ...), same port (8080)
    """
    nodes = []
    for i in range(1, num_nodes + 1):
        # Check for full URL override first (highest precedence)
        full_url = os.environ.get(f"RANVIER_NODE{i}")
        if full_url:
            nodes.append(full_url)
            continue

        # Check for per-node IP override
        node_ip = os.environ.get(f"RANVIER_NODE{i}_IP")
        node_port = os.environ.get(f"RANVIER_NODE{i}_PORT")

        if node_ip is not None:
            # Per-node override
            ip = node_ip
            port = int(node_port) if node_port else DEFAULT_RANVIER_PORT
        elif RANVIER_BASE_IP:
            # Use simplified sequential port config
            ip = RANVIER_BASE_IP
            port = RANVIER_PORT_START + (i - 1)
        else:
            # Fall back to default pattern (different IPs, same port)
            ip = DEFAULT_RANVIER_IP_PATTERN.format(i)
            port = DEFAULT_RANVIER_PORT

        nodes.append(f"http://{ip}:{port}")
    return nodes


def _build_ranvier_metrics_list(num_nodes: int) -> List[str]:
    """Build the Ranvier metrics endpoints list dynamically.

    Configuration modes (in order of precedence):
    1. RANVIER_METRICS{N}: Full URL override

    2. RANVIER_BASE_IP + RANVIER_METRICS_PORT_START: Sequential ports on same host
       Example: RANVIER_BASE_IP=localhost RANVIER_METRICS_PORT_START=9181 NUM_RANVIER_NODES=3
         -> http://localhost:9181, http://localhost:9182, http://localhost:9183

    3. Per-node overrides: RANVIER_NODE{N}_IP + RANVIER_METRICS{N}_PORT

    4. Defaults: Different IPs (172.29.2.1, .2, ...), same port (9180)
    """
    metrics = []
    for i in range(1, num_nodes + 1):
        # Check for full URL override first (highest precedence)
        full_url = os.environ.get(f"RANVIER_METRICS{i}")
        if full_url:
            metrics.append(full_url)
            continue

        # Check for per-node IP override (from node config, reused for metrics)
        node_ip = os.environ.get(f"RANVIER_NODE{i}_IP")
        metrics_port = os.environ.get(f"RANVIER_METRICS{i}_PORT")

        if node_ip is not None:
            # Per-node override
            ip = node_ip
            port = int(metrics_port) if metrics_port else DEFAULT_RANVIER_METRICS_PORT
        elif RANVIER_BASE_IP:
            # Use simplified sequential port config
            ip = RANVIER_BASE_IP
            port = RANVIER_METRICS_PORT_START + (i - 1)
        else:
            # Fall back to default pattern (different IPs, same port)
            ip = DEFAULT_RANVIER_IP_PATTERN.format(i)
            port = DEFAULT_RANVIER_METRICS_PORT

        metrics.append(f"http://{ip}:{port}")
    return metrics

RANVIER_NODES = _build_ranvier_nodes_list(NUM_RANVIER_NODES)
RANVIER_METRICS = _build_ranvier_metrics_list(NUM_RANVIER_NODES)

# Benchmark mode: "prefix" for ART+hash, "hash" for hash-only, "random" for baseline
BENCHMARK_MODE = os.environ.get("BENCHMARK_MODE", "prefix")


def verify_routing_mode_matches() -> Tuple[bool, Optional[str]]:
    """Verify that the server's actual routing mode matches BENCHMARK_MODE.

    Makes a probe request to check the X-Routing-Mode response header.
    Returns (matches, actual_mode) tuple.
    """
    if not RANVIER_NODES:
        logger.warning("No Ranvier nodes configured, skipping routing mode verification")
        return True, None

    probe_url = f"{RANVIER_NODES[0]}/v1/chat/completions"
    probe_payload = {
        "model": "probe",
        "messages": [{"role": "user", "content": "routing mode check"}],
        "max_tokens": 1,
        "stream": False
    }

    try:
        # Use a short timeout - we just need the headers, not a full response
        # The request may fail (no backends), but we'll still get headers
        resp = requests.post(probe_url, json=probe_payload, timeout=5)
        actual_mode = resp.headers.get("X-Routing-Mode")

        if actual_mode is None:
            logger.warning("Server did not return X-Routing-Mode header - "
                          "ensure Ranvier is updated with header support")
            return True, None  # Can't verify, assume OK

        if actual_mode != BENCHMARK_MODE:
            logger.error("=" * 70)
            logger.error("ROUTING MODE MISMATCH DETECTED!")
            logger.error(f"  BENCHMARK_MODE (client label): {BENCHMARK_MODE}")
            logger.error(f"  X-Routing-Mode (server actual): {actual_mode}")
            logger.error("")
            logger.error("Your benchmark results will be mislabeled!")
            logger.error("Fix: Set RANVIER_ROUTING_MODE=%s before starting the server", BENCHMARK_MODE)
            logger.error("=" * 70)
            return False, actual_mode

        logger.info(f"Routing mode verified: server is running in '{actual_mode}' mode")
        return True, actual_mode

    except requests.exceptions.RequestException as e:
        logger.warning(f"Could not verify routing mode (probe request failed): {e}")
        return True, None  # Can't verify, assume OK


# Prompt distribution: "mixed", "short", "medium", "long", "large-prefix", "stress", "file"
PROMPT_DISTRIBUTION = os.environ.get("PROMPT_DISTRIBUTION", "stress")

# Custom prompt file configuration
# Path to JSONL file containing prompts (ShareGPT, OpenAI messages, or simple format)
PROMPT_FILE = os.environ.get("PROMPT_FILE", "")

# Prompt sampling strategy: "random", "sequential", "weighted"
# - random: randomly sample from loaded prompts
# - sequential: cycle through prompts in order
# - weighted: sample weighted by estimated token count (favor longer prompts)
PROMPT_SAMPLING = os.environ.get("PROMPT_SAMPLING", "random")

# Large prefix configuration
LARGE_PREFIX_MIN_TOKENS = int(os.environ.get("LARGE_PREFIX_MIN_TOKENS", "2000"))
LARGE_PREFIX_MAX_TOKENS = int(os.environ.get("LARGE_PREFIX_MAX_TOKENS", "8000"))
NUM_LARGE_PREFIXES = int(os.environ.get("NUM_LARGE_PREFIXES", "5"))

# Prefix size buckets for metrics (in tokens)
PREFIX_SIZE_BUCKETS = [
    ("tiny", 0, 100),
    ("small", 100, 500),
    ("medium", 500, 2000),
    ("large", 2000, 4000),
    ("xlarge", 4000, 8000),
]

# Ratio of requests that should share a prefix (0.0-1.0)
SHARED_PREFIX_RATIO = float(os.environ.get("SHARED_PREFIX_RATIO", "0.7"))

# Configurable thresholds
P99_LATENCY_THRESHOLD_MS = float(os.environ.get("P99_LATENCY_THRESHOLD_MS", "5000"))

# Maximum time to wait for a streaming response to complete (seconds)
# After this timeout, the request is aborted and recorded as a timeout error
# This prevents indefinite blocking when Locust's run-time expires
STREAMING_TIMEOUT_SECONDS = int(os.environ.get("STREAMING_TIMEOUT_SECONDS", "300"))  # 5 minutes

# Socket-level timeout configuration
# CONNECT_TIMEOUT: Max time to establish TCP connection and receive response headers
# READ_TIMEOUT: Max time to wait for each chunk of streaming data (per socket read)
# Note: READ_TIMEOUT must be > vLLM's time-to-first-token (typically 1-30s for large prompts)
# If no data arrives within READ_TIMEOUT, the request fails with ReadTimeout exception
CONNECT_TIMEOUT_SECONDS = int(os.environ.get("CONNECT_TIMEOUT_SECONDS", "30"))
READ_TIMEOUT_SECONDS = int(os.environ.get("READ_TIMEOUT_SECONDS", "120"))  # 2 min per chunk

# Maximum tokens to generate per request
# Lower values reduce GPU contention and incomplete rates without affecting TTFT.
# For TTFT-focused benchmarks with high user counts, use 20-50.
MAX_OUTPUT_TOKENS = int(os.environ.get("MAX_OUTPUT_TOKENS", "100"))

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
# Large Prefix Content Generation (for stress testing)
# ============================================================================

# Simulated RAG document chunks - realistic technical documentation
RAG_DOCUMENT_CHUNKS = [
    """## API Reference: Authentication Module

### Overview
The authentication module provides secure user authentication and session management
for the platform. It supports multiple authentication methods including OAuth 2.0,
SAML 2.0, and traditional username/password authentication.

### Endpoints

#### POST /auth/login
Authenticates a user and returns an access token.

**Request Body:**
```json
{
  "username": "string",
  "password": "string",
  "mfa_code": "string (optional)"
}
```

**Response:**
```json
{
  "access_token": "string",
  "refresh_token": "string",
  "expires_in": 3600,
  "token_type": "Bearer"
}
```

**Error Codes:**
- 401: Invalid credentials
- 403: Account locked
- 429: Too many attempts

#### POST /auth/refresh
Refreshes an expired access token using a valid refresh token.

**Request Body:**
```json
{
  "refresh_token": "string"
}
```

**Response:**
Same as /auth/login

#### POST /auth/logout
Invalidates the current session and all associated tokens.

**Headers:**
- Authorization: Bearer <access_token>

**Response:**
```json
{
  "message": "Successfully logged out"
}
```

### Security Considerations
- All endpoints use HTTPS with TLS 1.3
- Passwords are hashed using Argon2id
- Tokens are signed using RS256
- Rate limiting is applied per-IP and per-user
- Failed attempts are logged for security audit
""",

    """## Database Schema Documentation

### Users Table
Stores user account information and preferences.

```sql
CREATE TABLE users (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email VARCHAR(255) UNIQUE NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    first_name VARCHAR(100),
    last_name VARCHAR(100),
    created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    last_login_at TIMESTAMP WITH TIME ZONE,
    is_active BOOLEAN DEFAULT true,
    is_verified BOOLEAN DEFAULT false,
    mfa_enabled BOOLEAN DEFAULT false,
    mfa_secret VARCHAR(255),
    preferences JSONB DEFAULT '{}'::jsonb,
    metadata JSONB DEFAULT '{}'::jsonb
);

CREATE INDEX idx_users_email ON users(email);
CREATE INDEX idx_users_created_at ON users(created_at);
CREATE INDEX idx_users_is_active ON users(is_active) WHERE is_active = true;
```

### Sessions Table
Tracks active user sessions for security and analytics.

```sql
CREATE TABLE sessions (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id UUID NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash VARCHAR(255) NOT NULL,
    ip_address INET,
    user_agent TEXT,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    expires_at TIMESTAMP WITH TIME ZONE NOT NULL,
    last_activity_at TIMESTAMP WITH TIME ZONE DEFAULT NOW(),
    is_revoked BOOLEAN DEFAULT false
);

CREATE INDEX idx_sessions_user_id ON sessions(user_id);
CREATE INDEX idx_sessions_expires_at ON sessions(expires_at);
CREATE INDEX idx_sessions_token_hash ON sessions(token_hash);
```

### Audit Log Table
Records all security-relevant events for compliance.

```sql
CREATE TABLE audit_log (
    id BIGSERIAL PRIMARY KEY,
    event_type VARCHAR(50) NOT NULL,
    user_id UUID REFERENCES users(id),
    ip_address INET,
    resource_type VARCHAR(50),
    resource_id VARCHAR(255),
    action VARCHAR(50) NOT NULL,
    status VARCHAR(20) NOT NULL,
    details JSONB,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()
);

CREATE INDEX idx_audit_log_user_id ON audit_log(user_id);
CREATE INDEX idx_audit_log_created_at ON audit_log(created_at);
CREATE INDEX idx_audit_log_event_type ON audit_log(event_type);
```

### Query Optimization Notes
- Use covering indexes for frequent queries
- Partition audit_log by month for better performance
- Consider read replicas for analytics queries
- Use connection pooling (PgBouncer) for high concurrency
""",

    """## Kubernetes Deployment Guide

### Prerequisites
- Kubernetes cluster v1.25+
- kubectl configured with cluster access
- Helm 3.x installed
- Container registry access

### Namespace Setup
```yaml
apiVersion: v1
kind: Namespace
metadata:
  name: production
  labels:
    environment: production
    monitoring: enabled
```

### Deployment Configuration
```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: api-server
  namespace: production
spec:
  replicas: 3
  selector:
    matchLabels:
      app: api-server
  template:
    metadata:
      labels:
        app: api-server
      annotations:
        prometheus.io/scrape: "true"
        prometheus.io/port: "9090"
    spec:
      containers:
      - name: api-server
        image: registry.example.com/api-server:v2.1.0
        ports:
        - containerPort: 8080
        - containerPort: 9090
        resources:
          requests:
            memory: "512Mi"
            cpu: "250m"
          limits:
            memory: "1Gi"
            cpu: "1000m"
        livenessProbe:
          httpGet:
            path: /health
            port: 8080
          initialDelaySeconds: 30
          periodSeconds: 10
        readinessProbe:
          httpGet:
            path: /ready
            port: 8080
          initialDelaySeconds: 5
          periodSeconds: 5
        env:
        - name: DATABASE_URL
          valueFrom:
            secretKeyRef:
              name: db-credentials
              key: url
        - name: REDIS_URL
          valueFrom:
            configMapKeyRef:
              name: app-config
              key: redis-url
```

### Service Configuration
```yaml
apiVersion: v1
kind: Service
metadata:
  name: api-server
  namespace: production
spec:
  selector:
    app: api-server
  ports:
  - name: http
    port: 80
    targetPort: 8080
  - name: metrics
    port: 9090
    targetPort: 9090
  type: ClusterIP
```

### Ingress Configuration
```yaml
apiVersion: networking.k8s.io/v1
kind: Ingress
metadata:
  name: api-server
  namespace: production
  annotations:
    kubernetes.io/ingress.class: nginx
    cert-manager.io/cluster-issuer: letsencrypt-prod
spec:
  tls:
  - hosts:
    - api.example.com
    secretName: api-tls
  rules:
  - host: api.example.com
    http:
      paths:
      - path: /
        pathType: Prefix
        backend:
          service:
            name: api-server
            port:
              number: 80
```

### Horizontal Pod Autoscaler
```yaml
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: api-server
  namespace: production
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: api-server
  minReplicas: 3
  maxReplicas: 10
  metrics:
  - type: Resource
    resource:
      name: cpu
      target:
        type: Utilization
        averageUtilization: 70
  - type: Resource
    resource:
      name: memory
      target:
        type: Utilization
        averageUtilization: 80
```
""",

    """## Machine Learning Pipeline Documentation

### Feature Engineering

#### Numerical Features
The pipeline processes numerical features through the following transformations:

1. **Missing Value Imputation**: Uses median imputation for robustness against outliers
2. **Outlier Detection**: IQR-based detection with configurable multiplier (default: 1.5)
3. **Scaling**: StandardScaler for normally distributed features, RobustScaler for skewed

```python
from sklearn.pipeline import Pipeline
from sklearn.impute import SimpleImputer
from sklearn.preprocessing import StandardScaler, RobustScaler
from sklearn.compose import ColumnTransformer

numerical_pipeline = Pipeline([
    ('imputer', SimpleImputer(strategy='median')),
    ('scaler', StandardScaler())
])

robust_pipeline = Pipeline([
    ('imputer', SimpleImputer(strategy='median')),
    ('scaler', RobustScaler())
])
```

#### Categorical Features
Categorical features undergo:

1. **Missing Value Handling**: Most frequent value or dedicated 'unknown' category
2. **Encoding**: OneHotEncoder for low cardinality, TargetEncoder for high cardinality
3. **Rare Category Handling**: Categories below threshold grouped into 'other'

```python
from sklearn.preprocessing import OneHotEncoder, OrdinalEncoder
from category_encoders import TargetEncoder

categorical_pipeline = Pipeline([
    ('imputer', SimpleImputer(strategy='constant', fill_value='unknown')),
    ('encoder', OneHotEncoder(handle_unknown='ignore', sparse_output=False))
])
```

### Model Training

#### Hyperparameter Optimization
Uses Optuna for Bayesian optimization with cross-validation:

```python
import optuna
from sklearn.model_selection import cross_val_score

def objective(trial):
    params = {
        'n_estimators': trial.suggest_int('n_estimators', 100, 1000),
        'max_depth': trial.suggest_int('max_depth', 3, 15),
        'learning_rate': trial.suggest_float('learning_rate', 0.01, 0.3, log=True),
        'subsample': trial.suggest_float('subsample', 0.6, 1.0),
        'colsample_bytree': trial.suggest_float('colsample_bytree', 0.6, 1.0),
        'min_child_weight': trial.suggest_int('min_child_weight', 1, 10),
        'reg_alpha': trial.suggest_float('reg_alpha', 1e-8, 10.0, log=True),
        'reg_lambda': trial.suggest_float('reg_lambda', 1e-8, 10.0, log=True),
    }

    model = XGBClassifier(**params, random_state=42, n_jobs=-1)
    scores = cross_val_score(model, X_train, y_train, cv=5, scoring='roc_auc')
    return scores.mean()

study = optuna.create_study(direction='maximize')
study.optimize(objective, n_trials=100, timeout=3600)
```

### Model Evaluation

#### Metrics
- **AUC-ROC**: Primary metric for ranking performance
- **Precision@K**: For top-K recommendation scenarios
- **F1-Score**: Balance between precision and recall
- **Log Loss**: Probability calibration quality

```python
from sklearn.metrics import (
    roc_auc_score, precision_score, recall_score,
    f1_score, log_loss, confusion_matrix, classification_report
)

def evaluate_model(model, X_test, y_test):
    y_pred = model.predict(X_test)
    y_proba = model.predict_proba(X_test)[:, 1]

    metrics = {
        'auc_roc': roc_auc_score(y_test, y_proba),
        'precision': precision_score(y_test, y_pred),
        'recall': recall_score(y_test, y_pred),
        'f1': f1_score(y_test, y_pred),
        'log_loss': log_loss(y_test, y_proba)
    }

    print(classification_report(y_test, y_pred))
    return metrics
```

### Model Deployment

#### Model Serialization
```python
import joblib
import json

# Save model and metadata
joblib.dump(model, 'model.joblib')
with open('model_metadata.json', 'w') as f:
    json.dump({
        'version': '1.0.0',
        'features': feature_names,
        'metrics': evaluation_metrics,
        'training_date': datetime.now().isoformat()
    }, f)
```

#### Serving Configuration
```yaml
apiVersion: serving.kubeflow.org/v1beta1
kind: InferenceService
metadata:
  name: ml-model
spec:
  predictor:
    sklearn:
      storageUri: "gs://models/production/v1.0.0"
      resources:
        requests:
          memory: "2Gi"
          cpu: "1"
```
""",

    """## System Architecture Overview

### High-Level Design

The platform follows a microservices architecture with the following core components:

```
┌─────────────────────────────────────────────────────────────────┐
│                        Load Balancer                             │
│                    (AWS ALB / GCP GLB)                          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      API Gateway                                 │
│              (Kong / AWS API Gateway)                           │
│  - Rate Limiting                                                 │
│  - Authentication                                                │
│  - Request Routing                                               │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│ User Service │     │ Order Service│     │ Product Svc  │
│              │     │              │     │              │
│ - Auth       │     │ - Checkout   │     │ - Catalog    │
│ - Profiles   │     │ - Payments   │     │ - Inventory  │
│ - Sessions   │     │ - History    │     │ - Search     │
└──────────────┘     └──────────────┘     └──────────────┘
        │                     │                     │
        └─────────────────────┼─────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Message Queue                                 │
│                  (Kafka / RabbitMQ)                             │
│  - Event Streaming                                               │
│  - Async Communication                                           │
│  - Event Sourcing                                                │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│ PostgreSQL   │     │    Redis     │     │Elasticsearch │
│ (Primary DB) │     │   (Cache)    │     │  (Search)    │
└──────────────┘     └──────────────┘     └──────────────┘
```

### Service Communication Patterns

#### Synchronous (REST/gRPC)
- Used for: Real-time queries, user-facing operations
- Timeout: 30 seconds
- Retry policy: 3 attempts with exponential backoff

#### Asynchronous (Message Queue)
- Used for: Background processing, notifications, analytics
- Delivery guarantee: At-least-once
- Consumer groups for horizontal scaling

### Data Flow Example: Order Processing

1. User submits order via API Gateway
2. Order Service validates and creates order record
3. Order Service publishes `order.created` event
4. Payment Service consumes event, processes payment
5. Payment Service publishes `payment.completed` event
6. Inventory Service reserves items
7. Notification Service sends confirmation email

### Fault Tolerance

#### Circuit Breaker Pattern
```python
from circuitbreaker import circuit

@circuit(failure_threshold=5, recovery_timeout=30)
def call_external_service(url, payload):
    response = requests.post(url, json=payload, timeout=5)
    response.raise_for_status()
    return response.json()
```

#### Retry with Exponential Backoff
```python
from tenacity import retry, stop_after_attempt, wait_exponential

@retry(
    stop=stop_after_attempt(3),
    wait=wait_exponential(multiplier=1, min=1, max=10)
)
def resilient_operation():
    return perform_risky_operation()
```

### Observability Stack

- **Metrics**: Prometheus + Grafana
- **Logging**: ELK Stack (Elasticsearch, Logstash, Kibana)
- **Tracing**: Jaeger / OpenTelemetry
- **Alerting**: PagerDuty integration

### Security Layers

1. **Network**: VPC isolation, security groups
2. **Transport**: TLS 1.3, mTLS for service-to-service
3. **Application**: JWT validation, RBAC
4. **Data**: Encryption at rest (AES-256), field-level encryption for PII
""",
]

# Few-shot examples for coding tasks
FEW_SHOT_EXAMPLES = [
    """Here are some examples of how to implement common patterns:

Example 1: Implementing a Retry Decorator
```python
import functools
import time
import random

def retry(max_attempts=3, delay=1.0, backoff=2.0, exceptions=(Exception,)):
    def decorator(func):
        @functools.wraps(func)
        def wrapper(*args, **kwargs):
            attempts = 0
            current_delay = delay

            while attempts < max_attempts:
                try:
                    return func(*args, **kwargs)
                except exceptions as e:
                    attempts += 1
                    if attempts == max_attempts:
                        raise

                    jitter = random.uniform(0, 0.1) * current_delay
                    time.sleep(current_delay + jitter)
                    current_delay *= backoff

        return wrapper
    return decorator

# Usage:
@retry(max_attempts=3, delay=1.0, exceptions=(ConnectionError, TimeoutError))
def fetch_data(url):
    response = requests.get(url, timeout=5)
    response.raise_for_status()
    return response.json()
```

Example 2: Implementing a Simple Cache
```python
from functools import lru_cache
from datetime import datetime, timedelta
import threading

class TTLCache:
    def __init__(self, ttl_seconds=300):
        self._cache = {}
        self._lock = threading.RLock()
        self.ttl = timedelta(seconds=ttl_seconds)

    def get(self, key):
        with self._lock:
            if key in self._cache:
                value, expiry = self._cache[key]
                if datetime.now() < expiry:
                    return value
                del self._cache[key]
            return None

    def set(self, key, value):
        with self._lock:
            expiry = datetime.now() + self.ttl
            self._cache[key] = (value, expiry)

    def delete(self, key):
        with self._lock:
            self._cache.pop(key, None)

    def clear(self):
        with self._lock:
            self._cache.clear()

# Usage with decorator
def cached(cache, key_func=None):
    def decorator(func):
        @functools.wraps(func)
        def wrapper(*args, **kwargs):
            if key_func:
                key = key_func(*args, **kwargs)
            else:
                key = (args, tuple(sorted(kwargs.items())))

            result = cache.get(key)
            if result is not None:
                return result

            result = func(*args, **kwargs)
            cache.set(key, result)
            return result
        return wrapper
    return decorator
```

Example 3: Implementing a Rate Limiter
```python
import time
from collections import deque
import threading

class RateLimiter:
    def __init__(self, max_requests, window_seconds):
        self.max_requests = max_requests
        self.window_seconds = window_seconds
        self.requests = deque()
        self._lock = threading.Lock()

    def acquire(self):
        with self._lock:
            now = time.time()

            # Remove old requests outside the window
            while self.requests and self.requests[0] <= now - self.window_seconds:
                self.requests.popleft()

            if len(self.requests) < self.max_requests:
                self.requests.append(now)
                return True

            return False

    def wait_and_acquire(self):
        while not self.acquire():
            time.sleep(0.1)
        return True

# Token bucket implementation
class TokenBucket:
    def __init__(self, capacity, refill_rate):
        self.capacity = capacity
        self.tokens = capacity
        self.refill_rate = refill_rate
        self.last_refill = time.time()
        self._lock = threading.Lock()

    def acquire(self, tokens=1):
        with self._lock:
            self._refill()

            if self.tokens >= tokens:
                self.tokens -= tokens
                return True
            return False

    def _refill(self):
        now = time.time()
        elapsed = now - self.last_refill
        refill_amount = elapsed * self.refill_rate
        self.tokens = min(self.capacity, self.tokens + refill_amount)
        self.last_refill = now
```

Example 4: Async Context Manager
```python
import asyncio
from contextlib import asynccontextmanager

@asynccontextmanager
async def managed_resource(resource_id):
    resource = await acquire_resource(resource_id)
    try:
        yield resource
    finally:
        await release_resource(resource)

# Database connection pool example
class AsyncConnectionPool:
    def __init__(self, dsn, min_size=5, max_size=20):
        self.dsn = dsn
        self.min_size = min_size
        self.max_size = max_size
        self._pool = None
        self._lock = asyncio.Lock()

    async def initialize(self):
        import asyncpg
        self._pool = await asyncpg.create_pool(
            self.dsn,
            min_size=self.min_size,
            max_size=self.max_size
        )

    @asynccontextmanager
    async def connection(self):
        async with self._pool.acquire() as conn:
            yield conn

    @asynccontextmanager
    async def transaction(self):
        async with self._pool.acquire() as conn:
            async with conn.transaction():
                yield conn

    async def close(self):
        await self._pool.close()

# Usage:
async def get_user(pool, user_id):
    async with pool.connection() as conn:
        return await conn.fetchrow(
            'SELECT * FROM users WHERE id = $1',
            user_id
        )
```
""",

    """Here are examples of implementing design patterns in Python:

Example 1: Factory Pattern
```python
from abc import ABC, abstractmethod
from typing import Dict, Type

class Notification(ABC):
    @abstractmethod
    def send(self, recipient: str, message: str) -> bool:
        pass

class EmailNotification(Notification):
    def __init__(self, smtp_config: dict):
        self.smtp = smtp_config

    def send(self, recipient: str, message: str) -> bool:
        # Send email implementation
        print(f"Sending email to {recipient}: {message}")
        return True

class SMSNotification(Notification):
    def __init__(self, api_key: str):
        self.api_key = api_key

    def send(self, recipient: str, message: str) -> bool:
        # Send SMS implementation
        print(f"Sending SMS to {recipient}: {message}")
        return True

class PushNotification(Notification):
    def __init__(self, firebase_config: dict):
        self.firebase = firebase_config

    def send(self, recipient: str, message: str) -> bool:
        # Send push notification
        print(f"Sending push to {recipient}: {message}")
        return True

class NotificationFactory:
    _registry: Dict[str, Type[Notification]] = {}

    @classmethod
    def register(cls, notification_type: str):
        def decorator(notification_class: Type[Notification]):
            cls._registry[notification_type] = notification_class
            return notification_class
        return decorator

    @classmethod
    def create(cls, notification_type: str, **config) -> Notification:
        if notification_type not in cls._registry:
            raise ValueError(f"Unknown notification type: {notification_type}")
        return cls._registry[notification_type](**config)

# Register implementations
NotificationFactory.register("email")(EmailNotification)
NotificationFactory.register("sms")(SMSNotification)
NotificationFactory.register("push")(PushNotification)
```

Example 2: Observer Pattern
```python
from abc import ABC, abstractmethod
from typing import List, Any
from weakref import WeakSet

class Observer(ABC):
    @abstractmethod
    def update(self, event: str, data: Any) -> None:
        pass

class Observable:
    def __init__(self):
        self._observers: WeakSet[Observer] = WeakSet()

    def subscribe(self, observer: Observer) -> None:
        self._observers.add(observer)

    def unsubscribe(self, observer: Observer) -> None:
        self._observers.discard(observer)

    def notify(self, event: str, data: Any = None) -> None:
        for observer in self._observers:
            observer.update(event, data)

class EventBus:
    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._subscribers = {}
        return cls._instance

    def subscribe(self, event: str, callback):
        if event not in self._subscribers:
            self._subscribers[event] = []
        self._subscribers[event].append(callback)

    def publish(self, event: str, data: Any = None):
        if event in self._subscribers:
            for callback in self._subscribers[event]:
                callback(data)

# Usage:
event_bus = EventBus()
event_bus.subscribe("user.created", lambda user: print(f"New user: {user}"))
event_bus.publish("user.created", {"id": 1, "name": "John"})
```

Example 3: Strategy Pattern
```python
from abc import ABC, abstractmethod
from dataclasses import dataclass
from decimal import Decimal
from typing import Protocol

class PricingStrategy(Protocol):
    def calculate_price(self, base_price: Decimal, quantity: int) -> Decimal:
        ...

class RegularPricing:
    def calculate_price(self, base_price: Decimal, quantity: int) -> Decimal:
        return base_price * quantity

class BulkPricing:
    def __init__(self, threshold: int, discount: Decimal):
        self.threshold = threshold
        self.discount = discount

    def calculate_price(self, base_price: Decimal, quantity: int) -> Decimal:
        total = base_price * quantity
        if quantity >= self.threshold:
            total *= (1 - self.discount)
        return total

class TieredPricing:
    def __init__(self, tiers: list):
        self.tiers = sorted(tiers, key=lambda x: x[0])

    def calculate_price(self, base_price: Decimal, quantity: int) -> Decimal:
        total = Decimal(0)
        remaining = quantity
        prev_threshold = 0

        for threshold, discount in self.tiers:
            if remaining <= 0:
                break
            tier_qty = min(remaining, threshold - prev_threshold)
            tier_price = base_price * (1 - discount)
            total += tier_price * tier_qty
            remaining -= tier_qty
            prev_threshold = threshold

        if remaining > 0:
            total += base_price * remaining

        return total

@dataclass
class ShoppingCart:
    pricing_strategy: PricingStrategy

    def calculate_total(self, items: list) -> Decimal:
        total = Decimal(0)
        for item in items:
            total += self.pricing_strategy.calculate_price(
                item['price'], item['quantity']
            )
        return total
```

Example 4: Decorator Pattern
```python
from abc import ABC, abstractmethod
from functools import wraps
import time
import logging

class DataSource(ABC):
    @abstractmethod
    def read_data(self, key: str) -> dict:
        pass

    @abstractmethod
    def write_data(self, key: str, data: dict) -> bool:
        pass

class DatabaseSource(DataSource):
    def read_data(self, key: str) -> dict:
        # Simulated database read
        return {"key": key, "value": "from_db"}

    def write_data(self, key: str, data: dict) -> bool:
        # Simulated database write
        return True

class CachingDecorator(DataSource):
    def __init__(self, source: DataSource, cache: dict = None):
        self._source = source
        self._cache = cache or {}

    def read_data(self, key: str) -> dict:
        if key in self._cache:
            return self._cache[key]
        data = self._source.read_data(key)
        self._cache[key] = data
        return data

    def write_data(self, key: str, data: dict) -> bool:
        result = self._source.write_data(key, data)
        if result:
            self._cache[key] = data
        return result

class LoggingDecorator(DataSource):
    def __init__(self, source: DataSource, logger: logging.Logger = None):
        self._source = source
        self._logger = logger or logging.getLogger(__name__)

    def read_data(self, key: str) -> dict:
        self._logger.info(f"Reading data for key: {key}")
        start = time.time()
        result = self._source.read_data(key)
        self._logger.info(f"Read completed in {time.time() - start:.3f}s")
        return result

    def write_data(self, key: str, data: dict) -> bool:
        self._logger.info(f"Writing data for key: {key}")
        return self._source.write_data(key, data)

# Usage: Stack decorators
data_source = LoggingDecorator(CachingDecorator(DatabaseSource()))
```
""",
]

# Long system instruction templates
LONG_SYSTEM_INSTRUCTIONS = [
    """You are an expert AI coding assistant with deep knowledge of software engineering best practices. Your role is to help developers write clean, efficient, and maintainable code.

## Core Principles

1. **Code Quality**
   - Write code that is readable and self-documenting
   - Follow the principle of least surprise
   - Prefer clarity over cleverness
   - Use meaningful variable and function names

2. **Design Patterns**
   - Apply appropriate design patterns when they solve real problems
   - Avoid over-engineering and premature abstraction
   - Follow SOLID principles where applicable
   - Consider the trade-offs of each pattern

3. **Performance**
   - Write efficient code, but prioritize readability first
   - Profile before optimizing
   - Understand Big O complexity
   - Consider memory usage and cache efficiency

4. **Security**
   - Never trust user input
   - Use parameterized queries to prevent SQL injection
   - Properly escape output to prevent XSS
   - Follow the principle of least privilege
   - Keep dependencies updated

5. **Testing**
   - Write tests that document behavior
   - Aim for high coverage of critical paths
   - Use appropriate test types (unit, integration, e2e)
   - Make tests deterministic and fast

## Response Format

When providing code solutions:

1. First, understand the problem completely
2. Ask clarifying questions if requirements are ambiguous
3. Provide a working solution with explanations
4. Include error handling and edge cases
5. Suggest improvements or alternatives when relevant

## Language-Specific Guidelines

### Python
- Follow PEP 8 style guide
- Use type hints for function signatures
- Prefer f-strings for string formatting
- Use context managers for resource handling
- Leverage dataclasses and Pydantic for data models

### JavaScript/TypeScript
- Use TypeScript when possible for type safety
- Prefer async/await over callbacks
- Use modern ES6+ features appropriately
- Follow functional programming principles where beneficial
- Handle promises correctly with proper error handling

### Go
- Follow effective Go guidelines
- Use proper error handling (no panic for recoverable errors)
- Leverage goroutines and channels appropriately
- Keep functions small and focused
- Use interfaces for abstraction

### Rust
- Embrace the ownership model
- Use Result for error handling
- Prefer iterators over manual loops
- Leverage the type system for safety
- Use appropriate smart pointers

## Communication Style

- Be concise but thorough
- Explain the "why" behind recommendations
- Provide examples when helpful
- Acknowledge trade-offs and alternatives
- Be honest about limitations or uncertainty

Remember: The goal is to help developers become better at their craft, not just solve immediate problems.""",

    """You are a specialized data science and machine learning assistant. Your expertise covers the entire ML lifecycle from data exploration to model deployment.

## Capabilities

### Data Analysis
- Exploratory data analysis (EDA)
- Statistical testing and hypothesis validation
- Feature engineering and selection
- Data cleaning and preprocessing
- Visualization best practices

### Machine Learning
- Supervised learning (classification, regression)
- Unsupervised learning (clustering, dimensionality reduction)
- Deep learning architectures
- Time series forecasting
- Natural language processing
- Computer vision

### MLOps
- Model versioning and experiment tracking
- CI/CD for ML pipelines
- Model monitoring and drift detection
- A/B testing and canary deployments
- Feature stores and data versioning

## Analysis Framework

When approaching any data science problem:

1. **Understand the Business Context**
   - What problem are we solving?
   - Who are the stakeholders?
   - What decisions will this analysis inform?
   - What are the success metrics?

2. **Data Understanding**
   - What data is available?
   - What is the data quality?
   - Are there any biases or limitations?
   - How was the data collected?

3. **Methodology Selection**
   - What approach best fits the problem?
   - What are the assumptions?
   - How will we validate results?
   - What are the computational constraints?

4. **Implementation**
   - Use appropriate tools and libraries
   - Write reproducible code
   - Document assumptions and decisions
   - Create clear visualizations

5. **Evaluation**
   - Use appropriate metrics
   - Consider business impact
   - Validate with domain experts
   - Plan for monitoring

## Code Standards

When writing data science code:

```python
# Standard imports organization
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from sklearn.model_selection import train_test_split, cross_val_score
from sklearn.preprocessing import StandardScaler
from sklearn.metrics import classification_report, confusion_matrix

# Configuration
plt.style.use('seaborn-v0_8-whitegrid')
pd.set_option('display.max_columns', 100)
np.random.seed(42)

# Constants at the top
RANDOM_STATE = 42
TEST_SIZE = 0.2
CV_FOLDS = 5

# Functions with docstrings and type hints
def prepare_features(df: pd.DataFrame, target_col: str) -> tuple:
    '''Prepare features for modeling.

    Args:
        df: Input dataframe
        target_col: Name of target column

    Returns:
        Tuple of (X, y) arrays
    '''
    X = df.drop(columns=[target_col])
    y = df[target_col]
    return X, y
```

## Visualization Guidelines

- Always label axes and include titles
- Use colorblind-friendly palettes
- Choose appropriate chart types for data
- Keep visualizations simple and focused
- Include context and annotations when helpful

## Common Pitfalls to Avoid

1. **Data Leakage**: Ensure strict separation of train/test data
2. **Selection Bias**: Validate data representativeness
3. **Overfitting**: Use proper validation strategies
4. **P-hacking**: Pre-register hypotheses when possible
5. **Ignoring Domain Knowledge**: Collaborate with subject matter experts

## Communication

When explaining results:
- Start with the key insight or recommendation
- Provide supporting evidence and analysis
- Acknowledge uncertainty and limitations
- Suggest next steps or follow-up analyses
- Tailor technical depth to the audience""",

    """You are an expert DevOps and Site Reliability Engineer (SRE). Your role is to help teams build, deploy, and operate reliable, scalable systems.

## Areas of Expertise

### Infrastructure as Code
- Terraform, Pulumi, CloudFormation
- Ansible, Chef, Puppet
- Container orchestration (Kubernetes, ECS, Nomad)
- Service mesh (Istio, Linkerd)

### CI/CD
- Pipeline design and optimization
- Testing strategies (unit, integration, e2e)
- Deployment strategies (blue-green, canary, rolling)
- GitOps workflows

### Monitoring & Observability
- Metrics (Prometheus, Datadog, CloudWatch)
- Logging (ELK, Loki, Splunk)
- Tracing (Jaeger, Zipkin, OpenTelemetry)
- Alerting and on-call practices

### Cloud Platforms
- AWS, GCP, Azure
- Multi-cloud and hybrid architectures
- Cost optimization
- Security best practices

## SRE Principles

### Service Level Objectives (SLOs)

Define reliability targets based on user experience:

```yaml
# Example SLO specification
service: api-gateway
slos:
  - name: availability
    target: 99.9%
    window: 30d
    sli:
      type: availability
      good_events: successful_requests
      total_events: total_requests

  - name: latency
    target: 95%
    window: 30d
    sli:
      type: latency
      threshold_ms: 200
      percentile: 95

error_budget:
  calculation: 1 - slo_target
  burn_rate_alerts:
    - severity: critical
      burn_rate: 14.4  # 2h budget in 1h
      window: 1h
    - severity: warning
      burn_rate: 6     # 6h budget in 6h
      window: 6h
```

### Error Budgets

- Use error budgets to balance reliability and velocity
- When budget is exhausted, focus on reliability
- When budget is healthy, ship faster
- Make data-driven decisions about risk

### Incident Management

1. **Detection**: Automated alerting based on SLIs
2. **Response**: Clear on-call escalation paths
3. **Mitigation**: Runbooks and automation
4. **Resolution**: Root cause analysis
5. **Prevention**: Blameless postmortems

## Kubernetes Best Practices

### Resource Management
```yaml
apiVersion: v1
kind: Pod
spec:
  containers:
  - name: app
    resources:
      requests:
        memory: "256Mi"
        cpu: "250m"
      limits:
        memory: "512Mi"
        cpu: "500m"
    livenessProbe:
      httpGet:
        path: /health
        port: 8080
      initialDelaySeconds: 30
      periodSeconds: 10
    readinessProbe:
      httpGet:
        path: /ready
        port: 8080
      initialDelaySeconds: 5
      periodSeconds: 5
```

### Security
- Use RBAC with least privilege
- Enable Pod Security Standards
- Scan images for vulnerabilities
- Use network policies to limit traffic
- Encrypt secrets with external KMS

### Reliability
- Set appropriate resource limits
- Use Pod Disruption Budgets
- Implement graceful shutdown
- Use anti-affinity for HA
- Test failure scenarios regularly

## Monitoring Strategy

### The Four Golden Signals

1. **Latency**: Time to serve a request
2. **Traffic**: Demand on the system
3. **Errors**: Rate of failed requests
4. **Saturation**: How full the system is

### Alert Design

Good alerts are:
- **Actionable**: Someone needs to do something
- **Urgent**: Requires immediate attention
- **Symptomatic**: Based on user-visible symptoms
- **Non-noisy**: Low false positive rate

## Automation Philosophy

Automate:
- Repetitive manual tasks
- Toil that doesn't add value
- Incident response procedures
- Capacity planning

But:
- Document what you automate
- Handle failures gracefully
- Keep humans in the loop for critical decisions
- Test automation regularly""",
]


# ============================================================================
# Custom Prompt File Loading
# ============================================================================
# Extracted to prompt_loader.py for reuse and standalone validation.
# See: python tests/integration/prompt_loader.py --help

from prompt_loader import PromptLoader

# Module-level prompt loader instance
_prompt_loader: Optional[PromptLoader] = None


def load_prompts_from_file(file_path: str) -> Tuple[int, int, Dict[str, int]]:
    """Load prompts from a JSONL file.

    Supports ShareGPT, OpenAI messages, and simple formats.

    Args:
        file_path: Path to the JSONL file

    Returns:
        Tuple of (total_loaded, total_failed, format_counts)
    """
    global _prompt_loader

    if _prompt_loader is None:
        _prompt_loader = PromptLoader(
            shared_prefix_ratio=SHARED_PREFIX_RATIO,
            sampling=PROMPT_SAMPLING
        )

    if _prompt_loader.is_loaded:
        stats = _prompt_loader.get_stats()
        return stats.total_loaded, stats.total_failed, stats.format_counts

    stats = _prompt_loader.load_file(file_path)

    if stats.errors:
        for error in stats.errors[:5]:
            logger.warning(error)
        if len(stats.errors) > 5:
            logger.warning(f"... and {len(stats.errors) - 5} more errors")

    return stats.total_loaded, stats.total_failed, stats.format_counts


def get_file_prompt() -> Optional[Tuple[List[dict], str, int]]:
    """Get a prompt from the loaded file prompts.

    Uses the configured sampling strategy (random, sequential, or weighted).
    Respects SHARED_PREFIX_RATIO to group prompts by common prefix.

    Returns:
        Tuple of (messages, prefix_hash, estimated_tokens) or None if no prompts loaded.
    """
    if _prompt_loader is None or not _prompt_loader.is_loaded:
        return None

    return _prompt_loader.get_prompt()


def get_file_prompt_stats() -> Dict[str, any]:
    """Get statistics about loaded file prompts."""
    if _prompt_loader is None or not _prompt_loader.is_loaded:
        return {
            "loaded": False,
            "total_prompts": 0,
            "prefix_groups": 0,
            "avg_tokens": 0,
            "min_tokens": 0,
            "max_tokens": 0
        }

    stats = _prompt_loader.get_stats()
    return {
        "loaded": True,
        "total_prompts": stats.total_loaded,
        "prefix_groups": stats.prefix_groups,
        "avg_tokens": stats.avg_tokens,
        "min_tokens": stats.min_tokens,
        "max_tokens": stats.max_tokens
    }


def _file_prompts_loaded() -> bool:
    """Check if file prompts are loaded (for backward compatibility)."""
    return _prompt_loader is not None and len(_prompt_loader) > 0


# Backward compatibility: expose as property-like check
class _FilePromptsProxy:
    """Proxy to check if file prompts are loaded."""
    def __bool__(self):
        return _file_prompts_loaded()
    def __len__(self):
        return len(_prompt_loader) if _prompt_loader else 0

_file_prompts = _FilePromptsProxy()


# ============================================================================
# FNV-1a Hash for Backend Prediction
# ============================================================================
# Replicates Ranvier's hash_prefix() logic from src/router_service.cpp
# Used to predict backend routing when X-Backend-ID header is missing

FNV_OFFSET_BASIS = 14695981039346656037
FNV_PRIME = 1099511628211
DEFAULT_BLOCK_ALIGNMENT = 16
DEFAULT_PREFIX_TOKEN_LENGTH = 128


def fnv1a_hash_tokens(token_ids: List[int], prefix_len: int,
                      block_alignment: int = DEFAULT_BLOCK_ALIGNMENT) -> int:
    """Compute FNV-1a hash on prefix tokens, matching Ranvier's hash_prefix().

    This replicates the exact hash computation from src/router_service.cpp:244
    to predict which backend the server would route to.

    Args:
        token_ids: List of token IDs (int32)
        prefix_len: Number of tokens to hash
        block_alignment: Align to this boundary (default: 16, matches server)

    Returns:
        64-bit hash value
    """
    import struct

    # Align to block_alignment boundary (matches server logic)
    aligned_len = (prefix_len // block_alignment) * block_alignment
    if aligned_len == 0:
        aligned_len = prefix_len

    # Clamp to actual token count
    aligned_len = min(aligned_len, len(token_ids))
    if aligned_len == 0:
        return FNV_OFFSET_BASIS

    # Convert tokens to bytes (int32, little-endian to match x86)
    token_bytes = b''.join(struct.pack('<i', t) for t in token_ids[:aligned_len])

    # FNV-1a hash
    hash_val = FNV_OFFSET_BASIS
    for byte in token_bytes:
        hash_val ^= byte
        hash_val *= FNV_PRIME
        hash_val &= 0xFFFFFFFFFFFFFFFF  # Keep 64-bit

    return hash_val


def predict_backend_from_hash(token_ids: List[int], num_backends: int,
                              prefix_token_count: Optional[int] = None) -> int:
    """Predict which backend the server would route to using consistent hash.

    This replicates Ranvier's hash fallback logic for when X-Backend-ID is missing.
    Only accurate when BENCHMARK_MODE is 'prefix' or 'hash' (not 'random').

    Args:
        token_ids: Full list of token IDs for the request
        num_backends: Number of registered backends
        prefix_token_count: Optional prefix boundary (e.g., system message token count).
                           If provided, hash is computed on tokens[0:prefix_token_count].
                           Otherwise, uses DEFAULT_PREFIX_TOKEN_LENGTH (128).

    Returns:
        Predicted backend ID (1-indexed)
    """
    if not token_ids or num_backends <= 0:
        return 1  # Fallback to first backend

    # Determine prefix length (matches server logic in get_backend_for_prefix)
    if prefix_token_count and prefix_token_count > 0:
        prefix_len = min(prefix_token_count, len(token_ids))
    else:
        prefix_len = min(DEFAULT_PREFIX_TOKEN_LENGTH, len(token_ids))

    prefix_hash = fnv1a_hash_tokens(token_ids, prefix_len)
    return (prefix_hash % num_backends) + 1  # Backend IDs are 1-indexed


def estimate_tokens(text: str) -> int:
    """Estimate token count from text length.

    Uses 3.5 chars/token as a balanced approximation for mixed content:
    - Pure English prose: ~4.5 chars/token
    - Code/technical docs: ~3.0 chars/token
    - Mixed (RAG docs): ~3.5 chars/token
    """
    return max(1, int(len(text) / 3.5))


def generate_large_prefix(
    target_tokens: int,
    prefix_type: str = "mixed",
    prefix_id: int = 0
) -> str:
    """Generate a large prefix of approximately target_tokens size.

    Args:
        target_tokens: Target number of tokens (2000-8000)
        prefix_type: Type of prefix content: "rag", "fewshot", "system", "mixed"
        prefix_id: Unique identifier for this prefix (for variation)

    Returns:
        Large prefix text of approximately target_tokens
    """
    prefix_parts = []
    current_tokens = 0

    if prefix_type == "rag" or prefix_type == "mixed":
        # Add RAG document chunks
        chunks = RAG_DOCUMENT_CHUNKS.copy()
        random.shuffle(chunks)

        for chunk in chunks:
            if current_tokens >= target_tokens:
                break
            prefix_parts.append(chunk)
            current_tokens += estimate_tokens(chunk)

    if prefix_type == "fewshot" or (prefix_type == "mixed" and current_tokens < target_tokens):
        # Add few-shot examples
        for example in FEW_SHOT_EXAMPLES:
            if current_tokens >= target_tokens:
                break
            prefix_parts.append(example)
            current_tokens += estimate_tokens(example)

    if prefix_type == "system" or (prefix_type == "mixed" and current_tokens < target_tokens):
        # Add long system instructions
        for instruction in LONG_SYSTEM_INSTRUCTIONS:
            if current_tokens >= target_tokens:
                break
            prefix_parts.append(instruction)
            current_tokens += estimate_tokens(instruction)

    # If we still need more tokens, pad with documentation-style content
    padding_chunks = [
        "### Additional Context\n" + "Lorem ipsum " * 50 + "\n",
        "### Technical Specifications\n" + "Configuration details " * 40 + "\n",
        "### Implementation Notes\n" + "Reference documentation " * 45 + "\n",
    ]

    while current_tokens < target_tokens:
        for chunk in padding_chunks:
            if current_tokens >= target_tokens:
                break
            prefix_parts.append(chunk)
            current_tokens += estimate_tokens(chunk)

    # Add unique prefix identifier for routing consistency
    header = f"[Context ID: {prefix_id}]\n\n"

    return header + "\n\n".join(prefix_parts)


# Pre-generated large prefixes for consistent routing
_large_prefixes: List[Tuple[str, int]] = []  # (prefix_text, estimated_tokens)

def initialize_large_prefixes():
    """Initialize the pool of large prefixes for stress testing."""
    global _large_prefixes

    if _large_prefixes:
        return

    logger.info(f"Generating {NUM_LARGE_PREFIXES} large prefixes...")

    prefix_types = ["rag", "fewshot", "system", "mixed"]

    for i in range(NUM_LARGE_PREFIXES):
        # Random size within configured range
        target_tokens = random.randint(LARGE_PREFIX_MIN_TOKENS, LARGE_PREFIX_MAX_TOKENS)
        prefix_type = prefix_types[i % len(prefix_types)]

        prefix_text = generate_large_prefix(target_tokens, prefix_type, i)
        actual_tokens = estimate_tokens(prefix_text)

        _large_prefixes.append((prefix_text, actual_tokens))
        logger.info(f"  Generated prefix {i}: ~{actual_tokens} tokens ({prefix_type})")

    logger.info(f"Large prefix generation complete")


def get_prefix_size_bucket(token_count: int) -> str:
    """Get the size bucket for a given token count."""
    for bucket_name, min_tokens, max_tokens in PREFIX_SIZE_BUCKETS:
        if min_tokens <= token_count < max_tokens:
            return bucket_name
    return "xlarge"

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
    prefix_size_bucket: str = "unknown"
    estimated_prefix_tokens: int = 0
    # Track whether we received HTTP 200 to distinguish incomplete vs failed
    got_http_ok: bool = False
    # Sub-category for incomplete requests: streaming_timeout, read_timeout, no_data, connection_reset
    incomplete_reason: Optional[str] = None


@dataclass
class BenchmarkStats:
    """Aggregated benchmark statistics."""
    total_requests: int = 0
    successful_requests: int = 0
    failed_requests: int = 0      # Actual errors (non-2xx, parse errors, timeouts)
    incomplete_requests: int = 0  # Got HTTP 200 but terminated before TTFT recorded
    incomplete_streaming_timeout: int = 0  # STREAMING_TIMEOUT hit during iter_lines()
    incomplete_read_timeout: int = 0       # ReadTimeout during iter_lines() after HTTP 200
    incomplete_no_data: int = 0            # iter_lines() ended normally but no data: line
    incomplete_connection_reset: int = 0   # ConnectionError/socket error after HTTP 200

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

    # TTFT by prefix size bucket (for stress testing)
    ttft_by_bucket: Dict[str, List[float]] = field(default_factory=lambda: defaultdict(list))
    ttft_cache_hit_by_bucket: Dict[str, List[float]] = field(default_factory=lambda: defaultdict(list))
    ttft_cache_miss_by_bucket: Dict[str, List[float]] = field(default_factory=lambda: defaultdict(list))
    requests_by_bucket: Dict[str, int] = field(default_factory=lambda: defaultdict(int))

    # Lock for thread safety
    _lock: Lock = field(default_factory=Lock)

    def record_request(self, metrics: RequestMetrics):
        """Record metrics from a completed request."""
        with self._lock:
            self.total_requests += 1

            if metrics.ttft_ms is None:
                # Distinguish between actual failures and incomplete requests
                # Incomplete: got HTTP 200 but stream was interrupted before TTFT
                # Failed: HTTP error, connection timeout, or other actual error
                if metrics.got_http_ok:
                    self.incomplete_requests += 1
                    # Sub-categorize by reason
                    reason = metrics.incomplete_reason
                    if reason == "streaming_timeout":
                        self.incomplete_streaming_timeout += 1
                    elif reason == "read_timeout":
                        self.incomplete_read_timeout += 1
                    elif reason == "no_data":
                        self.incomplete_no_data += 1
                    elif reason == "connection_reset":
                        self.incomplete_connection_reset += 1
                else:
                    self.failed_requests += 1
                return

            self.successful_requests += 1
            self.total_prompt_tokens += metrics.prompt_tokens
            self.total_completion_tokens += metrics.completion_tokens

            if metrics.total_time_ms:
                self.total_generation_time_s += metrics.total_time_ms / 1000.0

            # Track by prefix size bucket
            bucket = metrics.prefix_size_bucket
            if bucket and bucket != "unknown":
                self.ttft_by_bucket[bucket].append(metrics.ttft_ms)
                self.requests_by_bucket[bucket] += 1

            # Track cache hits based on prefix routing
            # Skip cache tracking if backend_id is unknown (can't determine hit/miss)
            if metrics.prompt_prefix_hash and metrics.backend_id is not None:
                expected_backend = self.prefix_to_backend.get(metrics.prompt_prefix_hash)

                if expected_backend is None:
                    # First request with this prefix - cache miss
                    self.prefix_to_backend[metrics.prompt_prefix_hash] = metrics.backend_id
                    self.cache_misses += 1
                    self.ttft_cache_miss.append(metrics.ttft_ms)
                    metrics.is_cache_hit = False
                    if bucket and bucket != "unknown":
                        self.ttft_cache_miss_by_bucket[bucket].append(metrics.ttft_ms)
                elif expected_backend == metrics.backend_id:
                    # Same backend - cache hit
                    self.cache_hits += 1
                    self.ttft_cache_hit.append(metrics.ttft_ms)
                    metrics.is_cache_hit = True
                    if bucket and bucket != "unknown":
                        self.ttft_cache_hit_by_bucket[bucket].append(metrics.ttft_ms)
                else:
                    # Different backend - cache miss (routing failure)
                    self.cache_misses += 1
                    self.ttft_cache_miss.append(metrics.ttft_ms)
                    metrics.is_cache_hit = False
                    if bucket and bucket != "unknown":
                        self.ttft_cache_miss_by_bucket[bucket].append(metrics.ttft_ms)

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

            # Calculate incomplete rate (percentage of requests that were incomplete)
            incomplete_rate_pct = 0.0
            if self.total_requests > 0:
                incomplete_rate_pct = (self.incomplete_requests / self.total_requests) * 100

            summary = {
                "total_requests": self.total_requests,
                "successful_requests": self.successful_requests,
                "failed_requests": self.failed_requests,
                "incomplete_requests": self.incomplete_requests,
                "incomplete_streaming_timeout": self.incomplete_streaming_timeout,
                "incomplete_read_timeout": self.incomplete_read_timeout,
                "incomplete_no_data": self.incomplete_no_data,
                "incomplete_connection_reset": self.incomplete_connection_reset,
                "incomplete_rate_pct": incomplete_rate_pct,
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

            # Add per-bucket statistics for large-prefix stress testing
            if self.ttft_by_bucket:
                bucket_stats = {}
                for bucket_name in ["tiny", "small", "medium", "large", "xlarge"]:
                    if bucket_name in self.ttft_by_bucket and self.ttft_by_bucket[bucket_name]:
                        all_ttft = self.ttft_by_bucket[bucket_name]
                        hit_ttft = self.ttft_cache_hit_by_bucket.get(bucket_name, [])
                        miss_ttft = self.ttft_cache_miss_by_bucket.get(bucket_name, [])

                        bucket_stats[bucket_name] = {
                            "requests": self.requests_by_bucket.get(bucket_name, 0),
                            "ttft_p50_ms": self._percentile(all_ttft, 0.50),
                            "ttft_p99_ms": self._percentile(all_ttft, 0.99),
                            "ttft_min_ms": min(all_ttft) if all_ttft else None,
                            "ttft_max_ms": max(all_ttft) if all_ttft else None,
                            "cache_hit_ttft_p50_ms": self._percentile(hit_ttft, 0.50) if hit_ttft else None,
                            "cache_hit_ttft_p99_ms": self._percentile(hit_ttft, 0.99) if hit_ttft else None,
                            "cache_miss_ttft_p50_ms": self._percentile(miss_ttft, 0.50) if miss_ttft else None,
                            "cache_miss_ttft_p99_ms": self._percentile(miss_ttft, 0.99) if miss_ttft else None,
                            "cache_hit_count": len(hit_ttft),
                            "cache_miss_count": len(miss_ttft),
                        }

                        # Calculate improvement for this bucket
                        if hit_ttft and miss_ttft:
                            hit_p50 = self._percentile(hit_ttft, 0.50)
                            miss_p50 = self._percentile(miss_ttft, 0.50)
                            bucket_stats[bucket_name]["ttft_improvement_pct"] = \
                                self._calculate_improvement(miss_p50, hit_p50)

                summary["bucket_stats"] = bucket_stats

            return summary

    def _percentile(self, data: List[float], p: float) -> Optional[float]:
        """Calculate percentile using linear interpolation (matches numpy).

        For P99 with 100 samples: (100-1) * 0.99 = 98.01 -> interpolate between index 98 and 99
        This avoids the issue where int() truncation returns the max value for P99.
        """
        if not data:
            return None
        sorted_data = sorted(data)
        n = len(sorted_data)

        # Calculate interpolated index
        idx = (n - 1) * p
        lower = int(idx)
        upper = min(lower + 1, n - 1)

        # Linear interpolation between adjacent values
        weight = idx - lower
        return sorted_data[lower] * (1 - weight) + sorted_data[upper] * weight

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


def get_histogram_avg(metrics_url: str, metric_name: str) -> Optional[float]:
    """Get average value from a Prometheus histogram (sum/count)."""
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return None

        sum_val = None
        count_val = None

        for line in resp.text.split("\n"):
            if line.startswith("#"):
                continue
            if f"{metric_name}_sum" in line:
                match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                if match:
                    sum_val = float(match.group(1))
            elif f"{metric_name}_count" in line:
                match = re.search(r"(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*$", line)
                if match:
                    count_val = float(match.group(1))

        if sum_val is not None and count_val is not None and count_val > 0:
            return sum_val / count_val
        return None
    except requests.exceptions.RequestException as e:
        logger.warning(f"Failed to fetch histogram from {metrics_url}: {e}")
        return None


def get_histogram_percentile(metrics_url: str, metric_name: str, percentile: float) -> Optional[float]:
    """Calculate percentile from Prometheus histogram buckets using linear interpolation.

    Reference: https://prometheus.io/docs/practices/histograms/#quantiles

    Args:
        metrics_url: Base URL for Prometheus metrics endpoint
        metric_name: Name of the histogram metric (without _bucket suffix)
        percentile: Percentile to calculate (0.0 to 1.0, e.g., 0.5 for P50, 0.99 for P99)

    Returns:
        Estimated percentile value, or None if calculation fails
    """
    try:
        resp = requests.get(f"{metrics_url}/metrics", timeout=5)
        if resp.status_code != 200:
            return None

        # Parse bucket data: metric_name_bucket{le="X"} value
        # Buckets are cumulative counts up to boundary le
        buckets = []  # List of (upper_bound, cumulative_count)

        bucket_pattern = re.compile(
            rf'{metric_name}_bucket\{{le="([^"]+)"\}}\s+(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)'
        )

        for line in resp.text.split("\n"):
            if line.startswith("#"):
                continue
            match = bucket_pattern.search(line)
            if match:
                le_str = match.group(1)
                count = float(match.group(2))
                # Handle +Inf bucket
                if le_str == "+Inf":
                    upper_bound = float("inf")
                else:
                    upper_bound = float(le_str)
                buckets.append((upper_bound, count))

        if not buckets:
            return None

        # Sort buckets by upper bound
        buckets.sort(key=lambda x: x[0])

        # Get total count from +Inf bucket
        total_count = buckets[-1][1] if buckets else 0
        if total_count == 0:
            return None

        # Target count for the percentile
        target_count = percentile * total_count

        # Find the bucket where cumulative count crosses the target
        prev_bound = 0.0
        prev_count = 0.0

        for upper_bound, cumulative_count in buckets:
            if cumulative_count >= target_count:
                # Linear interpolation within this bucket
                # Formula: lower_bound + (upper_bound - lower_bound) * (target - prev_count) / (current - prev_count)
                if upper_bound == float("inf"):
                    # Can't interpolate into +Inf bucket, return previous bound
                    return prev_bound
                bucket_count = cumulative_count - prev_count
                if bucket_count == 0:
                    return prev_bound
                fraction = (target_count - prev_count) / bucket_count
                return prev_bound + (upper_bound - prev_bound) * fraction
            prev_bound = upper_bound
            prev_count = cumulative_count

        # If we get here, return the highest finite bucket bound
        for upper_bound, _ in reversed(buckets):
            if upper_bound != float("inf"):
                return upper_bound
        return None

    except requests.exceptions.RequestException as e:
        logger.warning(f"Failed to fetch histogram percentile from {metrics_url}: {e}")
        return None
    except (ValueError, ZeroDivisionError) as e:
        logger.warning(f"Failed to calculate percentile for {metric_name}: {e}")
        return None


def get_ranvier_latency_breakdown() -> dict:
    """Get Ranvier's internal latency breakdown from Prometheus metrics.

    Returns P50 and P99 latencies in milliseconds for:
    - routing_latency: Time spent making routing decision (includes tokenization + ART lookup)
    - tokenization_latency: Time spent tokenizing the request
    - art_lookup_latency: Time spent looking up prefix in ART (routing - tokenization)
    - connect_latency: Time to establish backend connection

    Note: Percentiles are calculated per-node then averaged across nodes.
    For accurate global percentiles, you would need histogram aggregation.
    """
    breakdown = {
        "routing_latency_p50_ms": None,
        "routing_latency_p99_ms": None,
        "tokenization_latency_p50_ms": None,
        "tokenization_latency_p99_ms": None,
        "art_lookup_latency_p50_ms": None,
        "art_lookup_latency_p99_ms": None,
        "connect_latency_p50_ms": None,
        "connect_latency_p99_ms": None,
    }

    # Collect percentiles from all nodes
    routing_p50_vals, routing_p99_vals = [], []
    tokenization_p50_vals, tokenization_p99_vals = [], []
    connect_p50_vals, connect_p99_vals = [], []

    for metrics_url in RANVIER_METRICS:
        # Routing latency (includes tokenization + ART lookup)
        # Note: Seastar metrics use "seastar_ranvier_" prefix
        # Known issue: Seastar truncates small bucket boundaries (10μs) to 0.000000,
        # making percentile calculation unreliable. Fall back to averages when needed.
        routing_p50 = get_histogram_percentile(metrics_url, "seastar_ranvier_router_routing_latency_seconds", 0.50)
        routing_p99 = get_histogram_percentile(metrics_url, "seastar_ranvier_router_routing_latency_seconds", 0.99)
        if routing_p50 is not None:
            routing_p50_vals.append(routing_p50)
            if routing_p99 is not None:
                routing_p99_vals.append(routing_p99)
        else:
            # Fall back to average if percentile fails (bucket precision issue)
            routing_avg = get_histogram_avg(metrics_url, "seastar_ranvier_router_routing_latency_seconds")
            if routing_avg is not None:
                routing_p50_vals.append(routing_avg)
                routing_p99_vals.append(routing_avg)

        # Tokenization latency
        tokenization_p50 = get_histogram_percentile(metrics_url, "seastar_ranvier_router_tokenization_latency_seconds", 0.50)
        tokenization_p99 = get_histogram_percentile(metrics_url, "seastar_ranvier_router_tokenization_latency_seconds", 0.99)
        if tokenization_p50 is not None:
            tokenization_p50_vals.append(tokenization_p50)
            if tokenization_p99 is not None:
                tokenization_p99_vals.append(tokenization_p99)
        else:
            tokenization_avg = get_histogram_avg(metrics_url, "seastar_ranvier_router_tokenization_latency_seconds")
            if tokenization_avg is not None:
                tokenization_p50_vals.append(tokenization_avg)
                tokenization_p99_vals.append(tokenization_avg)

        # Backend connect latency
        connect_p50 = get_histogram_percentile(metrics_url, "seastar_ranvier_backend_connect_duration_seconds", 0.50)
        connect_p99 = get_histogram_percentile(metrics_url, "seastar_ranvier_backend_connect_duration_seconds", 0.99)
        if connect_p50 is not None:
            connect_p50_vals.append(connect_p50)
            if connect_p99 is not None:
                connect_p99_vals.append(connect_p99)
        else:
            connect_avg = get_histogram_avg(metrics_url, "seastar_ranvier_backend_connect_duration_seconds")
            if connect_avg is not None:
                connect_p50_vals.append(connect_avg)
                connect_p99_vals.append(connect_avg)

    # Calculate averages across nodes (convert to ms)
    if routing_p50_vals:
        breakdown["routing_latency_p50_ms"] = (sum(routing_p50_vals) / len(routing_p50_vals)) * 1000
    if routing_p99_vals:
        breakdown["routing_latency_p99_ms"] = (sum(routing_p99_vals) / len(routing_p99_vals)) * 1000
    if tokenization_p50_vals:
        breakdown["tokenization_latency_p50_ms"] = (sum(tokenization_p50_vals) / len(tokenization_p50_vals)) * 1000
    if tokenization_p99_vals:
        breakdown["tokenization_latency_p99_ms"] = (sum(tokenization_p99_vals) / len(tokenization_p99_vals)) * 1000
    if connect_p50_vals:
        breakdown["connect_latency_p50_ms"] = (sum(connect_p50_vals) / len(connect_p50_vals)) * 1000
    if connect_p99_vals:
        breakdown["connect_latency_p99_ms"] = (sum(connect_p99_vals) / len(connect_p99_vals)) * 1000

    # Calculate ART lookup latency (routing - tokenization)
    if breakdown["routing_latency_p50_ms"] is not None and breakdown["tokenization_latency_p50_ms"] is not None:
        art_p50 = breakdown["routing_latency_p50_ms"] - breakdown["tokenization_latency_p50_ms"]
        breakdown["art_lookup_latency_p50_ms"] = max(0, art_p50)  # Ensure non-negative
    if breakdown["routing_latency_p99_ms"] is not None and breakdown["tokenization_latency_p99_ms"] is not None:
        art_p99 = breakdown["routing_latency_p99_ms"] - breakdown["tokenization_latency_p99_ms"]
        breakdown["art_lookup_latency_p99_ms"] = max(0, art_p99)  # Ensure non-negative

    return breakdown


def get_prefix_boundary_stats() -> dict:
    """Fetch prefix boundary usage statistics from all Ranvier nodes.

    These metrics indicate how often the system message prefix boundary
    optimization is being applied. High prefix_boundary_used ratio should
    correlate with improved cache hit rates for multi-turn conversation
    workloads with shared system prompts.

    Returns dict with:
    - prefix_boundary_used: Total count of requests where system message
      prefix boundary was identified and used for routing
    - prefix_boundary_skipped: Total count where boundary was skipped
      (no system messages, too short, disabled, or tokenization failed)
    - prefix_boundary_ratio_pct: Percentage of requests using prefix boundary
    """
    stats = {
        "prefix_boundary_used": 0,
        "prefix_boundary_skipped": 0,
        "prefix_boundary_ratio_pct": None,
    }

    for metrics_url in RANVIER_METRICS:
        used = get_metric_value(metrics_url, "seastar_ranvier_prefix_boundary_used")
        skipped = get_metric_value(metrics_url, "seastar_ranvier_prefix_boundary_skipped")

        if used is not None:
            stats["prefix_boundary_used"] += int(used)
        if skipped is not None:
            stats["prefix_boundary_skipped"] += int(skipped)

    total = stats["prefix_boundary_used"] + stats["prefix_boundary_skipped"]
    if total > 0:
        stats["prefix_boundary_ratio_pct"] = (stats["prefix_boundary_used"] / total) * 100

    return stats


def register_backends_on_all_nodes():
    """Register backends on all Ranvier nodes."""
    global _backends_registered
    if _backends_registered:
        return

    if SKIP_BACKEND_REGISTRATION:
        logger.info("Skipping backend registration (SKIP_BACKEND_REGISTRATION=true)")
        logger.info("Ensure backends are already registered via admin API or curl")
        _backends_registered = True
        return

    logger.info("Registering backends on all Ranvier nodes...")
    logger.info(f"Backends to register: {BACKENDS}")

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

    This matches what Ranvier does - extracting system messages as the
    "shared prefix" for routing affinity. Requests with the same system
    messages but different user queries should route to the same backend.
    """
    # Extract only system messages (matching Ranvier's extract_system_messages)
    system_text = ""
    for msg in messages:
        if msg.get("role") == "system":
            content = msg.get("content", "")
            system_text += content + "\n"

    if system_text:
        # Hash system messages only
        return str(hash(system_text))

    # No system messages - fall back to first 400 chars of all content
    # This handles prompts without system messages
    full_text = ""
    for msg in messages:
        role = msg.get("role", "")
        content = msg.get("content", "")
        full_text += f"{role}: {content}\n"

    prefix = full_text[:400]
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
                if "usage" in data and data["usage"] is not None:
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

def generate_prompt() -> Tuple[List[dict], str, int]:
    """Generate a prompt based on configured distribution.

    When PROMPT_FILE is set and distribution is "file", prompts are loaded
    exclusively from the file. For other distributions, file prompts are
    used as a fallback if the file is loaded and available.

    Returns:
        Tuple of (messages list, prefix_hash for cache tracking, estimated_token_count)
    """
    distribution = PROMPT_DISTRIBUTION.lower().replace("-", "_")

    # File mode: exclusively use prompts from file
    if distribution == "file":
        result = get_file_prompt()
        if result:
            return result
        # Fall back to mixed distribution if no file prompts available
        logger.warning("No file prompts available, falling back to mixed distribution")
        distribution = "mixed"

    if distribution == "short":
        messages, prefix_hash = _generate_short_prompt()
        return messages, prefix_hash, estimate_tokens(str(messages))
    elif distribution == "medium":
        messages, prefix_hash = _generate_medium_prompt()
        return messages, prefix_hash, estimate_tokens(str(messages))
    elif distribution == "long":
        messages, prefix_hash = _generate_long_prompt()
        return messages, prefix_hash, estimate_tokens(str(messages))
    elif distribution == "large_prefix":
        # Large prefix mode for stress testing KV cache
        return _generate_large_prefix_prompt()
    elif distribution == "stress":
        # Stress mode with mixed sizes biased toward large prefixes
        return _generate_stress_prompt()
    else:  # mixed
        # If PROMPT_FILE is set, mix file prompts with generated ones
        if PROMPT_FILE and _file_prompts:
            # 50% chance to use file prompts when available
            if random.random() < 0.5:
                result = get_file_prompt()
                if result:
                    return result

        # Weighted distribution: 30% short, 50% medium, 20% long
        r = random.random()
        if r < 0.3:
            messages, prefix_hash = _generate_short_prompt()
        elif r < 0.8:
            messages, prefix_hash = _generate_medium_prompt()
        else:
            messages, prefix_hash = _generate_long_prompt()
        return messages, prefix_hash, estimate_tokens(str(messages))


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


def _generate_large_prefix_prompt() -> Tuple[List[dict], str, int]:
    """Generate a prompt with a large shared prefix for stress testing.

    Returns:
        Tuple of (messages, prefix_hash, estimated_token_count)
    """
    # Ensure large prefixes are initialized
    initialize_large_prefixes()

    if not _large_prefixes:
        # Fallback to long prompt if generation failed
        messages, prefix_hash = _generate_long_prompt()
        return messages, prefix_hash, estimate_tokens(str(messages))

    # Select a prefix (shared for prefix-affinity routing)
    prefix_text, token_count = random.choice(_large_prefixes)

    # Generate different user queries for the same prefix
    user_queries = [
        "Based on the context above, summarize the key points.",
        "What are the main technical requirements described?",
        "Identify any potential issues or risks mentioned.",
        "How would you improve the implementation described?",
        "What are the dependencies between components?",
        "Provide a step-by-step implementation plan.",
        "What testing strategy would you recommend?",
        "How would you scale this architecture?",
        "What security considerations are important here?",
        "Compare the approaches mentioned and recommend one.",
    ]

    messages = [
        {"role": "system", "content": prefix_text},
        {"role": "user", "content": random.choice(user_queries)},
    ]

    return messages, hash_prompt_prefix(messages), token_count


def _generate_stress_prompt() -> Tuple[List[dict], str, int]:
    """Generate prompts with mixed sizes for comprehensive stress testing.

    Uses a distribution biased toward large prefixes to stress the KV cache.

    Returns:
        Tuple of (messages, prefix_hash, estimated_token_count)
    """
    # Weighted distribution: 10% small, 20% medium, 30% large, 40% xlarge
    r = random.random()
    if r < 0.1:
        # Small prompt
        messages, prefix_hash = _generate_short_prompt()
        return messages, prefix_hash, estimate_tokens(str(messages))
    elif r < 0.3:
        # Medium prompt
        messages, prefix_hash = _generate_medium_prompt()
        return messages, prefix_hash, estimate_tokens(str(messages))
    elif r < 0.6:
        # Large prefix (2000-4000 tokens)
        initialize_large_prefixes()
        if _large_prefixes:
            # Filter for large range
            large_prefixes = [(p, t) for p, t in _large_prefixes if 2000 <= t < 4000]
            if large_prefixes:
                prefix_text, token_count = random.choice(large_prefixes)
                messages = [
                    {"role": "system", "content": prefix_text},
                    {"role": "user", "content": "Analyze and summarize the key points."},
                ]
                return messages, hash_prompt_prefix(messages), token_count

        # Fallback
        return _generate_large_prefix_prompt()
    else:
        # XLarge prefix (4000-8000 tokens)
        initialize_large_prefixes()
        if _large_prefixes:
            # Filter for xlarge range
            xlarge_prefixes = [(p, t) for p, t in _large_prefixes if t >= 4000]
            if xlarge_prefixes:
                prefix_text, token_count = random.choice(xlarge_prefixes)
                messages = [
                    {"role": "system", "content": prefix_text},
                    {"role": "user", "content": "Provide a comprehensive analysis."},
                ]
                return messages, hash_prompt_prefix(messages), token_count

        # Fallback
        return _generate_large_prefix_prompt()


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
    logger.info(f"Number of Backends: {NUM_BACKENDS}")
    logger.info(f"Backends: {BACKENDS}")
    logger.info(f"Number of Ranvier Nodes: {NUM_RANVIER_NODES}")
    logger.info(f"Ranvier Nodes: {RANVIER_NODES}")

    # Log prefix size buckets (always useful for understanding results)
    logger.info(f"Prefix Size Buckets (tokens): tiny=0-100, small=100-500, medium=500-2000, large=2000-4000, xlarge=4000-8000")

    # Log distribution explanation
    dist = PROMPT_DISTRIBUTION.lower().replace("-", "_")
    dist_explanations = {
        "short": "100% short prompts -> mostly 'tiny' bucket (<100 tokens)",
        "medium": "100% medium prompts -> mostly 'small' bucket (100-500 tokens)",
        "long": "100% long prompts -> mostly 'small' bucket (100-500 tokens)",
        "mixed": "30% short, 50% medium, 20% long -> 'tiny' and 'small' buckets only",
        "large_prefix": "100% large prefixes -> 'large' and 'xlarge' buckets (2000-8000 tokens)",
        "stress": "10% small, 20% medium, 30% large, 40% xlarge -> biased toward large prefixes",
    }
    if dist in dist_explanations:
        logger.info(f"Distribution '{dist}': {dist_explanations[dist]}")

    # Log large prefix configuration if using stress modes
    if dist in ["large_prefix", "stress"]:
        logger.info(f"Large Prefix Config:")
        logger.info(f"  Min Tokens: {LARGE_PREFIX_MIN_TOKENS}")
        logger.info(f"  Max Tokens: {LARGE_PREFIX_MAX_TOKENS}")
        logger.info(f"  Num Prefixes: {NUM_LARGE_PREFIXES}")

    # Log generation and timeout configuration
    logger.info(f"Max Output Tokens: {MAX_OUTPUT_TOKENS}")
    logger.info(f"Timeouts: connect={CONNECT_TIMEOUT_SECONDS}s, read={READ_TIMEOUT_SECONDS}s/chunk, stream={STREAMING_TIMEOUT_SECONDS}s total")

    # Load prompts from file if PROMPT_FILE is set
    if PROMPT_FILE:
        logger.info(f"Loading prompts from file: {PROMPT_FILE}")
        logger.info(f"  Sampling strategy: {PROMPT_SAMPLING}")
        loaded, failed, format_counts = load_prompts_from_file(PROMPT_FILE)
        if loaded > 0:
            stats = get_file_prompt_stats()
            logger.info(f"  Loaded {loaded} prompts successfully")
            if failed > 0:
                logger.warning(f"  Failed to parse {failed} lines")
            logger.info(f"  Format breakdown: {format_counts}")
            logger.info(f"  Prefix groups: {stats['prefix_groups']}")
            logger.info(f"  Token stats: avg={stats['avg_tokens']}, min={stats['min_tokens']}, max={stats['max_tokens']}")
        else:
            logger.warning(f"  No prompts loaded from file - will use fallback generation")
            if dist == "file":
                logger.warning(f"  PROMPT_DISTRIBUTION=file but no prompts available!")

    logger.info("=" * 70)

    # Initialize client-side tokenizer if enabled
    # This allows bypassing ranvier's tokenization for benchmarking
    if _init_client_tokenizer():
        logger.info("Client-side tokenization ENABLED")
        logger.info("  Endpoint: /v1/completions (supports prompt_token_ids)")
        logger.info("  Ranvier: uses client tokens for routing (bypasses local tokenization)")
        logger.info("  vLLM: skips tokenization (uses prompt_token_ids directly)")
        logger.info("  Prefix hints: prefix_token_count sent for system messages")
    else:
        client_tokenize = os.environ.get("CLIENT_TOKENIZE", "false").lower() in ("true", "1", "yes")
        if client_tokenize:
            logger.warning("Client-side tokenization requested but failed to initialize")
        else:
            logger.info("Client-side tokenization DISABLED - ranvier will tokenize locally")

    # Pre-initialize large prefixes if using stress modes
    if dist in ["large_prefix", "stress"]:
        logger.info("Pre-generating large prefixes for stress testing...")
        initialize_large_prefixes()

    # Register backends
    register_backends_on_all_nodes()

    # Wait for backends to be ready
    logger.info("Waiting for backends to warm up...")
    time.sleep(5)

    # Verify routing mode matches expectations (catches BENCHMARK_MODE vs RANVIER_ROUTING_MODE mismatch)
    mode_matches, actual_mode = verify_routing_mode_matches()
    if not mode_matches:
        # Continue anyway but results will be clearly marked as potentially invalid
        logger.warning("Proceeding despite routing mode mismatch - results may be unreliable!")

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

    # Print prefix boundary optimization stats (server-side)
    prefix_stats = get_prefix_boundary_stats()
    if prefix_stats["prefix_boundary_used"] > 0 or prefix_stats["prefix_boundary_skipped"] > 0:
        logger.info(f"\nPrefix Boundary Optimization (Server-Side):")
        logger.info(f"  Prefix Boundary Used: {prefix_stats['prefix_boundary_used']}")
        logger.info(f"  Prefix Boundary Skipped: {prefix_stats['prefix_boundary_skipped']}")
        if prefix_stats["prefix_boundary_ratio_pct"] is not None:
            logger.info(f"  Usage Ratio: {prefix_stats['prefix_boundary_ratio_pct']:.1f}%")
            # Correlation hint
            if prefix_stats["prefix_boundary_ratio_pct"] < 50 and summary['cache_hit_rate_pct'] < 50:
                logger.info(f"  Note: Low prefix boundary usage may explain low cache hit rate")
                logger.info(f"        (requests may lack system messages or have short system prompts)")

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

    # Print per-bucket statistics (for large-prefix stress testing)
    bucket_stats = summary.get("bucket_stats", {})
    if bucket_stats:
        logger.info(f"\nTTFT by Prefix Size Bucket:")
        logger.info(f"  {'Bucket':<10} {'Reqs':>8} {'P50':>10} {'P99':>10} {'Hit P50':>10} {'Miss P50':>10} {'Improv%':>10}")
        logger.info(f"  {'-' * 68}")

        for bucket_name in ["tiny", "small", "medium", "large", "xlarge"]:
            if bucket_name in bucket_stats:
                b = bucket_stats[bucket_name]
                reqs = b.get("requests", 0)
                p50 = b.get("ttft_p50_ms")
                p99 = b.get("ttft_p99_ms")
                hit_p50 = b.get("cache_hit_ttft_p50_ms")
                miss_p50 = b.get("cache_miss_ttft_p50_ms")
                improv = b.get("ttft_improvement_pct")

                p50_str = f"{p50:.1f}ms" if p50 else "N/A"
                p99_str = f"{p99:.1f}ms" if p99 else "N/A"
                hit_str = f"{hit_p50:.1f}ms" if hit_p50 else "N/A"
                miss_str = f"{miss_p50:.1f}ms" if miss_p50 else "N/A"
                improv_str = f"{improv:.1f}%" if improv else "N/A"

                logger.info(f"  {bucket_name:<10} {reqs:>8} {p50_str:>10} {p99_str:>10} {hit_str:>10} {miss_str:>10} {improv_str:>10}")

        # Highlight large prefix improvements for stress testing
        large_stats = bucket_stats.get("large", {})
        xlarge_stats = bucket_stats.get("xlarge", {})

        if large_stats.get("ttft_improvement_pct"):
            logger.info(f"\n  Large Prefix (2000-4000 tokens) TTFT Improvement: {large_stats['ttft_improvement_pct']:.1f}%")
        if xlarge_stats.get("ttft_improvement_pct"):
            logger.info(f"  XLarge Prefix (4000-8000 tokens) TTFT Improvement: {xlarge_stats['ttft_improvement_pct']:.1f}%")

    # Print throughput
    logger.info(f"\nThroughput:")
    logger.info(f"  Total Prompt Tokens: {summary['total_prompt_tokens']}")
    logger.info(f"  Total Completion Tokens: {summary['total_completion_tokens']}")
    logger.info(f"  Tokens/Second: {summary['tokens_per_second']:.1f}")

    # Print request statistics
    logger.info(f"\nRequest Statistics:")
    logger.info(f"  Total Requests:      {summary['total_requests']}")
    logger.info(f"  Successful:          {summary['successful_requests']}")
    logger.info(f"  Failed (errors):     {summary['failed_requests']}")
    logger.info(f"  Incomplete (total):  {summary['incomplete_requests']}")
    if summary['incomplete_requests'] > 0:
        logger.info(f"    Streaming timeout: {summary['incomplete_streaming_timeout']}")
        logger.info(f"    Read timeout:      {summary['incomplete_read_timeout']}")
        logger.info(f"    No data received:  {summary['incomplete_no_data']}")
        logger.info(f"    Connection reset:  {summary['incomplete_connection_reset']}")
        uncategorized = (summary['incomplete_requests']
                         - summary['incomplete_streaming_timeout']
                         - summary['incomplete_read_timeout']
                         - summary['incomplete_no_data']
                         - summary['incomplete_connection_reset'])
        if uncategorized > 0:
            logger.info(f"    Uncategorized:     {uncategorized}")

    # Warn if incomplete rate is high (>10%)
    incomplete_rate = summary.get('incomplete_rate_pct', 0)
    if incomplete_rate > 10:
        reasons = {
            "streaming timeout": summary['incomplete_streaming_timeout'],
            "read timeout": summary['incomplete_read_timeout'],
            "no data received": summary['incomplete_no_data'],
            "connection reset": summary['incomplete_connection_reset'],
        }
        dominant = max(reasons, key=reasons.get) if any(reasons.values()) else "unknown"
        dominant_count = reasons.get(dominant, 0)
        dominant_pct = (dominant_count / summary['incomplete_requests'] * 100
                        if summary['incomplete_requests'] > 0 else 0)
        logger.warning(
            f"High incomplete rate ({incomplete_rate:.1f}%): "
            f"{summary['incomplete_requests']} requests terminated before TTFT. "
            f"Dominant reason: {dominant} ({dominant_pct:.0f}%). "
            "Consider reducing --users or increasing --stop-timeout."
        )

    # Print Ranvier internal latency breakdown (P50/P99)
    latency_breakdown = get_ranvier_latency_breakdown()
    logger.info(f"\nRanvier Latency Breakdown (percentiles):")
    logger.info(f"  {'Metric':<25} {'P50':>10} {'P99':>10}")
    logger.info(f"  {'-'*45}")

    # Routing Decision (total)
    p50 = latency_breakdown.get("routing_latency_p50_ms")
    p99 = latency_breakdown.get("routing_latency_p99_ms")
    if p50 is not None or p99 is not None:
        p50_str = f"{p50:.2f}ms" if p50 is not None else "N/A"
        p99_str = f"{p99:.2f}ms" if p99 is not None else "N/A"
        logger.info(f"  {'Routing Decision':<25} {p50_str:>10} {p99_str:>10}")

    # Tokenization (sub-component of routing)
    p50 = latency_breakdown.get("tokenization_latency_p50_ms")
    p99 = latency_breakdown.get("tokenization_latency_p99_ms")
    if p50 is not None or p99 is not None:
        p50_str = f"{p50:.2f}ms" if p50 is not None else "N/A"
        p99_str = f"{p99:.2f}ms" if p99 is not None else "N/A"
        logger.info(f"  {'  - Tokenization':<25} {p50_str:>10} {p99_str:>10}")

    # ART Lookup (sub-component of routing)
    p50 = latency_breakdown.get("art_lookup_latency_p50_ms")
    p99 = latency_breakdown.get("art_lookup_latency_p99_ms")
    if p50 is not None or p99 is not None:
        p50_str = f"{p50:.2f}ms" if p50 is not None else "N/A"
        p99_str = f"{p99:.2f}ms" if p99 is not None else "N/A"
        logger.info(f"  {'  - ART Lookup':<25} {p50_str:>10} {p99_str:>10}")

    # Backend Connect
    p50 = latency_breakdown.get("connect_latency_p50_ms")
    p99 = latency_breakdown.get("connect_latency_p99_ms")
    if p50 is not None or p99 is not None:
        p50_str = f"{p50:.2f}ms" if p50 is not None else "N/A"
        p99_str = f"{p99:.2f}ms" if p99 is not None else "N/A"
        logger.info(f"  {'Backend Connect':<25} {p50_str:>10} {p99_str:>10}")

    # Add to summary for JSON output
    summary.update(latency_breakdown)
    summary.update(prefix_stats)

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

        # Generate prompt (now returns token count for stress testing)
        messages, prefix_hash, estimated_tokens = generate_prompt()

        # Determine prefix size bucket for metrics tracking
        prefix_size_bucket = get_prefix_size_bucket(estimated_tokens)

        # Build request body based on whether client tokenization is enabled
        # When enabled, use /v1/completions endpoint which supports prompt_token_ids
        # This allows vLLM to skip tokenization entirely for maximum efficiency
        token_ids = tokenize_messages(messages)

        if token_ids is not None:
            # Client tokenization enabled - use /v1/completions endpoint
            # Convert messages to prompt string and include token IDs
            endpoint = "/v1/completions"
            request_body = {
                "model": os.environ.get("VLLM_MODEL", "default"),
                "prompt": messages_to_prompt(messages),
                "prompt_token_ids": token_ids,
                "stream": True,
                "stream_options": {"include_usage": True},
                "max_tokens": MAX_OUTPUT_TOKENS,
            }

            # Calculate and include prefix_token_count for routing hints
            # This tells Ranvier how many tokens are the "shared prefix" (system messages)
            prefix_token_count = tokenize_system_messages(messages)
            if prefix_token_count is not None and prefix_token_count > 0:
                request_body["prefix_token_count"] = prefix_token_count
        else:
            # No client tokenization - use standard chat completions endpoint
            endpoint = "/v1/chat/completions"
            request_body = {
                "model": os.environ.get("VLLM_MODEL", "default"),
                "messages": messages,
                "stream": True,
                "stream_options": {"include_usage": True},
                "max_tokens": MAX_OUTPUT_TOKENS,
            }

        # Initialize metrics
        metrics = RequestMetrics(
            request_id=f"req-{self._request_count}",
            prompt_prefix_hash=prefix_hash,
            routing_mode=BENCHMARK_MODE,
            prefix_size_bucket=prefix_size_bucket,
            estimated_prefix_tokens=estimated_tokens,
        )

        start_time = time.perf_counter()
        ttft = None
        response_text = ""

        try:
            # Use tuple timeout: (connect_timeout, read_timeout)
            # read_timeout applies to each socket read, preventing indefinite blocking
            # on iter_lines() when vLLM is slow or unresponsive
            resp = requests.post(
                f"{target_url}{endpoint}",
                json=request_body,
                headers={"Content-Type": "application/json"},
                stream=True,
                timeout=(CONNECT_TIMEOUT_SECONDS, READ_TIMEOUT_SECONDS),
            )

            # Get backend ID from response headers (Ranvier sets X-Backend-ID)
            metrics.backend_id = get_backend_from_response(dict(resp.headers))
            if metrics.backend_id is None:
                if SINGLE_BACKEND_MODE:
                    # Single backend mode: all requests go to backend 1
                    metrics.backend_id = "1"
                elif BENCHMARK_MODE == "random":
                    # Random mode: can't predict backend - exclude from cache tracking
                    logger.warning("X-Backend-ID header missing in random mode - "
                                  "excluding from cache hit tracking")
                    # metrics.backend_id remains None
                elif token_ids is not None:
                    # CLIENT_TOKENIZE enabled - predict backend using hash
                    # This replicates Ranvier's consistent hash fallback logic
                    ptc = request_body.get("prefix_token_count")
                    predicted = predict_backend_from_hash(token_ids, NUM_BACKENDS, ptc)
                    metrics.backend_id = str(predicted)
                    logger.debug(f"X-Backend-ID missing - predicted backend {predicted} from hash")
                else:
                    # No tokens available (CLIENT_TOKENIZE disabled) - can't predict
                    logger.warning("X-Backend-ID header missing and CLIENT_TOKENIZE disabled - "
                                  "excluding from cache hit tracking")
                    # metrics.backend_id remains None

            if resp.status_code != 200:
                events.request.fire(
                    request_type="POST",
                    name=f"{endpoint} (node{node_index + 1})",
                    response_time=(time.perf_counter() - start_time) * 1000,
                    response_length=0,
                    exception=Exception(f"Status: {resp.status_code}"),
                    context={},
                )
                _benchmark_stats.record_request(metrics)
                return

            # Mark that we received HTTP 200 - distinguishes incomplete from failed
            metrics.got_http_ok = True

            # Process streaming response with timeout protection
            stream_timed_out = False
            for line in resp.iter_lines():
                # Check for streaming timeout to prevent indefinite blocking
                elapsed_seconds = time.perf_counter() - start_time
                if elapsed_seconds > STREAMING_TIMEOUT_SECONDS:
                    stream_timed_out = True
                    metrics.incomplete_reason = "streaming_timeout"
                    logger.warning(
                        f"Streaming timeout after {elapsed_seconds:.1f}s "
                        f"(limit: {STREAMING_TIMEOUT_SECONDS}s)"
                    )
                    break

                if line:
                    decoded = line.decode("utf-8")
                    response_text += decoded + "\n"

                    if decoded.startswith("data: "):
                        if ttft is None:
                            ttft = (time.perf_counter() - start_time) * 1000

                        if decoded == "data: [DONE]":
                            break

            total_time = (time.perf_counter() - start_time) * 1000

            # Handle streaming timeout as an error
            if stream_timed_out:
                events.request.fire(
                    request_type="POST",
                    name=f"{endpoint} (node{node_index + 1})",
                    response_time=total_time,
                    response_length=len(response_text),
                    exception=Exception(f"Streaming timeout after {STREAMING_TIMEOUT_SECONDS}s"),
                    context={},
                )
                _benchmark_stats.record_request(metrics)
                return

            # Parse usage stats
            prompt_tokens, completion_tokens = parse_sse_usage(response_text)
            metrics.prompt_tokens = prompt_tokens
            metrics.completion_tokens = completion_tokens
            metrics.ttft_ms = ttft
            metrics.total_time_ms = total_time

            # Sub-categorize incomplete: iter_lines() ended without any data: line
            if ttft is None and not stream_timed_out:
                metrics.incomplete_reason = "no_data"

            # Record to global stats
            _benchmark_stats.record_request(metrics)

            # Fire Locust events
            events.request.fire(
                request_type="POST",
                name=f"{endpoint} (node{node_index + 1})",
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

                # Record bucket-specific TTFT for stress testing
                if prefix_size_bucket and prefix_size_bucket != "unknown":
                    events.request.fire(
                        request_type="GET",
                        name=f"TTFT ({prefix_size_bucket})",
                        response_time=ttft,
                        response_length=0,
                        exception=None,
                        context={},
                    )

                    # Record bucket + cache status for detailed analysis
                    events.request.fire(
                        request_type="GET",
                        name=f"TTFT ({prefix_size_bucket} {cache_status})",
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

        except ReadTimeout as e:
            total_time = (time.perf_counter() - start_time) * 1000
            if metrics.got_http_ok:
                metrics.incomplete_reason = "read_timeout"
            error_msg = f"Socket read timeout after {READ_TIMEOUT_SECONDS}s - vLLM may be overloaded or unresponsive"
            events.request.fire(
                request_type="POST",
                name=f"{endpoint} (node{node_index + 1})",
                response_time=total_time,
                response_length=0,
                exception=Exception(error_msg),
                context={},
            )
            _benchmark_stats.record_request(metrics)
            logger.warning(f"Request to node{node_index + 1} failed: {error_msg}")
        except ConnectTimeout as e:
            total_time = (time.perf_counter() - start_time) * 1000
            error_msg = f"Connection timeout after {CONNECT_TIMEOUT_SECONDS}s - Ranvier node may be down"
            events.request.fire(
                request_type="POST",
                name=f"{endpoint} (node{node_index + 1})",
                response_time=total_time,
                response_length=0,
                exception=Exception(error_msg),
                context={},
            )
            _benchmark_stats.record_request(metrics)
            logger.warning(f"Request to node{node_index + 1} failed: {error_msg}")
        except Exception as e:
            total_time = (time.perf_counter() - start_time) * 1000
            if metrics.got_http_ok:
                metrics.incomplete_reason = "connection_reset"
            events.request.fire(
                request_type="POST",
                name=f"{endpoint} (node{node_index + 1})",
                response_time=total_time,
                response_length=0,
                exception=e,
                context={},
            )
            _benchmark_stats.record_request(metrics)
            logger.warning(f"Request to node{node_index + 1} failed: {e}")
