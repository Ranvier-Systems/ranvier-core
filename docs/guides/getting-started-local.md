# Getting Started with Ranvier Local

Ranvier Local is a zero-config LLM traffic controller for your machine. It auto-discovers local LLM servers (Ollama, vLLM, LM Studio, llama.cpp), then routes each request by intent, priority, and prefix cache affinity — no configuration needed.

## Prerequisites

A local LLM server running on a standard port:

| Server | Default Port |
|--------|-------------|
| Ollama | 11434 |
| vLLM | 8000 |
| LM Studio | 1234 |
| llama.cpp | 8080 (conflicts with Ranvier — use `--port` to change one) |
| Text Generation WebUI | 5000 |
| LocalAI | 3000 |

Ranvier scans all six ports on startup and registers every server it finds.

## Install

Pick one:

```bash
# Docker (quickest)
docker run --cap-add=IPC_LOCK --network=host ghcr.io/ranvier-systems/ranvier --local

# Homebrew (macOS/Linux)
brew install ranvier-systems/tap/ranvier

# Binary (GitHub Releases)
curl -fsSL https://github.com/ranvier-systems/ranvier-core/releases/latest/download/ranvier-linux-amd64 -o ranvier
chmod +x ranvier
```

## Start

```bash
ranvier --local
```

Expected output:

```
╔═══════════════════════════════════════════════╗
║           Ranvier Local Mode                  ║
║                                               ║
║  API:       http://localhost:8080             ║
║  Dashboard: http://localhost:9180/dashboard   ║
║  Metrics:   http://localhost:9180/metrics     ║
║                                               ║
║  Discovering backends on ports:               ║
║    11434 (Ollama), 8080 (vLLM), 1234 (LM      ║
║    Studio), 8000 (llama.cpp), 5000, 3000      ║
╚═══════════════════════════════════════════════╝
...
INFO  ranvier - Local mode enabled — applying overrides
INFO  ranvier -   Auto-discovery enabled on ports: 11434, 8080, 1234, 8000, 5000, 3000
INFO  ranvier - Local backend discovery started (6 ports)
INFO  ranvier - Ranvier listening on http://0.0.0.0:8080
```

If discovery finds backends, you'll also see lines like:

```
INFO  ranvier.discovery - Discovered ollama on port 11434 (id=1000, models: [llama3.2:1b])
```

Flags you might need:

```bash
ranvier --local --smp 2          # Use 2 cores (default: 1)
ranvier --local --memory 512M    # More memory (default: 256M)
ranvier --local --config my.yaml # Override specific settings
```

## Configure Your IDE

Point your AI coding agent's API base URL to Ranvier:

```
http://localhost:8080
```

That's it. Ranvier proxies `/v1/chat/completions` (and other OpenAI-compatible endpoints) to the best available backend. See the [IDE Integration Guide](ide-integration.md) for per-agent setup.

## Verify It Works

**Quick check** — confirm the dashboard and API are responding:

```bash
# Dashboard stats (should return JSON with uptime, request counts)
curl -s http://localhost:9180/dashboard/stats

# Discovered backends (should list any detected local LLM servers)
curl -s http://localhost:8080/admin/dump/backends
```

Or open [http://localhost:9180/dashboard](http://localhost:9180/dashboard) in your browser for a live view of backends, agents, queue depths, and request rates.

**Send a test request** (adjust the model name to one you have pulled in Ollama):

```bash
curl -s http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "User-Agent: Cursor/1.0" \
  -d '{
    "model": "llama3.2:1b",
    "messages": [{"role": "user", "content": "Say hello in one sentence."}]
  }'
```

After this request, the dashboard should show:
- A backend entry with a green status dot
- "Cursor IDE" in the agents table with a request count of 1
- Footer stats with an incrementing request total

**Check agent tracking and metrics:**

```bash
# Detected agents (should show "Cursor IDE" after the request above)
curl -s http://localhost:8080/admin/agents

# Scheduler queue stats
curl -s http://localhost:8080/admin/scheduler/stats

# Prometheus metrics
curl -s http://localhost:9180/metrics | grep ranvier_http_requests
```

### Troubleshooting: No Backends Discovered

If `/admin/dump/backends` shows `backend_count: 0`:

- **Docker networking**: Auto-discovery scans `127.0.0.1`, which inside a container is the container itself — not your host machine. Use `--network=host` when running Ranvier in Docker, or register backends manually:
  ```bash
  curl -X POST "http://localhost:8080/admin/backends?id=1&ip=host.docker.internal&port=11434"
  ```
- **Ollama not ready**: Verify Ollama responds to the endpoint Ranvier probes:
  ```bash
  curl -s http://localhost:11434/v1/models
  ```
  If this returns empty or errors, Ollama may still be loading. Wait and retry.
- **No models pulled**: Ollama must have at least one model pulled (`ollama pull llama3`) to return results on `/v1/models`.
- **Scan interval**: Discovery re-scans every 10 seconds. A newly started backend will appear on the next cycle.

## What's Happening Under the Hood

When a request arrives, Ranvier:

1. **Classifies intent** — autocomplete (FIM), chat, or edit — based on the request payload structure.
2. **Assigns priority** — `CRITICAL` for streaming IDE requests (Cursor, Claude Code), `HIGH` for agents like Cline/Aider, `NORMAL` for everything else. Detected from `User-Agent` header automatically.
3. **Estimates cost** — input token count as a proxy for prefill compute.
4. **Routes by prefix affinity** — sends the request to the backend most likely to have the relevant KV cache, reducing Time-To-First-Token.
5. **Load-balances** — if multiple backends match, picks the least loaded one (P2C algorithm).

Priority queue ensures interactive requests are never blocked by batch jobs. Agents are tracked per-session with pause/resume support via the admin API.

For the full architecture, see [VISION.md](../architecture/VISION.md).

## Next Steps

- **Custom config**: Copy [`ranvier-local.yaml.example`](../../ranvier-local.yaml.example) and adjust discovery ports or routing mode.
- **Add cloud backends**: Mix local and remote backends in a single config. See the [Cloud Deployment Guide](cloud-deployment.md).
- **Dashboard**: Open [http://localhost:9180](http://localhost:9180) for a live view of backends, agents, queue depths, and throughput. Pause/resume agents directly from the UI.
- **Monitor**: Scrape `:9180/metrics` with Prometheus for cache hit rates, P99 latency, and per-agent traffic.
- **Pause an agent**: `curl -X POST "http://localhost:8080/admin/agents/pause?agent_id=aider"` to temporarily hold an agent's requests, or use the dashboard's Pause button. Pausing doesn't reject requests — they queue in the scheduler until the agent is resumed or the request times out. The agent's request counter still increments (requests are identified on arrival), but `requests_paused_rejected` tracks how many were dropped while paused.
