#!/usr/bin/env bash
# =============================================================================
# Ranvier Core Validation Suite - Perf-Based Atomic Audit
# =============================================================================
# Analyzes the compiled binary to verify that atomic reference counting
# (shared_ptr) has been removed from RadixTree hot paths.
#
# This script looks for:
#   - 'lock' prefix instructions (x86 atomic operations)
#   - 'xadd' instructions (atomic fetch-and-add, used by shared_ptr)
#   - 'cmpxchg' instructions (compare-and-swap)
#
# These instructions indicate atomic operations that can cause cache line
# bouncing in the shared-nothing Seastar architecture.
#
# Success Criteria:
#   Zero atomic instructions in RadixTree symbols (unique_ptr refactor complete)
#
# Exit Codes:
#   0 - No atomic instructions found in hot path symbols
#   1 - Atomic instructions detected or analysis failed
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/common.sh"

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
BINARY="${RANVIER_BINARY:-./build/ranvier_server}"
OUTPUT_DIR="${VALIDATION_LOG_DIR:-./validation/reports/logs}"
DISASM_FILE="${OUTPUT_DIR}/radix_tree_disasm.txt"
ATOMIC_REPORT="${OUTPUT_DIR}/atomic_audit_report.txt"

# Symbols to analyze (RadixTree hot path functions)
# These are the main code paths that should be atomic-free
HOTPATH_SYMBOLS=(
    "radix_tree"
    "RadixTree"
    "Node4"
    "Node16"
    "Node48"
    "Node256"
    "insert"
    "lookup"
    "find_child"
    "add_child"
    "grow"
)

# Patterns that indicate atomic operations
ATOMIC_PATTERNS=(
    "lock"        # x86 LOCK prefix for atomic operations
    "xadd"        # Atomic fetch-and-add (shared_ptr ref counting)
    "cmpxchg"     # Compare-and-exchange
    "xchg"        # Atomic exchange
)

# Patterns to exclude (false positives)
EXCLUDE_PATTERNS=(
    "unlock"      # pthread_mutex_unlock, etc.
    "clock"       # System clock functions
    "block"       # Memory block, disk block
    "flock"       # File locking (not in-memory atomic)
)

# -----------------------------------------------------------------------------
# Functions
# -----------------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  -b, --binary PATH        Path to Ranvier binary (default: $BINARY)
  -p, --pid PID            Analyze running process by PID
  -o, --output DIR         Output directory (default: $OUTPUT_DIR)
  -t, --threshold N        Max allowed atomic instructions (default: $ATOMIC_INSTRUCTION_THRESHOLD)
  -v, --verbose            Show detailed disassembly output
  -h, --help               Show this help message

Analysis Methods:
  1. Static analysis (default): Disassembles the binary with objdump
  2. Runtime analysis (--pid): Uses perf to sample a running process

Example:
  $(basename "$0") --binary ./build/ranvier
  $(basename "$0") --pid 12345 --verbose

EOF
}

parse_args() {
    ANALYZE_PID=""
    VERBOSE=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            -b|--binary)
                BINARY="$2"
                shift 2
                ;;
            -p|--pid)
                ANALYZE_PID="$2"
                shift 2
                ;;
            -o|--output)
                OUTPUT_DIR="$2"
                DISASM_FILE="${OUTPUT_DIR}/radix_tree_disasm.txt"
                ATOMIC_REPORT="${OUTPUT_DIR}/atomic_audit_report.txt"
                shift 2
                ;;
            -t|--threshold)
                ATOMIC_INSTRUCTION_THRESHOLD="$2"
                shift 2
                ;;
            -v|--verbose)
                VERBOSE="1"
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done
}

check_binary() {
    if [[ ! -f "$BINARY" ]]; then
        log_error "Binary not found: $BINARY"
        return 1
    fi

    # Check if binary has symbols
    if ! nm "$BINARY" &>/dev/null; then
        log_warn "Binary may be stripped, analysis may be limited"
    fi

    return 0
}

get_symbol_addresses() {
    local binary="$1"
    local pattern="$2"

    # Use nm to get symbol addresses matching pattern
    nm -C "$binary" 2>/dev/null | grep -i "$pattern" | awk '{print $1, $3}' || true
}

disassemble_symbol_range() {
    local binary="$1"
    local symbol="$2"

    # Get the disassembly for functions containing the symbol
    objdump -d -C "$binary" 2>/dev/null | \
        awk -v sym="$symbol" '
        /^[0-9a-f]+ <.*'"$symbol"'.*>:/ { printing=1 }
        printing { print }
        /^$/ && printing { printing=0 }
        ' || true
}

# AWK script for analyzing atomic instructions
ATOMIC_AWK_SCRIPT='
BEGIN {
    atomic_count = 0
    lock_count = 0
    xadd_count = 0
    cmpxchg_count = 0
    current_func = ""
}

# Track current function
/^[0-9a-f]+ <.*>:/ {
    current_func = $0
    gsub(/^[0-9a-f]+ </, "", current_func)
    gsub(/>:.*/, "", current_func)
}

