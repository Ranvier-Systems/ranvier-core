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

## DTLS Encryption

Gossip traffic can be encrypted using DTLS 1.2 (Datagram TLS) for secure cluster communication. This provides mutual TLS authentication and encryption over UDP.

### Enabling DTLS

```yaml
cluster:
  tls:
    enabled: true
    cert_file: /etc/ranvier/cluster.crt
    key_file: /etc/ranvier/cluster.key
    ca_file: /etc/ranvier/ca.crt
    verify_peer: true  # Enable mTLS
```

### Handshake Flow

```
Node A                              Node B
   |                                   |
   |  ClientHello                      |
   |---------------------------------->|
   |                                   |
   |  ServerHello, Certificate,        |
   |  CertificateRequest, ServerDone   |
   |<----------------------------------|
   |                                   |
   |  Certificate, ClientKeyExchange,  |
   |  CertificateVerify, Finished      |
   |---------------------------------->|
   |                                   |
   |  Finished                         |
   |<----------------------------------|
   |                                   |
   |  [Encrypted gossip traffic]       |
   |<=================================>|
```

### Reactor Stall Prevention

DTLS encryption uses OpenSSL which performs CPU-intensive operations. To prevent these from blocking Seastar's reactor thread (causing stalls that delay all other network I/O), the gossip service implements adaptive offloading via the `CryptoOffloader` class.

#### CryptoOffloader

The `CryptoOffloader` class (`src/crypto_offloader.hpp`) provides intelligent routing of crypto operations:

- **Symmetric operations** (AES-GCM encrypt/decrypt): Fast (~1-50μs), run inline on reactor
- **Asymmetric operations** (RSA/ECDH handshakes): Slow (~1-10ms), offloaded to `seastar::async`

The offloader uses **latency estimation** to predict operation cost before execution:

```cpp
// Estimated latency in microseconds
SYMMETRIC: 5 + (data_size / 1024)     // ~1μs per KB
HANDSHAKE_INITIATE: 2000              // RSA/ECDHE key generation
HANDSHAKE_CONTINUE: 500               // Signature verification
UNKNOWN: 10 + (data_size * 10 / 1024) // Conservative estimate
```

#### Configuration

| Config | Default | Description |
|--------|---------|-------------|
| `size_threshold_bytes` | 1024 | Symmetric ops on data larger than this may be offloaded |
| `stall_threshold_us` | 500 | Operations exceeding this trigger stall warnings |
| `offload_latency_threshold_us` | 100 | Estimated latency above this triggers offloading |
| `max_queue_depth` | 1024 | Maximum in-flight async operations |
| `symmetric_always_inline` | true | Always run small symmetric ops inline |
| `handshake_always_offload` | true | Always offload handshakes |

#### Offloading Decision Flow

```
1. Is offloader disabled? → Run inline
2. Is force_offload enabled? → Offload
3. Is it a handshake? → Offload (if handshake_always_offload)
4. Is it symmetric + small data? → Run inline (if symmetric_always_inline)
5. Estimate latency → Offload if > offload_latency_threshold_us
```

#### Typical Timing

| Operation | Typical Time | Path |
|-----------|--------------|------|
| Heartbeat encrypt (2 bytes) | ~5μs | Inline |
| Small route encrypt (52 bytes) | ~10μs | Inline |
| Large route encrypt (1036 bytes) | ~50-80μs | Inline (below latency threshold) |
| DTLS handshake initiate | ~1-2ms | Offloaded (seastar::async) |
| DTLS handshake continue | ~0.5ms | Offloaded (seastar::async) |

#### Queue Depth Limiting

When the async queue is full (`max_queue_depth` reached), operations fall back to inline execution with a warning logged. This prevents unbounded memory growth during crypto operation bursts.

### Certificate Hot-Reload

Certificates can be reloaded without restart:

```yaml
cluster:
  tls:
    cert_reload_interval_seconds: 3600  # Check hourly
```

When certificates change:
1. New certs loaded into DTLS context
2. Existing sessions invalidated
3. Handshakes re-initiated with all peers (parallel, gated)

### Session Management

- Sessions cached per peer address
- Idle sessions cleaned up after 5 minutes
- Graceful shutdown waits for in-flight handshakes via `seastar::gate`

### mTLS Lockdown Mode

When `mtls_enabled` is set to `true`, the gossip service enforces strict DTLS-only communication. **Any packet that is not a valid DTLS packet from an established session is silently dropped.**

```yaml
cluster:
  tls:
    enabled: true
    verify_peer: true
  mtls_enabled: true  # Enforce DTLS-only communication
```

