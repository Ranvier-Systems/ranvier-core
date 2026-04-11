#!/usr/bin/env python3
"""Smoke tests for the §15 Intelligence Layer and v2.1.0 features.

These are the first pytest-style integration tests in ``tests/integration/``
and the proof point for the shared harness defined in ``conftest.py``.

Coverage scope (deliberately small — this is PR #1 of a multi-PR §6 plan):

* **v2.1.0 partial tokenization** — verify ``tokenization_partial_total``
  increments for routing-path requests and ``tokenization_deferred_full_total``
  does not (token forwarding is disabled in the default test config).

* **§15 1.2 priority tier classification** — verify that
  ``X-Ranvier-Priority`` header requests bump the matching
  ``proxy_requests_by_priority{priority=...}`` counter.

* **§15 1.4 intent classification** — verify that EDIT and CHAT requests
  bump the matching ``proxy_requests_by_intent{intent=...}`` counter.

All three tests use the session-scoped ``ranvier_cluster`` fixture.  The
docker-compose test profile enables every §15 toggle, so the classifiers
and metrics are live.

Build constraint: requires Docker and a built Ranvier image.  Do not run in
the sandbox — the developer runs these in their Docker environment.
"""

from __future__ import annotations

from conftest import (
    metric_is_registered,
    send_chat_request,
    sum_metric_by_labels,
    sum_metric_by_substring,
)

# Substrings used to locate metrics regardless of Seastar's prefix.
# Seastar registers metrics under the ``seastar_ranvier_`` namespace, so
# substring matching is more robust than hard-coding the prefix.
METRIC_TOKENIZATION_PARTIAL = "tokenization_partial_total"
METRIC_TOKENIZATION_DEFERRED_FULL = "tokenization_deferred_full_total"
METRIC_PROXY_REQUESTS_BY_PRIORITY = "proxy_requests_by_priority"
METRIC_PROXY_REQUESTS_BY_INTENT = "proxy_requests_by_intent"


def test_partial_tokenization_metric_increments(ranvier_cluster):
    """A routing-path request should increment the partial tokenization counter.

    Covers v2.1.0: partial tokenization truncates the tokenizer input to a
    byte budget so only the prefix needed for routing is produced.  The
    counter is incremented once per request whose input is truncated.

    We also verify that ``tokenization_deferred_full_total`` does NOT
    increment: that counter only fires when token forwarding is enabled,
    which is off in ``docker-compose.test.yml`` by default.
    """
    metrics_url = ranvier_cluster.nodes["node1"]["metrics"]
    api_url = ranvier_cluster.nodes["node1"]["api"]

    # Distinguish "binary lacks the feature" from "counter didn't bump".
    # Both produce a 0 from sum_metric_by_substring, so we check
    # registration up front and fail with an actionable message if the
    # counter is missing. Common cause: stale Docker image predating the
    # partial-tokenization PR (#417) — rebuild with `docker compose build`.
    assert metric_is_registered(metrics_url, METRIC_TOKENIZATION_PARTIAL), (
        f"{METRIC_TOKENIZATION_PARTIAL} is not exposed in /metrics — the "
        f"running Ranvier binary predates BACKLOG §1.4 (PR #417). Rebuild "
        f"the image with `docker compose -f docker-compose.test.yml build` "
        f"and retry."
    )
    assert metric_is_registered(metrics_url, METRIC_TOKENIZATION_DEFERRED_FULL), (
        f"{METRIC_TOKENIZATION_DEFERRED_FULL} is not exposed in /metrics — "
        f"the running Ranvier binary predates BACKLOG §1.4 (PR #417). "
        f"Rebuild the image with "
        f"`docker compose -f docker-compose.test.yml build` and retry."
    )

    before_partial = sum_metric_by_substring(metrics_url, METRIC_TOKENIZATION_PARTIAL)
    before_deferred = sum_metric_by_substring(metrics_url, METRIC_TOKENIZATION_DEFERRED_FULL)
    print(f"  Before: partial={before_partial}, deferred_full={before_deferred}")

    # The truncation budget is `prefix_token_length * partial_tokenize_bytes_per_token`
    # = 128 * 6 = 768 bytes by default (see src/http_controller.cpp:114-116 and
    # src/config_schema.hpp:59,257). Truncation only fires when the extracted
    # text exceeds this budget, so we need a content string comfortably larger.
    # Using ~2000 bytes gives us a wide safety margin against any future
    # budget bumps.
    base = (
        "This is a sufficiently long test prompt for the Ranvier partial "
        "tokenization smoke test. It contains well over the seven-hundred-"
        "sixty-eight byte truncation threshold so that the routing tokenizer "
        "actually engages the byte-budget truncation path introduced in "
        "v2.1.0 (BACKLOG section 1.4). "
    )
    long_content = base * 8  # ~2000 bytes
    assert len(long_content) > 768, (
        f"Test prompt must exceed the 768-byte routing budget; got "
        f"{len(long_content)} bytes"
    )

    status, response_text, _headers = send_chat_request(
        api_url,
        messages=[{"role": "user", "content": long_content}],
    )
    assert status == 200, f"Chat request failed: status={status} body={response_text!r}"
    assert "backend" in response_text.lower(), (
        f"Mock backend response missing expected marker: {response_text!r}"
    )

    after_partial = sum_metric_by_substring(metrics_url, METRIC_TOKENIZATION_PARTIAL)
    after_deferred = sum_metric_by_substring(metrics_url, METRIC_TOKENIZATION_DEFERRED_FULL)
    print(f"  After:  partial={after_partial}, deferred_full={after_deferred}")

    assert after_partial >= before_partial + 1, (
        f"tokenization_partial_total should have incremented by >=1, "
        f"but went from {before_partial} to {after_partial}"
    )
    # Token forwarding is disabled in the default test config, so the
    # deferred-full tokenization path must not be exercised.
    assert after_deferred == before_deferred, (
        f"tokenization_deferred_full_total should NOT increment when "
        f"token forwarding is disabled, but went from "
        f"{before_deferred} to {after_deferred}"
    )


