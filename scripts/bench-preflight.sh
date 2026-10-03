#!/bin/bash
# =============================================================================
# Benchmark preflight — catch the silly error BEFORE the 8-hour GPU run.
# =============================================================================
#
# Runs, in order, everything that can fail a campaign without a GPU having
# done any work: shell/python syntax, the parser unit tests, the compose file,
# the Locust image (does it build, does every module import inside it), the
# host toolchain, and a dry run of the suite you are about to launch. With
# --smoke it then runs ONE short A/B (1 minute per arm, 2 users, 2 repeats)
# through the real pipeline — vLLM start, per-arm Ranvier restart, warm-up,
# metrics snapshots, manifests, arm-validity checks, repeat aggregation — and
# verifies the artefacts that the campaign's conclusions will be read from.
#
# Usage:
#   ./scripts/bench-preflight.sh                       # static + container checks (~2 min)
#   ./scripts/bench-preflight.sh --suite epsilon       # dry-run that suite instead of rebaseline
#   ./scripts/bench-preflight.sh --smoke               # + 1-minute A/B x2 on the 8B model (~15-25 min, needs GPUs)
#   ./scripts/bench-preflight.sh --smoke --model meta-llama/CodeLlama-13b-Instruct-hf
#
# Exit 0 only when every check passed. Read the FAIL lines; each names the
# file or step to fix.
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

SUITE="rebaseline"
SMOKE=false
SMOKE_MODEL="meta-llama/Llama-3.1-8B-Instruct"
SMOKE_OUT="benchmark-reports/preflight-smoke"

while [[ $# -gt 0 ]]; do
    case $1 in
        --suite) SUITE="$2"; shift 2 ;;
        --smoke) SMOKE=true; shift ;;
        --model) SMOKE_MODEL="$2"; shift 2 ;;
        -h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1"; exit 2 ;;
    esac
done

PASS=0; FAIL=0; WARN=0
ok()   { echo "  PASS  $*"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL  $*"; FAIL=$((FAIL + 1)); }
warn() { echo "  WARN  $*"; WARN=$((WARN + 1)); }
section() { echo; echo "== $* =="; }

# -----------------------------------------------------------------------------
section "1. Shell syntax"
for f in scripts/bench.sh scripts/bench-runner.sh scripts/bench-residency-ab.sh tests/integration/run-benchmark.sh; do
    if bash -n "$f" 2>/tmp/preflight_err; then ok "$f"; else bad "$f: $(head -1 /tmp/preflight_err)"; fi
done

# -----------------------------------------------------------------------------
section "2. Python byte-compile (the files the Locust container will import)"
PYFILES=(tests/integration/locustfile_real.py tests/integration/locustfile.py tests/integration/prom_scrape.py
         tests/integration/prompt_loader.py tests/integration/results_parser.py tests/integration/mock_backend.py)
for f in "${PYFILES[@]}"; do
    if python3 -m py_compile "$f" 2>/tmp/preflight_err; then ok "$f"; else bad "$f: $(tail -1 /tmp/preflight_err)"; fi
done

# -----------------------------------------------------------------------------
section "3. Parser and scrape unit tests"
TESTS=(tests/integration/test_prom_scrape.py tests/integration/test_results_real_log.py
       tests/integration/test_results_prometheus.py tests/integration/test_results_aggregate.py
       tests/integration/test_results_manifest.py)
if python3 -c "import pytest" 2>/dev/null; then
    if out=$(python3 -m pytest -q "${TESTS[@]}" 2>&1); then ok "$(echo "$out" | tail -1)"; else bad "pytest failed:"; echo "$out" | tail -15; fi
else
    warn "pytest not installed on this host (pip install pytest) — skipping the 61 parser tests"
fi

# -----------------------------------------------------------------------------
section "4. Host toolchain"
for t in docker python3 curl git; do
    if command -v "$t" >/dev/null 2>&1; then ok "$t"; else bad "$t not found (bench.sh needs it)"; fi
done
# bc and jq are NOT needed by the GPU campaign: bench.sh uses bc only inside a
# guarded block with an integer fallback (KV-fit autosizing), and jq only by the
# mock-CI gate (run-benchmark.sh). Report, do not block.
for t in bc jq; do
    if command -v "$t" >/dev/null 2>&1; then ok "$t (optional for bench.sh; required by run-benchmark.sh)"; else warn "$t not found — fine for bench.sh/bench-runner.sh; install (apt-get install -y bc jq) before running the CI gate script locally"; fi
done
if docker ps >/dev/null 2>&1; then ok "docker usable without sudo"; else bad "cannot run docker (add user to docker group, or run under sudo)"; fi
if docker compose version >/dev/null 2>&1; then ok "docker compose v2"; else bad "docker compose v2 not available"; fi
if command -v nvidia-smi >/dev/null 2>&1; then
    GPUS=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | wc -l | tr -d ' ')
    [[ "$GPUS" -ge 1 ]] && ok "nvidia-smi: $GPUS GPU(s): $(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | sort -u | tr '\n' ';')" || bad "nvidia-smi present but reports no GPUs"
