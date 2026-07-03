# CLAUDE.md - Ranvier Core

Layer 7+ LLM traffic controller. C++20 on Seastar (shared-nothing, thread-per-core).

## Build Constraints

- **Static analysis only.** Do not attempt to run `cmake`, `make`, or build. Seastar dependencies are too heavy for the sandbox.
- **API verification:** Verify syntax against Seastar documentation logic.
- **Manual verification:** The developer builds in their Docker container and provides logs if it fails.
- **Do NOT read** the full `/docs` or `/assets` folders (large token-heavy files).

## Full Context

Read `.dev-context/claude-context.md` for all project context: architecture, source layout, key types, coding conventions, the 24 Hard Rules, and dependencies.

## Workflows

Task-specific skills live in `.claude/skills/` — start with `/orient` if unsure which applies (it routes to `/plan`, `/implement`, `/review`, `/validate`, `/debug-build`, etc.). Supporting reference docs and historical records are indexed in `.dev-context/README.md`.
