#!/usr/bin/env python3
"""
Config Loading Integration Tests for Ranvier Core

Validates BACKLOG.md §6.6 requirements for startup/reload config behavior:
  - YAML config loaded correctly
  - Environment variables override YAML
  - --dry-run validates without starting the server
  - Invalid config produces a clear error

These tests complement tests/integration/test_negative_paths.py, which covers
hot-reload resilience (SIGHUP with invalid YAML preserves the old config).
The suite here focuses on INITIAL load behavior and the dry-run code path --
different code paths in src/main.cpp and src/config_loader.cpp.

Usage:
    python3 tests/integration/test_config_loading.py

Requirements:
    - Docker and docker-compose installed
    - requests library
"""

import os
import subprocess
import sys
import time
import unittest

try:
    import requests  # noqa: F401  (imported for parity with sibling suites)
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)

from conftest import (
    ClusterTestCase,
    COMPOSE_FILE,
    CONTAINER_NAMES,
    NODES,
    get_compose_cmd,
    send_chat_request,
)


# Path where Ranvier loads its YAML config inside the container.
# docker-compose.test.yml passes ``--config /tmp/ranvier.yaml``; the container
# is read_only but /tmp is a tmpfs mount, so we can write the file with
# ``docker exec`` + shell redirection.  Matches test_negative_paths.py.
_CONTAINER_CONFIG_PATH = "/tmp/ranvier.yaml"

# Application::reload_config() enforces a 10-second cooldown between SIGHUPs.
# Wait slightly longer to be safe when tests need to chain reloads.
_RELOAD_COOLDOWN_WAIT = 12


def _write_container_config(container: str, yaml_text: str) -> None:
    """Write ``yaml_text`` to _CONTAINER_CONFIG_PATH inside ``container``.

    Uses ``docker exec -i ... sh -c 'cat > /tmp/ranvier.yaml'`` and pipes the
    YAML on stdin so we don't have to worry about shell quoting of special
    characters inside the YAML (colons, braces, newlines).
    """
    subprocess.run(
        ["docker", "exec", "-i", container, "sh", "-c",
         f"cat > {_CONTAINER_CONFIG_PATH}"],
        input=yaml_text,
        capture_output=True, text=True, timeout=10,
        check=True,
    )


def _remove_container_config(container: str) -> None:
    """Remove the YAML config file from the container (ignore errors)."""
    subprocess.run(
        ["docker", "exec", container, "rm", "-f", _CONTAINER_CONFIG_PATH],
        capture_output=True, text=True, timeout=10,
    )


def _send_sighup(container: str) -> None:
    """Send SIGHUP to the Ranvier process inside ``container``."""
    result = subprocess.run(
        ["docker", "kill", "--signal=HUP", container],
        capture_output=True, text=True, timeout=10,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"docker kill --signal=HUP {container} failed: {result.stderr}"
        )


def _get_container_logs(container: str, tail: int = 200) -> str:
    """Return the last ``tail`` lines of the container's logs (stdout+stderr)."""
    result = subprocess.run(
        ["docker", "logs", "--tail", str(tail), container],
        capture_output=True, text=True, timeout=10,
    )
    # docker logs writes container stdout to our stdout and stderr to stderr.
    return (result.stdout or "") + (result.stderr or "")


# =============================================================================
# Test Suite
# =============================================================================

