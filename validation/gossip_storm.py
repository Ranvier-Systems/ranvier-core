#!/usr/bin/env python3
# =============================================================================
# Ranvier Core Validation Suite - SMP Gossip Storm Generator
# =============================================================================
# Floods the cluster gossip port (UDP) with valid Route Announcement packets
# to test SMP message queue handling under extreme load.
#
# This validates that the shared-nothing architecture properly handles
# high-frequency gossip traffic without SMP queue overflow.
#
# Usage:
#   python3 gossip_storm.py --target 127.0.0.1 --port 9190 --pps 5000 --duration 60
#
# Monitoring:
#   - Watch /proc/net/softnet_stat for dropped packets
#   - Check Prometheus endpoint for SMP metrics
# =============================================================================

import argparse
import ipaddress
import json
import os
import random
import signal
import socket
import struct
import sys
import threading
import time
import urllib.request
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from typing import List, Optional, Tuple

# Try to import scapy for advanced packet crafting (optional)
try:
    from scapy.all import IP, UDP, Raw, send, conf
    SCAPY_AVAILABLE = True
    # Disable scapy verbosity
    conf.verb = 0
except ImportError:
    SCAPY_AVAILABLE = False


# =============================================================================
# Gossip Protocol Constants (matching gossip_service.hpp)
# =============================================================================
GOSSIP_PACKET_TYPE_ROUTE_ANNOUNCEMENT = 0x01
GOSSIP_PACKET_TYPE_HEARTBEAT = 0x02
GOSSIP_PACKET_TYPE_ROUTE_ACK = 0x03

PROTOCOL_VERSION = 2
HEADER_SIZE = 12  # type(1) + version(1) + seq_num(4) + backend_id(4) + token_count(2)
MAX_TOKENS = 256
MAX_PACKET_SIZE = HEADER_SIZE + (MAX_TOKENS * 4)


# =============================================================================
# Statistics Tracking
# =============================================================================
@dataclass
class StormStats:
    """Statistics for the gossip storm test."""
    packets_sent: int = 0
    bytes_sent: int = 0
    send_errors: int = 0
    start_time: float = 0.0
    end_time: float = 0.0
    target_pps: int = 0
    actual_pps: float = 0.0
    softnet_drops_before: int = 0
    softnet_drops_after: int = 0
    smp_queue_max: int = 0
    prometheus_samples: List[dict] = field(default_factory=list)


# =============================================================================
# Packet Generation
# =============================================================================
def serialize_route_announcement(
    backend_id: int,
    tokens: List[int],
    seq_num: int
) -> bytes:
    """
    Serialize a RouteAnnouncementPacket in wire format.

    Wire format (big-endian):
        [type:1][version:1][seq_num:4][backend_id:4][token_count:2][tokens:4*N]
    """
    token_count = min(len(tokens), MAX_TOKENS)

    # Build header
    header = struct.pack(
        '>BBIHH',
        GOSSIP_PACKET_TYPE_ROUTE_ANNOUNCEMENT,  # type (1 byte)
        PROTOCOL_VERSION,                        # version (1 byte)
        seq_num,                                 # seq_num (4 bytes, big-endian)
        backend_id,                              # backend_id (4 bytes, big-endian)
        token_count                              # token_count (2 bytes, big-endian)
    )

    # Note: struct format for header is: B=1byte, B=1byte, I=4bytes, H=2bytes
    # But we need seq_num as 4 bytes and backend_id as 4 bytes
    # Let's fix the format string
    header = struct.pack('>BB', GOSSIP_PACKET_TYPE_ROUTE_ANNOUNCEMENT, PROTOCOL_VERSION)
    header += struct.pack('>I', seq_num)      # 4 bytes big-endian
    header += struct.pack('>I', backend_id)   # 4 bytes big-endian
    header += struct.pack('>H', token_count)  # 2 bytes big-endian

    # Build tokens (each 4 bytes, big-endian)
    token_data = b''.join(struct.pack('>I', t) for t in tokens[:token_count])

    return header + token_data


def generate_random_packet(seq_num: int) -> bytes:
    """Generate a random but valid route announcement packet."""
    backend_id = random.randint(1, 1000)
    token_count = random.randint(4, 64)  # Realistic token prefix lengths
    tokens = [random.randint(0, 50000) for _ in range(token_count)]

    return serialize_route_announcement(backend_id, tokens, seq_num)


