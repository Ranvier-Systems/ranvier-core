# Ranvier Core - Production Readiness Validation Suite v1.0

Automated validation suite to verify that architectural refactors have successfully
eliminated bottlenecks and reactor stalls in the Ranvier Core load balancer.

## Overview

This suite validates four key metrics that prove the Seastar shared-nothing
architecture is being respected:

| Test | Purpose | Success Criteria |
|------|---------|------------------|
| **Reactor Stall Detection** | Catches blocking operations on hot path | Zero reactor stalls |
| **Disk I/O Decoupling** | Validates async persistence | P99 latency < 10% degradation under disk stress |
| **SMP Bus Pressure** | Tests gossip handling under load | No SMP queue overflow |
| **Atomic-Free Execution** | Verifies shared_ptr removal | Zero atomic instructions in RadixTree |

## Quick Start

```bash
# Run full validation suite
./validation/validate_v1.sh

# Quick validation (shorter durations)
./validation/validate_v1.sh --quick

# CI mode (strict thresholds)
./validation/validate_v1.sh --ci

# Run specific test
./validation/validate_v1.sh --test stall
```

## Prerequisites

### Required
- Ranvier binary (build with `make build`)
- curl, nc, awk, grep (standard Unix tools)
- bc (for calculations)

### Recommended
- `wrk` - High-performance HTTP benchmarking tool
- `stress-ng` - System stress testing tool
- `perf` - Linux performance analysis tools
- `objdump` - Binary disassembly tool
- Python 3 with optional `scapy` library

### Installation (Ubuntu/Debian)

```bash
# Required tools
sudo apt-get install curl netcat-openbsd gawk bc

# Recommended tools
sudo apt-get install wrk stress-ng linux-tools-common linux-tools-$(uname -r) binutils

# Python scapy (optional, for IP spoofing in gossip test)
pip3 install scapy
```

## Test Details

### 1. Reactor Stall Detection (`stall_watchdog.sh`)

Launches Ranvier with aggressive Seastar stall detection:

```bash
--task-quota-ms 0.1        # 100μs task quota (catches micro-stalls)
--blocked-reactor-reports-per-core 10  # Detailed backtraces
```

**What it tests:**
- No blocking system calls on reactor thread
- No synchronous disk I/O
- No CPU-intensive operations blocking the event loop

**Run standalone:**
```bash
./validation/stall_watchdog.sh --duration 120s --task-quota 0.05
```

### 2. Disk I/O Decoupling (`disk_stress.sh`)

Validates that the async persistence refactor successfully decouples the
hot path from disk operations.

**Test procedure:**
1. Measure baseline P99 latency without disk stress
2. Start disk I/O stress (stress-ng or Python fallback)
3. Measure P99 latency under disk saturation
4. Verify degradation is within threshold (default: 10%)

**Run standalone:**
```bash
./validation/disk_stress.sh --threshold 15 --stress-duration 120s
```

### 3. SMP Gossip Storm (`gossip_storm.py`)

Floods the UDP gossip port with valid Route Announcement packets to test
SMP message queue handling under extreme load.

**Configuration:**
- Target: 5,000+ packets per second
- Valid protocol v2 packets
- Monitors `/proc/net/softnet_stat` for drops
- Checks Prometheus metrics for SMP queue depth

**Run standalone:**
```bash
python3 ./validation/gossip_storm.py \
    --target 127.0.0.1 \
    --port 9190 \
    --pps 5000 \
    --duration 60 \
    --prometheus http://127.0.0.1:9180/metrics
```

**With IP spoofing (requires root):**
```bash
sudo python3 ./validation/gossip_storm.py --use-scapy --pps 10000
```

### 4. Atomic-Free Execution (`atomic_audit.sh`)

Analyzes the compiled binary to verify that atomic reference counting
(shared_ptr) has been removed from RadixTree hot paths.

**What it looks for:**
- `lock` prefix instructions (x86 atomic operations)
- `xadd` instructions (atomic fetch-and-add, shared_ptr)
- `cmpxchg` instructions (compare-and-swap)

**Run standalone:**
```bash
./validation/atomic_audit.sh --binary ./build/ranvier --verbose
```

**Runtime analysis of running process:**
```bash
./validation/atomic_audit.sh --pid $(pgrep ranvier)
```

