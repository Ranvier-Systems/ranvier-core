# Ranvier Blog Content Strategy

## Content Mix: 70/20/10 Rule

- **70% "Give value"** — Helps the reader regardless of whether they use Ranvier
- **20% "Show the thinking"** — Hard problem + how we solved it; Ranvier is the case study, not the pitch
- **10% "Product updates"** — Announcements, benchmarks, release notes (only after earning attention)

**Key principle:** Never publish two "show the thinking" posts in a row. Always alternate with a "give value" post.

## Recommended Cadence: 2 posts/month

## Publishing Sequence

| # | Post | Category | Why this order |
|---|------|----------|----------------|
| 1 | Why Your Load Balancer Is Wasting Your GPUs (published 2026-03-16) | Show the thinking | Establishes the problem space |
| 2 | 24 Hard Rules for Writing Correct Async C++ | Give value | Broadest audience, zero self-promotion, establishes credibility |
| 3 | KV Cache Locality: The Hidden Variable in Your LLM Serving Cost | Show the thinking | Sequel to post #1, now lands with earned credibility. Lead with economics, Ranvier is the proof point |
| 4 | Tokenization Is the Bottleneck You're Not Measuring | Give value | Practical, surprising, applies to anyone running LLM inference |
| 5 | What Happens When Your LLM Load Balancer Learns (Multi-Depth Route Learning) | Show the thinking | Most differentiated technical idea — audience now trusts you enough to engage |
| 6 | Your LLM Gateway Doesn't Need to Be Written in Python | Give value | Contrarian, generates discussion, drives traffic back to earlier posts |
| 7 | Gossip Protocols for GPU Cluster Coordination | Show the thinking | Deep systems content for the audience you've built |
| 8 | Not All Requests Are Equal: Priority-Aware LLM Traffic Shaping | Give value | Relatable ops problem, applicable beyond Ranvier |

## Future Ideas (Unscheduled)

- **Building a Shared-Nothing LLM Proxy on Seastar** — Why Seastar, SMP storms, gate-guarded timers
- **Circuit Breakers for LLM Backends: When 200 OK Doesn't Mean Healthy** — LLM backends fail in weird ways
- **Adaptive Radix Trees: The Data Structure Behind Sub-50μs Routing Decisions** — Deep dive on the ART implementation
