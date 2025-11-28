# tools/mock_gpu.py
import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import time

class MockGPUHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        # 1. Read the request
        content_length = int(self.headers['Content-Length'])
        post_data = self.rfile.read(content_length).decode('utf-8')
        
        # 2. Simulate "Inference"
        print(f"[{self.server.server_address[1]}] Processing request: {post_data[:50]}...")
        time.sleep(0.5) # Simulate latency
        
        # 3. Send Response (OpenAI Format)
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.end_headers()
        
        response = {
            "id": "chatcmpl-mock",
            "choices": [{
                "message": {
                    "role": "assistant",
                    "content": f"Response from GPU running on port {self.server.server_address[1]}"
                }
            }]
        }
        self.wfile.write(json.dumps(response).encode('utf-8'))

    def log_message(self, format, *args):
        return # Silence default logging

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8081)
    args = parser.parse_args()
    
    print(f"🚀 Mock GPU online at http://localhost:{args.port}")
    HTTPServer(('localhost', args.port), MockGPUHandler).serve_forever()
