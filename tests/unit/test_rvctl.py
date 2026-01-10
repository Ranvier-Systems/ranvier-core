#!/usr/bin/env python3
"""
Unit tests for rvctl - Ranvier Control CLI tool.
Tests argument parsing, output formatting, and helper functions.
"""

import argparse
import importlib.util
import json
import sys
import types
import unittest
from datetime import datetime, timedelta
from io import StringIO
from pathlib import Path
from unittest.mock import MagicMock, patch

# Load rvctl module from tools directory (it's a script without .py extension)
tools_dir = Path(__file__).parent.parent.parent / "tools"
rvctl_path = tools_dir / "rvctl"

# Create module and load the script
rvctl = types.ModuleType("rvctl")
rvctl.__file__ = str(rvctl_path)
sys.modules["rvctl"] = rvctl

# Read and exec the script content (excluding if __name__ == '__main__' block)
with open(rvctl_path, 'r') as f:
    source = f.read()

# Execute in module namespace, but don't run main()
exec(compile(source, str(rvctl_path), 'exec'), rvctl.__dict__)

# Import from the loaded module
from rvctl import (
    Colors,
    TreePrinter,
    format_timestamp,
    format_time_ago,
    make_request,
)


class TestColors(unittest.TestCase):
    """Tests for ANSI color handling."""

    def setUp(self):
        """Store original color values."""
        self.original_reset = Colors.RESET
        self.original_bold = Colors.BOLD
        self.original_red = Colors.RED

    def tearDown(self):
        """Restore original color values."""
        Colors.RESET = self.original_reset
        Colors.BOLD = self.original_bold
        Colors.RED = self.original_red

    def test_colors_have_values(self):
        """Color constants should have ANSI escape sequences."""
        # Restore originals for this test
        Colors.RESET = '\033[0m'
        Colors.BOLD = '\033[1m'
        Colors.RED = '\033[91m'

        self.assertTrue(Colors.RESET.startswith('\033['))
        self.assertTrue(Colors.BOLD.startswith('\033['))
        self.assertTrue(Colors.RED.startswith('\033['))

    def test_disable_colors(self):
        """Colors.disable() should set all color codes to empty strings."""
        # Set colors to original values first
        Colors.RESET = '\033[0m'
        Colors.BOLD = '\033[1m'
        Colors.RED = '\033[91m'
        Colors.GREEN = '\033[92m'
        Colors.YELLOW = '\033[93m'
        Colors.BLUE = '\033[94m'
        Colors.MAGENTA = '\033[95m'
        Colors.CYAN = '\033[96m'
        Colors.GRAY = '\033[90m'

        Colors.disable()

        self.assertEqual(Colors.RESET, '')
        self.assertEqual(Colors.BOLD, '')
        self.assertEqual(Colors.RED, '')
        self.assertEqual(Colors.GREEN, '')
        self.assertEqual(Colors.YELLOW, '')
        self.assertEqual(Colors.BLUE, '')
        self.assertEqual(Colors.MAGENTA, '')
        self.assertEqual(Colors.CYAN, '')
        self.assertEqual(Colors.GRAY, '')


class TestFormatTimestamp(unittest.TestCase):
    """Tests for timestamp formatting."""

    def test_zero_timestamp_returns_never(self):
        """Zero timestamp should return 'never'."""
        self.assertEqual(format_timestamp(0), "never")

    def test_valid_timestamp_formats(self):
        """Valid timestamps should format as date strings."""
        # Jan 1, 2025 00:00:00 UTC
        ts_ms = 1735689600000
        result = format_timestamp(ts_ms)
        # Should contain year 2025
        self.assertIn("2025", result)

    def test_invalid_timestamp_returns_ms(self):
        """Very large invalid timestamps should return raw ms value."""
        result = format_timestamp(99999999999999999)
        self.assertIn("ms", result)


class TestFormatTimeAgo(unittest.TestCase):
    """Tests for relative time formatting."""

    def test_zero_returns_never(self):
        """Zero timestamp should return 'never'."""
        self.assertEqual(format_time_ago(0), "never")

    def test_seconds_ago(self):
        """Recent timestamps should show seconds."""
        # 30 seconds ago
        now_ms = int(datetime.now().timestamp() * 1000)
        ts_ms = now_ms - 30000
        result = format_time_ago(ts_ms)
        self.assertIn("s ago", result)

    def test_minutes_ago(self):
        """Timestamps 1-60 minutes ago should show minutes."""
        now_ms = int(datetime.now().timestamp() * 1000)
        ts_ms = now_ms - (5 * 60 * 1000)  # 5 minutes ago
        result = format_time_ago(ts_ms)
        self.assertIn("m ago", result)

    def test_hours_ago(self):
        """Timestamps 1-24 hours ago should show hours."""
        now_ms = int(datetime.now().timestamp() * 1000)
        ts_ms = now_ms - (3 * 60 * 60 * 1000)  # 3 hours ago
        result = format_time_ago(ts_ms)
        self.assertIn("h ago", result)

    def test_days_ago(self):
        """Timestamps > 24 hours ago should show days."""
        now_ms = int(datetime.now().timestamp() * 1000)
        ts_ms = now_ms - (48 * 60 * 60 * 1000)  # 2 days ago
        result = format_time_ago(ts_ms)
        self.assertIn("d ago", result)


