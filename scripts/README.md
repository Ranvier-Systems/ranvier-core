# Scripts

CI, benchmarking, and infrastructure setup scripts.

| Script | Purpose |
|--------|---------|
| `bench.sh` | Consolidated benchmark runner for Lambda Labs multi-GPU instances (setup, run, A/B — use `--setup`, `--compare`, `--skip-vllm`) |
| `bench-runner.sh` | Multi-run suite driver over `bench.sh` |
| `bench-residency-ab.sh` | Cache-residency routing A/B (churn workload) |
| `docker-cleanup.sh` | Docker container and image cleanup utilities |
| `run-multi-gpu-benchmark.sh` | Retired — hard-exits with a pointer to `bench.sh --skip-vllm` |
| `setup-lambda-benchmark.sh` | Retired — hard-exits with a pointer to `bench.sh --setup` |

For runtime utilities (inspecting routes, managing backends, etc.), see `tools/`.
