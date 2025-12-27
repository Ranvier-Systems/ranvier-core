# Ranvier Core: Context & Constraints

## Project Essence
Layer 7+ load balancer for LLM inference using **Prefix Caching** to prevent KV-cache thrashing.

## Architecture Reference
- **Tech Stack:** C++20, Seastar (shared-nothing architecture), SQLite (WAL mode).
- **Data Flow:** HttpController → TokenizerService (GPT-2) → RouterService → RadixTree (ART) → ConnectionPool → GPU Backend.
- **Sharding:** Every CPU core has its own shard. RouterService broadcasts updates across shards.

## Critical Constraints for Coding
1. **NO LOCKS:** This is a Seastar project. Never use `std::mutex` or atomics. Use `seastar::smp::submit_to` for cross-core communication.
2. **Async Only:** All I/O and cross-shard calls must return `seastar::future<>`.
3. **Prefix Logic:** Routing is based on the Adaptive Radix Tree (ART) lookup of token sequences.

## Documentation Map
- **API:** Admin on `:8080`, Data on `:8080`, Metrics on `:9180`.
- **Persistence:** SQLite tracks backends and routes.