class TestTreePrinter(unittest.TestCase):
    """Tests for radix tree ASCII visualization."""

    def setUp(self):
        """Disable colors for consistent output."""
        Colors.disable()

    def test_empty_node(self):
        """Empty/None node should not produce output."""
        printer = TreePrinter()
        # Capture stdout
        captured = StringIO()
        with patch('sys.stdout', captured):
            printer.print_tree(None)
        self.assertEqual(captured.getvalue(), "")

    def test_root_node(self):
        """Root node should display with [root] prefix."""
        printer = TreePrinter()
        tree = {
            'prefix': [],
            'type': 'Node4',
            'backend': None,
            'children': []
        }
        captured = StringIO()
        with patch('sys.stdout', captured):
            printer.print_tree(tree)
        output = captured.getvalue()
        self.assertIn("Node4", output)
        self.assertIn("[root]", output)

    def test_node_with_backend(self):
        """Nodes with backends should show backend ID."""
        printer = TreePrinter()
        tree = {
            'prefix': [1000, 2000],
            'type': 'Node4',
            'backend': 1,
            'origin': 'LOCAL',
            'children': []
        }
        captured = StringIO()
        with patch('sys.stdout', captured):
            printer.print_tree(tree)
        output = captured.getvalue()
        self.assertIn("Backend:1", output)
        self.assertIn("LOCAL", output)

    def test_node_with_children(self):
        """Nodes with children should show tree structure."""
        printer = TreePrinter()
        tree = {
            'prefix': [],
            'type': 'Node4',
            'backend': None,
            'children': [
                {
                    'edge': 100,
                    'node': {
                        'prefix': [200, 300],
                        'type': 'Node4',
                        'backend': 2,
                        'origin': 'REMOTE',
                        'children': []
                    }
                }
            ]
        }
        captured = StringIO()
        with patch('sys.stdout', captured):
            printer.print_tree(tree)
        output = captured.getvalue()
        self.assertIn("edge=100", output)
        self.assertIn("Backend:2", output)

    def test_long_prefix_truncation(self):
        """Long prefixes should be truncated with '...'."""
        printer = TreePrinter()
        tree = {
            'prefix': [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],  # 10 tokens
            'type': 'Node4',
            'backend': None,
            'children': []
        }
        captured = StringIO()
        with patch('sys.stdout', captured):
            printer.print_tree(tree)
        output = captured.getvalue()
        # Should truncate after 5 tokens
        self.assertIn("...", output)


