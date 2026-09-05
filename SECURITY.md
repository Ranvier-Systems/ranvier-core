# Security Policy

## Reporting a vulnerability

Please do not open a public issue for a suspected vulnerability.

Use GitHub's private vulnerability reporting for this repository:
**Security → Report a vulnerability**
(https://github.com/Ranvier-Systems/ranvier-core/security/advisories/new).

You will get an acknowledgement within 7 days and a status update within 30 days.
This project has one part-time maintainer; please allow for that when planning
disclosure. Coordinated disclosure after a fix is released is the default. If a
report is out of scope or not a vulnerability, you will be told why.

## Supported versions

| Version | Supported |
|---|---|
| 2.1.x (latest release) | yes |
| `main` (unreleased) | best effort |
| 2.0.x, 1.0.x | no, upgrade to 2.1.x |

## Scope

The `ranvier` binary and its configuration surface, the container images
published under `ghcr.io/ranvier-systems`, and the Helm chart in
`deploy/helm/ranvier`.

## Known hardening gaps (read before deploying)

These are tracked in `BACKLOG.md` §4. They are limitations to design around,
not vulnerabilities to report:

- Connections from Ranvier to backends are plain TCP. Terminate TLS in front of
  Ranvier and keep backend traffic on a trusted network.
- API keys are read from configuration in plaintext. Inject the config file
  from your platform's secret store rather than committing it.
- There is a single admin role. The metrics/admin port (9180 by default) must
  not be reachable from untrusted networks.
- Rate limiting is per client IP only.
- No seccomp profile ships yet, and the container requires `CAP_IPC_LOCK`.