class ConfigLoadingTest(ClusterTestCase):
    """Integration tests for initial config loading and --dry-run validation."""

    PROJECT_NAME = "ranvier-config-loading-test"
    AUTO_REGISTER_BACKENDS = True

    # =========================================================================
    # Test 1: Valid YAML is loaded and applied
    # =========================================================================

    def test_01_yaml_config_loaded_correctly(self):
        """Write a valid YAML config on ranvier1, SIGHUP, and verify the
        reload succeeded.

        RanvierConfig::load() parses YAML sections (server, database, health,
        pool, routing, ...), then apply_env_overrides() layers env vars on
        top.  A successful reload logs ``Configuration reloaded successfully
        on all cores`` (see src/application.cpp:1478).  We use that log line
        as proof that the YAML parsed cleanly and passed validation --
        handle_dump_config only exposes local_mode, so per-section values
        (like health.check_interval_seconds) aren't directly observable via
        the admin API.
        """
        print("\nTest: YAML config loaded correctly")

        node1_api = NODES["node1"]["api"]
        container = CONTAINER_NAMES["node1"]

        # Sanity check: node is healthy before we touch its config
        print("  Verifying node1 is healthy before reload...")
        status, _, _ = send_chat_request(
            node1_api, [{"role": "user", "content": "pre-load-check"}],
            timeout=10, retries=1,
        )
        self.assertEqual(status, 200, "Node1 should be healthy before config load test")

        # Set a non-default health.check_interval_seconds.  The test config
        # default is 30s (see docker-compose.test.yml env vars); 15s is a
        # clearly different, still-valid value.
        yaml_text = (
            "health:\n"
            "  check_interval_seconds: 15\n"
        )

        # Timestamp marker: only count "reloaded successfully" log lines that
        # appear AFTER we send our SIGHUP, not any from earlier in the run.
        # We snapshot the current log length (line count) as the cutoff.
        pre_logs = _get_container_logs(container, tail=500)
        pre_line_count = pre_logs.count("\n")

        print(f"  Writing YAML to {_CONTAINER_CONFIG_PATH} in {container}...")
        _write_container_config(container, yaml_text)

        try:
            # This is the first SIGHUP in this test method; the fixture brings
            # up a fresh cluster and no prior test ran against this cluster,
            # so the 10s cooldown has already elapsed.  Still, a small 3s
            # settle is prudent in case setUpClass finished moments before.
            time.sleep(3)

            print("  Sending SIGHUP to apply YAML config...")
            _send_sighup(container)

            # Wait for the reload to complete on all cores.  The reload is
            # async (seastar::async offloads the blocking std::ifstream), and
            # then propagates via invoke_on_all across shards -- a few
            # seconds is enough, but we poll to be robust under slow CI.
            print("  Waiting for 'Configuration reloaded successfully' log line...")
            success_line = "Configuration reloaded successfully on all cores"
            deadline = time.time() + 10
            reloaded = False
            while time.time() < deadline:
                logs = _get_container_logs(container, tail=500)
                # Only consider lines after the pre-SIGHUP cutoff
                new_portion = "\n".join(logs.split("\n")[pre_line_count:])
                if success_line in new_portion:
                    reloaded = True
                    break
                # Guard against a validation error or rate-limit rejection
                # getting silently swallowed: fail fast with a helpful message
                for bad in (
                    "Config reload failed - validation error",
                    "Config reload failed:",
                    "Config reload rate-limited",
                ):
                    if bad in new_portion:
                        self.fail(
                            f"Config reload did not succeed -- log line found: '{bad}'. "
                            f"Recent logs:\n{new_portion[-2000:]}"
                        )
                time.sleep(0.5)

            self.assertTrue(
                reloaded,
                f"Expected '{success_line}' in container logs after SIGHUP. "
                f"Recent logs:\n{_get_container_logs(container, tail=200)[-2000:]}"
            )

            # Cross-check: SIGHUP receipt was logged too (belt and suspenders --
            # confirms our signal reached the handler, not just that *some*
            # earlier reload succeeded).
            sighup_line = "SIGHUP received - triggering configuration reload"
            logs = _get_container_logs(container, tail=500)
            new_portion = "\n".join(logs.split("\n")[pre_line_count:])
            self.assertIn(
                sighup_line, new_portion,
                "Expected SIGHUP receipt to be logged after our docker kill --signal=HUP"
            )

            # And the server is still serving requests with the new config
            print("  Verifying node1 still serves requests after reload...")
            status, _, _ = send_chat_request(
                node1_api, [{"role": "user", "content": "post-load-check"}],
                timeout=10, retries=1,
            )
            self.assertEqual(
                status, 200,
                "Node1 should still serve requests after valid YAML reload"
            )

        finally:
            # Restore env-var-only defaults: remove the file, then SIGHUP
            # (after the cooldown) so subsequent tests start from a clean
            # baseline.  Best-effort -- swallow cooldown rejections since
            # they don't affect correctness of later tests that will wait.
            print("  Cleanup: removing config file and reloading defaults...")
            _remove_container_config(container)
            time.sleep(_RELOAD_COOLDOWN_WAIT)
            try:
                _send_sighup(container)
                time.sleep(3)
            except RuntimeError as e:
                print(f"    Cleanup SIGHUP failed (non-fatal): {e}")

        print("  PASSED: Valid YAML loaded and applied via SIGHUP")


# =============================================================================
# Main
# =============================================================================

def main():
    """Run the config loading integration tests."""
    print("=" * 70)
    print("Ranvier Core - Config Loading Integration Tests")
    print("=" * 70)
    print("\nThese tests validate startup/reload config behavior:")
    print("  - YAML config loaded correctly")
    print("  - (more tests to come)")
    print("")

    try:
        compose_cmd = get_compose_cmd()
        print(f"Using: {' '.join(compose_cmd)}")
    except RuntimeError as e:
        print(f"Error: {e}")
        sys.exit(1)

    if not os.path.exists(COMPOSE_FILE):
        print(f"Error: Docker Compose file not found: {COMPOSE_FILE}")
        sys.exit(1)

    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(ConfigLoadingTest)
    suite = unittest.TestSuite(sorted(suite, key=lambda t: t.id()))

    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
