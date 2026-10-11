# 2026-10-10 — native KV-event stream, first hardware contact (kvevents suite)

**Compare files only, reconstructed.** The instance was terminated before `bench-archive.sh`
ran, so the manifests, aggregates, runner summary and per-node Prometheus dumps are lost. The
four files here are the `compare_*.txt` outputs exactly as printed by `results_parser.py compare`
on the instance and pasted into the working session; nothing was edited. Treat the "Ranvier Env"
and counter lines as the manifest.

| File | Instance / image | What it is | P99 vs least-loaded |
|---|---|---|---|
| `compare_20261010_154055.txt` | 151-145-81-236, image from `b9628f1` | kvevents run 1, rr-first. `--kv-events` requested but `kv_ops=0`: the stream never connected (publisher endpoint defect). **Stream-off control repeat 1** of the fitted 13B 20u row vs `least_loaded`. | −13.2% |
| `compare_20261010_160636.txt` | same | run 2, prefix-first, `kv_ops=0`. **Control repeat 2.** | −12.5% |
| `compare_20261010_163220.txt` | same | run 3, rr-first, `kv_ops=0`. **Control repeat 3.** | −7.1% |
| `compare_20261010_224819.txt` | 158-101-147-226, published image `818f6e3` | kvevents run 1 with the stream **live** (`kv_ops` 1.2 M / 2.0 M, 394 verified hits, 98 verified evictions, 87,015 routes materialized): the regression that triggered the ablation. | **+84.8%** |

The three controls are the comparison every stream-on number of 2026-10-10/11 is read against
(median −12.5%, KV hits 61–68%); they reproduce the baseline suite's row 12 (2026-10-08,
−14.6 / −13.8 / −20.8%) on a third instance. Record and reading: `docs/benchmarks/benchmark-results-current.md`,
resume checklist item 7; CHANGELOG (Fixed, 2026-10-10 entries).
