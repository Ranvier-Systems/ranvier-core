# IDE Integration Guide

Configure your AI coding agent to route through Ranvier. Each agent gets automatic detection, priority assignment, and per-agent metrics — just point the API base URL to Ranvier.

**Ranvier endpoint:** `http://localhost:8080` (local) or your cluster address.

---

## Cursor

**Config:** Settings → Models → OpenAI API Base URL

```
http://localhost:8080
```

Ranvier auto-detects Cursor from the `User-Agent` header and assigns **CRITICAL** priority. No additional configuration needed.

---

## Claude Code

**Config:** Set the base URL environment variable:

```bash
export ANTHROPIC_BASE_URL=http://localhost:8080
```

Or in your shell profile (`~/.bashrc`, `~/.zshrc`):

```bash
echo 'export ANTHROPIC_BASE_URL=http://localhost:8080' >> ~/.bashrc
```

Ranvier auto-detects `claude-code` from the `User-Agent` header. Priority: **CRITICAL**.

---

## Cline (VS Code)

**Config:** VS Code Settings → Extensions → Cline → API Base URL

```
http://localhost:8080
```

Ranvier auto-detects `Cline` from the `User-Agent` header. Priority: **HIGH**.

---

## Aider

**Config:** Pass the base URL on the command line:

```bash
aider --openai-api-base http://localhost:8080
```

Or set the environment variable:

```bash
export OPENAI_API_BASE=http://localhost:8080
```

Ranvier auto-detects `Aider` from the `User-Agent` header. Priority: **HIGH**.

---

## Continue (VS Code / JetBrains)

**Config:** In `~/.continue/config.json`:

```json
{
  "models": [{
    "provider": "openai",
    "apiBase": "http://localhost:8080",
    "model": "llama3"
  }]
}
```

Ranvier auto-detects Continue if the `User-Agent` header contains the agent name.

---

## Custom / Other Agents

For agents Ranvier doesn't auto-detect, use explicit headers:

```bash
curl http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -H "X-Ranvier-Agent: my-agent-name" \
  -H "X-Ranvier-Priority: critical" \
  -d '{"model": "llama3", "messages": [{"role": "user", "content": "Hello"}]}'
```

**Headers:**

| Header | Values | Description |
|--------|--------|-------------|
| `X-Ranvier-Agent` | any string | Identifies the agent for tracking and pause/resume |
| `X-Ranvier-Priority` | `critical`, `high`, `normal`, `low` | Overrides auto-detected priority |

If neither header is set, Ranvier extracts the agent name from the first token of the `User-Agent` header (before `/`).

---

## Priority Tiers

| Priority | Agents | Behavior |
|----------|--------|----------|
| **CRITICAL** | Cursor, Claude Code | Served first, never queued behind lower tiers |
| **HIGH** | Cline, Aider | Served before NORMAL/LOW |
| **NORMAL** | Auto-detected unknown agents | Default tier |
| **LOW** | Batch jobs, background tasks | Served last, may be deferred under load |

Priorities are auto-assigned based on agent detection. Override per-request with `X-Ranvier-Priority`.

---

## Verify Your Setup

After configuring your agent, confirm Ranvier sees it:

```bash
# List all detected agents
curl -s http://localhost:8080/admin/agents

# Check agent-specific metrics
curl -s http://localhost:9180/metrics | grep seastar_ranvier_agents_tracked

# Check priority queue depth
curl -s http://localhost:9180/metrics | grep seastar_ranvier_scheduler_queue_depth
```

### Pause / Resume an Agent

```bash
# Pause — requests from this agent return 503
curl -X POST http://localhost:8080/admin/agents/aider/pause

# Resume
curl -X POST http://localhost:8080/admin/agents/aider/resume
```

Useful when one agent is overwhelming local backends and you need to prioritize another.
