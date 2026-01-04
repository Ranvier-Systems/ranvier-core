# Gossip Protocol Internals

The gossip protocol provides reliable state synchronization between Ranvier nodes in a cluster. This document covers wire formats, reliability mechanisms, and debugging.

## Overview

Ranvier uses a custom UDP-based gossip protocol to propagate route announcements between cluster nodes. The protocol is designed for:

- **Low latency**: UDP avoids TCP connection overhead
- **Reliability**: ACK-based delivery with retries handles packet loss
- **Duplicate suppression**: Sliding window filters retransmissions
- **Simplicity**: Fire-and-forget semantics with best-effort ordering
- **Split-brain detection**: Quorum-based health checks prevent divergent state

## Packet Types

| Type | Value | Description |
|------|-------|-------------|
| `ROUTE_ANNOUNCEMENT` | `0x01` | New route learned (tokens → backend mapping) |
| `HEARTBEAT` | `0x02` | Keep-alive for peer liveness detection |
| `ROUTE_ACK` | `0x03` | Acknowledgment for route announcement |

## Wire Formats

All multi-byte integers are encoded **big-endian**.

### Route Announcement (v2)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     Type      |    Version    |          Sequence Number      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      Sequence Number (cont)   |          Backend ID           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|        Backend ID (cont)      |         Token Count           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Token[0]                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Token[1]                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           ...                                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| Type | 0 | 1 byte | `0x01` (ROUTE_ANNOUNCEMENT) |
| Version | 1 | 1 byte | Protocol version (`0x02`) |
| Sequence Number | 2 | 4 bytes | Per-peer monotonic sequence |
| Backend ID | 6 | 4 bytes | Target backend identifier |
| Token Count | 10 | 2 bytes | Number of tokens (max 256) |
| Tokens | 12 | 4×N bytes | Token IDs (GPT-2 encoded) |

**Header size**: 12 bytes
**Max packet size**: 12 + (256 × 4) = 1036 bytes

### Route ACK

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     Type      |    Version    |          Sequence Number      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      Sequence Number (cont)   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| Type | 0 | 1 byte | `0x03` (ROUTE_ACK) |
| Version | 1 | 1 byte | Protocol version (`0x02`) |
| Sequence Number | 2 | 4 bytes | Sequence being acknowledged |

**Packet size**: 6 bytes (fixed)

### Heartbeat

```
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     Type      |    Version    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Field | Offset | Size | Description |
|-------|--------|------|-------------|
| Type | 0 | 1 byte | `0x02` (HEARTBEAT) |
| Version | 1 | 1 byte | Protocol version (`0x02`) |

**Packet size**: 2 bytes (fixed)

## Reliable Delivery

### Sequence Numbers

Each node maintains a **per-peer sequence counter** for outbound route announcements:

- Starts at 1 (never 0)
- Monotonically increasing per peer
- Wraps at 2^32 (handled by sliding window)

### ACK Mechanism

1. **Sender** broadcasts route announcement with unique sequence number
2. **Receiver** sends ACK with same sequence number
3. **Sender** removes pending entry when ACK received

```
Node A                              Node B
   |                                   |
   |  ROUTE_ANNOUNCEMENT (seq=1)       |
   |---------------------------------->|
   |                                   |
   |           ROUTE_ACK (seq=1)       |
   |<----------------------------------|
   |                                   |
