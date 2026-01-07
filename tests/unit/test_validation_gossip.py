#!/usr/bin/env python3
"""
Unit tests for the validation suite gossip storm generator.
Tests packet serialization to ensure wire format compatibility with C++ implementation.
"""

import struct
import sys
import unittest
from pathlib import Path

# Add validation directory to path for imports
validation_dir = Path(__file__).parent.parent.parent / "validation"
sys.path.insert(0, str(validation_dir))

# Import the module under test
from gossip_storm import (
    serialize_route_announcement,
    generate_random_packet,
    generate_diverse_source_ips,
    GOSSIP_PACKET_TYPE_ROUTE_ANNOUNCEMENT,
    PROTOCOL_VERSION,
    HEADER_SIZE,
    MAX_TOKENS,
)


class TestPacketSerialization(unittest.TestCase):
    """Tests for gossip packet serialization."""

    def test_header_size(self):
        """Header should be exactly 12 bytes: type(1) + version(1) + seq(4) + backend(4) + count(2)."""
        self.assertEqual(HEADER_SIZE, 12)

    def test_empty_token_packet(self):
        """Packet with zero tokens should be header-only."""
        packet = serialize_route_announcement(
            backend_id=1,
            tokens=[],
            seq_num=0
        )
        self.assertEqual(len(packet), HEADER_SIZE)

    def test_single_token_packet(self):
        """Packet with one token should be header + 4 bytes."""
        packet = serialize_route_announcement(
            backend_id=1,
            tokens=[42],
            seq_num=0
        )
        self.assertEqual(len(packet), HEADER_SIZE + 4)

    def test_header_format(self):
        """Test that header is correctly formatted in big-endian."""
        packet = serialize_route_announcement(
            backend_id=0x12345678,
            tokens=[0xAABBCCDD],
            seq_num=0xDEADBEEF
        )

        # Parse header
        ptype, version = struct.unpack('>BB', packet[0:2])
        seq_num = struct.unpack('>I', packet[2:6])[0]
        backend_id = struct.unpack('>I', packet[6:10])[0]
        token_count = struct.unpack('>H', packet[10:12])[0]

        self.assertEqual(ptype, GOSSIP_PACKET_TYPE_ROUTE_ANNOUNCEMENT)
        self.assertEqual(version, PROTOCOL_VERSION)
        self.assertEqual(seq_num, 0xDEADBEEF)
        self.assertEqual(backend_id, 0x12345678)
        self.assertEqual(token_count, 1)

    def test_token_serialization(self):
        """Tokens should be serialized as big-endian 4-byte integers."""
        tokens = [0x00000001, 0x12345678, 0xFFFFFFFF]
        packet = serialize_route_announcement(
            backend_id=1,
            tokens=tokens,
            seq_num=0
        )

        # Extract tokens from packet
        token_data = packet[HEADER_SIZE:]
        self.assertEqual(len(token_data), len(tokens) * 4)

        parsed_tokens = []
        for i in range(len(tokens)):
            offset = i * 4
            t = struct.unpack('>I', token_data[offset:offset+4])[0]
            parsed_tokens.append(t)

        self.assertEqual(parsed_tokens, tokens)

    def test_max_tokens_truncation(self):
        """Packets should truncate tokens beyond MAX_TOKENS."""
        tokens = list(range(MAX_TOKENS + 100))  # More than max
        packet = serialize_route_announcement(
            backend_id=1,
            tokens=tokens,
            seq_num=0
        )

        # Token count in header should be capped
        token_count = struct.unpack('>H', packet[10:12])[0]
        self.assertEqual(token_count, MAX_TOKENS)

        # Packet size should reflect truncation
        expected_size = HEADER_SIZE + MAX_TOKENS * 4
        self.assertEqual(len(packet), expected_size)

    def test_random_packet_generation(self):
        """Random packets should be valid and reasonably sized."""
        for seq_num in range(100):
            packet = generate_random_packet(seq_num)

            # Should be at least header size
            self.assertGreaterEqual(len(packet), HEADER_SIZE)

            # Should not exceed max packet size
            max_size = HEADER_SIZE + MAX_TOKENS * 4
            self.assertLessEqual(len(packet), max_size)

            # Header should be valid
            ptype, version = struct.unpack('>BB', packet[0:2])
            self.assertEqual(ptype, GOSSIP_PACKET_TYPE_ROUTE_ANNOUNCEMENT)
            self.assertEqual(version, PROTOCOL_VERSION)

    def test_sequence_number_in_random_packets(self):
        """Sequence numbers should be correctly embedded in random packets."""
        for expected_seq in [0, 1, 100, 0xFFFFFFFF]:
            packet = generate_random_packet(expected_seq)
            actual_seq = struct.unpack('>I', packet[2:6])[0]
            self.assertEqual(actual_seq, expected_seq)


