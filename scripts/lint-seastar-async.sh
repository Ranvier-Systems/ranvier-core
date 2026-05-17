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
# Documentation mentions (lines starting with `*` for doxygen, lines
# starting with `//`, or where `seastar::async(` only appears inside
# a `//` comment) are ignored automatically — no marker needed.
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
raw_matches=$(grep -rnE \
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
#     the preceding line.
#   - violation: real call site with no marker.
violations=()
allowed_count=0
doc_count=0

while IFS=: read -r file line content; do
    # Skip pure-comment lines (doxygen continuation `*` or single-line `//`)
    # by looking at the first non-whitespace character.
    leading=$(printf '%s\n' "$content" | sed -E 's/^[[:space:]]*//')
    first_char="${leading:0:1}"
    if [[ "$first_char" == "*" || "$first_char" == "/" ]]; then
        if [[ "${leading:0:2}" == "//" || "$first_char" == "*" ]]; then
            doc_count=$((doc_count + 1))
            continue
        fi
    fi

    # Strip everything from `//` to end-of-line, then re-check whether
    # the line still contains `seastar::async(`. Catches inline-comment
    # mentions like `enum { ASYNC, /* uses seastar::async() */ };`.
    code_only=$(printf '%s\n' "$content" | sed -E 's@//.*$@@')
    if ! printf '%s\n' "$code_only" | grep -qE 'seastar::async[[:space:]]*\('; then
        doc_count=$((doc_count + 1))
        continue
    fi

    # Real call site. Check for marker on this line, or in the contiguous
    # block of comment lines immediately above it. Walking up through
    # comment lines (rather than checking only line-1) lets the marker
    # live in a multi-line `//` comment block that explains the rationale,
    # which is the natural shape for non-trivial justifications.
    if printf '%s\n' "$content" | grep -q 'rule12-allow:'; then
        allowed_count=$((allowed_count + 1))
        continue
    fi
    found_marker=0
    walk_line=$((line - 1))
    while [[ $walk_line -ge 1 ]]; do
        walk_content=$(sed -n "${walk_line}p" "$file")
        walk_trimmed=$(printf '%s\n' "$walk_content" | sed -E 's/^[[:space:]]*//')
        walk_first="${walk_trimmed:0:1}"
        walk_first2="${walk_trimmed:0:2}"
        # Stop walking once we hit a non-comment line.
        if [[ "$walk_first" != "*" && "$walk_first2" != "//" ]]; then
            break
        fi
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
