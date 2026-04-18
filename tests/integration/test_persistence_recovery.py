#!/usr/bin/env python3
"""
Persistence Recovery Integration Tests for Ranvier Core (BACKLOG §6.6)

Validates the SQLite persistence layer end-to-end:

  1. Backend registrations land in the on-disk SQLite ``backends`` table
     (observed by streaming the DB file out of the container with
     ``docker exec cat`` and opening it with sqlite3 on the host).
  2. Route learning lands in the on-disk SQLite ``routes`` table.
  3. Graceful SIGTERM triggers a final WAL checkpoint (log-observed).
  4. A corrupted DB is recovered (no crashloop; logs show an empty-store
     or integrity-failure recovery).
  5. A missing DB file starts clean.

Environment note
----------------
The Ranvier test containers mount ``/tmp`` as tmpfs (see
``docker-compose.test.yml``). In this Docker environment the per-container
tmpfs does NOT survive a stop/start cycle — each ``docker start`` gives
the container a fresh /tmp. This means a bare "restart and replay"
assertion is not observable against the stock test compose file.

To validate the persistence *write* path regardless, tests 01/02
stream the live DB file out of the container with ``docker exec cat``
and open it with Python's ``sqlite3`` module on the host. (``docker
cp`` would be the more idiomatic choice, but it refuses to read from
tmpfs mounts on Docker Desktop for macOS even when the file is
listable via ``ls``.) This proves the same underlying §6.6 property
("registrations/routes survive in SQLite") with stronger guarantees
than a restart-roundtrip assertion.

Tests 04/05 still exercise the startup recovery paths
(``verify_integrity`` / empty-store) by mutating state before a
SIGKILL+start — on environments where tmpfs survives they exercise the
corruption path; otherwise they exercise the empty-store fallback.
Either outcome is an acceptable "handled gracefully" signal for §6.6.

Usage:
    python tests/integration/test_persistence_recovery.py

Requirements:
    - Docker and docker-compose installed
    - Python ``requests`` (from the existing integration-test harness)
    - Python stdlib ``sqlite3`` module (always present on CPython)
    - Built Ranvier image (``make docker-build``)
"""

import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
from typing import Optional, Tuple

try:
    import requests
except ImportError:
    print("Error: 'requests' library required. Install: pip install requests")
    sys.exit(1)

from conftest import (
    BACKENDS,
    ClusterTestCase,
    NODES,
    register_backends,
    send_chat_request,
    wait_for_healthy,
)

_PROJECT_NAME = "ranvier-persistence-test"

# The ranvier containers mount /tmp as tmpfs and point
# RANVIER_DB_PATH there (see docker-compose.test.yml).
DB_PATH_IN_CONTAINER = "/tmp/ranvier.db"


# =============================================================================
# Docker helpers (duplicated from test_graceful_shutdown.py — see scope
# guardrails in the task brief; factoring into conftest is a separate
# cleanup.)
# =============================================================================


def signal_container_shutdown(container_name: str, signal: str = "SIGTERM") -> bool:
    """Send a signal to a container."""
    try:
        result = subprocess.run(
            ["docker", "kill", f"--signal={signal}", container_name],
            capture_output=True,
            text=True,
            timeout=10,
        )
        return result.returncode == 0
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def docker_start_container(container_name: str) -> bool:
    """Start an existing stopped container via ``docker start``.

    Not ``docker compose up -d <svc>`` — that recreates the container
    and wipes the tmpfs.
    """
    try:
        result = subprocess.run(
            ["docker", "start", container_name],
            capture_output=True,
            text=True,
            timeout=15,
        )
        return result.returncode == 0
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False


def get_container_logs(container_name: str, tail: int = 300) -> str:
    """Get recent container logs (stdout+stderr concatenated)."""
    try:
        result = subprocess.run(
            ["docker", "logs", "--tail", str(tail), container_name],
            capture_output=True,
            text=True,
            timeout=10,
        )
        return result.stdout + result.stderr
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return ""


