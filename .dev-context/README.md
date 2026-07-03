# .dev-context/ — Index

This directory holds the project's **reference docs** (kept current) and **historical records** (frozen — quote them, never edit them). Workflows live as skills in `.claude/skills/`; start any session with `/orient`.

The former `claude-*-prompt.md` templates were converted into skills (2026-07-03) and removed; git history has the originals. Mapping: impl→`/implement`, review→`/review`, debug→`/debug-build`, perf→`/perf`, planning→`/plan`, doc→`/doc`, refactor→`/refactor`, audit→`/audit`, adversarial-audit→`/adversarial-audit`, incident→`/incident`, pattern-extractor→`/extract-pattern`, strategic-alignment→`/strategic-review`.

## Living references (update when the code changes)

| File | Contents | Kept current by |
|------|----------|-----------------|
| `claude-context.md` | **The core doc.** Architecture, source layout, key types, conventions, the 24 Hard Rules. Read every session. | `/doc` (layout sync), `/extract-pattern` (rules) |
| `seastar-pitfalls-reference.md` | Seastar pitfalls not (yet) elevated to Hard Rules — the "minor leagues" | `/extract-pattern` promotes entries |
| `claude-locust-sync-map.md` | Python↔C++ couplings (FNV hash pipeline, metrics names) that break *silently* when one side drifts. Check before touching `router_service.cpp`, `http_controller.cpp`, `request_rewriter.hpp`, or the locustfiles; bump "Last verified" when re-checked | `/doc`, `/benchmark` |
| `cheatsheet.md` | ~2500-line operational runbook: dev-container commands, cluster bring-up, backend registration, benchmark recipes, troubleshooting. **Grep it, don't read it whole.** | Ad hoc |

## Historical records (frozen)

| File(s) | What happened |
|---------|---------------|
| `adversarial-audit-2026-01-14.md`, `adversarial-audit-2026-02-12.md` | Adversarial audit reports (the 02-12 one includes the post-fix verification pass — findings A1–A10/E1/... are cited by skills as worked examples) |
| `audit-fix-prompts.md` | Self-contained fix prompts generated from the 2026-01-14 audit — the template `/adversarial-audit` follows for new findings |
| `investigation-289-routing-regression.md` → `investigation-may22-affinity-thrashing-reproduction.md` → `next-benchmark-checklist.md` | The affinity-thrashing investigation arc, in reading order. `next-benchmark-checklist.md` also documents the bench.sh flag gotchas `/benchmark` cites |
| `investigations/` | Long-running investigation write-ups (one file per topic). **New investigations go here** — see the post-incident section of `/incident` for the expected structure |
| `http-controller-review.md`, `router-service-review.md` | Deep component reviews (point-in-time) |

## Conventions

- New investigation → `investigations/<topic>.md`; new audit report → `adversarial-audit-YYYY-MM-DD.md` (root).
- A dated record is never edited after the fact, with one exception: appending a clearly-marked verification/correction note (see the `*Corrected 2026-05-05*` trail in claude-context.md's Hard Rules, or the 2026-02-13 verification note in the 02-12 audit).
- If you add/remove/rename files under `src/`, update the Source Code Layout in `claude-context.md` in the same PR.