**Packet Filtering Logic:**

When a packet arrives and `mtls_enabled` is true:

1. **Check if DTLS handshake packet** (content type 20/21/22, version 0xFE):
   - If yes → Allow through (needed to establish sessions)
2. **Check if packet belongs to established DTLS session**:
   - If yes → Allow through (will be decrypted)
3. **Otherwise** → Drop silently, increment `dtls_lockdown_drops` counter

**Allowed Content Types:**
| Type | Name | Purpose |
|------|------|---------|
| 20 | ChangeCipherSpec | Part of handshake |
| 21 | Alert | Handshake error handling |
| 22 | Handshake | Actual handshake messages |
| 23 | ApplicationData | Encrypted gossip (only after session established) |

**Automatic Handshake Initiation:**

When new peers are discovered via DNS, DTLS handshakes are automatically initiated **before** any routing data is exchanged:

```cpp
void refresh_peers() {
    // ... discover new peers via DNS ...

    for (const auto& peer : new_peers_for_handshake) {
        // Initiate handshake before any route exchange
        initiate_peer_handshake(peer);
    }
}
```

This ensures encrypted channels are established with all peers before sensitive routing data is transmitted.

**Security Benefits:**
- Prevents plaintext gossip traffic even if misconfigured
- Ensures all route announcements are authenticated
- Blocks rogue nodes from injecting routes without valid certificates

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

### Replay Attack Prevention (Sequence Number Hardening)

The sliding window provides protection against **replay attacks** where an attacker captures old routing messages and re-sends them to overwrite newer routes:

**Security Properties:**
1. **Duplicate detection**: Prevents processing the same message twice
2. **Replay attack prevention**: Old captured packets are rejected
3. **Memory-bounded**: Window size limits memory usage while maintaining security

**Critical Behavior:** The sliding window **persists across resync events**. When the gossip service enters resync mode (e.g., during network partition recovery), the sequence number windows are **NOT cleared**. This ensures attackers cannot exploit the resync to replay old captured packets.

```cpp
void start_resync() {
    // SECURITY: DO NOT clear _received_seq_windows!
    // Clearing would allow replay attacks after network recovery.
    _pending_acks.clear();  // Only clear pending outbound acks
}
```

**Window Sizing:** The window should be large enough to cover:
- Maximum expected in-flight messages during normal operation
- Any network delays that might cause out-of-order delivery

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

### Recently-Seen Quorum Check

In addition to the basic alive/dead peer tracking, Ranvier uses a **stricter quorum check** that counts only peers seen within a configurable time window:

```yaml
cluster:
  quorum_check_window_seconds: 30  # Count peers seen within this window
```

**Why Recently-Seen?**

The alive/dead state can lag behind actual network conditions:
- A peer marked "alive" may not have communicated for several heartbeat intervals
- During network partitions, the alive flag may not reflect current reachability

The `check_quorum()` method counts peers based on `last_seen` timestamp:

```
recently_seen = count of peers where (now - last_seen) <= quorum_check_window
quorum_nodes = recently_seen + 1 (self)
state = (quorum_nodes >= required) ? HEALTHY : DEGRADED
```

**Example:**
```
Cluster: 5 nodes, threshold=0.5, quorum_check_window=30s

Peer A: last_seen 5s ago  ✓ (recently seen)
Peer B: last_seen 10s ago ✓ (recently seen)
Peer C: last_seen 40s ago ✗ (stale)
Peer D: last_seen 60s ago ✗ (stale)

recently_seen = 2, total = 5, required = 3
quorum_nodes = 2 + 1 (self) = 3 >= 3 → HEALTHY
```

This is more conservative than just checking alive flags and provides faster detection of network issues.

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

1. **Outbound route writes rejected**: `broadcast_route()` returns immediately without sending
2. **Incoming routes rejected**: Route announcements from peers are ACKed but not applied
3. **Existing routes served**: Cached routes remain valid for incoming requests
4. **Metric updated**: `ranvier_cluster_quorum_state` gauge set to 0
5. **Log emitted**: `ERROR: QUORUM LOST: Cluster entering DEGRADED mode`

**Why reject incoming routes?**

When a node loses quorum, it may be on the wrong side of a network partition. Accepting routes from the few peers it can still reach could lead to:
- Accepting stale routes from similarly partitioned nodes
- Diverging from the majority partition's routing state
- Potential route hijacking if an attacker controls the minority partition

When quorum is restored:

1. **Route writes enabled**: Normal gossip propagation resumes
2. **Incoming routes accepted**: Route announcements applied to RadixTree
3. **Metric updated**: Gauge set to 1
4. **Log emitted**: `INFO: QUORUM RESTORED: Cluster returning to HEALTHY mode`

### Fail-Open Mode (Optional)

By default, degraded clusters reject route operations (fail-closed). However, for scenarios where **availability is prioritized over consistency**, you can enable **fail-open mode** via `fail_open_on_quorum_loss: true`.

```yaml
cluster:
  fail_open_on_quorum_loss: true  # Enable fail-open during split-brain
```

**Behavior with fail-open enabled:**

| Operation | Default (fail-closed) | With fail_open_on_quorum_loss=true |
|-----------|----------------------|-----------------------------------|
| Outbound route writes | Rejected | Allowed (random routing to healthy backends) |
| Incoming gossip routes | Rejected (ACKed but not applied) | Accepted |
| Routing mode | Prefix-affinity | Random (load-balanced across healthy backends) |

**When to use fail-open:**

- **Availability-critical deployments**: Where serving some requests incorrectly is better than serving none
- **Single-node testing**: When running development clusters where quorum loss is expected
- **Edge deployments**: Where network partitions are common but service must continue

**Trade-offs:**

| Mode | Advantage | Risk |
|------|-----------|------|
| Fail-closed (default) | Prevents divergent state, no stale routes | Complete unavailability during split-brain |
| Fail-open | Continues serving during partitions | May route requests to suboptimal backends, possible stale data |

**Metrics for monitoring fail-open:**

| Metric | Description |
|--------|-------------|
| `ranvier_cluster_routes_allowed_fail_open` | Route broadcasts allowed due to fail-open mode |
| `ranvier_cluster_gossip_accepted_fail_open` | Incoming gossip accepted due to fail-open mode |

**Routing behavior during fail-open:**

When `fail_open_on_quorum_loss=true` and the cluster enters DEGRADED mode:

1. `RouterService::route_request()` detects fail-open mode via `GossipService::is_fail_open_mode()`
2. Instead of prefix-affinity routing, routes to a **random healthy backend**
3. This load-balances requests across available backends without relying on potentially stale routing data

```
┌─────────────────────────────────────────────────────────────────┐
│ Request arrives during split-brain with fail-open enabled       │
├─────────────────────────────────────────────────────────────────┤
│ 1. Check quorum state → DEGRADED                                │
│ 2. Check fail_open_on_quorum_loss → true                        │
│ 3. Skip RadixTree lookup (may have stale data)                  │
│ 4. Select random healthy backend from available pool            │
│ 5. Forward request → may miss KV-cache but service continues    │
└─────────────────────────────────────────────────────────────────┘
```

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
  quorum_enabled: true                    # Enable quorum checks
  quorum_threshold: 0.5                   # Fraction for quorum (0.5 = majority)
  reject_routes_on_quorum_loss: true      # Reject writes when degraded
  fail_open_on_quorum_loss: false         # true = random routing during split-brain
  quorum_warning_threshold: 1             # Warn when margin <= this (0 disables)
  quorum_check_window_seconds: 30         # Window for recently-seen check

  # DTLS security lockdown
  mtls_enabled: false                     # Enforce DTLS-only (drop plaintext packets)
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
RANVIER_CLUSTER_FAIL_OPEN_ON_QUORUM_LOSS=false
RANVIER_CLUSTER_QUORUM_WARNING_THRESHOLD=1
RANVIER_CLUSTER_QUORUM_CHECK_WINDOW=30

# DTLS security lockdown
RANVIER_CLUSTER_MTLS_ENABLED=false
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
| `ranvier_cluster_routes_rejected_degraded` | Counter | Outbound routes rejected due to degraded state |
| `ranvier_cluster_routes_rejected_incoming_degraded` | Counter | Incoming routes rejected due to degraded state |
| `ranvier_cluster_peers_recently_seen` | Gauge | Peers seen within quorum check window |
| `ranvier_cluster_routes_allowed_fail_open` | Counter | Route broadcasts allowed due to fail-open mode |
| `ranvier_cluster_gossip_accepted_fail_open` | Counter | Incoming gossip accepted due to fail-open mode |

