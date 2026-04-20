#!/usr/bin/env python3
"""Streaming response integration tests for Ranvier Core.

Covers two BACKLOG items:
- §6.2 "Create streaming response test suite"
- §6.7 "Test large payload handling"

Uses the ``ClusterTestCase`` harness from ``conftest.py``.  Depends on the
enhanced mock backend (§6.1) for latency injection, failure-mode
simulation, and the ``/debug/requests`` log.
"""

from __future__ import annotations

import json
import os
import resource
import sys
import time
import unittest
import uuid

try:
    import requests
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)

from conftest import (
    ClusterTestCase,
    DOCKER_HOST,
    MOCK_BACKEND_PORTS,
    NODES,
    REQUEST_TIMEOUT,
    free_docker_subnet,
    send_chat_request,
)


def _set_failure_mode(backend_id: int, mode: str) -> None:
    """Set sticky failure mode on one backend via its admin port."""
    port = MOCK_BACKEND_PORTS[backend_id]
    resp = requests.post(
        f"http://{DOCKER_HOST}:{port}/admin/failure-mode",
        params={"mode": mode},
        timeout=5,
    )
    resp.raise_for_status()


def _set_all_failure_modes(mode: str) -> None:
    for bid in MOCK_BACKEND_PORTS:
        _set_failure_mode(bid, mode)


def _get_debug_requests(backend_id: int):
    port = MOCK_BACKEND_PORTS[backend_id]
    resp = requests.get(
        f"http://{DOCKER_HOST}:{port}/debug/requests", timeout=5,
    )
    resp.raise_for_status()
    return resp.json()


def _clear_debug_requests(backend_id: int):
    port = MOCK_BACKEND_PORTS[backend_id]
    resp = requests.delete(
        f"http://{DOCKER_HOST}:{port}/debug/requests", timeout=5,
    )
    resp.raise_for_status()


def _find_request_by_id(request_id: str):
    """Return ``(backend_id, entry)`` for the first match across both backends."""
    for bid in MOCK_BACKEND_PORTS:
        for entry in _get_debug_requests(bid):
            if entry.get("headers", {}).get("X-Request-ID") == request_id:
                return bid, entry
    return None, None


def _stream_request(
    api_url: str,
    messages,
    *,
    extra_headers=None,
    timeout: int = REQUEST_TIMEOUT,
):
    """Raw ``requests.post`` with ``stream=True``.

    ``send_chat_request`` in conftest fully drains the body before returning,
    which defeats assertions about interleaved chunk arrival and header
    timing.  This helper returns the live ``Response`` so the caller can
    iterate lines or chunks and inspect byte counts incrementally.  Kept
    local to this file because only the streaming tests need it.
    """
    body = {"model": "test-model", "messages": messages, "stream": True}
    headers = {"Content-Type": "application/json"}
    if extra_headers:
        headers.update(extra_headers)
    return requests.post(
        f"{api_url}/v1/chat/completions",
        json=body,
        headers=headers,
        stream=True,
        timeout=timeout,
    )


def _read_vm_rss_kb() -> int:
    """Return the current process's RSS in KB from /proc/self/status.

    Falls back to ``getrusage(RUSAGE_SELF).ru_maxrss`` (which is KB on Linux)
    if /proc isn't available.
    """
    try:
        with open("/proc/self/status", "r") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    # "VmRSS:\t<N> kB"
                    return int(line.split()[1])
    except OSError:
        pass
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