else
    warn "nvidia-smi not found — fine for static checks, fatal for a real run"
fi
if [[ -n "${HF_TOKEN:-}" ]]; then ok "HF_TOKEN set"; else bad "HF_TOKEN not set (gated models will fail to download at vLLM start)"; fi
if python3 -c "import yaml" 2>/dev/null; then ok "python3 yaml module"; else warn "python3 has no yaml module; compose validation relies on docker compose only"; fi
AVAIL_TMP=$(df -Pm /tmp 2>/dev/null | awk 'NR==2{print $4}'); AVAIL_HERE=$(df -Pm . 2>/dev/null | awk 'NR==2{print $4}')
[[ "${AVAIL_TMP:-0}" -ge 5000 ]] && ok "/tmp free: ${AVAIL_TMP} MB (vLLM logs, core dumps)" || warn "/tmp has only ${AVAIL_TMP:-?} MB free"
[[ "${AVAIL_HERE:-0}" -ge 5000 ]] && ok "repo volume free: ${AVAIL_HERE} MB (benchmark-reports)" || warn "repo volume has only ${AVAIL_HERE:-?} MB free"
# vLLM runs as HOST processes (Locust and Ranvier run in containers under the
# daemon's own limits), so the shell's fd limit applies to vLLM only. 1024 is
# ample for the suites here (<= 64 concurrent users); bench.sh --setup does not
# change it. Raise in the launching shell if you go far beyond that.
ULIM=$(ulimit -n); [[ "$ULIM" -ge 4096 ]] && ok "ulimit -n $ULIM (applies to the host vLLM processes)" || warn "ulimit -n is $ULIM: fine for the built-in suites; run 'ulimit -n 65536' in this shell first if you benchmark hundreds of users"

# -----------------------------------------------------------------------------
section "5. Compose file"
if docker compose -f docker-compose.benchmark-real.yml config -q 2>/tmp/preflight_err; then
    ok "docker-compose.benchmark-real.yml validates"
else
    bad "compose config: $(head -3 /tmp/preflight_err | tr '\n' ' ')"
fi
if grep -q 'RANVIER_DB_PATH=/var/lib/ranvier/ranvier.db' docker-compose.benchmark-real.yml && grep -q '/var/lib/ranvier:size=' docker-compose.benchmark-real.yml; then
    ok "routing DB on container-local tmpfs (no cross-arm carry-over)"
else
    bad "routing DB is not on the container-local tmpfs — the fix-1 change is missing from this checkout"
fi
if [[ -f /tmp/ranvier.db ]]; then warn "legacy host /tmp/ranvier.db exists; harmless now (no longer read) but you may delete it"; fi

# -----------------------------------------------------------------------------
section "6. Locust image: build and import every module inside the container"
if docker ps >/dev/null 2>&1; then
    if docker build -q -t ranvier-locust:preflight -f tests/integration/Dockerfile.locust tests/integration/ >/tmp/preflight_build.log 2>&1; then
        ok "Locust image builds"
        for m in locustfile_real locustfile prom_scrape prompt_loader; do
            if docker run --rm --entrypoint python -e NUM_BACKENDS=2 -e BENCHMARK_MODE=prefix ranvier-locust:preflight \
                    -c "import $m" >/tmp/preflight_err 2>&1; then
                ok "import $m inside the image"
            else
                bad "import $m failed inside the image:"; grep -v 'MonkeyPatch\|monkey.patch' /tmp/preflight_err | tail -5
            fi
        done
        LV=$(docker run --rm --entrypoint python ranvier-locust:preflight -c "import locust; print(locust.__version__)" 2>/dev/null | tail -1)
        [[ "$LV" == 2.24.* ]] && ok "locust $LV (the version the parser's column mapping was verified against)" || warn "locust $LV: the CSV percentile column mapping was verified on 2.24.0"
        docker rmi ranvier-locust:preflight >/dev/null 2>&1 || true
    else
        bad "Locust image build failed:"; tail -10 /tmp/preflight_build.log
    fi
