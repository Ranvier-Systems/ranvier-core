#!/usr/bin/env python3
"""
rvctl - Ranvier Control CLI

A command-line tool to inspect and manage a running Ranvier instance.
Communicates with the Ranvier Admin API via JSON/HTTP.

Usage:
    rvctl inspect routes [--prefix TOKEN_IDS] [--shard SHARD_ID]
    rvctl inspect backends
    rvctl cluster status
    rvctl drain <backend_id>

Authentication:
    Set RANVIER_ADMIN_KEY environment variable or use --admin-key flag.

Examples:
    # Inspect all routes across shards
    rvctl inspect routes

    # Inspect routes with a specific prefix filter
    rvctl inspect routes --prefix "1234,5678,9012"

    # Show backend health status
    rvctl inspect backends

    # Show cluster/quorum status
    rvctl cluster status

    # Drain a specific backend
    rvctl drain 1
"""

import argparse
import json
import os
import sys
import urllib.request
import urllib.error
from datetime import datetime
from typing import Optional, Dict, Any, List


# ANSI color codes for terminal output
class Colors:
    RESET = '\033[0m'
    BOLD = '\033[1m'
    RED = '\033[91m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    MAGENTA = '\033[95m'
    CYAN = '\033[96m'
    GRAY = '\033[90m'

    @classmethod
    def disable(cls):
        """Disable colors for non-TTY output."""
        cls.RESET = ''
        cls.BOLD = ''
        cls.RED = ''
        cls.GREEN = ''
        cls.YELLOW = ''
        cls.BLUE = ''
        cls.MAGENTA = ''
        cls.CYAN = ''
        cls.GRAY = ''


# Check if output is a TTY
if not sys.stdout.isatty():
    Colors.disable()


def make_request(url: str, method: str = 'GET', admin_key: Optional[str] = None,
                 data: Optional[bytes] = None) -> Dict[str, Any]:
    """Make an HTTP request to the Ranvier Admin API."""
    headers = {
        'Content-Type': 'application/json',
        'Accept': 'application/json',
    }

    if admin_key:
        headers['Authorization'] = f'Bearer {admin_key}'

    req = urllib.request.Request(url, method=method, headers=headers, data=data)

    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            body = response.read().decode('utf-8')
            return json.loads(body) if body else {}
    except urllib.error.HTTPError as e:
        body = e.read().decode('utf-8') if e.fp else ''
        try:
            error_data = json.loads(body)
            error_msg = error_data.get('error', str(e))
        except json.JSONDecodeError:
            error_msg = body or str(e)
        print(f"{Colors.RED}Error:{Colors.RESET} HTTP {e.code}: {error_msg}", file=sys.stderr)
        sys.exit(1)
    except urllib.error.URLError as e:
        print(f"{Colors.RED}Error:{Colors.RESET} Connection failed: {e.reason}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"{Colors.RED}Error:{Colors.RESET} {e}", file=sys.stderr)
        sys.exit(1)


def format_timestamp(ms: int) -> str:
    """Format a millisecond timestamp to human-readable string."""
    if ms == 0:
        return "never"
    try:
        dt = datetime.fromtimestamp(ms / 1000)
        return dt.strftime('%Y-%m-%d %H:%M:%S')
    except (ValueError, OSError):
        return f"{ms}ms"


def format_time_ago(ms: int) -> str:
    """Format a millisecond timestamp as 'X ago' relative time."""
    if ms == 0:
        return "never"
    try:
        dt = datetime.fromtimestamp(ms / 1000)
        now = datetime.now()
        delta = now - dt
        seconds = int(delta.total_seconds())

        if seconds < 0:
            return "in the future"
        elif seconds < 60:
            return f"{seconds}s ago"
        elif seconds < 3600:
            return f"{seconds // 60}m ago"
        elif seconds < 86400:
            return f"{seconds // 3600}h ago"
        else:
            return f"{seconds // 86400}d ago"
    except (ValueError, OSError):
        return f"{ms}ms"


class TreePrinter:
    """Prints a radix tree in ASCII tree format."""

    def __init__(self, show_details: bool = True):
        self.show_details = show_details

    def print_tree(self, node: Dict[str, Any], prefix: str = "", is_last: bool = True,
                   path: Optional[List[int]] = None):
        """Recursively print a tree node."""
        if path is None:
            path = []

        # Guard against None or empty nodes
        if not node:
            return

        # Build the connection characters
        connector = "└── " if is_last else "├── "
        extension = "    " if is_last else "│   "

        # Build node label
        node_prefix = node.get('prefix', [])
        backend = node.get('backend')
        origin = node.get('origin', 'LOCAL')
        node_type = node.get('type', 'Node4')

        # Current path including this node's prefix
        full_path = path + node_prefix

        # Format the prefix display
        if node_prefix:
            prefix_str = f"[{','.join(str(t) for t in node_prefix[:5])}{'...' if len(node_prefix) > 5 else ''}]"
        else:
            prefix_str = "[root]" if not path else "[]"

        # Node type color
        type_color = {
            'Node4': Colors.GREEN,
            'Node16': Colors.YELLOW,
            'Node48': Colors.MAGENTA,
            'Node256': Colors.RED,
        }.get(node_type, Colors.RESET)

        # Format backend info
        if backend is not None:
            backend_str = f" {Colors.CYAN}→ Backend:{backend}{Colors.RESET}"
            origin_color = Colors.GREEN if origin == 'LOCAL' else Colors.YELLOW
            origin_str = f" ({origin_color}{origin}{Colors.RESET})"
        else:
            backend_str = ""
            origin_str = ""

        # Print this node
        node_line = f"{prefix}{connector}{type_color}{node_type}{Colors.RESET} {prefix_str}{backend_str}{origin_str}"
        print(node_line)

        # Print children
        children = node.get('children', [])
        for i, child_entry in enumerate(children):
            is_last_child = (i == len(children) - 1)
            edge = child_entry.get('edge', 0)
            child_node = child_entry.get('node', {})

            # Add edge info to path
            child_path = full_path + [edge]

            # Print edge
            edge_prefix = prefix + extension
            edge_connector = "└─▶ " if is_last_child else "├─▶ "
            print(f"{edge_prefix}{edge_connector}{Colors.GRAY}edge={edge}{Colors.RESET}")

            # Print child node
            self.print_tree(child_node, edge_prefix + ("    " if is_last_child else "│   "),
                           True, child_path)


def cmd_inspect_routes(args):
    """Handle 'inspect routes' command."""
    base_url = args.url.rstrip('/')

    # Build URL with optional prefix filter
    url = f"{base_url}/admin/dump/tree"
    if args.prefix:
        url += f"?prefix={args.prefix}"

    print(f"{Colors.BOLD}Fetching routes from {args.url}...{Colors.RESET}")
    print()

    data = make_request(url, admin_key=args.admin_key)

    shard_id = data.get('shard_id', 0)
    tree = data.get('tree')
    prefix_filter = data.get('prefix_filter')
    error = data.get('error')

    print(f"{Colors.BOLD}Radix Tree State{Colors.RESET} (Shard {shard_id})")
    print("=" * 50)

    if prefix_filter:
        print(f"Prefix Filter: [{','.join(str(t) for t in prefix_filter)}]")
        print()

    if error:
        print(f"{Colors.YELLOW}Warning:{Colors.RESET} {error}")
        print()

    if tree is None or tree.get('type') == 'empty':
        print(f"{Colors.GRAY}(empty tree){Colors.RESET}")
        return

    # Print tree
    printer = TreePrinter(show_details=args.verbose)
    printer.print_tree(tree)

    # Count routes (leaf nodes with backends)
    def count_routes(node: Dict) -> int:
        if not node:
            return 0
        count = 1 if node.get('backend') is not None else 0
        for child in node.get('children', []):
            child_node = child.get('node') if child else None
            count += count_routes(child_node) if child_node else 0
        return count

    route_count = count_routes(tree)
    print()
    print(f"{Colors.BOLD}Total Routes:{Colors.RESET} {route_count}")


def cmd_inspect_backends(args):
    """Handle 'inspect backends' command."""
    base_url = args.url.rstrip('/')
    url = f"{base_url}/admin/dump/backends"

    print(f"{Colors.BOLD}Fetching backends from {args.url}...{Colors.RESET}")
    print()

    data = make_request(url, admin_key=args.admin_key)

    shard_id = data.get('shard_id', 0)
    backends = data.get('backends', [])
    backend_count = data.get('backend_count', len(backends))

    print(f"{Colors.BOLD}Backend Status{Colors.RESET} (Shard {shard_id})")
    print("=" * 80)

    if not backends:
        print(f"{Colors.GRAY}(no backends registered){Colors.RESET}")
        return

    # Table header
    print(f"{'ID':>4} │ {'Address':^21} │ {'Weight':>6} │ {'Priority':>8} │ {'Status':^15}")
    print("─" * 4 + "─┼─" + "─" * 21 + "─┼─" + "─" * 6 + "─┼─" + "─" * 8 + "─┼─" + "─" * 15)

    for b in backends:
        bid = b.get('id', 0)
        address = f"{b.get('address', '?')}:{b.get('port', 0)}"
        weight = b.get('weight', 100)
        priority = b.get('priority', 0)
        is_draining = b.get('is_draining', False)
        is_dead = b.get('is_dead', False)

        # Determine status
        if is_dead:
            status = f"{Colors.RED}DEAD{Colors.RESET}"
            status_raw = "DEAD"
        elif is_draining:
            status = f"{Colors.YELLOW}DRAINING{Colors.RESET}"
            status_raw = "DRAINING"
        else:
            status = f"{Colors.GREEN}HEALTHY{Colors.RESET}"
            status_raw = "HEALTHY"

        # Weight display
        if weight == 0:
            weight_str = f"{Colors.GRAY}0{Colors.RESET}"
        else:
            weight_str = str(weight)

        print(f"{bid:>4} │ {address:^21} │ {weight_str:>6} │ {priority:>8} │ {status:^15}")

        # Show drain info if draining
        if is_draining and b.get('drain_start_ms', 0) > 0:
            drain_time = format_time_ago(b['drain_start_ms'])
            print(f"     │ {Colors.GRAY}└─ Draining since {drain_time}{Colors.RESET}")

    print()
    print(f"{Colors.BOLD}Total Backends:{Colors.RESET} {backend_count}")

    # Summary stats
    healthy = sum(1 for b in backends if not b.get('is_draining') and not b.get('is_dead'))
    draining = sum(1 for b in backends if b.get('is_draining'))
    dead = sum(1 for b in backends if b.get('is_dead'))

    print(f"  {Colors.GREEN}Healthy:{Colors.RESET} {healthy}  "
          f"{Colors.YELLOW}Draining:{Colors.RESET} {draining}  "
          f"{Colors.RED}Dead:{Colors.RESET} {dead}")


def cmd_cluster_status(args):
    """Handle 'cluster status' command."""
    base_url = args.url.rstrip('/')
    url = f"{base_url}/admin/dump/cluster"

    print(f"{Colors.BOLD}Fetching cluster status from {args.url}...{Colors.RESET}")
    print()

    data = make_request(url, admin_key=args.admin_key)

    cluster_enabled = data.get('cluster_enabled', False)

    if not cluster_enabled:
        print(f"{Colors.YELLOW}Cluster mode is not enabled.{Colors.RESET}")
        print("Enable clustering in ranvier.yaml to use cluster features.")
        return

    quorum_state = data.get('quorum_state', 'UNKNOWN')
    quorum_required = data.get('quorum_required', 0)
    peers_alive = data.get('peers_alive', 0)
    total_peers = data.get('total_peers', 0)
    peers_recently_seen = data.get('peers_recently_seen', 0)
    is_draining = data.get('is_draining', False)
    local_backend_id = data.get('local_backend_id', 0)
    peers = data.get('peers', [])

    # Quorum status with color
    if quorum_state == 'HEALTHY':
        quorum_display = f"{Colors.GREEN}HEALTHY{Colors.RESET}"
    else:
        quorum_display = f"{Colors.RED}DEGRADED{Colors.RESET}"

    print(f"{Colors.BOLD}Cluster Status{Colors.RESET}")
    print("=" * 60)
    print(f"  Quorum State:       {quorum_display}")
    print(f"  Quorum Required:    {quorum_required} peers")
    print(f"  Peers Alive:        {peers_alive} / {total_peers}")
    print(f"  Peers Recently Seen: {peers_recently_seen}")
    print(f"  Local Backend ID:   {local_backend_id}")
    if is_draining:
        print(f"  Node Status:        {Colors.YELLOW}DRAINING{Colors.RESET}")
    else:
        print(f"  Node Status:        {Colors.GREEN}ACTIVE{Colors.RESET}")

    print()
    print(f"{Colors.BOLD}Peer Table{Colors.RESET}")
    print("─" * 60)

    if not peers:
        print(f"{Colors.GRAY}  (no peers configured){Colors.RESET}")
    else:
        print(f"  {'Address':^25} │ {'Status':^10} │ {'Last Seen':^15}")
        print("  " + "─" * 25 + "─┼─" + "─" * 10 + "─┼─" + "─" * 15)

        for peer in peers:
            address = f"{peer.get('address', '?')}:{peer.get('port', 0)}"
            is_alive = peer.get('is_alive', False)
            last_seen_ms = peer.get('last_seen_ms', 0)
            associated_backend = peer.get('associated_backend')

            if is_alive:
                status = f"{Colors.GREEN}ALIVE{Colors.RESET}"
            else:
                status = f"{Colors.RED}DEAD{Colors.RESET}"

            last_seen = format_time_ago(last_seen_ms)

            backend_str = f" (backend {associated_backend})" if associated_backend else ""
            print(f"  {address:^25} │ {status:^10} │ {last_seen:^15}{backend_str}")


def cmd_drain(args):
    """Handle 'drain' command."""
    backend_id = args.backend_id
    base_url = args.url.rstrip('/')
    url = f"{base_url}/admin/drain?backend_id={backend_id}"

    print(f"{Colors.BOLD}Initiating drain for backend {backend_id}...{Colors.RESET}")

    data = make_request(url, method='POST', admin_key=args.admin_key)

    status = data.get('status', 'unknown')
    action = data.get('action', 'unknown')
    message = data.get('message', '')
    returned_id = data.get('backend_id', backend_id)

    if status == 'ok':
        print(f"{Colors.GREEN}Success:{Colors.RESET} Backend {returned_id} drain initiated")
        if message:
            print(f"  {message}")
    else:
        print(f"{Colors.YELLOW}Status:{Colors.RESET} {status}")
        if message:
            print(f"  {message}")


def main():
    parser = argparse.ArgumentParser(
        description='Ranvier Control CLI - Inspect and manage Ranvier instances',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )

    # Global options
    parser.add_argument('--url', '-u', default='http://localhost:8080',
                        help='Ranvier Admin API URL (default: http://localhost:8080)')
    parser.add_argument('--admin-key', '-k',
                        default=os.environ.get('RANVIER_ADMIN_KEY'),
                        help='Admin API key (or set RANVIER_ADMIN_KEY env var)')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Enable verbose output')

    subparsers = parser.add_subparsers(dest='command', help='Commands')

    # inspect command
    inspect_parser = subparsers.add_parser('inspect', help='Inspect Ranvier state')
    inspect_subparsers = inspect_parser.add_subparsers(dest='inspect_target', help='What to inspect')

    # inspect routes
    routes_parser = inspect_subparsers.add_parser('routes', help='Inspect radix tree routes')
    routes_parser.add_argument('--prefix', '-p',
                               help='Filter by prefix (comma-separated token IDs)')
    routes_parser.add_argument('--shard', '-s', type=int,
                               help='Query specific shard (not yet implemented)')

    # inspect backends
    backends_parser = inspect_subparsers.add_parser('backends', help='Inspect backend status')

    # cluster command
    cluster_parser = subparsers.add_parser('cluster', help='Cluster operations')
    cluster_subparsers = cluster_parser.add_subparsers(dest='cluster_action', help='Cluster action')

    # cluster status
    status_parser = cluster_subparsers.add_parser('status', help='Show cluster status')

    # drain command
    drain_parser = subparsers.add_parser('drain', help='Drain a backend')
    drain_parser.add_argument('backend_id', type=int, help='Backend ID to drain')

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    # Handle inspect command
    if args.command == 'inspect':
        if not args.inspect_target:
            inspect_parser.print_help()
            sys.exit(1)

        if args.inspect_target == 'routes':
            cmd_inspect_routes(args)
        elif args.inspect_target == 'backends':
            cmd_inspect_backends(args)
        else:
            print(f"Unknown inspect target: {args.inspect_target}", file=sys.stderr)
            sys.exit(1)

    # Handle cluster command
    elif args.command == 'cluster':
        if not args.cluster_action:
            cluster_parser.print_help()
            sys.exit(1)

        if args.cluster_action == 'status':
            cmd_cluster_status(args)
        else:
            print(f"Unknown cluster action: {args.cluster_action}", file=sys.stderr)
            sys.exit(1)

    # Handle drain command
    elif args.command == 'drain':
        cmd_drain(args)

    else:
        print(f"Unknown command: {args.command}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
