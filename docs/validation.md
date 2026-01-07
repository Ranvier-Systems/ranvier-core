# Production Readiness Validation Suite

The Ranvier Core validation suite provides automated testing to verify that architectural
refactors maintain the Seastar shared-nothing guarantees required for production deployment.

## Overview

The validation suite tests four key metrics that prove the load balancer is
production-ready:

| Test | What It Validates | Success Criteria |
|------|-------------------|------------------|
| Reactor Stall Detection | No blocking operations on hot path | Zero stalls under load |
| Disk I/O Decoupling | Async persistence is working | P99 < 10% degradation |
| SMP Gossip Storm | Cross-core messaging efficiency | No queue overflow |
| Atomic-Free Execution | shared_ptr removed from hot paths | Zero atomic ops in RadixTree |

## Quick Start

```bash
# Build first
make build

# Run full validation suite
make validate

# Quick validation (shorter durations)
make validate-quick

# CI mode (strict thresholds)
make validate-ci
```

## Test Details

### 1. Reactor Stall Detection

**Purpose:** Detect any blocking operations that would stall the Seastar reactor.

**Mechanism:**
- Launches Ranvier with `--task-quota-ms 0.1` (100μs task quota)
- Uses `--blocked-reactor-reports-per-core 10` for detailed backtraces
- Generates load while monitoring logs for "Reactor stall detected"

**Configuration:**
```bash
# Environment variables
TASK_QUOTA_MS=0.1          # Task time slice in milliseconds
STALL_THRESHOLD=0          # Maximum allowed stalls
STALL_TEST_DURATION=60     # Test duration in seconds
```

**Running standalone:**
```bash
./validation/stall_watchdog.sh \
    --duration 120s \
    --task-quota 0.05 \
    --poll-mode
```

### 2. Disk I/O Decoupling

**Purpose:** Verify that SQLite persistence doesn't block the reactor.

**Mechanism:**
1. Measure baseline P99 latency without disk stress
2. Start heavy disk I/O on the SQLite WAL path
3. Measure P99 latency under stress
4. Verify degradation is within threshold

**Configuration:**
```bash
# Environment variables
P99_LATENCY_DEGRADATION_THRESHOLD=10  # Maximum % increase allowed
DISK_TEST_DURATION=60                  # Test duration
STRESS_NG_HDD_WORKERS=4               # Number of stress workers
```

**Running standalone:**
```bash
./validation/disk_stress.sh \
    --threshold 15 \
    --stress-duration 120s \
    --hdd-workers 8
```

**Dependencies:**
- `wrk` (required)
- `stress-ng` (recommended, Python fallback available)

### 3. SMP Gossip Storm

**Purpose:** Test gossip handling under extreme UDP flood conditions.

**Mechanism:**
- Floods gossip port with 5000+ valid Route Announcement packets/second
- Monitors `/proc/net/softnet_stat` for kernel packet drops
- Checks Prometheus metrics for SMP queue depth

**Configuration:**
```bash
# Environment variables
SMP_QUEUE_OVERFLOW_THRESHOLD=1000  # Maximum allowed queue depth
GOSSIP_TEST_DURATION=60            # Test duration
```

**Running standalone:**
```bash
python3 ./validation/gossip_storm.py \
    --target 127.0.0.1 \
    --port 9190 \
    --pps 10000 \
    --duration 120 \
    --prometheus http://127.0.0.1:9180/metrics
```

**Protocol Details:**
The generator creates valid protocol v2 packets:
```
Wire format: [type:1][version:1][seq_num:4][backend_id:4][token_count:2][tokens:4*N]
```

### 4. Atomic-Free Execution Audit

**Purpose:** Verify that shared_ptr atomic operations are removed from hot paths.

**Mechanism:**
- Disassembles the binary with `objdump`
- Scans RadixTree symbols for:
  - `lock` prefix (x86 atomic operations)
  - `xadd` (atomic fetch-and-add, used by shared_ptr)
  - `cmpxchg` (compare-and-swap)

**Configuration:**
```bash
# Environment variables
ATOMIC_INSTRUCTION_THRESHOLD=0  # Maximum allowed atomic instructions
```

**Running standalone:**
```bash
./validation/atomic_audit.sh \
    --binary ./build/ranvier_server \
    --threshold 0 \
    --verbose
```

**Runtime analysis:**
```bash
# Analyze a running process
./validation/atomic_audit.sh --pid $(pgrep ranvier)
```

## Reports

Reports are generated in `validation/reports/`:

```
validation/reports/
├── validation_v1_20240115_120000.json   # Main validation report
└── logs/
    ├── stall_watchdog_*.log             # Stall detection logs
    ├── disk_stress_baseline.txt         # Baseline latency
    ├── disk_stress_under_load.txt       # Stressed latency
    ├── gossip_storm_report.json         # Gossip test results
    ├── radix_tree_disasm.txt            # RadixTree disassembly
    └── atomic_audit_report.txt          # Atomic instruction report
```

### JSON Report Format

```json
{
  "suite_name": "Production Readiness v1.0",
  "version": "1.0.0",
  "overall_status": "PASS",
  "summary": {
    "total": 4,
    "passed": 4,
    "failed": 0,
    "skipped": 0
  },
  "thresholds": {
    "stall_threshold": 0,
    "latency_degradation_percent": 10,
    "smp_queue_overflow": 1000,
    "atomic_instructions": 0
  },
  "tests": { ... }
}
```

## CI/CD Integration

### GitHub Actions

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

      - name: Run validation
        run: make validate-ci

      - name: Upload results
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: validation-results
          path: validation/reports/
```

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | All tests passed |
| 1 | One or more tests failed |
| 2 | Setup/configuration error |

## Prerequisites

### Required
- Ranvier binary (build with `make build`)
- `curl`, `nc`, `awk`, `grep`, `bc`

### Recommended
- `wrk` - High-performance HTTP benchmarking
- `stress-ng` - Disk stress testing
- `objdump` - Binary disassembly
- `perf` - Runtime profiling

### Installation (Ubuntu/Debian)

```bash
# Required
sudo apt-get install curl netcat-openbsd gawk bc

# Recommended
sudo apt-get install wrk stress-ng binutils linux-tools-common

# Python (for gossip storm)
sudo apt-get install python3
pip3 install scapy  # Optional, for IP spoofing
```

## Architecture Background

### Why These Tests Matter

Seastar provides a shared-nothing architecture where each CPU core runs an
independent reactor. These tests verify Ranvier properly leverages this:

1. **Reactor Stalls** - Any blocking call (sync disk I/O, mutex wait) stalls
   all connections on that core.

2. **Disk Decoupling** - SQLite must be queued asynchronously via
   `AsyncPersistenceManager` to prevent WAL writes from blocking.

3. **SMP Pressure** - Cross-core communication via SMP queues is expensive.
   Gossip handling should minimize cross-core messaging.

4. **Atomic-Free** - `shared_ptr` uses atomic reference counting which causes
   cache line bouncing. `unique_ptr` with explicit ownership is preferred.

### Related Documentation

- [Architecture Overview](architecture.md)
- [Async Persistence](internals/async-persistence.md)
- [Gossip Protocol](internals/gossip-protocol.md)
- [RadixTree Implementation](internals/radix-tree.md)