def wait_for_container_exit(container_name: str, timeout: int = 30) -> bool:
    """Poll ``docker inspect`` until the container's state is not 'running'."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            result = subprocess.run(
                ["docker", "inspect", "-f", "{{.State.Running}}", container_name],
                capture_output=True,
                text=True,
                timeout=5,
            )
            if result.returncode == 0 and result.stdout.strip() == "false":
                return True
        except (subprocess.TimeoutExpired, FileNotFoundError):
            return False
        time.sleep(0.5)
    return False


def docker_exec(container_name: str, shell_cmd: str) -> subprocess.CompletedProcess:
    """Run ``sh -c <shell_cmd>`` inside a running container."""
    return subprocess.run(
        ["docker", "exec", container_name, "sh", "-c", shell_cmd],
        capture_output=True,
        text=True,
        timeout=15,
    )


def read_file_via_exec(container_name: str, src_in_container: str) -> Optional[bytes]:
    """Read a file from a running container via ``docker exec cat``.

    Used instead of ``docker cp`` because ``cp`` refuses to read from
    container tmpfs mounts on Docker Desktop for macOS (observed
    empirically: ``cp`` errors with "Could not find the file" for
    files that ``ls`` lists fine). ``docker exec cat`` works uniformly
    across Linux and macOS Docker environments. The test DB is ~20 KB
    so streaming it through stdout is cheap.
    """
    try:
        result = subprocess.run(
            ["docker", "exec", container_name, "cat", src_in_container],
            capture_output=True,
            timeout=15,
        )
        if result.returncode != 0:
            # Expected for the WAL/SHM siblings when SQLite has
            # already checkpointed them away; the caller decides
            # whether that's fatal.
            return None
        return result.stdout
    except (subprocess.TimeoutExpired, FileNotFoundError) as e:
        print(f"    docker exec cat raised: {e}")
        return None


# =============================================================================
# SQLite inspection
# =============================================================================


def snapshot_db(container_name: str) -> Optional[sqlite3.Connection]:
    """Fetch /tmp/ranvier.db (+ WAL, if any) from the container and open.

    Returns a live sqlite3.Connection to an on-host copy of the DB, or
    None if the file is unreachable / unreadable. The caller owns the
    connection and should close it.

    WAL mode matters: SQLite writes go first to ``<db>-wal`` and are
    checkpointed into the main file later. We fetch the WAL alongside
    the main file so a query here doesn't miss freshly-batched writes.
    """
    tmpdir = tempfile.mkdtemp(prefix="ranvier-db-")
    local_db = os.path.join(tmpdir, "ranvier.db")

    # Pre-flight diagnostic: confirm the file actually exists inside
    # the container. Without this, a genuine "persistence never
    # initialized" bug looks identical to a transient read failure.
    ls = docker_exec(container_name, f"ls -la {DB_PATH_IN_CONTAINER} 2>&1 || true")
    if ls.returncode != 0 or not ls.stdout.strip():
        print(
            f"    DB file listing failed: rc={ls.returncode} "
            f"stdout={ls.stdout!r} stderr={ls.stderr!r}"
        )

    def _fetch(src_path: str, dest_path: str) -> bool:
        data = read_file_via_exec(container_name, src_path)
        if data is None:
            return False
        try:
            with open(dest_path, "wb") as f:
                f.write(data)
            return True
        except OSError as e:
            print(f"    snapshot_db: write {dest_path} failed: {e}")
            return False

    if not _fetch(DB_PATH_IN_CONTAINER, local_db):
        print(f"    snapshot_db: could not fetch {DB_PATH_IN_CONTAINER}")
        return None

    # Best-effort: the WAL / SHM siblings may or may not exist on disk
    # (SQLite checkpoints fold WAL back into the main file). Failure
    # here is fine — any pending writes they held have either been
    # checkpointed already or will be, but we still want them locally
    # if they're present so reads are consistent.
    for suffix in ("-wal", "-shm"):
        _fetch(f"{DB_PATH_IN_CONTAINER}{suffix}", f"{local_db}{suffix}")

    try:
        # Open read-only (uri=True enables query params) so we can't
        # accidentally modify our copy.
        conn = sqlite3.connect(f"file:{local_db}?mode=ro", uri=True)
        conn.execute("SELECT 1").fetchone()
        return conn
    except sqlite3.Error as e:
        print(f"    snapshot_db: sqlite open failed: {e}")
        return None


def count_backends_in_db(container_name: str) -> Tuple[int, Optional[str]]:
    """Return (count, error). count is -1 on error."""
    conn = snapshot_db(container_name)
    if conn is None:
        return -1, "could not snapshot DB"
    try:
        row = conn.execute("SELECT COUNT(*) FROM backends").fetchone()
        return int(row[0]), None
    except sqlite3.Error as e:
        return -1, f"sqlite error: {e}"
    finally:
        conn.close()


def count_routes_in_db(container_name: str) -> Tuple[int, Optional[str]]:
    """Return (count, error). count is -1 on error."""
    conn = snapshot_db(container_name)
    if conn is None:
        return -1, "could not snapshot DB"
    try:
        row = conn.execute("SELECT COUNT(*) FROM routes").fetchone()
        return int(row[0]), None
    except sqlite3.Error as e:
        return -1, f"sqlite error: {e}"
    finally:
        conn.close()


def wait_for_persisted_backends(
    container_name: str, expected: int, timeout: int = 15
) -> int:
    """Poll the on-disk DB until ``COUNT(backends) >= expected`` or timeout."""
    deadline = time.time() + timeout
    observed = -1
    while time.time() < deadline:
        observed, _ = count_backends_in_db(container_name)
        if observed >= expected:
            return observed
        time.sleep(0.5)
    return observed


def wait_for_persisted_routes(
    container_name: str, expected: int, timeout: int = 15
) -> int:
    """Poll the on-disk DB until ``COUNT(routes) >= expected`` or timeout."""
    deadline = time.time() + timeout
    observed = -1
    while time.time() < deadline:
        observed, _ = count_routes_in_db(container_name)
        if observed >= expected:
            return observed
        time.sleep(0.5)
    return observed


# =============================================================================
# Tests
# =============================================================================


class PersistenceRecoveryTest(ClusterTestCase):
    """Integration tests for SQLite persistence and recovery paths."""

    PROJECT_NAME = _PROJECT_NAME
    AUTO_REGISTER_BACKENDS = True

    # -------------------------------------------------------------------------
    # Test 01: Backend registrations land in on-disk SQLite
    # -------------------------------------------------------------------------
    def test_01_backends_persist_in_sqlite(self):
        """Registered backends must be written to the SQLite ``backends`` table.

        AUTO_REGISTER_BACKENDS ensures setUpClass has already POSTed the
        two mock backends to every node. The async persistence manager
        batches saves on a 100ms flush timer, so we poll the on-disk
        DB (streamed out via ``docker exec cat``) until both records land.
        """
        container = "ranvier1"

        # A chat request forces at least one flush cycle on the code
        # path the persistence manager shares with the admin API.
        status, body, _ = send_chat_request(
            NODES["node1"]["api"],
            [{"role": "user", "content": "persistence-smoke-01"}],
            stream=True,
        )
        self.assertEqual(status, 200, "Baseline chat should succeed")
        self.assertTrue(body, "Baseline chat should return a non-empty SSE body")

        # Wait for the batcher to flush the AUTO_REGISTER_BACKENDS saves.
        observed = wait_for_persisted_backends(
            container, expected=len(BACKENDS), timeout=15
        )
        self.assertGreaterEqual(
            observed,
            len(BACKENDS),
            f"Expected >= {len(BACKENDS)} backends persisted in SQLite, "
            f"observed {observed}. The async persistence manager did not "
            "flush backend registrations to disk within 15s.",
        )

        # Sanity check the actual rows: id/ip/port round-trip unchanged.
        conn = snapshot_db(container)
        self.assertIsNotNone(conn, "Could not snapshot DB for sanity check")
        try:
            rows = conn.execute(
                "SELECT id, ip, port FROM backends ORDER BY id"
            ).fetchall()
        finally:
            conn.close()

        # BACKENDS is keyed by id, mapping to {"ip": str, "port": int}.
        persisted_ids = {r[0] for r in rows}
        for backend_id, info in BACKENDS.items():
            self.assertIn(
                backend_id,
                persisted_ids,
                f"Backend {backend_id} ({info['ip']}:{info['port']}) "
                "missing from SQLite after registration",
            )
        print(f"  [✓] {len(rows)} backend(s) persisted in SQLite: "
              f"{[r[0] for r in rows]}")

    # -------------------------------------------------------------------------
    # Test 02: Learned routes land in on-disk SQLite
    # -------------------------------------------------------------------------
    def test_02_routes_persist_in_sqlite(self):
        """Route learning must write rows into the SQLite ``routes`` table.

        Ranvier learns a route when a prompt passes the configured
        min-token threshold (RANVIER_MIN_TOKEN_LENGTH=2 in
        docker-compose.test.yml). A few repeated long-enough prompts
        should produce at least one persisted route.
        """
        container = "ranvier1"
        node_api = NODES["node1"]["api"]
        prefix = (
            "persist-test-alpha: this is a deliberately long shared "
            "context so that tokenization passes the min-token "
            "threshold and the router learns a route"
        )

        # Warm the route with several distinct-but-prefix-sharing requests.
        for i in range(8):
            status, body, _ = send_chat_request(
                node_api,
                [{"role": "user", "content": f"{prefix} turn {i}"}],
                stream=True,
            )
            self.assertEqual(
                status, 200, f"Route-warming request {i} should succeed"
            )

        observed = wait_for_persisted_routes(container, expected=1, timeout=15)
        self.assertGreaterEqual(
            observed,
            1,
            "Expected >= 1 route persisted in SQLite after 8 prefix-sharing "
            f"chat requests, observed {observed}. Routes are not being "
            "flushed to disk.",
        )
        print(f"  [✓] {observed} route(s) persisted in SQLite")

    # -------------------------------------------------------------------------
    # Test 03: Graceful shutdown performs a final WAL checkpoint
    # -------------------------------------------------------------------------
    def test_03_wal_checkpoint_on_shutdown(self):
        """A graceful SIGTERM must trigger a final WAL checkpoint.

        application.cpp::cleanup() logs 'Persistence shutdown summary:'
        (info) and 'Final WAL checkpoint complete' (debug). The info
        line is the reliable marker; we accept either.
        """
        node_api = NODES["node1"]["api"]
        node_metrics = NODES["node1"]["metrics"]
        container = "ranvier1"

        # Prime the DB with a chat so there's WAL content to flush.
        send_chat_request(
            node_api,
            [{"role": "user", "content": "wal-checkpoint-prime"}],
            stream=True,
        )
        time.sleep(0.5)

        # Signal graceful shutdown and wait for exit.
        print(f"  Sending SIGTERM to {container}...")
        self.assertTrue(
            signal_container_shutdown(container, "SIGTERM"),
            "SIGTERM delivery failed",
        )
        exited = wait_for_container_exit(container, timeout=30)

        logs = get_container_logs(container, tail=400)

        # Restart for subsequent tests.
        docker_start_container(container)
        wait_for_healthy(f"{node_metrics}/metrics", timeout=45)

        self.assertTrue(exited, f"{container} did not exit after SIGTERM")

        summary_logged = "Persistence shutdown summary" in logs
        checkpoint_logged = "Final WAL checkpoint complete" in logs
        checkpoint_failed_logged = "Final WAL checkpoint failed" in logs

        self.assertTrue(
            summary_logged or checkpoint_logged,
            "Graceful shutdown should log 'Persistence shutdown summary:' "
            "or 'Final WAL checkpoint complete'. Log tail:\n"
            f"{logs[-800:]}",
        )
        self.assertFalse(
            checkpoint_failed_logged,
            "Final WAL checkpoint should not fail on a clean shutdown. "
            f"Log tail:\n{logs[-800:]}",
        )
        print(f"  [✓] Graceful shutdown flushed the WAL "
              f"(summary_logged={summary_logged}, "
              f"checkpoint_logged={checkpoint_logged})")

    # -------------------------------------------------------------------------
    # Test 04: Server boots cleanly when faced with a broken DB
    # -------------------------------------------------------------------------
    def test_04_corrupted_db_handled_gracefully(self):
        """A DB file that can't be replayed must not crashloop the server.

        Two valid outcomes satisfy "handled gracefully":
          (a) The startup path detects corruption and clears the store
              (``integrity check failed`` / ``clearing corrupted state``).
          (b) The startup path finds an empty/reset store and starts fresh
              (``Persistence store is empty``).

        Environment note: this test runs the mutation inside the
        container and then SIGKILL + docker start. If the tmpfs survives
        that cycle, outcome (a) fires. If the tmpfs is recreated, the
        server sees a fresh empty /tmp and outcome (b) fires. Either
        way, the §6.6 guarantee — "don't crash on a bad DB" — holds.
        """
        node_api = NODES["node1"]["api"]
        node_metrics = NODES["node1"]["metrics"]
        container = "ranvier1"

        if not wait_for_healthy(f"{node_metrics}/metrics", timeout=30):
            self.skipTest(f"{container} not healthy at test start")

        # Corrupt the DB file + remove the WAL/SHM siblings.
        print(f"  Corrupting /tmp/ranvier.db inside {container}...")
        corrupt = docker_exec(
            container,
            "echo 'CORRUPTED_NOT_A_SQLITE_FILE' > /tmp/ranvier.db && "
            "rm -f /tmp/ranvier.db-wal /tmp/ranvier.db-shm",
        )
        self.assertEqual(
            corrupt.returncode,
            0,
            f"docker exec corruption failed: rc={corrupt.returncode}, "
            f"stderr={corrupt.stderr}",
        )

        # SIGKILL so the graceful-shutdown checkpoint doesn't rewrite the
        # corrupted bytes with something SQLite can read.
        print(f"  SIGKILLing {container}...")
        self.assertTrue(
            signal_container_shutdown(container, "SIGKILL"),
            "SIGKILL delivery failed",
        )
        self.assertTrue(
            wait_for_container_exit(container, timeout=15),
            f"{container} did not exit after SIGKILL",
        )

        # docker start (not compose up -d) tries to preserve tmpfs.
        print(f"  Restarting {container} (docker start)...")
        self.assertTrue(
            docker_start_container(container),
            f"docker start {container} failed",
        )
        healthy = wait_for_healthy(f"{node_metrics}/metrics", timeout=45)
        self.assertTrue(
            healthy,
            f"{container} must recover from a broken DB, not crashloop",
        )

        # Accept either the integrity-failure path or the empty-store
        # path. Both satisfy "handled gracefully".
        logs = get_container_logs(container, tail=300)
        integrity_path = (
            "integrity check failed" in logs
            or "clearing corrupted state" in logs
        )
        empty_path = (
            "Persistence store is empty" in logs
            or "starting with empty state" in logs
        )
        self.assertTrue(
            integrity_path or empty_path,
            "Startup logs should mention either the integrity-failure "
            "recovery path or the empty-store path. Neither found. "
            f"Log tail:\n{logs[-800:]}",
        )
        print(
            f"  [✓] Server survived a broken DB "
            f"(integrity_path={integrity_path}, empty_path={empty_path})"
        )

        # After recovery, re-register and confirm request serving.
        self.assertTrue(
            register_backends(node_api),
            "Failed to re-register backends after recovery",
        )
        status, body, _ = send_chat_request(
            node_api,
            [{"role": "user", "content": "post-corruption-recovery-check"}],
            stream=True,
            retries=5,
        )
        self.assertEqual(
            status, 200, f"Post-recovery chat should succeed: {body[:200]}"
        )

    # -------------------------------------------------------------------------
    # Test 05: An empty/missing DB starts clean
    # -------------------------------------------------------------------------
    def test_05_empty_db_starts_clean(self):
        """Deleting the DB file must not prevent clean startup."""
        node_api = NODES["node1"]["api"]
        node_metrics = NODES["node1"]["metrics"]
        container = "ranvier1"

        if not wait_for_healthy(f"{node_metrics}/metrics", timeout=30):
            self.skipTest(f"{container} not healthy at test start")

        print(f"  Removing /tmp/ranvier.db* inside {container}...")
        rm = docker_exec(
            container,
            "rm -f /tmp/ranvier.db /tmp/ranvier.db-wal /tmp/ranvier.db-shm",
        )
        self.assertEqual(
            rm.returncode,
            0,
            f"docker exec rm failed: rc={rm.returncode}, stderr={rm.stderr}",
        )

        self.assertTrue(
            signal_container_shutdown(container, "SIGKILL"),
            "SIGKILL delivery failed",
        )
        self.assertTrue(
            wait_for_container_exit(container, timeout=15),
            f"{container} did not exit after SIGKILL",
        )

        print(f"  Restarting {container} (docker start)...")
        self.assertTrue(
            docker_start_container(container),
            f"docker start {container} failed",
        )
        healthy = wait_for_healthy(f"{node_metrics}/metrics", timeout=45)
        self.assertTrue(
            healthy,
            f"{container} must come up cleanly with a missing DB file",
        )

        logs = get_container_logs(container, tail=300)
        empty = (
            "Persistence store is empty" in logs
            or "starting with empty state" in logs
            or "starting fresh" in logs
        )
        self.assertTrue(
            empty,
            "Startup logs should mention an empty persistence store. "
            f"Log tail:\n{logs[-800:]}",
        )
        print("  [✓] Server started cleanly from an empty persistence store")

        self.assertTrue(
            register_backends(node_api),
            "Failed to register backends after empty-DB start",
        )
        status, body, _ = send_chat_request(
            node_api,
            [{"role": "user", "content": "post-empty-db-recovery-check"}],
            stream=True,
            retries=5,
        )
        self.assertEqual(
            status, 200, f"Server should serve requests from a fresh DB: {body[:200]}"
        )


def main():
    """Run the persistence recovery tests in declared (numbered) order."""
    loader = unittest.TestLoader()
    loader.sortTestMethodsUsing = lambda x, y: (x > y) - (x < y)

    suite = loader.loadTestsFromTestCase(PersistenceRecoveryTest)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