def generate_diverse_source_ips(count: int, base_network: str = "10.0.0.0/8") -> List[str]:
    """Generate diverse source IP addresses for spoofed packets."""
    network = ipaddress.ip_network(base_network)
    ips = []

    for _ in range(count):
        # Generate random IP within the network
        random_offset = random.randint(1, network.num_addresses - 2)
        ip = network.network_address + random_offset
        ips.append(str(ip))

    return ips


# =============================================================================
# Monitoring Functions
# =============================================================================
def read_softnet_stats() -> Tuple[int, int, int]:
    """
    Read /proc/net/softnet_stat for packet drop information.

    Returns:
        Tuple of (processed, dropped, time_squeeze) totals across all CPUs
    """
    total_processed = 0
    total_dropped = 0
    total_squeeze = 0

    try:
        with open('/proc/net/softnet_stat', 'r') as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 3:
                    # Values are in hex
                    total_processed += int(parts[0], 16)
                    total_dropped += int(parts[1], 16)
                    total_squeeze += int(parts[2], 16)
    except (FileNotFoundError, PermissionError):
        pass

    return total_processed, total_dropped, total_squeeze


def fetch_prometheus_metrics(endpoint: str) -> dict:
    """
    Fetch and parse Prometheus metrics from Ranvier.

    Returns dict with relevant SMP and gossip metrics.
    """
    metrics = {}

    try:
        with urllib.request.urlopen(endpoint, timeout=5) as response:
            content = response.read().decode('utf-8')

            for line in content.split('\n'):
                if line.startswith('#') or not line.strip():
                    continue

                # Parse metric name and value
                try:
                    parts = line.split()
                    if len(parts) >= 2:
                        name = parts[0].split('{')[0]  # Remove labels
                        value = float(parts[-1])

                        # Collect relevant metrics
                        if any(k in name.lower() for k in ['smp', 'gossip', 'queue', 'reactor']):
                            metrics[name] = value
                except (ValueError, IndexError):
                    continue
    except Exception as e:
        metrics['_fetch_error'] = str(e)

    return metrics


def monitor_metrics_thread(
    stats: StormStats,
    prometheus_endpoint: str,
    interval: float,
    stop_event: threading.Event
):
    """Background thread to periodically collect metrics."""
    while not stop_event.is_set():
        try:
            metrics = fetch_prometheus_metrics(prometheus_endpoint)
            metrics['_timestamp'] = time.time()
            stats.prometheus_samples.append(metrics)

            # Track max SMP queue depth if available
            for name, value in metrics.items():
                if 'queue' in name.lower() and 'depth' in name.lower():
                    stats.smp_queue_max = max(stats.smp_queue_max, int(value))
        except Exception:
            pass

        stop_event.wait(interval)