# Check for LOCK prefix (must be followed by instruction)
/\tlock\s/ {
    # Exclude false positives
    if ($0 !~ /unlock|clock|block|flock/) {
        lock_count++
        atomic_count++
        if (verbose) {
            print "LOCK @ " current_func ": " $0
        }
    }
}

# Check for XADD (atomic fetch-and-add)
/\txadd/ {
    xadd_count++
    atomic_count++
    if (verbose) {
        print "XADD @ " current_func ": " $0
    }
}

# Check for CMPXCHG (compare-and-swap)
/\tcmpxchg/ {
    cmpxchg_count++
    atomic_count++
    if (verbose) {
        print "CMPXCHG @ " current_func ": " $0
    }
}

# Check for XCHG (atomic exchange, but not with same register)
/\txchg\s+[^,]+,[^,]+/ {
    # Skip xchg with same register (register renaming, not atomic)
    split($0, parts, /\t/)
    if (parts[3] !~ /([a-z]+),\1/) {
        atomic_count++
        if (verbose) {
            print "XCHG @ " current_func ": " $0
        }
    }
}

END {
    print "---SUMMARY---"
    print "total_atomic:" atomic_count
    print "lock_prefix:" lock_count
    print "xadd:" xadd_count
    print "cmpxchg:" cmpxchg_count
}
'

analyze_static() {
    # All log output goes to stderr so only the count is returned via stdout
    log_info "Performing static analysis of binary: $BINARY" >&2

    mkdir -p "$OUTPUT_DIR"

    # Disassemble entire binary (filtered for RadixTree)
    log_info "Disassembling RadixTree-related symbols..." >&2

    > "$DISASM_FILE"

    for pattern in "${HOTPATH_SYMBOLS[@]}"; do
        log_info "  Analyzing symbols matching: $pattern" >&2
        disassemble_symbol_range "$BINARY" "$pattern" >> "$DISASM_FILE"
    done

    local disasm_count
    disasm_count=$(wc -l < "$DISASM_FILE")

    if [[ $disasm_count -eq 0 ]]; then
        log_warn "No matching symbols found. Binary may be stripped." >&2
        log_info "Falling back to filtered binary analysis..." >&2

        # Filtered disassembly - only extract relevant sections to avoid processing entire binary
        # Focus on .text section and filter for our patterns during extraction
        objdump -d -C "$BINARY" 2>/dev/null | \
            awk '
            /^[0-9a-f]+ <.*>:/ { in_func = 1; func = $0 }
            in_func && /\t(lock|xadd|cmpxchg|xchg)\s/ {
                if (func) { print func; func = "" }
                print
            }
            /^$/ { in_func = 0 }
            ' > "$DISASM_FILE"
        disasm_count=$(wc -l < "$DISASM_FILE")

        # If still no atomic instructions found, the test passes trivially
        if [[ $disasm_count -eq 0 ]]; then
            log_info "No atomic instructions found in binary (filtered analysis)" >&2
        fi
    fi

    log_info "Disassembly complete: $disasm_count lines" >&2

    # Analyze for atomic instructions
    log_info "Analyzing for atomic instructions..." >&2

    local analysis
    analysis=$(awk -v verbose="${VERBOSE:-0}" "$ATOMIC_AWK_SCRIPT" "$DISASM_FILE")

    # Parse results
    local total_atomic lock_prefix xadd cmpxchg
    total_atomic=$(echo "$analysis" | grep "total_atomic:" | cut -d: -f2)
    lock_prefix=$(echo "$analysis" | grep "lock_prefix:" | cut -d: -f2)
    xadd=$(echo "$analysis" | grep "xadd:" | cut -d: -f2)
    cmpxchg=$(echo "$analysis" | grep "cmpxchg:" | cut -d: -f2)

    # Generate report
    cat > "$ATOMIC_REPORT" <<EOF
================================================================================
ATOMIC INSTRUCTION AUDIT REPORT
================================================================================
Binary: $BINARY
Analysis Date: $(date -Iseconds)
Method: Static disassembly analysis

RESULTS:
--------
Total atomic instructions in RadixTree symbols: $total_atomic
  - LOCK prefix instructions: $lock_prefix
  - XADD instructions: $xadd
  - CMPXCHG instructions: $cmpxchg

Threshold: $ATOMIC_INSTRUCTION_THRESHOLD
Status: $([ "$total_atomic" -le "$ATOMIC_INSTRUCTION_THRESHOLD" ] && echo "PASS" || echo "FAIL")

ANALYSIS DETAILS:
-----------------
$(echo "$analysis" | grep -v "^---" | grep -v "^total" | grep -v "^lock_prefix" | grep -v "^xadd" | grep -v "^cmpxchg" || echo "No detailed findings")

NOTES:
------
- XADD instructions indicate atomic reference counting (shared_ptr)
- LOCK prefix on memory operations causes cache line bouncing
- The shared_ptr -> unique_ptr refactor should eliminate these in RadixTree

EOF

    # Print report to stderr for visibility, only return count via stdout
    cat "$ATOMIC_REPORT" >&2

    echo "$total_atomic"
}

