# Scripts

CI, benchmarking, and infrastructure setup scripts.

| Script | Purpose |
|--------|---------|
| `bench.sh` | Consolidated benchmark runner for Lambda Labs multi-GPU instances (setup, run, A/B — use `--setup`, `--compare`, `--skip-vllm`) |
| `bench-runner.sh` | Multi-run suite driver over `bench.sh` |
| `bench-archive.sh` | Copy a run directory's summaries (compare files, manifests, aggregates, prefix-arm Prometheus dumps) into `docs/benchmarks/results/<date>-<leg>/`; the raw data stays out of git |
| `bench-residency-ab.sh` | Cache-residency routing A/B (churn workload) |
| `docker-cleanup.sh` | Docker container and image cleanup utilities |
| `run-multi-gpu-benchmark.sh` | Retired — hard-exits with a pointer to `bench.sh --skip-vllm` |
| `setup-lambda-benchmark.sh` | Retired — hard-exits with a pointer to `bench.sh --setup` |

For runtime utilities (inspecting routes, managing backends, etc.), see `tools/`.
