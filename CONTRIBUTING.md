# Contributing to Ranvier Core

Thanks for your interest. This is a single-maintainer project. Issues and pull
requests are welcome and are reviewed on a best-effort basis, usually within a
week rather than a day.

## Before you start

- Read `.dev-context/claude-context.md`: the architecture, the source layout,
  and the Hard Rules for Seastar code (shared-nothing ownership, nothing
  blocking on the reactor, shard-local state). A PR that violates a Hard Rule
  will be asked to change regardless of its test results.
- Check `BACKLOG.md` for open items and `CHANGELOG.md` for what is already on
  `main` but unreleased, so work is not duplicated.

## Building and testing

Seastar is heavy, so builds happen in the dev container or the base image
(see README → Development Setup):

- `docker pull ghcr.io/ranvier-systems/ranvier-base:latest`, then open the
  folder in VS Code Dev Containers; or build the base image yourself with
  `docker build -f Dockerfile.base -t ghcr.io/ranvier-systems/ranvier-base:latest .`
- `make build` and `make test` (unit tests)
- `make sanitize-test` (ASan/UBSan), `make fuzz-run-all` (libFuzzer harnesses)
- `make test-integration-fast` (mock-backend integration)
- `scripts/lint-seastar-async.sh` enforces Hard Rule #12 (no blocking calls on
  the reactor). CI runs it; run it before pushing.

## Pull requests

- One change per PR, with tests for what it changes.
- Hot-path changes carry a note on what was measured, or why measurement is not
  needed for this change.
- Anything user-visible gets an entry under `## [Unreleased]` in `CHANGELOG.md`.
- Benchmark claims must come from a recorded run under `docs/benchmarks/`, with
  workload, hardware and repeat count stated. This project deprecated its own
  earlier headline number for lack of exactly this discipline; please help keep
  the bar where it is now.
- Do not change the default `--smp` or memory flags, or the Docker and Helm
  defaults, without explaining why in the PR.

## AI-assisted contributions

Much of this codebase was written with AI coding agents working under the
maintainer's direction and review. Agent-assisted PRs are welcome on the same
terms as any other: you are the author, you have read and understood every line
you submit, and it passes CI and review. Please say in the PR description that
an agent was used; it tells the reviewer where to look hardest.

## Licensing

Contributions are accepted under the Apache License 2.0, inbound the same as
outbound. By submitting a PR you confirm you have the right to contribute the
code in it.
