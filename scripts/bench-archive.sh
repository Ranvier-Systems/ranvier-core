#!/bin/bash
# =============================================================================
# bench-archive.sh — copy a benchmark run directory's SUMMARIES into the repo.
# =============================================================================
#
# A bench-runner.sh output directory is 50-100 MB per suite (Locust CSVs,
# per-request logs, vLLM logs). The results doc cites a few hundred KB of it:
# the compare files, the runner summary, the repeat aggregates, each arm's
# manifest.json, and the prefix arms' per-node Prometheus dumps (the routing
# counters). This script copies exactly those into
# docs/benchmarks/results/<name>/ in the flat layout the existing archives use
# (<arm>.manifest.json, <arm>.prometheus_metrics_nodeN.txt.gz, ...), so the
# evidence behind a verdict is in git while the raw data goes elsewhere (a
# tarball attached to the release, see docs/benchmarks/benchmark-results-current.md).
#
# Usage:
#   ./scripts/bench-archive.sh <run-dir> <archive-name> [options]
#
#   <run-dir>        a bench-runner.sh --output-dir (e.g. benchmark-reports-combo)
#   <archive-name>   docs/benchmarks/results/<archive-name>; convention <date>-<leg>,
#                    e.g. 2026-10-05-combo
#
# Options:
#   --date-prefix YYYYMMDD   only arms, compare files and runner summaries whose
#                            timestamp starts with this (a directory that holds
#                            several campaigns: one call per date)
#   --with-rr-dumps          also keep the round-robin arms' Prometheus dumps
#                            (default: prefix arms only — every cited counter is theirs)
#   --with-logs              also keep the per-node Ranvier container logs
#                            (ranvier_nodeN.log, gzipped; 100-500 KB each, so off by default)
#   --results-dir DIR        archive root (default docs/benchmarks/results)
#   --dry-run                print what would be copied, copy nothing
#
# Examples:
#   ./scripts/bench-archive.sh benchmark-reports-combo 2026-10-05-combo
#   ./scripts/bench-archive.sh benchmark-reports 2026-10-01-rebaseline --date-prefix 20261001
#
# Prints one line per archive: file count and size. Exit 1 if the run dir is
# missing or nothing matched; the caller decides whether that is a problem.
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

usage() { sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

SRC=""; NAME=""; PREFIX=""; WITH_RR=false; WITH_LOGS=false; DRY=false
RESULTS_DIR="$REPO_ROOT/docs/benchmarks/results"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --date-prefix) PREFIX="$2"; shift 2 ;;
        --with-rr-dumps) WITH_RR=true; shift ;;
        --with-logs) WITH_LOGS=true; shift ;;
        --results-dir) RESULTS_DIR="$2"; shift 2 ;;
        --dry-run) DRY=true; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
        *) if [[ -z "$SRC" ]]; then SRC="$1"; elif [[ -z "$NAME" ]]; then NAME="$1";
           else echo "unexpected argument: $1" >&2; exit 2; fi; shift ;;
    esac
done
[[ -n "$SRC" && -n "$NAME" ]] || { usage >&2; exit 2; }
[[ -d "$SRC" ]] || { echo "bench-archive: run dir not found: $SRC" >&2; exit 1; }
case "$NAME" in */*|.|..) echo "bench-archive: archive name must be a single path component" >&2; exit 2 ;; esac

DST="$RESULTS_DIR/$NAME"
copied=0

put() {   # put <source file> <destination file> [gzip]
    local from="$1" to="$2" mode="${3:-copy}"
    [[ -f "$from" ]] || return 0
    if $DRY; then echo "  would ${mode}: $(basename "$from") -> ${to#"$RESULTS_DIR"/}"
    elif [[ "$mode" == gzip ]]; then gzip -9c "$from" > "$to"
    else cp "$from" "$to"; fi
    copied=$((copied + 1))
}

$DRY || mkdir -p "$DST"

# Suite-level files: compare_<ts>.txt and runner_summary_<ts>.md carry the
# timestamp and honour --date-prefix.
shopt -s nullglob
for f in "$SRC"/compare_"${PREFIX}"*.txt "$SRC"/runner_summary_"${PREFIX}"*.md; do
    put "$f" "$DST/$(basename "$f")"
done

# Per-arm files, flattened under the arm's directory name
for arm in "$SRC"/"${PREFIX}"*_8gpu_*/; do
    arm="${arm%/}"; a="$(basename "$arm")"
    put "$arm/manifest.json" "$DST/$a.manifest.json"
    keep_dumps=false
    case "$a" in *_prefix) keep_dumps=true ;; esac
    $WITH_RR && keep_dumps=true
    for n in 1 2 3; do
        $keep_dumps && put "$arm/prometheus_metrics_node$n.txt" "$DST/$a.prometheus_metrics_node$n.txt.gz" gzip
        $WITH_LOGS && put "$arm/ranvier_node$n.log" "$DST/$a.ranvier_node$n.log.gz" gzip
    done
done
# Repeat aggregates (aggregates/agg_*.json) carry no timestamp: they describe
# the whole output directory, so they ride along only when something dated
# matched (always, without --date-prefix).
if [[ $copied -gt 0 ]]; then
    for f in "$SRC"/aggregates/agg_*.json; do
        put "$f" "$DST/$(basename "$f")"
    done
fi
shopt -u nullglob

if [[ $copied -eq 0 ]]; then
    echo "bench-archive: nothing matched in $SRC${PREFIX:+ with prefix $PREFIX}" >&2
    $DRY || rmdir "$DST" 2>/dev/null
    exit 1
fi
if $DRY; then
    echo "${DST#"$REPO_ROOT"/}: $copied files (dry run)"
else
    echo "${DST#"$REPO_ROOT"/}: $copied files, $(du -sh "$DST" | cut -f1)"
fi
