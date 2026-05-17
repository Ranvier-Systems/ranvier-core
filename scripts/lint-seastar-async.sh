#!/bin/bash
# ---------------------------------------------------------
# Rule #12 Lint: seastar::async() call sites
# ---------------------------------------------------------
# Scans src/ and tests/ for `seastar::async(` call sites and requires
# each one to carry a `// rule12-allow: <reason>` marker — on the
# same line, or on the line directly above the call.
#
# Why this lint exists:
#   seastar::async() does NOT offload to a thread pool. It runs in a
#   seastar::thread (stackful coroutine) ON the reactor. Wrapping a
#   blocking call in seastar::async stalls the reactor exactly as if
#   the call had been made directly. See:
#     - .dev-context/claude-context.md Hard Rule #12
#     - BACKLOG.md §17 (P0 audit that prompted this lint)
#
# Documentation mentions are ignored automatically — no marker needed.
# The classifier strips C-style comments (`//`-to-EOL, `/* ... */`,
# trailing `/* ...`) and leading doxygen-continuation `*`, then re-checks
# whether `seastar::async(` still appears. Comment-only mentions drop
# out; real call sites survive and require a marker.
#
# Usage:
#   ./scripts/lint-seastar-async.sh             # scan src/ and tests/
#   ./scripts/lint-seastar-async.sh src/foo.cpp # scan specific paths
#
# Exit codes:
#   0 - lint passed
#   1 - unmarked seastar::async( call site(s) found
#   2 - script error (missing tool, etc.)
# ---------------------------------------------------------

set -euo pipefail

# Default scan roots when no args given.
SCAN_PATHS=("src" "tests")
if [[ $# -gt 0 ]]; then
    SCAN_PATHS=("$@")
fi

# Resolve repository root so the script works from any cwd.
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
cd "$repo_root"

# Collect all `seastar::async(` occurrences across the scan paths.
# Plain `grep -rn` is used (not `git grep`) so untracked files — e.g.
# a new .cpp added in the same commit as this lint runs against — are
# also scanned. The scan paths are small, so the speed difference is
# negligible. Restrict to C/C++ source extensions to skip generated
# binaries, dashboard HTML, and the like.
#
# `-H` forces grep to prefix every match line with the filename, even
# when the user passes a single file argument (in that case, grep
# defaults to LINE:CONTENT without the FILE: prefix, which would break
# the `IFS=: read -r file line content` parser below — `seastar::`
# contains its own colons).
raw_matches=$(grep -rnHE \
    --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.cc' \
    'seastar::async[[:space:]]*\(' \
    "${SCAN_PATHS[@]}" 2>/dev/null || true)

if [[ -z "$raw_matches" ]]; then
    echo "Rule #12 lint: no seastar::async( occurrences found in ${SCAN_PATHS[*]} — passes vacuously."
    exit 0
fi

# Walk each match. We classify each line as one of:
#   - documentation: comment-only mention, ignored.
#   - allowed: real call site with a rule12-allow: marker on this or
#     the preceding contiguous comment lines.
#   - violation: real call site with no marker.
#
# Classifier runs two passes per line:
#   1. Whole-line comment? (leading `//` or `*` doxygen continuation)
#      -> doc.
#   2. Otherwise, strip `// ...` trailing and `/* ... */` inline block
#      comments from the line and re-check for `seastar::async(`.
#      If absent, the mention was inside a comment -> doc. If still
#      present, it's a real call site and we look for the marker.
#
# Not bulletproof: won't track an open `/*` across lines, and won't
# preserve `//` inside a string literal. Adequate for this codebase —
# the lint is a guardrail, not an adversarial parser.
violations=()
allowed_count=0
doc_count=0

while IFS=: read -r file line content; do
    leading=$(printf '%s\n' "$content" | sed -E 's/^[[:space:]]*//')

    # Pass 1: whole-line comments. Doxygen continuation (`*` at line
    # start) and `//`-prefixed lines are documentation no matter what
    # they contain.
    case "$leading" in
        '*'*|'//'*)
            doc_count=$((doc_count + 1))
            continue
            ;;
    esac

    # Pass 2: strip inline comments and re-check. Catches mentions like
    # `auto x = foo(); /* uses seastar::async() */` and trailing
    # `// ... seastar::async() ...` on otherwise-real code lines that
    # don't actually call seastar::async themselves.
    code_only=$(printf '%s\n' "$content" | sed -E \
        -e 's@/\*[^*]*\*+([^/*][^*]*\*+)*/@@g' \
        -e 's@//.*$@@')
    if ! printf '%s\n' "$code_only" | grep -qE 'seastar::async[[:space:]]*\('; then
        doc_count=$((doc_count + 1))
        continue
    fi

    # Real call site. Check for marker on this line, or in the contiguous
    # block of comment lines immediately above it. Walking up through
    # comment lines (rather than checking only line-1) lets the marker
    # live in a multi-line `//` comment block that explains the rationale,
    # which is the natural shape for non-trivial justifications. A blank
    # line breaks the walk — markers must be directly adjacent to the call.
    if printf '%s\n' "$content" | grep -q 'rule12-allow:'; then
        allowed_count=$((allowed_count + 1))
        continue
    fi
    # Note: O(walk_distance) `sed -n Np FILE` reads per call site. Fine
    # for this codebase; if call-site count grows past ~hundreds, slurp
    # the file once into an array instead.
    found_marker=0
    walk_line=$((line - 1))
    while [[ $walk_line -ge 1 ]]; do
        walk_content=$(sed -n "${walk_line}p" "$file")
        walk_trimmed=$(printf '%s\n' "$walk_content" | sed -E 's/^[[:space:]]*//')
        # Stop walking once we hit a non-comment line (including blanks).
        if [[ -z "$walk_trimmed" ]]; then
            break
        fi
        case "$walk_trimmed" in
            '*'*|'//'*) ;;  # comment continuation; keep walking
            *) break ;;     # code line; stop
        esac
        if printf '%s\n' "$walk_content" | grep -q 'rule12-allow:'; then
            found_marker=1
            break
        fi
        walk_line=$((walk_line - 1))
    done

    if [[ $found_marker -eq 1 ]]; then
        allowed_count=$((allowed_count + 1))
        continue
    fi

    violations+=("$file:$line: $leading")
done <<< "$raw_matches"

if [[ ${#violations[@]} -gt 0 ]]; then
    echo "ERROR: Rule #12 lint failed — seastar::async( call site(s) without rule12-allow: marker:"
    echo ""
    printf '  %s\n' "${violations[@]}"
    echo ""
    echo "seastar::async() runs in a seastar::thread ON the reactor — it does"
    echo "NOT offload to a thread pool. Wrapping a blocking call in it stalls"
    echo "the shard exactly as a direct call would. See:"
    echo "  - .dev-context/claude-context.md Hard Rule #12"
    echo "  - BACKLOG.md §17 (the audit that found three production violations)"
    echo ""
    echo "If this call site is legitimate (e.g., the lambda uses .get() on"
    echo "Seastar futures, uses seastar::thread::yield() for reactor fairness,"
    echo "or is itself the offload-helper primitive), add:"
    echo ""
    echo "    // rule12-allow: <one-line reason>"
    echo "    co_await seastar::async([...] { ... });"
    echo ""
    echo "on the call line or the line directly above. The reason text is"
    echo "free-form — explain WHY this call is not the Rule #12 anti-pattern."
    exit 1
fi

total=$((allowed_count + doc_count))
echo "Rule #12 lint passed."
echo "  ${allowed_count} marked call site(s), ${doc_count} documentation mention(s) — ${total} total."
exit 0
