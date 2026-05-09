# Fuzz Harnesses

libFuzzer harnesses for boundaries identified in the
[request-lifecycle crash-risk audit](../../docs/audits/request-lifecycle-crash-audit.md).

These exist to convert the audit's static "could crash" findings into either
"does crash" (real bug) or "ran clean for N hours" (audit was wrong /
already mitigated).

## Targets

| Harness | Boundary | Audit findings exercised |
|---------|----------|---------------------------|
| `radix_tree_fuzz.cpp` | `RadixTree::insert` / `RadixTree::lookup` | H8 (split_long_prefix off-by-one), L9 (recursive subspan) |
| `request_rewriter_fuzz.cpp` | `RequestRewriter::extract_*` (JSON body parsing) | M6 (RapidJSON nesting), M4 (large allocations), L5 (vocab ID validation) |
| `stream_parser_fuzz.cpp` | `StreamParser::push` (chunked HTTP / SSE) | H10 (chunk trailer length), M11 (status snoop split), M12 (Content-Length cast) |

## Building

### Prerequisites

libFuzzer is a clang feature (no GCC equivalent), so the harnesses
require **clang and compiler-rt** in the build environment. The
production builder image (`Dockerfile.production`) is GCC-only — install
clang separately, or use a developer container.

```sh
# Fedora (matches Dockerfile.base)
dnf install -y clang compiler-rt llvm

# Debian / Ubuntu
apt-get install -y clang llvm

# Verify clang is on PATH before configuring CMake
which clang clang++ && clang --version
```

If you see `is not a full path and was not found in the PATH` from CMake,
clang isn't installed (or isn't on `PATH`); install it first or pass an
absolute path via `-DCMAKE_C_COMPILER=/usr/bin/clang`.

### Configure & build

The fuzzers are opt-in. Configure with:

```sh
cmake -B build-fuzz \
  -DRANVIER_BUILD_FUZZERS=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-fuzz --target radix_tree_fuzz request_rewriter_fuzz stream_parser_fuzz
```

(Drop `stream_parser_fuzz` from the target list if Seastar isn't found —
CMake will print `Seastar not found — skipping stream_parser_fuzz` and
only the two pure-C++ harnesses build.)

`stream_parser_fuzz` only builds when Seastar is found by CMake; the other
two are pure C++ and build without Seastar.

The harnesses link against `-fsanitize=fuzzer,address,undefined` by default
when `RANVIER_BUILD_FUZZERS=ON` and the compiler is clang. ASan + UBSan are
on so genuine crashes (OOB, UAF, signed overflow) get caught even when the
input doesn't trigger an outright SIGSEGV.

## Running

```sh
# 30-minute fuzz run with a 256 KiB input cap
./build-fuzz/tests/fuzz/radix_tree_fuzz \
    -max_total_time=1800 -max_len=262144 -print_final_stats=1

# Continue from a corpus directory
mkdir -p tests/fuzz/corpus/radix_tree
./build-fuzz/tests/fuzz/radix_tree_fuzz \
    tests/fuzz/corpus/radix_tree -max_total_time=1800
```

A short smoke run (~5 min each) on every commit is enough to catch
regressions; a longer run (overnight or on CI nightly) will exercise the
deeper paths the audit was concerned about.

## What "passing" means

- **Crash within minutes:** the audit was right about that boundary; treat
  the reproducer as a P0 fix.
- **No crash after several hours of fuzzing on a corpus seeded from the
  unit tests:** the audit's concern about that boundary is unfounded *for
  inputs the harness can construct*. Update the audit doc to mark the
  finding `MITIGATED-BY-FUZZ`.
- **No crash but coverage is low:** the harness probably isn't reaching the
  flagged code. Inspect `-print_pcs=1` output and either expand the harness
  or downgrade the finding to "static-only, can't verify."

## Caveats

- These harnesses cover *static* boundaries. Cross-shard, threading, and
  shutdown-ordering findings (H4, H5, M1, M14) are not fuzzable here — they
  need `seastar-tsan` or a dedicated stress test.
- `stream_parser_fuzz` operates on a single `StreamParser` instance per
  input, splitting the input into pseudo-random chunks. It does not
  exercise the full HTTP-over-TCP path.