class StreamingTest(ClusterTestCase):
    """Streaming-response behavior and large-payload handling."""

    PROJECT_NAME = "ranvier-streaming-test"
    AUTO_REGISTER_BACKENDS = True

    @classmethod
    def setUpClass(cls):
        free_docker_subnet()
        super().setUpClass()

    def setUp(self):
        # Every test starts with a clean admin state so one test's failure
        # mode can't bleed into the next.  The happy-path tests depend on
        # mode=none on both backends.
        _set_all_failure_modes("none")

    # ------------------------------------------------------------------
    # test_01: SSE line format
    # ------------------------------------------------------------------

    def test_01_sse_format_valid(self):
        """Every non-blank SSE line is ``data: {json}`` or ``data: [DONE]``."""
        print("\nTest: SSE format is valid")
        api_url = NODES["node1"]["api"]

        resp = _stream_request(
            api_url, [{"role": "user", "content": "sse format check"}],
        )
        self.assertEqual(resp.status_code, 200)

        saw_content = False
        saw_stop = False
        saw_done = False
        chunk_count = 0

        for raw_line in resp.iter_lines():
            if not raw_line:
                continue
            line = raw_line.decode("utf-8")
            self.assertTrue(
                line.startswith("data: "),
                f"Every non-blank line must start with 'data: ', got: {line!r}",
            )
            payload = line[len("data: "):]
            if payload == "[DONE]":
                saw_done = True
                continue

            chunk = json.loads(payload)  # JSON chunks must parse
            chunk_count += 1

            self.assertEqual(chunk.get("object"), "chat.completion.chunk")
            self.assertIn("id", chunk)
            self.assertIsInstance(chunk["id"], str)
            self.assertTrue(chunk["id"], "chunk id should be non-empty")
            self.assertIn("created", chunk)
            self.assertIsInstance(chunk["created"], int)
            self.assertIn("model", chunk)
            self.assertIsInstance(chunk["model"], str)

            choices = chunk.get("choices")
            self.assertIsInstance(choices, list)
            self.assertTrue(len(choices) >= 1)
            delta = choices[0].get("delta")
            self.assertIsInstance(delta, dict)

            if isinstance(delta.get("content"), str) and delta["content"]:
                saw_content = True
            if choices[0].get("finish_reason") == "stop":
                saw_stop = True

        self.assertTrue(chunk_count > 0, "Expected at least one JSON chunk")
        self.assertTrue(saw_content, "Expected at least one chunk with delta.content")
        self.assertTrue(saw_stop, "Expected final chunk with finish_reason='stop'")
        self.assertTrue(saw_done, "Expected [DONE] sentinel")
        print(f"  PASSED ({chunk_count} chunks)")

    # ------------------------------------------------------------------
    # test_02: Transfer-Encoding: chunked, no Content-Length, actually streamed
    # ------------------------------------------------------------------

    def test_02_chunked_transfer_encoding(self):
        """Response uses chunked encoding and is actually streamed."""
        print("\nTest: Transfer-Encoding: chunked with incremental delivery")
        api_url = NODES["node1"]["api"]

        # Add per-chunk latency so buffering vs. streaming is observable:
        # with 100ms between chunks, a buffered proxy delivers all bytes in
        # one read, while a streaming proxy delivers multiple.
        resp = _stream_request(
            api_url,
            [{"role": "user", "content": "chunked encoding check"}],
            extra_headers={"X-Mock-Latency-Ms": "100"},
        )
        self.assertEqual(resp.status_code, 200)

        te = resp.headers.get("Transfer-Encoding", "")
        self.assertEqual(
            te.lower(), "chunked",
            f"Expected Transfer-Encoding: chunked, got {te!r}",
        )
        self.assertNotIn(
            "Content-Length", resp.headers,
            "A chunked streaming response must not advertise Content-Length",
        )

        reads_with_data = 0
        total_bytes = 0
        for chunk in resp.iter_content(chunk_size=None):
            if chunk:
                reads_with_data += 1
                total_bytes += len(chunk)

        self.assertTrue(total_bytes > 0, "Expected non-empty body")
        self.assertGreater(
            reads_with_data, 1,
            "Response was delivered in a single read — either the proxy "
            "buffered the whole body or the mock backend collapsed chunks",
        )
        print(f"  PASSED ({reads_with_data} reads, {total_bytes} bytes)")

    # ------------------------------------------------------------------
    # test_03: [DONE] is the final non-empty data: line
    # ------------------------------------------------------------------

    def test_03_done_sentinel_always_last(self):
        """On a healthy stream, ``data: [DONE]`` is the last data line."""
        print("\nTest: [DONE] sentinel is last")
        api_url = NODES["node1"]["api"]

        resp = _stream_request(
            api_url, [{"role": "user", "content": "done sentinel check"}],
        )
        self.assertEqual(resp.status_code, 200)

        data_lines = []
        for raw in resp.iter_lines():
            if not raw:
                continue
            decoded = raw.decode("utf-8")
            if decoded.startswith("data: "):
                data_lines.append(decoded)

        self.assertTrue(data_lines, "Expected at least one data: line")
        self.assertEqual(
            data_lines[-1], "data: [DONE]",
            f"Last data: line must be [DONE], got: {data_lines[-1]!r}",
        )
        # And no data: line may appear after it (redundant given [-1] above,
        # but makes the intent explicit).
        done_idx = data_lines.index("data: [DONE]")
        self.assertEqual(
            done_idx, len(data_lines) - 1,
            "Found data: lines after [DONE]",
        )
        print(f"  PASSED ({len(data_lines)} data lines)")

    # ------------------------------------------------------------------
    # test_04: Mid-stream interruption (reset mode)
    # ------------------------------------------------------------------

    def test_04_stream_interruption_mid_response(self):
        """reset mode truncates the stream; next request still works."""
        print("\nTest: Stream interruption mid-response (reset mode)")
        api_url = NODES["node1"]["api"]

        for bid in MOCK_BACKEND_PORTS:
            _clear_debug_requests(bid)

        # Task instructs us to forward ``X-Mock-Failure-Mode: reset`` through
        # the chat request and re-verify via /debug/requests.  Ranvier
        # currently constructs a fresh header set for the backend hop
        # (see test_http_pipeline.py::test_02 notes), so we also set the
        # sticky admin mode as a reliable fallback.  After the call we
        # inspect /debug/requests to see whether the header survived.
        _set_all_failure_modes("reset")
        test_id = f"stream-reset-{uuid.uuid4()}"

        partial_seen = False
        done_seen = False
        raised = None
        try:
            resp = _stream_request(
                api_url,
                [{"role": "user", "content": "interrupt this please"}],
                extra_headers={
                    "X-Mock-Failure-Mode": "reset",
                    "X-Request-ID": test_id,
                },
                timeout=10,
            )
            # Headers may arrive with 200 before the server aborts.
            for raw in resp.iter_lines():
                if not raw:
                    continue
                decoded = raw.decode("utf-8")
                if decoded == "data: [DONE]":
                    done_seen = True
                elif decoded.startswith("data: "):
                    partial_seen = True
        except (
            requests.exceptions.ChunkedEncodingError,
            requests.exceptions.ConnectionError,
        ) as e:
            raised = e
        finally:
            _set_all_failure_modes("none")

        # Confirm whether the header propagated to the backend.  Informative
        # only — the actual assertion is on client-observed behaviour.
        _bid, entry = _find_request_by_id(test_id)
        if entry is not None:
            fwd = entry.get("headers", {}).get("X-Mock-Failure-Mode")
            print(f"  X-Mock-Failure-Mode forwarded to backend: {fwd!r}")

        self.assertFalse(
            done_seen and raised is None,
            "reset mode must not produce a clean [DONE] stream — "
            "either the stream should truncate or the client should raise",
        )
        # Must see evidence of an interruption.  Either:
        #   (a) a chunked-encoding / connection error was raised, OR
        #   (b) we saw some data but never the [DONE] sentinel.
        self.assertTrue(
            raised is not None or (partial_seen and not done_seen),
            f"Expected interruption, got partial_seen={partial_seen}, "
            f"done_seen={done_seen}, raised={raised!r}",
        )

        # Follow-up: a fresh healthy request must still succeed.
        status, text, _ = send_chat_request(
            api_url, [{"role": "user", "content": "post-reset healthy check"}],
        )
        self.assertEqual(
            status, 200,
            f"Ranvier should recover after a reset; got status={status}",
        )
        self.assertTrue(text, "Post-reset response body should be non-empty")
        print("  PASSED")

    # ------------------------------------------------------------------
    # test_05: Slow backend doesn't stall headers
    # ------------------------------------------------------------------

    def test_05_slow_backend_does_not_stall_headers(self):
        """Headers flush before the body finishes even with per-chunk latency."""
        print("\nTest: Slow backend does not stall response headers")
        api_url = NODES["node1"]["api"]

        headers_budget_s = 2.0
        start = time.monotonic()
        resp = _stream_request(
            api_url,
            [{"role": "user", "content": "slow chunk test"}],
            extra_headers={"X-Mock-Latency-Ms": "200"},
            timeout=REQUEST_TIMEOUT,
        )
        header_time = time.monotonic() - start
        self.assertEqual(resp.status_code, 200)

        self.assertLess(
            header_time, headers_budget_s,
            f"Response headers took {header_time:.3f}s (>{headers_budget_s}s "
            "budget) — Ranvier is buffering instead of flushing headers "
            "before the first chunk completes",
        )

        body_start = time.monotonic()
        saw_done = False
        for raw in resp.iter_lines():
            if not raw:
                continue
            if raw.decode("utf-8") == "data: [DONE]":
                saw_done = True
                break
        body_time = time.monotonic() - body_start

        self.assertTrue(saw_done, "Expected [DONE] after streaming to completion")
        print(
            f"  PASSED (headers in {header_time:.3f}s, body in {body_time:.3f}s)"
        )

    # ------------------------------------------------------------------
    # test_06: >1 MB request body
    # ------------------------------------------------------------------

    def test_06_large_request_body_over_1mb(self):
        """A >1 MB honest request body streams a successful completion."""
        print("\nTest: Large request body (>1 MB)")
        api_url = NODES["node1"]["api"]

        for bid in MOCK_BACKEND_PORTS:
            _clear_debug_requests(bid)

        # Distinct from test_negative_paths.py::test_05b_oversized_request_body
        # which exercises a fraudulent 200 MB Content-Length over a raw
        # socket.  This test targets the honest >1 MB case.
        padding_size = int(1.5 * 1024 * 1024)  # 1.5 MB
        padding = ("lorem ipsum dolor sit amet " * 60_000)[:padding_size]
        self.assertGreater(len(padding), 1024 * 1024)

        test_id = f"stream-large-req-{uuid.uuid4()}"
        messages = [
            {"role": "system", "content": "you are a helpful assistant"},
            {"role": "user", "content": f"Please echo: {padding}"},
        ]
        serialized_len = len(json.dumps({"messages": messages}))
        self.assertGreater(
            serialized_len, 1024 * 1024,
            "Test setup error: serialised messages should exceed 1 MB",
        )

        status, text, resp_headers = send_chat_request(
            api_url,
            messages,
            extra_headers={"X-Request-ID": test_id},
            timeout=60,
        )
        self.assertEqual(
            status, 200,
            f"Expected 200 for a ~{serialized_len // 1024} KB body, got {status}",
        )
        self.assertTrue(text, "Expected non-empty streamed response body")

        bid, entry = _find_request_by_id(test_id)
        self.assertIsNotNone(
            entry,
            f"Forwarded request {test_id} not found in any backend's "
            "/debug/requests",
        )
        forwarded = json.loads(entry["body"])
        forwarded_last = forwarded["messages"][-1]["content"]
        original_last = messages[-1]["content"]

        # Token-forwarding rewriting can shift body size slightly but must
        # not truncate the 1.5 MB payload.  Allow ±10 % slack.
        ratio = len(forwarded_last) / len(original_last)
        self.assertGreater(
            ratio, 0.9,
            f"Forwarded last-message content length {len(forwarded_last)} "
            f"differs too much from original {len(original_last)} (ratio={ratio:.3f})",
        )
        print(
            f"  PASSED (serialised request ~{serialized_len // 1024} KB, "
            f"forwarded content {len(forwarded_last) // 1024} KB, ratio={ratio:.3f})"
        )

    # ------------------------------------------------------------------
    # test_07: >10 MB streamed response, memory stable
    # ------------------------------------------------------------------

    def test_07_large_streaming_response_over_10mb(self):
        """A >10 MB streamed response is consumed without unbounded memory growth."""
        print("\nTest: Large streaming response (>10 MB)")

        # The mock backend currently emits ~4 canned SSE chunks per request
        # (one per word of "Response from backend N") and offers no
        # "repeat N times" / "generate N chunks" admin knob.  Without that
        # knob we can't force a >10 MB response.
        #
        # TODO: once BACKLOG §6.1 "Enhance mock backend capabilities" is
        # extended with a chunk-count or response-size knob (e.g.
        # ``POST /admin/chunks?count=N`` or ``X-Mock-Chunk-Count`` header),
        # rewrite this test to:
        #   1. set chunks so total body > 10 MB,
        #   2. stream via iter_lines() counting bytes,
        #   3. assert final line == 'data: [DONE]' and total bytes > 10 MB,
        #   4. assert VmRSS delta < 50 MB (already plumbed below).
        self.skipTest(
            "mock_backend has no chunk-count/response-size knob yet "
            "(see BACKLOG §6.1 'Enhance mock backend capabilities'); "
            "unable to force a >10 MB response deterministically"
        )

        # The following block is intentionally unreachable today — kept so
        # the memory-boundedness wiring is in place once the knob lands.
        api_url = NODES["node1"]["api"]  # pragma: no cover
        rss_before_kb = _read_vm_rss_kb()  # pragma: no cover
        resp = _stream_request(  # pragma: no cover
            api_url,
            [{"role": "user", "content": "stream big please"}],
            extra_headers={"X-Mock-Chunk-Count": "200000"},
            timeout=120,
        )
        self.assertEqual(resp.status_code, 200)  # pragma: no cover

        total_bytes = 0  # pragma: no cover
        last_nonempty = b""  # pragma: no cover
        for raw in resp.iter_lines():  # pragma: no cover
            if not raw:
                continue
            total_bytes += len(raw)
            last_nonempty = raw

        rss_after_kb = _read_vm_rss_kb()  # pragma: no cover
        rss_delta_mb = (rss_after_kb - rss_before_kb) / 1024.0  # pragma: no cover

        self.assertGreater(total_bytes, 10 * 1024 * 1024)  # pragma: no cover
        self.assertEqual(last_nonempty, b"data: [DONE]")  # pragma: no cover
        self.assertLess(  # pragma: no cover
            rss_delta_mb, 50.0,
            f"RSS grew by {rss_delta_mb:.1f} MB while streaming "
            f"{total_bytes / (1024*1024):.1f} MB — memory not bounded",
        )


if __name__ == "__main__":
    unittest.main()
