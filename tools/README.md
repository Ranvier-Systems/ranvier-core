# Tools

Runtime utilities for operating and debugging Ranvier.

| Tool | Purpose |
|------|---------|
| `rvctl` | CLI for inspecting and managing a running Ranvier instance |
| `get-metrics.sh` | Fetch Prometheus metrics |
| `send-request.sh` | Send test requests to the router |
| `register-backends.sh` | Register backends with the router |
| `delete-backends.sh` | Remove backends from the router |
| `set-backend-weights.sh` | Adjust backend routing weights |
| `mock_gpu.py` | Mock GPU backend for local development |
| `run-mock-cpu.sh` | Run mock CPU backend |
| `sidecar.py` | Backend sidecar utility |
| `verify_draining_and_reload.py` | Verify draining and hot-reload behavior |

For CI, benchmarking, and setup scripts, see `scripts/`.
