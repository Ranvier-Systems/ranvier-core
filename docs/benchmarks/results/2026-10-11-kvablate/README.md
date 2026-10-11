# 2026-10-11 — kvablate: which half of the native stream broke affinity?

**Compare files only, reconstructed.** The instance (158-101-147-226, published image `818f6e3`,
checkout `88b87c7` with `--allow-stale-image`) was terminated before `bench-archive.sh` ran;
manifests, aggregates, runner summary and Prometheus dumps are lost. The three files are the
`compare_*.txt` outputs as printed on the instance and pasted into the working session, unedited.
The knobs were environment variables forwarded by the compose file, so they took effect on the
published image; the depth-gate fix in the checkout's `src/` was not in the image.

| File | Arm (prefix side) | P99 vs least-loaded | KV hit | Gini | Verified evictions |
|---|---|---|---|---|---|
| `compare_20261010_234705.txt` | `--kv-events-materialize off` (depth 2048) | +163.2% | 20.1 → 22.5% | 0.191 | 119 |
| `compare_20261011_001250.txt` | `--kv-events-depth 4096` (materialize on) | +77.0% | 22.6 → 25.0% | 0.145 | 109 |
| `compare_20261011_003832.txt` | both | +187.8% (P99 5.05 s, validation FAILED) | 21.3 → 24.5% | 0.139 | 97 |

Neither suspect moved the result; the ~100–120 verified evictions per arm are real block
evictions, and the damage was the router's response to them (verified-cold hit → miss placement
→ re-learn: a home migration per evicted block). Policy changed on 2026-10-11 (CHANGELOG, Changed).
Stream-off controls for this row: `../2026-10-10-kvevents-first-contact/` (−13.2 / −12.5 / −7.1%).
