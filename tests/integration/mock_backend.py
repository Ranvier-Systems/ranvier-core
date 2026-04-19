#!/usr/bin/env python3
"""
Mock vLLM Backend Server for Integration Testing

Simulates a vLLM-compatible chat completion endpoint that:
- Handles POST /v1/chat/completions
- Returns streaming SSE responses (text/event-stream)
- Tracks requests for debugging
- Echoes back a unique response based on the prompt

This allows integration tests to verify:
- Route learning (backend responds successfully)
- Request forwarding (correct backend receives request)
- Streaming response handling

Test-only knobs (all default to off so the happy path is unchanged):

* Latency injection
    - ``MOCK_LATENCY_MS`` env var: default per-chunk delay in milliseconds.
    - ``POST /admin/latency?ms=N``: update the default at runtime.
    - ``X-Mock-Latency-Ms: N`` request header: override for one request.

* Failure-mode simulation
    - ``POST /admin/failure-mode?mode=none|status_500|status_503|timeout|reset``
      sets a sticky mode applied to subsequent chat completions.
    - ``X-Mock-Failure-Mode: <mode>`` request header overrides for one request.
    - ``reset`` flushes a partial SSE chunk then closes the TCP socket
      without a clean FIN so streaming-interruption tests can assert on
      truncation.
    - ``timeout`` blocks past a typical client timeout without writing.

* Request log
    - ``GET /debug/requests`` returns a JSON list of recently received
      ``/v1/chat/completions`` POSTs (bounded ring buffer, cap 200).
    - ``DELETE /debug/requests`` clears the log.

* Prefix echo
    - When ``MOCK_PREFIX_ECHO=1`` (env) or ``X-Mock-Prefix-Echo: 1``
      (header), the first SSE chunk's ``delta.content`` is the first 32
      characters of the last user message instead of the canned text. The
      remaining chunks and the ``[DONE]`` sentinel are unchanged.
"""

import json
import os
import sys
import threading
import time
from collections import deque
from http.server import HTTPServer, BaseHTTPRequestHandler
from socket import SHUT_RDWR
from socketserver import ThreadingMixIn
from urllib.parse import parse_qs, urlparse

# Force unbuffered stdout for Docker logs
sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

BACKEND_ID = os.environ.get("BACKEND_ID", "unknown")

# -----------------------------------------------------------------------------
# Server-wide state for admin-controlled behaviors.
#
# All fields are mutated under ``_state_lock`` because ThreadingMixIn handles
# each request in its own thread. Reads are also taken under the lock to keep
# the snapshot consistent (e.g. so ``failure-mode`` and ``latency`` move
# together if a test sets them back-to-back).
# -----------------------------------------------------------------------------

_state_lock = threading.Lock()

_keepalive_enabled = os.environ.get("MOCK_KEEPALIVE", "0") == "1"
_latency_ms = max(0, int(os.environ.get("MOCK_LATENCY_MS", "0") or "0"))
_failure_mode = "none"  # one of VALID_FAILURE_MODES
_prefix_echo_enabled = os.environ.get("MOCK_PREFIX_ECHO", "0") == "1"

VALID_FAILURE_MODES = frozenset(
    {"none", "status_500", "status_503", "timeout", "reset"}
)

# Bounded ring buffer of recent chat-completion POSTs for /debug/requests.
# 200 entries is enough for the integration suites without holding request
# bodies indefinitely under load.
REQUEST_LOG_CAP = 200
_request_log: "deque[dict]" = deque(maxlen=REQUEST_LOG_CAP)
_request_log_lock = threading.Lock()

# How long ``failure-mode=timeout`` blocks before giving up. Long enough to
# trip any reasonable client timeout; daemon_threads=True ensures the worker
# is reaped on server shutdown.
TIMEOUT_FAILURE_SLEEP_S = 60.0


def _snapshot_state():
    """Return a consistent snapshot of admin-controlled state."""
    with _state_lock:
        return {
            "keepalive": _keepalive_enabled,
            "latency_ms": _latency_ms,
            "failure_mode": _failure_mode,
            "prefix_echo": _prefix_echo_enabled,
        }