def test_priority_tier_header_classifies_request(ranvier_cluster):
    """X-Ranvier-Priority: <tier> must bump the matching priority counter.

    Covers §15 1.2: explicit priority-tier classification via request
    header takes precedence over User-Agent matching and cost-based
    fallback.  We send three requests — ``critical``, ``low``, and no
    header — and assert each one bumps the correct per-label counter by
    exactly 1.

    For the no-header case we assert against the *total* across every
    priority label rather than a specific tier, because the cost-based
    fallback classifies small requests as LOW (below cost_threshold_low)
    even though the config default is NORMAL.  The intent of this check
    is "some tier counter increments", not "cost-based heuristic returns
    a specific value".
    """
    metrics_url = ranvier_cluster.nodes["node1"]["metrics"]
    api_url = ranvier_cluster.nodes["node1"]["api"]

    # Case 1: explicit critical header.
    before_crit = sum_metric_by_labels(
        metrics_url, METRIC_PROXY_REQUESTS_BY_PRIORITY, {"priority": "critical"}
    )
    print(f"  Before critical: counter={before_crit}")
    status, body, _ = send_chat_request(
        api_url,
        messages=[{"role": "user", "content": "critical priority test"}],
        extra_headers={"X-Ranvier-Priority": "critical"},
    )
    assert status == 200, f"critical request failed: {body!r}"
    after_crit = sum_metric_by_labels(
        metrics_url, METRIC_PROXY_REQUESTS_BY_PRIORITY, {"priority": "critical"}
    )
    print(f"  After critical:  counter={after_crit}")
    assert after_crit == before_crit + 1, (
        f"proxy_requests_by_priority{{priority=\"critical\"}} should have "
        f"incremented by exactly 1, but went from {before_crit} to {after_crit}"
    )

    # Case 2: explicit low header.
    before_low = sum_metric_by_labels(
        metrics_url, METRIC_PROXY_REQUESTS_BY_PRIORITY, {"priority": "low"}
    )
    print(f"  Before low: counter={before_low}")
    status, body, _ = send_chat_request(
        api_url,
        messages=[{"role": "user", "content": "low priority test"}],
        extra_headers={"X-Ranvier-Priority": "low"},
    )
    assert status == 200, f"low request failed: {body!r}"
    after_low = sum_metric_by_labels(
        metrics_url, METRIC_PROXY_REQUESTS_BY_PRIORITY, {"priority": "low"}
    )
    print(f"  After low:  counter={after_low}")
    assert after_low == before_low + 1, (
        f"proxy_requests_by_priority{{priority=\"low\"}} should have "
        f"incremented by exactly 1, but went from {before_low} to {after_low}"
    )

    # Case 3: no header — falls through to cost-based classification.
    # We assert on the total across all priority labels instead of a
    # specific tier to stay robust against cost-threshold tuning.
    before_total = sum_metric_by_substring(metrics_url, METRIC_PROXY_REQUESTS_BY_PRIORITY)
    print(f"  Before default: total={before_total}")
    status, body, _ = send_chat_request(
        api_url,
        messages=[{"role": "user", "content": "default priority (no header)"}],
    )
    assert status == 200, f"default request failed: {body!r}"
    after_total = sum_metric_by_substring(metrics_url, METRIC_PROXY_REQUESTS_BY_PRIORITY)
    print(f"  After default:  total={after_total}")
    assert after_total == before_total + 1, (
        f"proxy_requests_by_priority total should have incremented by "
        f"exactly 1 when no header is set, but went from "
        f"{before_total} to {after_total}"
    )


