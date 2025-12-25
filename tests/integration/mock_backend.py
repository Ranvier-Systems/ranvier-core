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
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

BACKEND_ID = os.environ.get("BACKEND_ID", "unknown")


class MockBackendHandler(BaseHTTPRequestHandler):
    """Handler for mock vLLM endpoints."""

    def log_message(self, format, *args):
        """Override to add backend ID to logs."""
        print(f"[Backend {BACKEND_ID}] {args[0]}")

    def do_GET(self):
        """Handle GET requests for health checks."""
        parsed = urlparse(self.path)

        if parsed.path == "/health" or parsed.path == "/":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            response = {"status": "ok", "backend_id": BACKEND_ID}
            self.wfile.write(json.dumps(response).encode())
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        """Handle POST requests for chat completions."""
        parsed = urlparse(self.path)

        if parsed.path == "/v1/chat/completions":
            self._handle_chat_completion()
        else:
            self.send_response(404)
            self.end_headers()

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
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"error": "Invalid JSON"}')
            return

        # Extract prompt from messages
        prompt = ""
        messages = request_data.get("messages", [])
        if messages:
            last_message = messages[-1]
            prompt = last_message.get("content", "")[:100]  # First 100 chars

        print(f"[Backend {BACKEND_ID}] Request {request_id}: prompt='{prompt[:50]}...'")

        # Send streaming response
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
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
            self.wfile.write(sse_line.encode())
            self.wfile.flush()
            time.sleep(0.01)  # Small delay to simulate streaming

        # Send final chunk with finish_reason
        final_chunk = {
            "id": f"chatcmpl-{request_id}",
            "object": "chat.completion.chunk",
            "created": int(time.time()),
            "model": f"mock-model-backend-{BACKEND_ID}",
            "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
        }
        self.wfile.write(f"data: {json.dumps(final_chunk)}\n\n".encode())
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


def main():
    """Start the mock backend server."""
    port = int(os.environ.get("PORT", 8000))
    server = HTTPServer(("0.0.0.0", port), MockBackendHandler)
    print(f"[Backend {BACKEND_ID}] Mock backend server starting on port {port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