class TestIPGeneration(unittest.TestCase):
    """Tests for source IP generation."""

    def test_ip_count(self):
        """Should generate requested number of IPs."""
        ips = generate_diverse_source_ips(50)
        self.assertEqual(len(ips), 50)

    def test_ip_format(self):
        """Generated IPs should be valid IPv4 strings."""
        import ipaddress
        ips = generate_diverse_source_ips(10)
        for ip in ips:
            # Should not raise
            parsed = ipaddress.ip_address(ip)
            self.assertEqual(parsed.version, 4)

    def test_ip_in_network(self):
        """Generated IPs should be within the specified network."""
        import ipaddress
        network = ipaddress.ip_network("192.168.0.0/16")
        ips = generate_diverse_source_ips(100, "192.168.0.0/16")

        for ip in ips:
            addr = ipaddress.ip_address(ip)
            self.assertIn(addr, network)

    def test_ip_uniqueness(self):
        """Generated IPs should be mostly unique (allowing some collisions in large space)."""
        ips = generate_diverse_source_ips(100, "10.0.0.0/8")
        unique_ips = set(ips)
        # Allow up to 10% collision rate
        self.assertGreaterEqual(len(unique_ips), 90)


class TestProtocolConstants(unittest.TestCase):
    """Tests for protocol constants matching C++ implementation."""

    def test_packet_type_route_announcement(self):
        """Route announcement type should be 0x01."""
        self.assertEqual(GOSSIP_PACKET_TYPE_ROUTE_ANNOUNCEMENT, 0x01)

    def test_protocol_version(self):
        """Protocol version should be 2 (v2 with sequence numbers)."""
        self.assertEqual(PROTOCOL_VERSION, 2)

    def test_max_tokens(self):
        """Max tokens should be 256."""
        self.assertEqual(MAX_TOKENS, 256)


class TestWireFormatCompatibility(unittest.TestCase):
    """Tests ensuring wire format matches C++ gossip_service.hpp specification."""

    def test_wire_format_example(self):
        """Test a specific packet matches expected wire format."""
        # Create a packet matching the C++ example
        packet = serialize_route_announcement(
            backend_id=100,
            tokens=[1000, 2000, 3000],
            seq_num=42
        )

        # Expected format (from gossip_service.hpp):
        # [type:1][version:1][seq_num:4][backend_id:4][token_count:2][tokens:4*N]
        expected = bytearray()
        expected.append(0x01)  # type = ROUTE_ANNOUNCEMENT
        expected.append(0x02)  # version = 2
        expected.extend(struct.pack('>I', 42))      # seq_num = 42
        expected.extend(struct.pack('>I', 100))     # backend_id = 100
        expected.extend(struct.pack('>H', 3))       # token_count = 3
        expected.extend(struct.pack('>I', 1000))    # token[0]
        expected.extend(struct.pack('>I', 2000))    # token[1]
        expected.extend(struct.pack('>I', 3000))    # token[2]

        self.assertEqual(packet, bytes(expected))


if __name__ == '__main__':
    unittest.main()
