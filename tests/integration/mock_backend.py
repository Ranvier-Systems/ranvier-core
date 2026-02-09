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
"""

import json
import os
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
from urllib.parse import urlparse

# Force unbuffered stdout for Docker logs
sys.stdout.reconfigure(line_buffering=True)
sys.stderr.reconfigure(line_buffering=True)

BACKEND_ID = os.environ.get("BACKEND_ID", "unknown")

# Server-wide state for admin-controlled behaviors
_keepalive_enabled = os.environ.get("MOCK_KEEPALIVE", "0") == "1"


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

    def do_GET(self):
        """Handle GET requests for health checks."""
        parsed = urlparse(self.path)

        if parsed.path == "/health" or parsed.path == "/":
            body = json.dumps({"status": "ok", "backend_id": BACKEND_ID, "keepalive": _keepalive_enabled}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        """Handle POST requests for chat completions and admin endpoints."""
        print(f"[Backend {BACKEND_ID}] POST received: {self.path}", flush=True)
        parsed = urlparse(self.path)

        if parsed.path == "/v1/chat/completions":
            self._handle_chat_completion()
        elif parsed.path == "/admin/keepalive":
            self._handle_admin_keepalive()
        else:
            print(f"[Backend {BACKEND_ID}] Unknown path: {parsed.path}", flush=True)
            self.send_response(404)
            self.end_headers()

    def _handle_admin_keepalive(self):
        """Toggle keep-alive mode. POST /admin/keepalive?enabled=1|0"""
        global _keepalive_enabled
        from urllib.parse import parse_qs
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)
        enabled = params.get("enabled", ["1"])[0]
        _keepalive_enabled = enabled == "1"
        print(f"[Backend {BACKEND_ID}] Keep-alive set to: {_keepalive_enabled}", flush=True)
        body = json.dumps({"keepalive": _keepalive_enabled}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def _handle_chat_completion(self):
        """Simulate a streaming chat completion response."""
        # Read request body
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8")

        # Extract request ID for tracing
        request_id = self.headers.get("X-Request-ID", "unknown")

        try:
            request_data = json.loads(body) if body else {}
        except json.JSONDecodeError:
            body = b'{"error": "Invalid JSON"}'
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            return

        # Extract prompt from messages
        prompt = ""
        messages = request_data.get("messages", [])
        if messages:
            last_message = messages[-1]
            prompt = last_message.get("content", "")[:100]  # First 100 chars

        print(f"[Backend {BACKEND_ID}] Request {request_id}: prompt='{prompt[:50]}...'", flush=True)

        # Send streaming response with chunked transfer encoding
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        if not _keepalive_enabled:
            self.send_header("Connection", "close")
        self.send_header("X-Request-ID", request_id)
        self.send_header("X-Backend-ID", BACKEND_ID)
        self.end_headers()

        # Generate streaming SSE chunks (simulating token generation)
        response_text = f"Response from backend {BACKEND_ID}"
        words = response_text.split()

        for i, word in enumerate(words):
            chunk_data = {
                "id": f"chatcmpl-{request_id}",
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": f"mock-model-backend-{BACKEND_ID}",
                "choices": [
                    {
                        "index": 0,
                        "delta": {"content": word + " "},
                        "finish_reason": None,
                    }
                ],
            }
            sse_line = f"data: {json.dumps(chunk_data)}\n\n"
            self.send_chunk(sse_line)
            time.sleep(0.01)  # Small delay to simulate streaming

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
    server = LoggingHTTPServer(("0.0.0.0", port), MockBackendHandler)
    print(f"[Backend {BACKEND_ID}] Server started, listening on 0.0.0.0:{port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