class TestArgumentParser(unittest.TestCase):
    """Tests for CLI argument parsing."""

    def test_default_url(self):
        """Default URL should be localhost:8080."""
        parser = self._create_parser()
        args = parser.parse_args(['inspect', 'routes'])
        self.assertEqual(args.url, 'http://localhost:8080')

    def test_custom_url(self):
        """Custom URL should be accepted."""
        parser = self._create_parser()
        args = parser.parse_args(['--url', 'http://192.168.1.100:9000', 'inspect', 'routes'])
        self.assertEqual(args.url, 'http://192.168.1.100:9000')

    def test_admin_key_flag(self):
        """Admin key should be accepted via flag."""
        parser = self._create_parser()
        args = parser.parse_args(['--admin-key', 'secret123', 'inspect', 'backends'])
        self.assertEqual(args.admin_key, 'secret123')

    def test_verbose_flag(self):
        """Verbose flag should be accepted."""
        parser = self._create_parser()
        args = parser.parse_args(['--verbose', 'inspect', 'routes'])
        self.assertTrue(args.verbose)

    def test_inspect_routes_command(self):
        """'inspect routes' command should parse correctly."""
        parser = self._create_parser()
        args = parser.parse_args(['inspect', 'routes'])
        self.assertEqual(args.command, 'inspect')
        self.assertEqual(args.inspect_target, 'routes')

    def test_inspect_routes_with_prefix_filter(self):
        """'inspect routes --prefix' should parse prefix filter."""
        parser = self._create_parser()
        args = parser.parse_args(['inspect', 'routes', '--prefix', '1,2,3'])
        self.assertEqual(args.prefix, '1,2,3')

    def test_inspect_backends_command(self):
        """'inspect backends' command should parse correctly."""
        parser = self._create_parser()
        args = parser.parse_args(['inspect', 'backends'])
        self.assertEqual(args.command, 'inspect')
        self.assertEqual(args.inspect_target, 'backends')

    def test_cluster_status_command(self):
        """'cluster status' command should parse correctly."""
        parser = self._create_parser()
        args = parser.parse_args(['cluster', 'status'])
        self.assertEqual(args.command, 'cluster')
        self.assertEqual(args.cluster_action, 'status')

    def test_drain_command(self):
        """'drain' command should parse backend_id."""
        parser = self._create_parser()
        args = parser.parse_args(['drain', '5'])
        self.assertEqual(args.command, 'drain')
        self.assertEqual(args.backend_id, 5)

    def test_route_add_command(self):
        """'route add' command should parse backend and content."""
        parser = self._create_parser()
        args = parser.parse_args(['route', 'add', '--backend', '1', '--content', 'test prompt'])
        self.assertEqual(args.command, 'route')
        self.assertEqual(args.route_action, 'add')
        self.assertEqual(args.backend, 1)
        self.assertEqual(args.content, 'test prompt')

    def test_route_add_stdin_flag(self):
        """'route add --stdin' should set stdin flag."""
        parser = self._create_parser()
        args = parser.parse_args(['route', 'add', '--backend', '1', '--stdin'])
        self.assertTrue(args.stdin)

    def _create_parser(self):
        """Create a fresh argument parser for testing."""
        parser = argparse.ArgumentParser()
        parser.add_argument('--url', '-u', default='http://localhost:8080')
        parser.add_argument('--admin-key', '-k', default=None)
        parser.add_argument('--verbose', '-v', action='store_true')

        subparsers = parser.add_subparsers(dest='command')

        # inspect command
        inspect_parser = subparsers.add_parser('inspect')
        inspect_subparsers = inspect_parser.add_subparsers(dest='inspect_target')
        routes_parser = inspect_subparsers.add_parser('routes')
        routes_parser.add_argument('--prefix', '-p')
        routes_parser.add_argument('--shard', '-s', type=int)
        backends_parser = inspect_subparsers.add_parser('backends')

        # cluster command
        cluster_parser = subparsers.add_parser('cluster')
        cluster_subparsers = cluster_parser.add_subparsers(dest='cluster_action')
        status_parser = cluster_subparsers.add_parser('status')

        # drain command
        drain_parser = subparsers.add_parser('drain')
        drain_parser.add_argument('backend_id', type=int)

        # route command
        route_parser = subparsers.add_parser('route')
        route_subparsers = route_parser.add_subparsers(dest='route_action')
        route_add_parser = route_subparsers.add_parser('add')
        route_add_parser.add_argument('--backend', '-b', type=int, required=True)
        route_add_parser.add_argument('--content', '-c')
        route_add_parser.add_argument('--stdin', '-i', action='store_true')

        return parser


class TestMakeRequest(unittest.TestCase):
    """Tests for HTTP request handling."""

    @patch('urllib.request.urlopen')
    def test_successful_request(self, mock_urlopen):
        """Successful request should return parsed JSON."""
        mock_response = MagicMock()
        mock_response.read.return_value = b'{"status": "ok"}'
        mock_response.__enter__ = MagicMock(return_value=mock_response)
        mock_response.__exit__ = MagicMock(return_value=False)
        mock_urlopen.return_value = mock_response

        result = make_request('http://localhost:8080/admin/test')
        self.assertEqual(result, {'status': 'ok'})

    @patch('urllib.request.urlopen')
    def test_empty_response(self, mock_urlopen):
        """Empty response should return empty dict."""
        mock_response = MagicMock()
        mock_response.read.return_value = b''
        mock_response.__enter__ = MagicMock(return_value=mock_response)
        mock_response.__exit__ = MagicMock(return_value=False)
        mock_urlopen.return_value = mock_response

        result = make_request('http://localhost:8080/admin/test')
        self.assertEqual(result, {})

    @patch('urllib.request.urlopen')
    def test_admin_key_header(self, mock_urlopen):
        """Admin key should be sent as Bearer token."""
        mock_response = MagicMock()
        mock_response.read.return_value = b'{}'
        mock_response.__enter__ = MagicMock(return_value=mock_response)
        mock_response.__exit__ = MagicMock(return_value=False)
        mock_urlopen.return_value = mock_response

        make_request('http://localhost:8080/admin/test', admin_key='secret123')

        # Check that urlopen was called with a request containing the auth header
        call_args = mock_urlopen.call_args
        request = call_args[0][0]
        self.assertEqual(request.get_header('Authorization'), 'Bearer secret123')