class MockBackendHandler(BaseHTTPRequestHandler):
    """Handler for mock vLLM endpoints."""

    # Use HTTP/1.1 for chunked transfer encoding support
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        """Override to add backend ID to logs."""
        print(f"[Backend {BACKEND_ID}] {args[0]}", flush=True)

    def log_request(self, code='-', size='-'):
        """Override to log all requests with details."""
        print(f"[Backend {BACKEND_ID}] {self.requestline} -> {code}", flush=True)

    # ---- chunked-transfer helpers --------------------------------------------

    def send_chunk(self, data):
        """Send a single chunk in HTTP chunked transfer encoding format."""
        if isinstance(data, str):
            data = data.encode('utf-8')
        # Chunked format: <hex size>\r\n<data>\r\n
        chunk = f"{len(data):x}\r\n".encode() + data + b"\r\n"
        self.wfile.write(chunk)
        self.wfile.flush()

    def end_chunked(self):
        """Send the final empty chunk to end chunked transfer."""
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def _send_json(self, status, payload):
        """Send a small JSON response (used by admin and debug endpoints)."""
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    # ---- HTTP method dispatch -----------------------------------------------

    def do_GET(self):
        """Handle GET requests for health checks and debug introspection."""
        parsed = urlparse(self.path)

        if parsed.path == "/health" or parsed.path == "/":
            state = _snapshot_state()
            body = json.dumps({
                "status": "ok",
                "backend_id": BACKEND_ID,
                "keepalive": state["keepalive"],
                "latency_ms": state["latency_ms"],
                "failure_mode": state["failure_mode"],
                "prefix_echo": state["prefix_echo"],
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
        elif parsed.path == "/debug/requests":
            self._handle_debug_requests_get()
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_DELETE(self):
        """Handle DELETE requests for clearing the debug log."""
        parsed = urlparse(self.path)
        if parsed.path == "/debug/requests":
            self._handle_debug_requests_delete()
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_POST(self):
        """Handle POST requests for chat completions and admin endpoints."""
        print(f"[Backend {BACKEND_ID}] POST received: {self.path}", flush=True)
        parsed = urlparse(self.path)

        if parsed.path == "/v1/chat/completions":
            self._handle_chat_completion()
        elif parsed.path == "/admin/keepalive":
            self._handle_admin_keepalive()
        elif parsed.path == "/admin/latency":
            self._handle_admin_latency()
        elif parsed.path == "/admin/failure-mode":
            self._handle_admin_failure_mode()
        elif parsed.path == "/admin/prefix-echo":
            self._handle_admin_prefix_echo()
        else:
            print(f"[Backend {BACKEND_ID}] Unknown path: {parsed.path}", flush=True)
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    # ---- admin endpoints -----------------------------------------------------

    def _handle_admin_keepalive(self):
        """Toggle keep-alive mode. POST /admin/keepalive?enabled=1|0"""
        global _keepalive_enabled
        params = parse_qs(urlparse(self.path).query)
        enabled = params.get("enabled", ["1"])[0] == "1"
        with _state_lock:
            _keepalive_enabled = enabled
        print(f"[Backend {BACKEND_ID}] Keep-alive set to: {enabled}", flush=True)
        self._send_json(200, {"keepalive": enabled})

    def _handle_admin_latency(self):
        """Set per-chunk latency in ms. POST /admin/latency?ms=N"""
        global _latency_ms
        params = parse_qs(urlparse(self.path).query)
        raw = params.get("ms", ["0"])[0]
        try:
            ms = max(0, int(raw))
        except ValueError:
            self._send_json(400, {"error": f"invalid ms: {raw!r}"})
            return
        with _state_lock:
            _latency_ms = ms
        print(f"[Backend {BACKEND_ID}] Latency set to: {ms}ms", flush=True)
        self._send_json(200, {"latency_ms": ms})

    def _handle_admin_failure_mode(self):
        """Set sticky failure mode. POST /admin/failure-mode?mode=<mode>"""
        global _failure_mode
        params = parse_qs(urlparse(self.path).query)
        mode = params.get("mode", ["none"])[0]
        if mode not in VALID_FAILURE_MODES:
            self._send_json(400, {
                "error": f"invalid mode: {mode!r}",
                "valid": sorted(VALID_FAILURE_MODES),
            })
            return
        with _state_lock:
            _failure_mode = mode
        print(f"[Backend {BACKEND_ID}] Failure mode set to: {mode}", flush=True)
        self._send_json(200, {"failure_mode": mode})

    def _handle_admin_prefix_echo(self):
        """Toggle prefix-echo mode. POST /admin/prefix-echo?enabled=1|0

        Runtime equivalent of the ``MOCK_PREFIX_ECHO`` env var and the
        ``X-Mock-Prefix-Echo`` header, for tests that go through the
        Ranvier proxy (which does not forward arbitrary request headers
        to the backend).
        """
        global _prefix_echo_enabled
        params = parse_qs(urlparse(self.path).query)
        enabled = params.get("enabled", ["1"])[0] == "1"
        with _state_lock:
            _prefix_echo_enabled = enabled
        print(f"[Backend {BACKEND_ID}] Prefix echo set to: {enabled}", flush=True)
        self._send_json(200, {"prefix_echo": enabled})

    # ---- /debug/requests -----------------------------------------------------

    def _handle_debug_requests_get(self):
        """Return the bounded request log as a JSON list."""
        with _request_log_lock:
            entries = list(_request_log)
        body = json.dumps(entries).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def _handle_debug_requests_delete(self):
        """Clear the request log."""
        with _request_log_lock:
            cleared = len(_request_log)
            _request_log.clear()
        print(f"[Backend {BACKEND_ID}] Cleared request log ({cleared} entries)",
              flush=True)
        self._send_json(200, {"cleared": cleared})

    def _record_request(self, path, body_text):
        """Append a POST to the bounded debug log."""
        # Copy headers into a plain dict so they're JSON-serialisable.
        headers = {k: v for k, v in self.headers.items()}
        entry = {
            "ts": time.time(),
            "path": path,
            "headers": headers,
            "body": body_text,
        }
        with _request_log_lock:
            _request_log.append(entry)

    # ---- chat completions ----------------------------------------------------

    def _resolve_failure_mode(self):
        """Return the failure mode that applies to this request.

        Per-request header wins over the sticky admin-configured mode. An
        unrecognised header value is ignored (treated as ``none``) so a
        malformed header can't take a backend offline.
        """
        header_mode = self.headers.get("X-Mock-Failure-Mode")
        if header_mode and header_mode in VALID_FAILURE_MODES:
            return header_mode
        with _state_lock:
            return _failure_mode

    def _resolve_latency_ms(self):
        """Return the per-chunk latency that applies to this request."""
        header_val = self.headers.get("X-Mock-Latency-Ms")
        if header_val is not None:
            try:
                return max(0, int(header_val))
            except ValueError:
                pass
        with _state_lock:
            return _latency_ms

    def _resolve_prefix_echo(self):
        """Return whether prefix echo is on for this request."""
        header_val = self.headers.get("X-Mock-Prefix-Echo")
        if header_val is not None:
            return header_val == "1"
        with _state_lock:
            return _prefix_echo_enabled

    def _abort_connection(self):
        """Close the TCP socket without a clean FIN.

        Used by ``failure-mode=reset`` to interrupt a stream mid-flight.
        ``shutdown(SHUT_RDWR)`` ensures the kernel stops buffering writes; the
        subsequent ``close()`` releases the descriptor. Any errors from a
        socket the server has already torn down are swallowed.
        """
        try:
            self.connection.shutdown(SHUT_RDWR)
        except OSError:
            pass
        try:
            self.connection.close()
        except OSError:
            pass

    def _handle_chat_completion(self):
        """Simulate a streaming chat completion response."""
        # Read request body
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8") if content_length else ""

        # Extract request ID for tracing
        request_id = self.headers.get("X-Request-ID", "unknown")

        # Always log the request so /debug/requests can replay it later.
        # Recording happens before any failure-mode short-circuit so tests can
        # assert on what the router *sent*, even when the backend returns 5xx
        # or aborts the connection.
        self._record_request(self.path, body)

        try:
            request_data = json.loads(body) if body else {}
        except json.JSONDecodeError:
            err = b'{"error": "Invalid JSON"}'
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(err)))
            self.end_headers()
            self.wfile.write(err)
            self.wfile.flush()
            return

        # Resolve test-only knobs once at the top of the handler so a header
        # change mid-stream can't desynchronise behaviour from logging.
        failure_mode = self._resolve_failure_mode()
        latency_ms = self._resolve_latency_ms()
        prefix_echo = self._resolve_prefix_echo()

        # ---- failure modes that short-circuit before streaming --------------

        if failure_mode == "status_500":
            err = b'{"error": "mock backend forced 500"}'
            self.send_response(500)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(err)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(err)
            self.wfile.flush()
            return

        if failure_mode == "status_503":
            err = b'{"error": "mock backend forced 503"}'
            self.send_response(503)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(err)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(err)
            self.wfile.flush()
            return

        if failure_mode == "timeout":
            # Do not write anything; just block until the client gives up.
            # Long sleep matches what a hung backend looks like in production.
            print(f"[Backend {BACKEND_ID}] failure-mode=timeout, sleeping "
                  f"{TIMEOUT_FAILURE_SLEEP_S}s", flush=True)
            try:
                time.sleep(TIMEOUT_FAILURE_SLEEP_S)
            except Exception:
                pass
            return

        # Extract prompt from the last user message. Used both for log line
        # and for the prefix-echo first chunk.
        prompt = ""
        messages = request_data.get("messages", [])
        if messages:
            last_message = messages[-1]
            content = last_message.get("content", "")
            if isinstance(content, str):
                prompt = content

        print(
            f"[Backend {BACKEND_ID}] Request {request_id}: "
            f"prompt='{prompt[:50]}...' "
            f"failure_mode={failure_mode} latency_ms={latency_ms} "
            f"prefix_echo={prefix_echo}",
            flush=True,
        )

        # Determine whether to keep the connection open. A reset always closes.
        with _state_lock:
            keepalive = _keepalive_enabled

        # Send streaming response with chunked transfer encoding
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        if not keepalive or failure_mode == "reset":
            self.send_header("Connection", "close")
        self.send_header("X-Request-ID", request_id)
        self.send_header("X-Backend-ID", BACKEND_ID)
        self.end_headers()

        # Build the per-chunk word list. In prefix-echo mode the *first* chunk
        # carries the first 32 chars of the last user message; the remaining
        # chunks fall back to the canned text so existing assertions on
        # subsequent tokens still pass.
        canned_text = f"Response from backend {BACKEND_ID}"
        canned_words = canned_text.split()
        if prefix_echo and prompt:
            chunks_content = [prompt[:32]] + [w + " " for w in canned_words]
        else:
            chunks_content = [w + " " for w in canned_words]

        latency_s = latency_ms / 1000.0 if latency_ms > 0 else 0.01

        try:
            for i, content in enumerate(chunks_content):
                chunk_data = {
                    "id": f"chatcmpl-{request_id}",
                    "object": "chat.completion.chunk",
                    "created": int(time.time()),
                    "model": f"mock-model-backend-{BACKEND_ID}",
                    "choices": [
                        {
                            "index": 0,
                            "delta": {"content": content},
                            "finish_reason": None,
                        }
                    ],
                }
                sse_line = f"data: {json.dumps(chunk_data)}\n\n"
                self.send_chunk(sse_line)

                # In reset mode, abort right after the first partial chunk so
                # the client sees a truncated stream.
                if failure_mode == "reset" and i == 0:
                    print(f"[Backend {BACKEND_ID}] failure-mode=reset, "
                          f"aborting after partial chunk", flush=True)
                    self._abort_connection()
                    return

                if latency_s > 0:
                    time.sleep(latency_s)

            # Send final chunk with finish_reason
            final_chunk = {
                "id": f"chatcmpl-{request_id}",
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": f"mock-model-backend-{BACKEND_ID}",
                "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
            }
            self.send_chunk(f"data: {json.dumps(final_chunk)}\n\n")
            self.send_chunk("data: [DONE]\n\n")

            # End chunked transfer
            self.end_chunked()
        except (BrokenPipeError, ConnectionResetError) as e:
            # Client (or our own reset) tore the connection down mid-stream;
            # nothing left to do but log and exit the handler.
            print(f"[Backend {BACKEND_ID}] connection dropped mid-stream: {e}",
                  flush=True)


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    """Threaded HTTP server that handles each request in a new thread."""
    daemon_threads = True  # Don't wait for threads on shutdown


class LoggingHTTPServer(ThreadedHTTPServer):
    """Threaded HTTP server with connection logging."""

    def get_request(self):
        """Override to log incoming connections."""
        conn, addr = super().get_request()
        print(f"[Backend {BACKEND_ID}] Connection from {addr}", flush=True)
        return conn, addr


def main():
    """Start the mock backend server."""
    port = int(os.environ.get("PORT", 8000))
    print(f"[Backend {BACKEND_ID}] Starting mock backend server on port {port}...", flush=True)
    state = _snapshot_state()
    print(f"[Backend {BACKEND_ID}] Initial state: {state}", flush=True)
    server = LoggingHTTPServer(("0.0.0.0", port), MockBackendHandler)
    print(f"[Backend {BACKEND_ID}] Server started, listening on 0.0.0.0:{port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