# =============================================================================
# Storm Generation Methods
# =============================================================================
class GossipStormGenerator:
    """Generates high-frequency gossip traffic for stress testing."""

    def __init__(
        self,
        target_host: str,
        target_port: int,
        packets_per_second: int,
        duration_seconds: int,
        use_scapy: bool = False,
        source_ip_count: int = 100
    ):
        self.target_host = target_host
        self.target_port = target_port
        self.pps = packets_per_second
        self.duration = duration_seconds
        self.use_scapy = use_scapy and SCAPY_AVAILABLE
        self.source_ips = generate_diverse_source_ips(source_ip_count)

        self.stats = StormStats(target_pps=packets_per_second)
        self.running = False
        self.stop_event = threading.Event()
        self.seq_num = 0

    def _send_packet_socket(self, sock: socket.socket, packet: bytes):
        """Send packet using standard socket (no IP spoofing)."""
        try:
            sock.sendto(packet, (self.target_host, self.target_port))
            self.stats.packets_sent += 1
            self.stats.bytes_sent += len(packet)
        except OSError as e:
            self.stats.send_errors += 1

    def _send_packet_scapy(self, packet: bytes, source_ip: str):
        """Send packet using scapy with spoofed source IP."""
        try:
            pkt = IP(src=source_ip, dst=self.target_host) / \
                  UDP(sport=random.randint(1024, 65535), dport=self.target_port) / \
                  Raw(load=packet)
            send(pkt, verbose=0)
            self.stats.packets_sent += 1
            self.stats.bytes_sent += len(packet)
        except Exception:
            self.stats.send_errors += 1

    def _worker_thread(self, worker_id: int, packets_per_worker: int):
        """Worker thread that sends packets at controlled rate."""
        sock = None
        if not self.use_scapy:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setblocking(False)

        # Calculate target interval between packets
        interval = 1.0 / (self.pps / 4)  # Assuming 4 workers

        try:
            while self.running and self.stats.packets_sent < packets_per_worker * 4:
                start = time.perf_counter()

                # Generate packet
                self.seq_num += 1
                packet = generate_random_packet(self.seq_num)

                # Send
                if self.use_scapy:
                    source_ip = random.choice(self.source_ips)
                    self._send_packet_scapy(packet, source_ip)
                else:
                    self._send_packet_socket(sock, packet)

                # Rate limiting - sleep for remaining interval
                elapsed = time.perf_counter() - start
                sleep_time = interval - elapsed
                if sleep_time > 0:
                    time.sleep(sleep_time)

        finally:
            if sock:
                sock.close()

    def run_socket_burst(self):
        """High-performance socket-based burst sending."""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Pre-generate packets for burst
        burst_size = 100
        packets = [generate_random_packet(i) for i in range(burst_size)]

        target = (self.target_host, self.target_port)
        end_time = time.time() + self.duration
        batch_interval = burst_size / self.pps

        try:
            while time.time() < end_time and self.running:
                batch_start = time.perf_counter()

                # Send burst of packets
                for packet in packets:
                    try:
                        sock.sendto(packet, target)
                        self.stats.packets_sent += 1
                        self.stats.bytes_sent += len(packet)
                    except OSError:
                        self.stats.send_errors += 1

                # Regenerate some packets for diversity
                for i in range(10):
                    self.seq_num += 1
                    packets[random.randint(0, burst_size - 1)] = generate_random_packet(self.seq_num)

                # Rate control
                elapsed = time.perf_counter() - batch_start
                sleep_time = batch_interval - elapsed
                if sleep_time > 0:
                    time.sleep(sleep_time)

        finally:
            sock.close()

    def run(self, prometheus_endpoint: Optional[str] = None) -> StormStats:
        """
        Run the gossip storm test.

        Args:
            prometheus_endpoint: Optional Prometheus endpoint for metrics collection

        Returns:
            StormStats with test results
        """
        self.running = True
        self.stats.start_time = time.time()

        # Record initial softnet stats
        _, self.stats.softnet_drops_before, _ = read_softnet_stats()

        # Start metrics monitoring thread
        monitor_thread = None
        if prometheus_endpoint:
            monitor_thread = threading.Thread(
                target=monitor_metrics_thread,
                args=(self.stats, prometheus_endpoint, 1.0, self.stop_event),
                daemon=True
            )
            monitor_thread.start()

        print(f"Starting gossip storm: {self.pps} PPS for {self.duration}s")
        print(f"Target: {self.target_host}:{self.target_port}")
        print(f"Method: {'scapy (IP spoofing)' if self.use_scapy else 'socket (burst)'}")

        # Run the storm
        try:
            self.run_socket_burst()
        except KeyboardInterrupt:
            print("\nInterrupted")
        finally:
            self.running = False
            self.stop_event.set()

        # Record final stats
        self.stats.end_time = time.time()
        _, self.stats.softnet_drops_after, _ = read_softnet_stats()

        # Calculate actual PPS
        duration = self.stats.end_time - self.stats.start_time
        if duration > 0:
            self.stats.actual_pps = self.stats.packets_sent / duration

        # Wait for monitoring thread
        if monitor_thread:
            monitor_thread.join(timeout=2)

        return self.stats


# =============================================================================
# SMP Queue Monitoring
# =============================================================================
def check_smp_queue_overflow(stats: StormStats, threshold: int) -> Tuple[bool, str]:
    """
    Check if SMP queue overflow occurred.

    Returns:
        Tuple of (passed, message)
    """
    # Check softnet drops
    drops = stats.softnet_drops_after - stats.softnet_drops_before

    messages = []
    passed = True

    if drops > threshold:
        passed = False
        messages.append(f"Softnet drops: {drops} (threshold: {threshold})")
    else:
        messages.append(f"Softnet drops: {drops} (OK)")

    # Check SMP queue depth from Prometheus
    if stats.smp_queue_max > threshold:
        passed = False
        messages.append(f"Max SMP queue depth: {stats.smp_queue_max} (threshold: {threshold})")
    elif stats.smp_queue_max > 0:
        messages.append(f"Max SMP queue depth: {stats.smp_queue_max} (OK)")

    return passed, '\n'.join(messages)