else
    warn "docker not usable; skipping image build and in-container imports"
fi

# -----------------------------------------------------------------------------
section "6b. Server image freshness (is ranvier:latest built from THIS checkout's src/?)"
if docker ps >/dev/null 2>&1; then
    if created=$(docker image inspect -f '{{.Created}}' ranvier:latest 2>/dev/null); then
        img_ts=$(date -d "$created" +%s 2>/dev/null || echo 0)
        src_ts=$(git log -1 --format=%ct -- src CMakeLists.txt Dockerfile.production 2>/dev/null || echo 0)
        src_desc=$(git log -1 --format='%h %s' -- src CMakeLists.txt Dockerfile.production 2>/dev/null || echo unknown)
        if [[ "$img_ts" -gt 0 && "$src_ts" -gt 0 && "$img_ts" -lt "$src_ts" ]]; then
            bad "ranvier:latest (built $(date -d "@$img_ts" '+%Y-%m-%d %H:%M')) predates the newest src/ commit: $src_desc"
            echo "        the suite would benchmark a server WITHOUT that change; run: ./scripts/bench-runner.sh --build-image ... (or docker build -t ranvier:latest -f Dockerfile.production .)"
        else
            ok "ranvier:latest built $(date -d "@$img_ts" '+%Y-%m-%d %H:%M' 2>/dev/null || echo unknown), at or after the newest src/ commit ($src_desc)"
        fi
        if [[ -n "$(git status --porcelain -- src CMakeLists.txt 2>/dev/null)" ]]; then
            warn "uncommitted changes under src/: the image cannot contain them"
        fi
    else
        warn "no ranvier:latest image yet; bench.sh will pull GHCR (built from MAIN) unless you pass --build-image"
    fi
else
    warn "docker not usable; skipping server image freshness check"
fi

# -----------------------------------------------------------------------------
section "7. Runner dry run for --suite $SUITE"
if out=$(./scripts/bench-runner.sh --suite "$SUITE" --dry-run 2>&1); then
    n=$(echo "$out" | grep -oE 'Suite: [a-z]+ \([0-9]+ runs\)' | head -1)
    ok "bench-runner --suite $SUITE --dry-run: ${n:-ok}"
    echo "$out" | grep -E '^\s*\[?[0-9]+(/[0-9]+)?\]? ' | sed 's/\x1b\[[0-9;]*m//g' | sed 's/^/        /' | head -14
else
    bad "bench-runner dry run failed:"; echo "$out" | tail -8
fi
if out=$(./scripts/bench.sh --dry-run --compare --warmup --duration 1m --users 2 --model "$SMOKE_MODEL" 2>&1); then
    ok "bench.sh --dry-run accepts the flags"
else
    if echo "$out" | grep -qi 'unknown option'; then bad "bench.sh rejected a flag: $(echo "$out" | grep -i 'unknown option')"; else warn "bench.sh --dry-run stopped at a pre-flight (expected off-GPU): $(echo "$out" | sed 's/\x1b\[[0-9;]*m//g' | grep -E '✗|Error|error' | head -1)"; fi
fi

