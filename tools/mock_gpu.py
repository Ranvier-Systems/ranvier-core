#!/usr/bin/env python3

import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn # <--- NEW: concurrency
import json
import time
import hashlib

# Global "VRAM"
vram_cache = set()

# Threaded Server to handle concurrent Locust users without queueing
class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True

class MockGPUHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        try:
            content_length = int(self.headers['Content-Length'])
            post_data = self.rfile.read(content_length).decode('utf-8')

            # 1. Logic: Identify the "System Prompt" (The Prefix)
            # In real LLMs, the KV Cache is effective if the prefix matches.
            cache_key = ""
            try:
                body_json = json.loads(post_data)
                if "messages" in body_json and len(body_json["messages"]) > 0:
                    # We only hash the FIRST message (System Prompt) for the cache check
                    # This matches how Ranvier routes (based on prefix)
                    first_msg = body_json["messages"][0]
                    cache_key = hashlib.md5(json.dumps(first_msg).encode()).hexdigest()
                else:
                    cache_key = hashlib.md5(post_data.encode()).hexdigest()
            except:
                cache_key = hashlib.md5(post_data.encode()).hexdigest()

            # 2. Check "VRAM"
            if cache_key in vram_cache:
                # CACHE HIT: 10ms (Simulate just processing the new tokens)
                latency = 0.01
                status = "HIT ⚡"
            else:
                # CACHE MISS: 500ms (Simulate Prefill of System Prompt)
                latency = 0.5
                status = "MISS 🐢"
                vram_cache.add(cache_key)

            # print(f"[{self.server.server_address[1]}] {status}") # Commented out to reduce spam
            time.sleep(latency)

            # 3. Send Response
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()

            response = {
                "id": "chatcmpl-mock",
                "choices": [{
                    "message": {
                        "role": "assistant",
                        "content": f"Response from GPU {self.server.server_address[1]} ({status})"
                    }
                }]
            }
            self.wfile.write(json.dumps(response).encode('utf-8'))
        except Exception as e:
            print(f"Error: {e}")

    def log_message(self, format, *args):
        return # Silence default logging

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8081)
    args = parser.parse_args()

    print(f"🚀 Mock GPU online at http://localhost:{args.port}")
    # Use Threaded Server
    server = ThreadedHTTPServer(('localhost', args.port), MockGPUHandler)
    server.serve_forever()