## Configuration

### Environment Variables

```bash
# Binary and config paths
export RANVIER_BINARY="./build/ranvier"
export RANVIER_CONFIG="./validation/config/test_config.yaml"

# Ports
export RANVIER_API_PORT=8080
export RANVIER_METRICS_PORT=9180
export RANVIER_GOSSIP_PORT=9190

# Test thresholds
export STALL_THRESHOLD=0
export P99_LATENCY_DEGRADATION_THRESHOLD=10
export SMP_QUEUE_OVERFLOW_THRESHOLD=1000
export ATOMIC_INSTRUCTION_THRESHOLD=0

# Load test settings
export WRK_DURATION=30s
export WRK_CONNECTIONS=100
export WRK_THREADS=4
```

### Test Configuration File

The test configuration is located at `validation/config/test_config.yaml`.
It's optimized for validation testing:

- RADIX routing mode (exercises RadixTree hot path)
- Gossip enabled on port 9190
- WAL mode for SQLite
- TLS/auth disabled for simplicity

## Output

### Reports

Reports are generated in `validation/reports/`:

```
validation/reports/
├── validation_v1_20240115_120000.json   # Main validation report
└── logs/
    ├── stall_watchdog_*.log             # Stall detection logs
    ├── disk_stress_baseline.txt         # Baseline latency measurements
    ├── disk_stress_under_load.txt       # Stressed latency measurements
    ├── gossip_storm_report.json         # Gossip test results
    ├── radix_tree_disasm.txt            # RadixTree disassembly
    └── atomic_audit_report.txt          # Atomic instruction analysis
```

### JSON Report Format

```json
{
  "suite_name": "Production Readiness v1.0",
  "version": "1.0.0",
  "timestamp": "2024-01-15T12:00:00+00:00",
  "overall_status": "PASS",
  "summary": {
    "total": 4,
    "passed": 4,
    "failed": 0,
    "skipped": 0
  },
  "tests": {
    "reactor_stall_detection": { "status": "PASS" },
    "disk_io_decoupling": { "status": "PASS" },
    "smp_gossip_storm": { "status": "PASS" },
    "atomic_free_execution": { "status": "PASS" }
  }
}
```

## CI/CD Integration

### GitHub Actions Example

```yaml
jobs:
  validation:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y wrk stress-ng binutils bc

      - name: Build
        run: make build

      - name: Run validation suite
        run: ./validation/validate_v1.sh --ci --output results.json

      - name: Upload results
        uses: actions/upload-artifact@v4
        with:
          name: validation-results
          path: |
            results.json
            validation/reports/
```

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | All tests passed |
| 1 | One or more tests failed |
| 2 | Setup/configuration error |

## Troubleshooting

### "Binary not found"
Build the Ranvier binary first:
```bash
make build
```

### "wrk not available"
Install wrk or the disk test will be skipped:
```bash
sudo apt-get install wrk
```

### "Permission denied for perf"
Run with sudo or adjust perf_event_paranoid:
```bash
sudo sysctl -w kernel.perf_event_paranoid=-1
```

### "No RadixTree symbols found"
The binary may be stripped. Build with debug symbols:
```bash
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
```

## Architecture Validation Explained

### Why These Tests Matter

The Seastar framework provides a shared-nothing architecture where each CPU
core runs an independent reactor with its own memory, avoiding locks and
cache line bouncing. These tests verify that the Ranvier refactors properly
leverage this architecture:

1. **Reactor Stalls** - Any blocking call (sync disk I/O, mutex wait) stalls
   the entire reactor, blocking all connections on that core.

2. **Disk Decoupling** - SQLite operations must be queued and executed
   asynchronously to prevent reactor stalls during WAL writes.

3. **SMP Pressure** - Cross-core communication via SMP queues is expensive.
   Gossip handling should minimize cross-core messaging.

4. **Atomic-Free** - shared_ptr uses atomic reference counting which causes
   cache line bouncing. unique_ptr with explicit ownership is preferred.

### Success Criteria

For "Production Readiness v1.0":

- Zero reactor stalls under load
- P99 latency stable (< 10% degradation) during disk saturation
- No SMP queue overflow under gossip flood
- Zero atomic instructions in RadixTree hot paths