# -----------------------------------------------------------------------------
if [[ "$SMOKE" = true ]]; then
    section "8. Smoke A/B through the real pipeline (1m/arm, 2 users, 2 repeats, $SMOKE_MODEL)"
    if [[ $FAIL -gt 0 ]]; then
        bad "skipping smoke run: $FAIL static check(s) failed above"
    else
        rm -rf "$SMOKE_OUT"; mkdir -p "$SMOKE_OUT"
        SMOKE_RUNS="$SMOKE_OUT/smoke.runs"
        echo "--compare --warmup --duration 1m --users 2 --spawn-rate 2 --model $SMOKE_MODEL" > "$SMOKE_RUNS"
        echo "  running: bench-runner.sh --suite custom --file $SMOKE_RUNS --repeat 2 --pause 5 --output-dir $SMOKE_OUT"
        if ./scripts/bench-runner.sh --suite custom --file "$SMOKE_RUNS" --repeat 2 --pause 5 --output-dir "$SMOKE_OUT" > "$SMOKE_OUT/runner_stdout.log" 2>&1; then
            ok "runner exit 0"
        else
            bad "runner exit $? — see $SMOKE_OUT/runner_stdout.log (last lines below)"; tail -15 "$SMOKE_OUT/runner_stdout.log"
        fi
        # Artefacts the campaign's conclusions are read from.
        mapfile -t MANIFESTS < <(find "$SMOKE_OUT" -name manifest.json -path '*gpu_*' 2>/dev/null | sort)
        [[ ${#MANIFESTS[@]} -eq 4 ]] && ok "4 arm report dirs (2 repeats x 2 arms) with manifest.json" || bad "expected 4 arm manifests, found ${#MANIFESTS[@]}"
        for m in "${MANIFESTS[@]}"; do
            d=$(dirname "$m"); tag=$(basename "$d")
            [[ -f "$d/FAILED" ]] && bad "$tag: FAILED marker: $(cat "$d/FAILED")"
            python3 -c "import json,sys; j=json.load(open(sys.argv[1])); r=j['regime']; assert j['routing']['hash_strategy']; print(r['kv_cache_tokens_min'], r['prefix_working_set_tokens_est'])" "$m" >/tmp/preflight_err 2>&1 \
                && ok "$tag: manifest parses; regime kv_min/working_set = $(cat /tmp/preflight_err | tr '\n' ' ')" \
                || bad "$tag: manifest.json missing regime/routing blocks: $(tail -1 /tmp/preflight_err)"
            [[ "$(cat /tmp/preflight_err 2>/dev/null)" == None* ]] && warn "$tag: KV capacity not captured (vLLM log format unrecognised?) — regime will read 'unknown'"
            s=$(ls "$d"/prometheus_metrics_start_node*.txt 2>/dev/null | wc -l); e=$(ls "$d"/prometheus_metrics_node*.txt 2>/dev/null | wc -l)
            [[ "$s" -eq 3 && "$e" -eq 3 ]] && ok "$tag: 3 start + 3 end Prometheus snapshots" || bad "$tag: Prometheus snapshots start=$s end=$e (expected 3/3)"
            for needle in "BENCHMARK_STATS_JSON:" "TTFT (raw samples" "Route Consistency:" "KV Prefix-Cache Hit Rate"; do
                grep -q "$needle" "$d/benchmark.log" 2>/dev/null && ok "$tag: log has '$needle'" || bad "$tag: log lacks '$needle'"
            done
            grep -q "KV Prefix-Cache Hit Rate: unavailable" "$d/benchmark.log" 2>/dev/null && warn "$tag: vLLM prefix-cache counters not found — paste 'curl backend:8000/metrics | grep prefix_cache' so the metric names can be added"
            grep -q "ROUTING MODE MISMATCH" "$d/benchmark.log" 2>/dev/null && bad "$tag: ROUTING MODE MISMATCH in log"
            if src=$(python3 -c "import sys; sys.path.insert(0, 'tests/integration'); from results_parser import parse_benchmark_log as p; r = p(sys.argv[1]); print(r.ttft_source, r.p99_ttft_ms, r.cache_hit_rate_pct, r.kv_prefix_cache_hit_rate_pct)" "$d/benchmark.log" 2>/tmp/preflight_err); then
                [[ "$src" == raw* ]] && ok "$tag: parser reads raw-sample TTFT (source p99 route_consistency kv_hit: $src)" || bad "$tag: parser ttft_source is not 'raw' ($src)"
            else
                bad "$tag: results_parser failed on the log: $(tail -1 /tmp/preflight_err)"
            fi
        done
        AGG=$(ls "$SMOKE_OUT"/aggregates/agg_*.json 2>/dev/null | head -1)
        if [[ -n "$AGG" ]] && python3 -c "import json,sys; j=json.load(open(sys.argv[1])); print(j['verdict'])" "$AGG" >/tmp/preflight_err 2>&1; then
            ok "repeat aggregation ran; verdict: $(cat /tmp/preflight_err)"
            grep -q 'counters_differenced' "$AGG" 2>/dev/null || true
        else
            bad "no aggregate JSON under $SMOKE_OUT/aggregates (repeat aggregation did not run)"
        fi
        BANNER=$(grep -h 'KV regime' "$SMOKE_OUT"/runner_*.log 2>/dev/null | head -1 | sed 's/\x1b\[[0-9;]*m//g')
        [[ -n "$BANNER" ]] && echo "  INFO  $BANNER" || warn "no 'KV regime' banner line found in the runner log"
        echo "  INFO  smoke artefacts kept under $SMOKE_OUT (delete before the real campaign if you want a clean tree)"
    fi
fi

# -----------------------------------------------------------------------------
echo
echo "== Preflight summary: $PASS passed, $WARN warnings, $FAIL failed =="
if [[ $FAIL -eq 0 ]]; then
    echo "Ready. Launch with:  ./scripts/bench-runner.sh --suite $SUITE"
    exit 0
else
    echo "Fix the FAIL lines above before launching."
    exit 1
fi