def test_intent_classification_observable(ranvier_cluster):
    """EDIT and CHAT intents must be observable via proxy_requests_by_intent.

    Covers §15 1.4: the intent classifier scans the first system message
    for tag patterns (``<edit>``, ``<diff>``, ``<rewrite>``, ``<patch>``)
    and keyword substrings (``diff``, ``rewrite``, ``refactor``, ``edit``,
    ``patch``, ``apply``).  A request with ``<edit>`` in the system content
    must classify as EDIT.  A request with no system message at all must
    fall through to the CHAT default.

    We snapshot both intent counters before, send one EDIT request and
    one CHAT request, and assert each bucket incremented by exactly 1.
    """
    metrics_url = ranvier_cluster.nodes["node1"]["metrics"]
    api_url = ranvier_cluster.nodes["node1"]["api"]

    before_edit = sum_metric_by_labels(
        metrics_url, METRIC_PROXY_REQUESTS_BY_INTENT, {"intent": "edit"}
    )
    before_chat = sum_metric_by_labels(
        metrics_url, METRIC_PROXY_REQUESTS_BY_INTENT, {"intent": "chat"}
    )
    print(f"  Before: edit={before_edit} chat={before_chat}")

    # EDIT: system message contains the literal <edit> tag pattern, which
    # the classifier matches case-sensitively (see src/intent_classifier.cpp).
    edit_status, edit_response, _ = send_chat_request(
        api_url,
        messages=[
            {
                "role": "system",
                "content": "You handle <edit> operations on source files.",
            },
            {"role": "user", "content": "Please update this function."},
        ],
    )
    assert edit_status == 200, f"EDIT request failed: {edit_response!r}"

    # CHAT: no system role at all.  The classifier walks looking for
    # "role":"system", finds nothing, and falls through to the CHAT default.
    # This avoids accidentally matching any of the edit keywords (which
    # includes the substring "edit", so even "editor" would match).
    chat_status, chat_response, _ = send_chat_request(
        api_url,
        messages=[
            {"role": "user", "content": "Hello, how are you today?"},
        ],
    )
    assert chat_status == 200, f"CHAT request failed: {chat_response!r}"

    after_edit = sum_metric_by_labels(
        metrics_url, METRIC_PROXY_REQUESTS_BY_INTENT, {"intent": "edit"}
    )
    after_chat = sum_metric_by_labels(
        metrics_url, METRIC_PROXY_REQUESTS_BY_INTENT, {"intent": "chat"}
    )
    print(f"  After:  edit={after_edit} chat={after_chat}")

    assert after_edit == before_edit + 1, (
        f"proxy_requests_by_intent{{intent=\"edit\"}} should have "
        f"incremented by exactly 1, but went from {before_edit} to {after_edit}"
    )
    assert after_chat == before_chat + 1, (
        f"proxy_requests_by_intent{{intent=\"chat\"}} should have "
        f"incremented by exactly 1, but went from {before_chat} to {after_chat}"
    )