```

### Retry Logic

If no ACK received within timeout:

1. **First retry** at `ack_timeout` (default: 100ms)
2. **Exponential backoff**: 100ms → 200ms → 400ms → 800ms (capped at 8× base)
3. **Max retries**: 3 (default), then log warning and drop

```
Time (ms)   Event
---------   -----
0           Send ROUTE_ANNOUNCEMENT (seq=1)
100         No ACK → Retry #1
300         No ACK → Retry #2 (200ms backoff)
700         No ACK → Retry #3 (400ms backoff)
1500        No ACK → Max retries exceeded, drop
```

### Duplicate Suppression

Receivers maintain a **sliding window** of recently seen sequence numbers per peer:

- Window size: 1000 (configurable via `gossip_dedup_window`)
- Uses both a `deque` (for ordering) and `unordered_set` (for O(1) lookup)
- When window full, oldest entries are evicted

**Why still ACK duplicates?** The sender may have missed our previous ACK, so we always respond to prevent unnecessary retries.

## Split-Brain Detection

Ranvier implements quorum-based split-brain detection to prevent divergent state when a network partition occurs.

### Quorum State

The cluster operates in one of two states:

| State | Condition | Behavior |
|-------|-----------|----------|
| `HEALTHY` | ≥ quorum peers reachable | Full operations (read + write routes) |
| `DEGRADED` | < quorum peers reachable | Read-only mode (serve existing routes, reject new writes) |

### Quorum Calculation

Quorum required = `floor(N × threshold) + 1`, capped at N

Where:
- **N** = total nodes (peers + self)
- **threshold** = configurable (default: 0.5 for majority)

Examples with default threshold (0.5):

| Cluster Size | Quorum Required | Can Survive |
|--------------|-----------------|-------------|
| 1 node | 1 | 0 failures |
| 2 nodes | 2 | 0 failures |
| 3 nodes | 2 | 1 failure |
| 5 nodes | 3 | 2 failures |
| 7 nodes | 4 | 3 failures |

### Warning Threshold

When the cluster is healthy but approaching quorum loss, a warning is logged. The `quorum_warning_threshold` controls how many nodes above quorum triggers the warning.

```
alive_nodes = 4, required = 3, warning_threshold = 1
margin = 4 - 3 = 1
margin(1) <= threshold(1) → WARNING logged
```

Warnings are rate-limited: only logged when entering/exiting the warning zone, not on every liveness check.

### Degraded Mode Behavior

When quorum is lost:

1. **Route writes rejected**: `broadcast_route()` returns immediately without sending
2. **Existing routes served**: Cached routes remain valid for incoming requests
3. **Metric updated**: `ranvier_cluster_quorum_state` gauge set to 0
4. **Log emitted**: `WARN: Cluster quorum LOST`

When quorum is restored:

1. **Route writes enabled**: Normal gossip propagation resumes
2. **Metric updated**: Gauge set to 1
3. **Log emitted**: `INFO: Cluster quorum RESTORED`

### Partition Scenarios

**Scenario 1: 3-node cluster, 1 node isolated**
```
[A]---[B]---[C]    →    [A]   [B]---[C]

Partition: A loses connectivity to B and C

Node A: alive=1, required=2 → DEGRADED (read-only)
Node B: alive=2, required=2 → HEALTHY
Node C: alive=2, required=2 → HEALTHY
```

**Scenario 2: 3-node cluster, network split**
```
[A]---[B]---[C]    →    [A]   [B]   [C]

Partition: All nodes isolated

Node A: alive=1, required=2 → DEGRADED
Node B: alive=1, required=2 → DEGRADED
Node C: alive=1, required=2 → DEGRADED
```

All nodes enter read-only mode, preventing divergent writes.

## Configuration

### YAML

```yaml
cluster:
  enabled: true
  gossip_port: 9190
  peers:
    - "10.0.0.2:9190"
    - "10.0.0.3:9190"

  # Timing
  gossip_heartbeat_interval_seconds: 5
  gossip_peer_timeout_seconds: 15

  # Reliable delivery
  gossip_reliable_delivery: true      # Enable ACKs/retries
  gossip_ack_timeout_ms: 100          # Initial retry timeout
  gossip_max_retries: 3               # Max retry attempts
  gossip_dedup_window: 1000           # Duplicate detection window

  # Split-brain detection
  quorum_enabled: true                # Enable quorum checks
  quorum_threshold: 0.5               # Fraction for quorum (0.5 = majority)
  reject_routes_on_quorum_loss: true  # Reject writes when degraded
  quorum_warning_threshold: 1         # Warn when margin <= this (0 disables)
```

### Environment Variables

```bash
RANVIER_CLUSTER_ENABLED=true
RANVIER_CLUSTER_GOSSIP_PORT=9190
RANVIER_CLUSTER_PEERS="10.0.0.2:9190,10.0.0.3:9190"

# Reliable delivery
RANVIER_CLUSTER_GOSSIP_RELIABLE_DELIVERY=true
RANVIER_CLUSTER_GOSSIP_ACK_TIMEOUT_MS=100
RANVIER_CLUSTER_GOSSIP_MAX_RETRIES=3
RANVIER_CLUSTER_GOSSIP_DEDUP_WINDOW=1000

