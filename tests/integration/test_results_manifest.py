#!/usr/bin/env python3
"""Unit tests for run-manifest comparability checks (BACKLOG §25 item 8).

Pure-Python: writes tiny manifest.json files to a tmp dir, no GPU needed.
Run: python3 -m pytest tests/integration/test_results_manifest.py -v
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from results_parser import (  # noqa: E402
    load_manifest,
    workload_mismatches,
    warn_on_workload_mismatch,
)


def _manifest(**workload):
    base = {
        "model": "meta-llama/Llama-3.1-8B-Instruct",
        "num_backends": "8",
        "users": "30",
        "duration": "30m",
        "prompt_distribution": "stress",
        "shared_prefix_ratio": "0.9",
        "num_large_prefixes": "50",
    }
    base.update(workload)
    return {"schema_version": 1, "workload": base}


def _write_run(tmp_path, name, manifest=None):
    d = tmp_path / name
    d.mkdir()
    (d / "benchmark.log").write_text("stub")
    if manifest is not None:
        (d / "manifest.json").write_text(json.dumps(manifest))
    return str(d)


# ---------------------------------------------------------------------------
# load_manifest
# ---------------------------------------------------------------------------

def test_load_manifest_from_dir(tmp_path):
    d = _write_run(tmp_path, "run1", _manifest())
    m = load_manifest(d)
    assert m is not None and m["workload"]["users"] == "30"


def test_load_manifest_from_logfile_sibling(tmp_path):
    d = _write_run(tmp_path, "run1", _manifest())
    m = load_manifest(os.path.join(d, "benchmark.log"))
    assert m is not None


def test_load_manifest_absent_returns_none(tmp_path):
    d = _write_run(tmp_path, "run1", manifest=None)
    assert load_manifest(d) is None


def test_load_manifest_corrupt_returns_none(tmp_path):
    d = tmp_path / "bad"
    d.mkdir()
    (d / "manifest.json").write_text("{not valid json")
    assert load_manifest(str(d)) is None


# ---------------------------------------------------------------------------
# workload_mismatches
# ---------------------------------------------------------------------------

def test_identical_workloads_no_mismatch():
    assert workload_mismatches([_manifest(), _manifest(), _manifest()]) == []


def test_differing_users_flagged():
    assert workload_mismatches([_manifest(users="30"), _manifest(users="20")]) == ["users"]


def test_multiple_differing_keys_sorted():
    diffs = workload_mismatches([
        _manifest(users="30", duration="30m"),
        _manifest(users="20", duration="10m"),
    ])
    assert diffs == ["duration", "users"]


def test_single_manifest_never_mismatches():
    assert workload_mismatches([_manifest()]) == []


# ---------------------------------------------------------------------------
# warn_on_workload_mismatch (end-to-end over real dirs)
# ---------------------------------------------------------------------------

def test_warn_returns_true_on_mismatch(tmp_path, capsys):
    a = _write_run(tmp_path, "a", _manifest(users="30"))
    b = _write_run(tmp_path, "b", _manifest(users="20"))
    assert warn_on_workload_mismatch([a, b]) is True
    err = capsys.readouterr().err
    assert "WORKLOAD MISMATCH" in err and "users" in err


def test_warn_returns_false_when_matching(tmp_path):
    a = _write_run(tmp_path, "a", _manifest())
    b = _write_run(tmp_path, "b", _manifest())
    assert warn_on_workload_mismatch([a, b]) is False


def test_warn_notes_missing_manifests_but_does_not_block(tmp_path, capsys):
    a = _write_run(tmp_path, "a", _manifest())
    b = _write_run(tmp_path, "b", manifest=None)  # older run, no manifest
    # Only 1 manifest present -> cannot compare -> no mismatch, but a note is emitted.
    assert warn_on_workload_mismatch([a, b]) is False
    assert "have no manifest.json" in capsys.readouterr().err


if __name__ == "__main__":
    sys.exit(__import__("pytest").main([__file__, "-v"]))