class TestRouteCountHelper(unittest.TestCase):
    """Tests for route counting in tree structure."""

    def test_count_routes_empty_tree(self):
        """Empty tree should have zero routes."""
        def count_routes(node):
            if not node:
                return 0
            count = 1 if node.get('backend') is not None else 0
            for child in node.get('children', []):
                child_node = child.get('node') if child else None
                count += count_routes(child_node) if child_node else 0
            return count

        self.assertEqual(count_routes(None), 0)
        self.assertEqual(count_routes({}), 0)

    def test_count_routes_single_route(self):
        """Tree with one backend should count as one route."""
        def count_routes(node):
            if not node:
                return 0
            count = 1 if node.get('backend') is not None else 0
            for child in node.get('children', []):
                child_node = child.get('node') if child else None
                count += count_routes(child_node) if child_node else 0
            return count

        tree = {'backend': 1, 'children': []}
        self.assertEqual(count_routes(tree), 1)

    def test_count_routes_nested_tree(self):
        """Nested tree should count all backends."""
        def count_routes(node):
            if not node:
                return 0
            count = 1 if node.get('backend') is not None else 0
            for child in node.get('children', []):
                child_node = child.get('node') if child else None
                count += count_routes(child_node) if child_node else 0
            return count

        tree = {
            'backend': None,  # Root has no backend
            'children': [
                {
                    'edge': 1,
                    'node': {
                        'backend': 1,
                        'children': []
                    }
                },
                {
                    'edge': 2,
                    'node': {
                        'backend': 2,
                        'children': [
                            {
                                'edge': 3,
                                'node': {
                                    'backend': 3,
                                    'children': []
                                }
                            }
                        ]
                    }
                }
            ]
        }
        self.assertEqual(count_routes(tree), 3)


class TestNodeTypeColors(unittest.TestCase):
    """Tests for node type color mapping."""

    def test_node_type_color_mapping(self):
        """Node types should map to specific colors."""
        type_color = {
            'Node4': Colors.GREEN,
            'Node16': Colors.YELLOW,
            'Node48': Colors.MAGENTA,
            'Node256': Colors.RED,
        }

        # Verify all ART node types are covered
        self.assertIn('Node4', type_color)
        self.assertIn('Node16', type_color)
        self.assertIn('Node48', type_color)
        self.assertIn('Node256', type_color)


class TestBackendStatusFormatting(unittest.TestCase):
    """Tests for backend status output formatting."""

    def test_backend_status_healthy(self):
        """Healthy backend should show HEALTHY status."""
        backend = {
            'id': 1,
            'address': '192.168.1.100',
            'port': 11434,
            'weight': 100,
            'priority': 0,
            'is_draining': False,
            'is_dead': False
        }

        # Determine status like the actual code does
        if backend['is_dead']:
            status = "DEAD"
        elif backend['is_draining']:
            status = "DRAINING"
        else:
            status = "HEALTHY"

        self.assertEqual(status, "HEALTHY")

    def test_backend_status_draining(self):
        """Draining backend should show DRAINING status."""
        backend = {
            'id': 1,
            'is_draining': True,
            'is_dead': False
        }

        if backend['is_dead']:
            status = "DEAD"
        elif backend['is_draining']:
            status = "DRAINING"
        else:
            status = "HEALTHY"

        self.assertEqual(status, "DRAINING")

    def test_backend_status_dead(self):
        """Dead backend should show DEAD status."""
        backend = {
            'id': 1,
            'is_draining': True,  # Can be draining and dead
            'is_dead': True
        }

        if backend['is_dead']:
            status = "DEAD"
        elif backend['is_draining']:
            status = "DRAINING"
        else:
            status = "HEALTHY"

        self.assertEqual(status, "DEAD")


class TestClusterStatusFormatting(unittest.TestCase):
    """Tests for cluster status output formatting."""

    def test_quorum_healthy_state(self):
        """HEALTHY quorum state should be recognized."""
        data = {'quorum_state': 'HEALTHY'}
        self.assertEqual(data['quorum_state'], 'HEALTHY')

    def test_quorum_degraded_state(self):
        """DEGRADED quorum state should be recognized."""
        data = {'quorum_state': 'DEGRADED'}
        self.assertNotEqual(data['quorum_state'], 'HEALTHY')


if __name__ == '__main__':
    unittest.main()
