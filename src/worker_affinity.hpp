// Ranvier Core — Foreign worker thread affinity helper
//
// Seastar pins each reactor thread to a specific CPU at startup. Plain
// std::thread workers spawned from a reactor (the persistence SQLite worker,
// the per-shard tokenizer FFI workers) inherit the spawning reactor's narrow
// affinity and can be migrated by the kernel onto any reactor core, which
// causes context-switch latency spikes on whichever reactor takes the hit.
//
// This module computes the "non-reactor" cpuset — the set of cores allowed
// to the process by the OS/cgroup minus the cores Seastar has pinned reactors
// to — and pins worker threads onto that set. Init is one-shot at startup;
// pinning is a single pthread_setaffinity_np per worker.
//
// USAGE:
//   1. In main(), before app_template::run(), call snapshot_process_cpuset().
//      At that moment the calling thread's affinity is the process-wide
//      allowed cpuset (cgroup-respecting). After Seastar starts, the main
//      thread is pinned narrow and the snapshot can't be recovered.
//   2. From application startup on shard 0, co_await
//      initialize_non_reactor_cpuset() once, before any worker thread is
//      spawned. This surveys reactor affinities and caches the non-reactor
//      cpuset.
//   3. From any reactor (the spawning one for each worker), call
//      pin_worker_to_non_reactor_core() with the std::thread::native_handle()
//      and a stable worker_index. Synchronous — no future to await.
//
// PLACEMENT STRATEGY:
//   The non-reactor cpuset has M cores. Worker `i` pins to cores[i % M],
//   round-robin with wrap. Single-worker callers pass worker_index=0.
//
// FAILURE MODES:
//   - M == 0 (every allowed core is a reactor — e.g. operator ran with
//     `--smp N` on an N-core box): one-shot warning logged at init, returns
//     false from every pin call, workers left with inherited affinity.
//   - Non-Linux build: compiled to a no-op returning false, one-shot info
//     log at snapshot.
//   - Syscall failure: logged at warn level, returns false.
//   - pin() called before init: returns false (no-op); used by unit tests
//     that bypass the startup chain.

#pragma once

#include <thread>

#include <seastar/core/future.hh>

namespace ranvier::worker_affinity {

// Snapshot the calling thread's CPU affinity into a process-wide static.
// MUST be called from main() before any Seastar startup. Idempotent: a
// second call is ignored (with a warning).
void snapshot_process_cpuset();

// Survey reactor-thread affinities across all shards and cache the
// non-reactor cpuset. MUST be called from a reactor thread once during
// startup, before any worker thread is spawned. Safe to call multiple times
// — only the first triggers the survey; subsequent calls return a ready
// future.
seastar::future<> initialize_non_reactor_cpuset();

// Pin the given OS thread to one of the non-reactor cores using round-robin
// placement. Synchronous — assumes initialize_non_reactor_cpuset() has
// already been awaited.
//
// thread_handle: pthread_t obtained from std::thread::native_handle().
// worker_index:  stable hint for round-robin selection across multiple
//                workers (e.g. shard_id for per-shard workers). Single-worker
//                callers pass 0.
// owner_label:   short identifier (e.g. "persistence", "tokenizer") used in
//                log messages only.
//
// Returns true if the affinity was successfully set, false otherwise:
// non-Linux build, init not yet run, empty non-reactor cpuset, or syscall
// failure. The worker continues to run regardless of the return value.
bool pin_worker_to_non_reactor_core(
    std::thread::native_handle_type thread_handle,
    unsigned worker_index,
    const char* owner_label);

} // namespace ranvier::worker_affinity