# =============================================================================
# Report Generation
# =============================================================================
def generate_report(stats: StormStats, passed: bool) -> dict:
    """Generate JSON report of test results."""
    return {
        "test_name": "SMP Gossip Storm",
        "timestamp": datetime.now().isoformat(),
        "status": "PASS" if passed else "FAIL",
        "configuration": {
            "target_pps": stats.target_pps,
            "duration_seconds": stats.end_time - stats.start_time
        },
        "results": {
            "packets_sent": stats.packets_sent,
            "bytes_sent": stats.bytes_sent,
            "send_errors": stats.send_errors,
            "actual_pps": round(stats.actual_pps, 2),
            "softnet_drops_before": stats.softnet_drops_before,
            "softnet_drops_after": stats.softnet_drops_after,
            "softnet_drops_delta": stats.softnet_drops_after - stats.softnet_drops_before,
            "smp_queue_max": stats.smp_queue_max,
            "prometheus_samples_count": len(stats.prometheus_samples)
        }
    }


# =============================================================================
# Main Entry Point
# =============================================================================
def main():
    parser = argparse.ArgumentParser(
        description='SMP Gossip Storm Generator for Ranvier validation',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic test with 5000 PPS for 60 seconds
  python3 gossip_storm.py --target 127.0.0.1 --port 9190 --pps 5000 --duration 60

  # With Prometheus monitoring
  python3 gossip_storm.py --target 127.0.0.1 --pps 5000 --prometheus http://127.0.0.1:9180/metrics

  # With IP spoofing (requires root and scapy)
  sudo python3 gossip_storm.py --target 127.0.0.1 --pps 5000 --use-scapy
        """
    )

    parser.add_argument('--target', '-t', default='127.0.0.1',
                        help='Target host (default: 127.0.0.1)')
    parser.add_argument('--port', '-p', type=int, default=9190,
                        help='Target gossip port (default: 9190)')
    parser.add_argument('--pps', type=int, default=5000,
                        help='Packets per second (default: 5000)')
    parser.add_argument('--duration', '-d', type=int, default=60,
                        help='Test duration in seconds (default: 60)')
    parser.add_argument('--use-scapy', action='store_true',
                        help='Use scapy for IP spoofing (requires root)')
    parser.add_argument('--prometheus', type=str,
                        help='Prometheus metrics endpoint URL')
    parser.add_argument('--threshold', type=int, default=1000,
                        help='SMP queue overflow threshold (default: 1000)')
    parser.add_argument('--output', '-o', type=str,
                        help='Output JSON report file')
    parser.add_argument('--quiet', '-q', action='store_true',
                        help='Quiet mode (minimal output)')

    args = parser.parse_args()

    # Handle signals for graceful shutdown
    generator = None

    def signal_handler(sig, frame):
        if generator:
            generator.running = False
        print("\nShutdown signal received")

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Check scapy availability
    if args.use_scapy and not SCAPY_AVAILABLE:
        print("Warning: scapy not available, falling back to socket mode")
        print("Install with: pip install scapy")
        args.use_scapy = False

    # Create and run generator
    generator = GossipStormGenerator(
        target_host=args.target,
        target_port=args.port,
        packets_per_second=args.pps,
        duration_seconds=args.duration,
        use_scapy=args.use_scapy
    )

    stats = generator.run(prometheus_endpoint=args.prometheus)

    # Check results
    passed, message = check_smp_queue_overflow(stats, args.threshold)

    # Generate report
    report = generate_report(stats, passed)

    if not args.quiet:
        print("\n" + "=" * 60)
        print("GOSSIP STORM TEST RESULTS")
        print("=" * 60)
        print(f"Status: {'PASS' if passed else 'FAIL'}")
        print(f"Packets sent: {stats.packets_sent:,}")
        print(f"Actual PPS: {stats.actual_pps:,.1f}")
        print(f"Send errors: {stats.send_errors:,}")
        print(f"\nSMP Queue Analysis:")
        print(message)
        print("=" * 60)

    # Write report
    if args.output:
        with open(args.output, 'w') as f:
            json.dump(report, f, indent=2)
        print(f"Report written to: {args.output}")
    else:
        print("\nJSON Report:")
        print(json.dumps(report, indent=2))

    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