analyze_runtime() {
    local pid="$1"

    log_info "Performing runtime analysis of PID: $pid"

    if ! kill -0 "$pid" 2>/dev/null; then
        log_error "Process $pid not found or not accessible"
        return 1
    fi

    mkdir -p "$OUTPUT_DIR"

    # Use perf to sample the running process
    log_info "Recording perf samples (10 seconds)..."

    local perf_data="${OUTPUT_DIR}/perf.data"

    # Record CPU cycles with call stacks
    sudo perf record \
        -p "$pid" \
        -g \
        --call-graph dwarf \
        -o "$perf_data" \
        sleep 10

    # Analyze perf data for atomic operations
    log_info "Analyzing perf samples..."

    sudo perf report \
        -i "$perf_data" \
        --stdio \
        --no-children \
        --sort=symbol \
        2>/dev/null | tee "${OUTPUT_DIR}/perf_report.txt"

    # Look for atomic-related symbols in hot paths
    log_info "Checking for atomic operations in hot symbols..."

    local atomic_hotspots
    atomic_hotspots=$(sudo perf report \
        -i "$perf_data" \
        --stdio \
        --no-children \
        2>/dev/null | \
        grep -E "(atomic|shared_ptr|__atomic|lock.*cmpxchg)" | \
        head -20 || true)

    if [[ -n "$atomic_hotspots" ]]; then
        log_warn "Found atomic operations in hot paths:"
        echo "$atomic_hotspots"
        return 1
    else
        log_success "No atomic operations found in hot paths"
        return 0
    fi
}

analyze_with_perf_annotate() {
    local binary="$1"

    log_info "Using perf annotate for detailed instruction analysis..."

    mkdir -p "$OUTPUT_DIR"

    # Find RadixTree functions
    local symbols
    symbols=$(nm -C "$binary" 2>/dev/null | grep -i "radix" | awk '{print $3}' | head -20)

    if [[ -z "$symbols" ]]; then
        log_warn "No RadixTree symbols found for annotation"
        return 1
    fi

    local atomic_found=0
    local annotate_file="${OUTPUT_DIR}/perf_annotate.txt"
    > "$annotate_file"

    for sym in $symbols; do
        log_info "  Annotating: $sym"

        # Use perf annotate to show assembly with source
        perf annotate \
            --stdio \
            -s "$sym" \
            "$binary" 2>/dev/null >> "$annotate_file" || true
    done

    # Check annotations for atomic instructions
    local atomic_in_radix
    atomic_in_radix=$(grep -c -E "\s(lock|xadd|cmpxchg)\s" "$annotate_file" 2>/dev/null) || atomic_in_radix=0

    log_info "Atomic instructions in RadixTree annotations: $atomic_in_radix"

    echo "$atomic_in_radix"
}

generate_json_report() {
    local atomic_count="$1"
    local status="$2"

    cat <<EOF
{
  "total_atomic_instructions": $atomic_count,
  "threshold": $ATOMIC_INSTRUCTION_THRESHOLD,
  "binary": "$BINARY",
  "analysis_method": "static_disassembly",
  "symbols_analyzed": [$(printf '"%s",' "${HOTPATH_SYMBOLS[@]}" | sed 's/,$//')],
  "disasm_file": "$DISASM_FILE",
  "report_file": "$ATOMIC_REPORT"
}
EOF
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
    parse_args "$@"

    log_section "Atomic Instruction Audit"

    # Verify dependencies
    local required_deps=(objdump nm awk)
    if [[ -n "$ANALYZE_PID" ]]; then
        required_deps+=(perf)
    fi
    check_dependencies "${required_deps[@]}"

    local atomic_count=0
    local status="FAIL"

    if [[ -n "$ANALYZE_PID" ]]; then
        # Runtime analysis
        if analyze_runtime "$ANALYZE_PID"; then
            atomic_count=0
        else
            atomic_count=1
        fi
    else
        # Static analysis
        if ! check_binary; then
            exit 1
        fi

        atomic_count=$(analyze_static)
    fi

    # Determine pass/fail
    if [[ "$atomic_count" -le "$ATOMIC_INSTRUCTION_THRESHOLD" ]]; then
        status="PASS"
        log_success "ATOMIC AUDIT: PASSED"
        log_info "Atomic instructions found: $atomic_count (threshold: $ATOMIC_INSTRUCTION_THRESHOLD)"
    else
        status="FAIL"
        log_error "ATOMIC AUDIT: FAILED"
        log_error "Atomic instructions found: $atomic_count (threshold: $ATOMIC_INSTRUCTION_THRESHOLD)"
        log_info "Review $ATOMIC_REPORT for details"
    fi

    # Generate JSON report
    generate_json_report "$atomic_count" "$status"

    [[ "$status" == "PASS" ]]
}

# Run if executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
