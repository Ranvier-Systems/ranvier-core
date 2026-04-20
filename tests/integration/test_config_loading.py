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
    import requests
except ImportError:
    print("Error: 'requests' library is required. Install with: pip install requests")
    sys.exit(1)

from conftest import (
    ClusterTestCase,
    COMPOSE_FILE,
    CONFIG_RELOAD_COOLDOWN_WAIT as _RELOAD_COOLDOWN_WAIT,
    CONTAINER_CONFIG_PATH as _CONTAINER_CONFIG_PATH,
    CONTAINER_NAMES,
    NODES,
    STARTUP_TIMEOUT,
    check_container_running,
    get_compose_cmd,
    run_compose,
    send_chat_request,
    sum_metric_by_substring,
    wait_for_healthy,
)


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


# Log lines emitted by application.cpp during reload_config().
_RELOAD_SUCCESS_LINE = "Configuration reloaded successfully on all cores"
_RELOAD_FAILURE_LINES = (
    "Config reload failed - validation error",
    "Config reload failed:",
    "Config reload rate-limited",
)


def wait_for_reload_success(
    testcase: unittest.TestCase,
    container: str,
    pre_line_count: int,
    *,
    timeout: float = 10.0,
) -> str:
    """Poll container logs until a reload-success line appears after the cutoff.

    Returns the "new portion" of the logs (everything past ``pre_line_count``
    lines) so callers can run additional assertions on it without re-reading
    docker logs.  Fails the test fast if a reload-failure line appears in the
    new portion first -- that way callers get a clear diagnostic instead of a
    generic timeout.
    """
    deadline = time.time() + timeout
    last_new_portion = ""
    while time.time() < deadline:
        logs = _get_container_logs(container, tail=500)
        new_portion = "\n".join(logs.split("\n")[pre_line_count:])
        last_new_portion = new_portion
        if _RELOAD_SUCCESS_LINE in new_portion:
            return new_portion
        for bad in _RELOAD_FAILURE_LINES:
            if bad in new_portion:
                testcase.fail(
                    f"Config reload did not succeed -- log line found: '{bad}'. "
                    f"Recent logs:\n{new_portion[-2000:]}"
                )
        time.sleep(0.5)

    testcase.fail(
        f"Expected '{_RELOAD_SUCCESS_LINE}' in container logs after SIGHUP. "
        f"Recent logs:\n{last_new_portion[-2000:]}"
    )
    return last_new_portion  # unreachable, keeps mypy happy


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
            new_portion = wait_for_reload_success(self, container, pre_line_count)

            # Cross-check: SIGHUP receipt was logged too (belt and suspenders --
            # confirms our signal reached the handler, not just that *some*
            # earlier reload succeeded).
            sighup_line = "SIGHUP received - triggering configuration reload"
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

    # =========================================================================
    # Test 2: Environment variables override YAML values
    # =========================================================================

    def test_02_env_vars_override_yaml(self):
        """Verify apply_env_overrides() wins when YAML and env disagree.

        docker-compose.test.yml sets ``RANVIER_MIN_TOKEN_LENGTH=2`` on every
        Ranvier node.  We write a YAML file that claims
        ``routing.min_token_length: 99`` and SIGHUP.  RanvierConfig::load()
        parses YAML first, then apply_env_overrides() clobbers with the env
        value -- so the effective threshold after reload is 2, not 99.

        Observable consequence: in PREFIX routing mode (the default), the
        HTTP path learns routes only when ``ctx->tokens.size() >=
        min_token_length`` (http_controller.cpp:975).  Normal chat prompts
        easily exceed 2 tokens but fall far below 99.  So if env wins,
        ``routes_total`` on node1 increases after a few requests; if YAML
        wins, it stays flat.
        """
        print("\nTest: Environment variables override YAML")

        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]
        container = CONTAINER_NAMES["node1"]

        # Wait out any residual RELOAD_COOLDOWN from test_01's cleanup SIGHUP.
        # test_01 ends with a SIGHUP + 3s settle, so ~9s of cooldown remains
        # when we enter here.  12s is the safe margin used throughout the
        # sibling suites.
        print(f"  Waiting {_RELOAD_COOLDOWN_WAIT}s for reload cooldown to expire...")
        time.sleep(_RELOAD_COOLDOWN_WAIT)

        # Baseline: how many routes has node1 learned so far?  The cluster
        # has been serving health and warm-up traffic since setUpClass, so
        # the counter is rarely zero -- we rely on a DELTA, not an absolute.
        initial_routes = sum_metric_by_substring(node1_metrics, "routes_total")
        print(f"  Initial routes_total on node1: {initial_routes}")

        # YAML sets an impossibly high threshold.  If env didn't override
        # YAML, the following warm-up traffic would NOT learn any routes.
        yaml_text = (
            "routing:\n"
            "  min_token_length: 99\n"
        )

        pre_logs = _get_container_logs(container, tail=500)
        pre_line_count = pre_logs.count("\n")

        print(f"  Writing YAML with routing.min_token_length=99 to {container}...")
        _write_container_config(container, yaml_text)

        try:
            print("  Sending SIGHUP to apply YAML (env should override)...")
            _send_sighup(container)

            print("  Waiting for 'Configuration reloaded successfully' log line...")
            wait_for_reload_success(self, container, pre_line_count)

            # Give shards a moment for the new config to settle on every core
            # (invoke_on_all finished before the log line, but route-learning
            # reads shard-local config -- small settle avoids a race).
            time.sleep(1)

            # Send a handful of chat requests with distinct, moderately-long
            # prompts.  Each prompt tokenizes to well over 2 tokens (env
            # threshold) but well under 99 (YAML threshold).  Distinct
            # content makes each request a new prefix in the ART, so each
            # should produce a fresh entry in routes_total.
            num_requests = 6
            print(f"  Sending {num_requests} learning-eligible requests to node1...")
            successful = 0
            for i in range(num_requests):
                prompt = (
                    f"env-override-verification-request-{i} "
                    "please reply with any content so route learning can run"
                )
                status, _, _ = send_chat_request(
                    node1_api, [{"role": "user", "content": prompt}],
                    timeout=15, retries=1,
                )
                if status == 200:
                    successful += 1

            self.assertGreater(
                successful, 0,
                f"At least some requests should succeed ({successful}/{num_requests})"
            )

            # Allow the batched route-learning buffer to flush into the ART
            # before we sample the metric.  router_service batches local
            # route learning (see comment near RouterService::learn_route_global).
            time.sleep(3)

            final_routes = sum_metric_by_substring(node1_metrics, "routes_total")
            delta = final_routes - initial_routes
            print(f"  Final routes_total on node1: {final_routes} (delta {delta:+.0f})")

            # If YAML had won, min_token_length would be 99 and NONE of our
            # short prompts would trigger learning -- delta would be 0.  A
            # positive delta is the observable proof that env overrode YAML.
            self.assertGreater(
                delta, 0,
                f"routes_total should increase after sending learning-eligible "
                f"requests if env (RANVIER_MIN_TOKEN_LENGTH=2) overrode YAML "
                f"(min_token_length=99).  Observed delta={delta}. "
                f"A zero delta means YAML's 99 won, i.e. env override is broken."
            )

        finally:
            print("  Cleanup: removing config file and reloading defaults...")
            _remove_container_config(container)
            time.sleep(_RELOAD_COOLDOWN_WAIT)
            try:
                _send_sighup(container)
                time.sleep(3)
            except RuntimeError as e:
                print(f"    Cleanup SIGHUP failed (non-fatal): {e}")

        print("  PASSED: Env var RANVIER_MIN_TOKEN_LENGTH=2 overrode YAML=99")

    # =========================================================================
    # Test 3: --dry-run validates without starting the server
    # =========================================================================

    def test_03_dry_run_validates_without_starting(self):
        """Run ``./ranvier_server --dry-run`` via docker exec and assert it
        validates + exits without touching the live server.

        main.cpp short-circuits on --dry-run at line 463 -- before any
        Seastar initialization -- so no --smp/--memory args are needed and
        the running parent process is unaffected.  Output format is fixed
        by run_dry_run_validation() at src/main.cpp:114:
          * Banner: ``Ranvier Core - Dry Run Validation``
          * Result:  ``Result: PASSED`` (exit 0) or ``Result: FAILED`` (exit 1)

        Whether /tmp/ranvier.yaml exists at exec time is irrelevant: dry-run
        handles "file not found, using defaults" identically.  We run with
        the container's default env (valid, since the cluster is healthy),
        which means PASSED is the expected outcome.
        """
        print("\nTest: --dry-run validates without starting the server")

        node1_api = NODES["node1"]["api"]
        container = CONTAINER_NAMES["node1"]

        # Sanity: node1 is healthy before we exec anything
        pre_status, _, _ = send_chat_request(
            node1_api, [{"role": "user", "content": "pre-dry-run-check"}],
            timeout=10, retries=1,
        )
        self.assertEqual(
            pre_status, 200,
            "Node1 should be healthy before --dry-run test"
        )

        # Run the dry-run validation inside the container.  The binary lives
        # at ./ranvier_server in the workdir (see docker-compose.test.yml).
        # --dry-run is parsed before Seastar starts, so no --smp/--memory is
        # required; we intentionally omit them to also regression-test that
        # the short-circuit really happens before Seastar's arg parser.
        print(f"  Running: docker exec {container} ./ranvier_server --dry-run "
              f"--config {_CONTAINER_CONFIG_PATH}")
        result = subprocess.run(
            ["docker", "exec", container,
             "./ranvier_server", "--dry-run", "--config", _CONTAINER_CONFIG_PATH],
            capture_output=True, text=True, timeout=30,
        )

        stdout = result.stdout or ""
        stderr = result.stderr or ""
        print(f"  Exit code: {result.returncode}")
        # Show a trimmed dump for easy post-mortem on CI failures
        if result.returncode != 0 or "PASSED" not in stdout:
            print(f"  STDOUT:\n{stdout}")
            print(f"  STDERR:\n{stderr}")

        self.assertEqual(
            result.returncode, 0,
            f"--dry-run should exit 0 on a valid config.  Exit={result.returncode}. "
            f"stdout tail:\n{stdout[-1000:]}\nstderr tail:\n{stderr[-500:]}"
        )
        self.assertIn(
            "Dry Run Validation", stdout,
            "Expected 'Dry Run Validation' banner in --dry-run stdout"
        )
        self.assertIn(
            "PASSED", stdout,
            "Expected 'PASSED' result line in --dry-run stdout"
        )

        # Crucial side-effect check: --dry-run must NOT have disrupted the
        # parent ranvier_server process that's serving traffic.  If the
        # exec had somehow signalled the parent (it can't, but prove it),
        # this request would hang or return non-200.
        print("  Verifying the running server is unaffected by the exec...")
        post_status, _, _ = send_chat_request(
            node1_api, [{"role": "user", "content": "post-dry-run-check"}],
            timeout=10, retries=1,
        )
        self.assertEqual(
            post_status, 200,
            "Node1 should still serve requests after --dry-run exec"
        )

        print("  PASSED: --dry-run validated config, exited 0, live server unaffected")

    # =========================================================================
    # Test 4: --dry-run with invalid config fails loudly
    # =========================================================================

    def test_04_dry_run_with_invalid_config_fails(self):
        """Write a clearly-invalid YAML and confirm --dry-run rejects it
        with a non-zero exit code and a FAILED result line.

        ``server.api_port: 0`` triggers the first rule in
        RanvierConfig::validate() (config_loader.cpp:1569): returns
        ``"server.api_port must be non-zero"``.  The dry-run summary then
        prints ``Result: FAILED (<n> errors)`` and exits 1.

        docker-compose.test.yml does NOT set RANVIER_API_PORT, so our YAML
        value really reaches validation -- apply_env_overrides() can't
        rescue it.
        """
        print("\nTest: --dry-run rejects invalid config")

        container = CONTAINER_NAMES["node1"]

        # Deliberately invalid: api_port must be non-zero per validate().
        yaml_text = (
            "server:\n"
            "  api_port: 0\n"
        )

        print(f"  Writing invalid YAML (api_port: 0) to {container}...")
        _write_container_config(container, yaml_text)

        try:
            print(f"  Running: docker exec {container} ./ranvier_server --dry-run "
                  f"--config {_CONTAINER_CONFIG_PATH}")
            result = subprocess.run(
                ["docker", "exec", container,
                 "./ranvier_server", "--dry-run", "--config", _CONTAINER_CONFIG_PATH],
                capture_output=True, text=True, timeout=30,
            )

            stdout = result.stdout or ""
            stderr = result.stderr or ""
            print(f"  Exit code: {result.returncode}")
            # Dump on unexpected success -- easier post-mortem than the bare assert
            if result.returncode == 0 or "FAILED" not in stdout:
                print(f"  STDOUT:\n{stdout}")
                print(f"  STDERR:\n{stderr}")

            self.assertEqual(
                result.returncode, 1,
                f"--dry-run should exit 1 on invalid config (api_port: 0), "
                f"got {result.returncode}.\n"
                f"stdout tail:\n{stdout[-1000:]}\nstderr tail:\n{stderr[-500:]}"
            )
            self.assertIn(
                "FAILED", stdout,
                "Expected 'FAILED' result line in --dry-run stdout for invalid config"
            )

        finally:
            # Leave the container's config pristine for any later tests.
            # No SIGHUP needed: the live parent process never loaded this
            # file (it was only read by the exec'd --dry-run child).
            print("  Cleanup: removing invalid config file...")
            _remove_container_config(container)

        print("  PASSED: --dry-run correctly rejected invalid config with exit 1")

    # =========================================================================
    # Test 5: Invalid YAML at startup produces a clear error
    # =========================================================================

    def test_05_invalid_yaml_at_startup_produces_clear_error(self):
        """Malformed YAML on startup must either crash with a clear error
        message or fall back cleanly to defaults -- never produce an opaque
        crash (raw libyaml stack trace, SIGSEGV, etc).

        Expected paths in the code:
          * YAML parse error -> RanvierConfig::load() throws
            ``std::runtime_error("Failed to parse config file: ...")``
            (config_loader.cpp:1555).
          * main.cpp catches and prints ``Failed to load config: ...`` to
            stderr, returns 1 (main.cpp:474-476).
          * If the file DOESN'T exist, load() returns defaults + env
            overrides (config_loader.cpp:796-799) and startup proceeds.

        Note on tmpfs behavior: docker-compose.test.yml declares /tmp as a
        tmpfs mount, and Docker tears down tmpfs on container stop.  After
        ``stop`` + ``start``, the YAML we wrote may or may not survive --
        this depends on the local Docker version/storage driver.  Both
        outcomes satisfy the §6.6 requirement ("invalid config produces a
        clear error, not a stack trace"): either we see the clean error
        line in logs, or the server came up cleanly because the tmpfs was
        wiped.  The test branches on the observed state.
        """
        print("\nTest: Invalid YAML at startup produces a clear error")

        node1_api = NODES["node1"]["api"]
        node1_metrics = NODES["node1"]["metrics"]
        container = CONTAINER_NAMES["node1"]

        # Pre-check: ranvier1 is healthy and has a running main process
        self.assertTrue(
            check_container_running(container, self.PROJECT_NAME),
            "ranvier1 should be running before the startup-error test"
        )

        # Snapshot current log length so we only examine the output that
        # appears AFTER our stop/start.  Pre-existing "Failed to parse"
        # lines from earlier tests (none expected, but be defensive) would
        # otherwise poison the assertion.
        pre_logs = _get_container_logs(container, tail=1000)
        pre_line_count = pre_logs.count("\n")

        # Write deliberately malformed YAML.  YAML::Load() throws on this
        # because of the unmatched braces and the bare colon sequence.
        malformed_yaml = "{{invalid: yaml: [unterminated"
        print("  Writing malformed YAML to container...")
        _write_container_config(container, malformed_yaml)

        # Drive the container through stop -> start.  run_compose(check=False)
        # so an exit-on-start (if tmpfs preserved the bad file) doesn't
        # raise inside this test -- we want to inspect the resulting state
        # rather than fail on a subprocess error.
        print("  Stopping ranvier1 via docker-compose...")
        run_compose(
            ["stop", "ranvier1"],
            project_name=self.PROJECT_NAME,
            check=False,
        )
        print("  Starting ranvier1 via docker-compose...")
        run_compose(
            ["start", "ranvier1"],
            project_name=self.PROJECT_NAME,
            check=False,
        )

        # Give the server a moment either to come up healthy or to exit
        # with a parse error.  wait_for_healthy returns False quickly if
        # the container has already exited (fast-fail via check_container_running).
        print("  Observing post-restart state (up to 25s)...")
        became_healthy = wait_for_healthy(
            f"{node1_metrics}/metrics",
            timeout=25,
            container_name=container,
            project_name=self.PROJECT_NAME,
        )

        post_logs = _get_container_logs(container, tail=1000)
        new_logs = "\n".join(post_logs.split("\n")[pre_line_count:])

        try:
            if became_healthy:
                # Tmpfs wiped the YAML during stop; server loaded defaults.
                # Per the BACKLOG §6.6 intent, this is an acceptable outcome.
                # We still make sure the server didn't silently log a raw
                # libyaml stack trace or other crash noise just before
                # recovering.
                print("  Container came up healthy (tmpfs wiped the bad YAML).")
                self.assertNotIn(
                    "terminate called",
                    new_logs,
                    "Startup logs contain 'terminate called' -- looks like a "
                    "raw abort, not a clean error path"
                )
                # Confirm end-to-end request path works post-restart
                status, _, _ = send_chat_request(
                    node1_api, [{"role": "user", "content": "post-restart-check"}],
                    timeout=10, retries=1,
                )
                self.assertEqual(
                    status, 200,
                    "ranvier1 should serve requests after restart with defaults"
                )
            else:
                # Container failed to come up -- the bad YAML must have
                # persisted across the stop.  Exactly the path that §6.6
                # wants us to validate: the error message should be clear
                # and caller-readable.
                print("  Container did NOT become healthy -- checking for clear error...")
                self.assertIn(
                    "Failed to parse config",
                    new_logs,
                    "Expected the clear error message 'Failed to parse config' "
                    "(config_loader.cpp:1555 -> main.cpp:475) when startup "
                    "rejects malformed YAML. Full new-log tail:\n"
                    f"{new_logs[-2000:]}"
                )
                # Also guard against the error being buried under a raw
                # C++ abort.  "terminate called" indicates an uncaught
                # exception or abort() -- not what we want users to see.
                self.assertNotIn(
                    "terminate called",
                    new_logs,
                    "Startup logs contain 'terminate called' -- a clear error "
                    "message was expected, not a raw abort.  Log tail:\n"
                    f"{new_logs[-2000:]}"
                )

        finally:
            # Restore ranvier1 to a healthy state for any later work.
            # Order matters: if the container is running, we can simply
            # wipe the file and SIGHUP-reload.  If it has exited (stuck in
            # a crash loop on the bad YAML), we must force-recreate so a
            # fresh tmpfs mounts over the bad config.
            print("  Cleanup: restoring ranvier1 to healthy state...")
            if check_container_running(container, self.PROJECT_NAME):
                _remove_container_config(container)
                # The live parent hasn't loaded this file (it only sees it
                # via SIGHUP), so no reload is strictly required -- but be
                # thorough in case a later test assumes defaults are active.
                time.sleep(_RELOAD_COOLDOWN_WAIT)
                try:
                    _send_sighup(container)
                    time.sleep(3)
                except RuntimeError as e:
                    print(f"    Cleanup SIGHUP failed (non-fatal): {e}")
            else:
                print("    Container exited -- forcing recreate for a fresh tmpfs...")
                run_compose(
                    ["up", "-d", "--force-recreate", "--no-deps", "ranvier1"],
                    project_name=self.PROJECT_NAME,
                    check=False,
                    show_output=True,
                )
                recovered = wait_for_healthy(
                    f"{node1_metrics}/metrics",
                    timeout=STARTUP_TIMEOUT,
                    container_name=container,
                    project_name=self.PROJECT_NAME,
                )
                if not recovered:
                    print("    WARNING: ranvier1 did not recover; later tests "
                          "in this class may fail.")

        print("  PASSED: Invalid YAML at startup produces clear error or clean fallback")

    # =========================================================================
    # Test 6: Missing config file falls back to defaults + env overrides
    # =========================================================================

    def test_06_missing_config_file_uses_defaults(self):
        """When /tmp/ranvier.yaml doesn't exist, startup must use built-in
        defaults (config_infra.hpp) with env-var overrides layered on top.

        Code path: config_loader.cpp:796-799 -- if the ifstream fails to
        open the file, ``RanvierConfig::load()`` calls
        apply_env_overrides() on a freshly-default-constructed config and
        returns it.  The default ``server.metrics_port`` is 9180
        (config_infra.hpp:35); no env var in docker-compose.test.yml
        overrides it, so the effective port stays 9180.

        We prove the defaults path by:
          * deleting any lingering /tmp/ranvier.yaml,
          * driving a full stop/start cycle so the server re-enters
            RanvierConfig::load() from scratch,
          * querying the metrics endpoint both externally (via the
            9181->9180 port mapping) and internally (docker exec curl
            http://localhost:9180/metrics) -- the internal probe is
            what actually proves the server bound to 9180 rather than
            some other default.
        """
        print("\nTest: Missing config file uses built-in defaults")

        node1_metrics = NODES["node1"]["metrics"]
        container = CONTAINER_NAMES["node1"]

        # Idempotently ensure no YAML exists.  If the live container is
        # already down from a failed earlier test, this silently no-ops.
        print(f"  Removing {_CONTAINER_CONFIG_PATH} (if present)...")
        if check_container_running(container, self.PROJECT_NAME):
            _remove_container_config(container)
        else:
            # Container is down -- nothing to remove; stop/start below
            # will start from a fresh tmpfs anyway.
            print("    Container is not running; tmpfs is already gone.")

        # Drive a clean startup.  stop/start (rather than SIGHUP) is the
        # correct primitive here: SIGHUP runs reload_config() against the
        # already-initialized server, which skips the startup-only code
        # paths we actually want to exercise (port binding, etc.).
        print("  Stopping ranvier1...")
        run_compose(
            ["stop", "ranvier1"],
            project_name=self.PROJECT_NAME,
            check=False,
        )
        print("  Starting ranvier1...")
        run_compose(
            ["start", "ranvier1"],
            project_name=self.PROJECT_NAME,
            check=False,
        )

        # Wait for ranvier1 to come up healthy on its (default) metrics
        # port.  Failure here would mean either the defaults themselves
        # don't produce a valid config (regression), or the env overrides
        # on a fresh config contradict each other -- both worth catching.
        print("  Waiting for ranvier1 to become healthy...")
        healthy = wait_for_healthy(
            f"{node1_metrics}/metrics",
            timeout=STARTUP_TIMEOUT,
            container_name=container,
            project_name=self.PROJECT_NAME,
        )
        self.assertTrue(
            healthy,
            "ranvier1 should start healthy when the config file is absent "
            "(defaults + env overrides path)"
        )

        # External probe: /metrics on 9181 (host) -> 9180 (container).
        # A 200 here proves the mapping-target port is actually open,
        # which is only true if the server bound to 9180 internally.
        print(f"  External probe: {node1_metrics}/metrics ...")
        ext_resp = requests.get(f"{node1_metrics}/metrics", timeout=5)
        self.assertEqual(
            ext_resp.status_code, 200,
            f"External /metrics should return 200 (via 9181->9180 mapping), "
            f"got {ext_resp.status_code}"
        )
        self.assertIn(
            "#", ext_resp.text,
            "Prometheus metrics output should contain '# HELP' / '# TYPE' comments"
        )

        # Internal probe: confirm the server bound to exactly port 9180
        # (the built-in default from config_infra.hpp).  A different
        # default (say 9090) would silently pass the external probe if
        # the compose mapping happened to match -- the internal curl
        # removes that ambiguity.  ``docker exec`` uses the container's
        # own network namespace, so localhost:9180 is the bind address.
        print(f"  Internal probe: docker exec {container} curl http://localhost:9180/metrics ...")
        int_result = subprocess.run(
            ["docker", "exec", container,
             "curl", "-sf", "-o", "/dev/null", "-w", "%{http_code}",
             "http://localhost:9180/metrics"],
            capture_output=True, text=True, timeout=10,
        )
        http_code = (int_result.stdout or "").strip()
        self.assertEqual(
            http_code, "200",
            f"Internal curl on http://localhost:9180/metrics should return 200 "
            f"(proving server bound to default metrics_port=9180). "
            f"Got exit={int_result.returncode}, http_code={http_code!r}, "
            f"stderr={int_result.stderr!r}"
        )

        # No cleanup step required: ranvier1 is healthy with no config
        # file, which is the correct steady state for this project (env
        # vars supply everything the tests need).

        print("  PASSED: Missing config file -> defaults + env overrides, metrics on :9180")


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
    print("  - Environment variables override YAML")
    print("  - --dry-run validates without starting the server")
    print("  - --dry-run rejects invalid config with exit 1")
    print("  - Invalid YAML at startup produces a clear error (or clean fallback)")
    print("  - Missing config file uses built-in defaults + env overrides")
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