# Split-brain detection
RANVIER_CLUSTER_QUORUM_ENABLED=true
RANVIER_CLUSTER_QUORUM_THRESHOLD=0.5
RANVIER_CLUSTER_REJECT_ROUTES_ON_QUORUM_LOSS=true
RANVIER_CLUSTER_QUORUM_WARNING_THRESHOLD=1
```

## Metrics

All metrics are exposed via Prometheus on port 9180.

### Packet Counters

| Metric | Description |
|--------|-------------|
| `ranvier_router_cluster_sync_sent` | Route announcements sent |
| `ranvier_router_cluster_sync_received` | Valid route announcements received |
| `ranvier_router_cluster_sync_invalid` | Malformed packets dropped |
| `ranvier_router_cluster_sync_untrusted` | Packets from unknown peers dropped |

### Reliable Delivery Counters

| Metric | Description |
|--------|-------------|
| `ranvier_cluster_acks_sent` | ACKs sent |
| `ranvier_cluster_acks_received` | ACKs received |
| `ranvier_cluster_retries_sent` | Retry transmissions |
| `ranvier_cluster_duplicates_suppressed` | Duplicate announcements filtered |
| `ranvier_cluster_max_retries_exceeded` | Announcements dropped after max retries |

### Peer Health

| Metric | Description |
|--------|-------------|
| `ranvier_cluster_peers_alive` | Number of peers currently alive |
| `ranvier_cluster_dns_discovery_success` | Successful DNS refreshes |
| `ranvier_cluster_dns_discovery_failure` | Failed DNS refreshes |

### Split-Brain Detection

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_cluster_quorum_state` | Gauge | Current quorum state (1=healthy, 0=degraded) |
| `ranvier_cluster_quorum_transitions` | Counter | Total state transitions (healthy↔degraded) |
| `ranvier_cluster_routes_rejected_degraded` | Counter | Routes rejected due to degraded state |

## Debugging

### Symptoms and Metrics

| Symptom | Metrics to Check | Likely Cause |
|---------|------------------|--------------|
| Route divergence | `max_retries_exceeded` high | Network partition or peer overload |
| High retries | `retries_sent` >> `acks_received` | Packet loss or slow peer |
| Duplicate processing | `duplicates_suppressed` = 0 | `dedup_window` too small |
| Routes rejected | `routes_rejected_degraded` > 0 | Cluster in degraded mode (quorum lost) |
| Quorum flapping | `quorum_transitions` increasing | Unstable network or peer health |

### Log Levels

```bash
# Enable trace logging for gossip
SEASTAR_LOGGER_LEVELS="ranvier.gossip=trace"
```

**Trace logs** include:
- Every ACK sent/received with sequence number
- Duplicate detection events
- Retry attempts with backoff timing

### Packet Capture

```bash
# Capture gossip traffic on port 9190
tcpdump -i any -n udp port 9190 -X
```

Example route announcement (12-byte header + 4 tokens):
```
0x0000:  01 02 00 00 00 01 00 00  00 2a 00 04 ...
         ^type ^ver ^---seq---^  ^--backend^ ^count
```

## Protocol Evolution

### Version History

| Version | Changes |
|---------|---------|
| 1 | Initial: 8-byte header, no sequence numbers |
| 2 | Added 4-byte sequence number, ACK packet type |

### Compatibility

- Nodes reject packets with mismatched protocol version
- Rolling upgrades require brief period of split-brain (old nodes ignore v2 packets)
- Recommended: upgrade all nodes simultaneously or use blue-green deployment

## Implementation Notes

### Thread Safety

- GossipService runs only on **Shard 0**
- Received routes broadcast to all shards via `smp::submit_to()`
- No locks required (Seastar's shared-nothing architecture)
- **Quorum state accessors** (`quorum_state()`, `has_quorum()`, `is_degraded()`) only return valid data on shard 0; use `submit_to(0, ...)` to query from other shards

### Memory Usage

Per peer:
- Sequence counter: 4 bytes
- Pending ACKs: ~1KB per outstanding announcement
- Dedup window: ~4KB for default 1000-entry window

Total overhead: ~6KB per peer (negligible for typical 3-node clusters)

## Source Files

| File | Purpose |
|------|---------|
| `src/gossip_service.hpp` | Packet structs, GossipService class, QuorumState enum |
| `src/gossip_service.cpp` | UDP send/receive, reliability logic, quorum tracking |
| `src/config.hpp` | ClusterConfig with reliability and quorum settings |
| `tests/unit/quorum_test.cpp` | Unit tests for quorum calculation and state transitions |