### DTLS Encryption

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_cluster_dtls_handshakes_started` | Counter | DTLS handshakes initiated |
| `ranvier_cluster_dtls_handshakes_completed` | Counter | DTLS handshakes completed successfully |
| `ranvier_cluster_dtls_handshakes_failed` | Counter | DTLS handshakes that failed |
| `ranvier_cluster_dtls_packets_encrypted` | Counter | Packets encrypted with DTLS |
| `ranvier_cluster_dtls_packets_decrypted` | Counter | Packets decrypted with DTLS |
| `ranvier_cluster_dtls_cert_reloads` | Counter | Certificate hot-reloads performed |
| `ranvier_cluster_dtls_lockdown_drops` | Counter | Packets dropped due to mTLS lockdown |

### Crypto Performance

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_cluster_crypto_stall_warnings` | Counter | Crypto ops exceeding 100μs stall threshold |
| `ranvier_cluster_crypto_ops_offloaded` | Counter | Crypto ops offloaded to background thread |
| `ranvier_cluster_crypto_batch_broadcasts` | Counter | Broadcasts using batch mode (>10 peers) |

## Debugging

### Symptoms and Metrics

| Symptom | Metrics to Check | Likely Cause |
|---------|------------------|--------------|
| Route divergence | `max_retries_exceeded` high | Network partition or peer overload |
| High retries | `retries_sent` >> `acks_received` | Packet loss or slow peer |
| Duplicate processing | `duplicates_suppressed` = 0 | `dedup_window` too small |
| Routes rejected (outbound) | `routes_rejected_degraded` > 0 | Cluster in degraded mode (quorum lost) |
| Routes rejected (inbound) | `routes_rejected_incoming_degraded` > 0 | Cluster in degraded mode (quorum lost) |
| Quorum flapping | `quorum_transitions` increasing | Unstable network or peer health |
| Stale peer detection | `peers_recently_seen` < `peers_alive` | Heartbeat interval too long or packet loss |
| DTLS handshake failures | `dtls_handshakes_failed` > 0 | Certificate mismatch or expiry |
| Packets dropped (mTLS) | `dtls_lockdown_drops` > 0 | Plaintext packets blocked by mTLS lockdown |
| Crypto stalls | `crypto_stall_warnings` > 0 | CPU overload or slow crypto hardware |
| High offload rate | `crypto_ops_offloaded` high | Many large packets (may indicate token explosion) |

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

### Route Batching (SMP Storm Prevention)

When receiving high volumes of remote route announcements, immediately broadcasting each route to all shards generates excessive cross-core traffic:

**Problem**: With 1000 routes/sec and 64 shards, naive broadcasting creates 64,000 SMP messages/sec, risking Seastar's internal message bus saturation.

**Solution**: Routes are buffered on Shard 0 and flushed in batches:

| Trigger | Condition |
|---------|-----------|
| Timer | Every 10ms (configurable via `RouteBatchConfig::FLUSH_INTERVAL`) |
| Buffer full | When buffer reaches 100 routes (configurable via `RouteBatchConfig::MAX_BATCH_SIZE`) |

**Batch Broadcast Flow**:
1. `learn_route_remote()` pushes route to `_pending_remote_routes` buffer
2. Timer or buffer-full condition triggers `flush_route_batch()`
3. Single `smp::submit_to()` per shard with entire batch (reduces messages from O(routes × shards) to O(shards))
4. Each shard applies batch to local RadixTree via `apply_route_batch_to_local_tree()`

**Performance Impact**:

| Scenario | Without Batching | With Batching | Reduction |
|----------|-----------------|---------------|-----------|
| 1000 routes/sec, 64 shards | 64,000 msg/sec | 640 msg/sec | 99% |
| 100 routes/sec, 16 shards | 1,600 msg/sec | 160 msg/sec | 90% |

**Shutdown Sequence**: `stop_gossip()` cancels the timer, stops GossipService (preventing new routes), then flushes remaining buffered routes to ensure no data loss.

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
| `src/gossip_service.cpp` | UDP send/receive, reliability logic, quorum tracking, DTLS crypto |
| `src/router_service.hpp` | PendingRemoteRoute, RouteBatchConfig, batch buffer |
| `src/router_service.cpp` | Route batching implementation (learn_route_remote, flush_route_batch) |
| `src/dtls_context.hpp` | DtlsContext and DtlsSession classes |
| `src/dtls_context.cpp` | OpenSSL-based DTLS implementation |
| `src/config.hpp` | ClusterConfig with reliability, quorum, and TLS settings |
| `tests/unit/quorum_test.cpp` | Unit tests for quorum calculation and state transitions |
| `tests/unit/gossip_protocol_test.cpp` | Wire format tests and crypto offload threshold tests |
| `tests/unit/route_batching_test.cpp` | Unit tests for route batching behavior |
