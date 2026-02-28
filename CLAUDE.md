# CLAUDE.md - Ranvier Core

Layer 7+ LLM traffic controller. C++20 on Seastar (shared-nothing, thread-per-core).

## Build Constraints

- **Static analysis only.** Do not attempt to run `cmake`, `make`, or build. Seastar dependencies are too heavy for the sandbox.
- **API verification:** Verify syntax against Seastar documentation logic.
- **Manual verification:** The developer builds in their Docker container and provides logs if it fails.
- **Do NOT read** the full `/docs` or `/assets` folders (large token-heavy files).

## Full Context

Read `.dev-context/claude-context.md` for all project context: architecture, source layout, key types, coding conventions, the 24 Hard Rules, dependencies, and workflow prompt templates.
