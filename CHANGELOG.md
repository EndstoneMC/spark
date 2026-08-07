# Changelog

All notable changes to endstone-spark are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased][Unreleased]

### Added

- Preserve exact sampled native PCs and their independently validated function
  roots in forward-compatible profile fields for offline normalization metrics.
- Resolve Linux vtable labels for shared implementations by parsing Itanium RTTI
  inheritance edges, attributing virtual functions to their common ancestor class
  instead of leaving the RVA unresolved.
- Label unresolved Linux main-executable functions that reference uniquely-owned
  BDS debug trace strings (`"N _functionName"` patterns) with low-confidence
  `str?` hints.
- Label unresolved Linux main-executable functions that reference multiple unique
  weak string hints with tentative `str?` accumulated labels.
- Propagate lambda type names from `std::function::__func` vtable thunks to their
  single large unresolved call target on Linux with low-confidence `call?` hints.
- Label unresolved Linux main-executable functions with characteristic code
  patterns (Knuth multiplicative hash, atomic `lock cmpxchg`/`lock xadd`) with
  low-confidence `type?` behavior hints.

### Fixed

- Recover Linux function extents from validated CIE/FDE records, including
  unindexed records in a safely terminated `.eh_frame`, instead of extending
  every function to the next unwind-table start.
- Keep addresses in unwind gaps and PLT entries out of adjacent functions while
  normalizing sampled PCs inside exact half-open function ranges.

### Changed

- Add deterministic Linux symbol-index timing, memory, range-quality, and batch
  diagnostics to profile metadata, and evaluate `Level::tick()` subtree
  readability separately from whole-process coverage.

## [0.4.1][0.4.1] - 2026-08-02

### Added

- Append evidence-tagged native name guesses to unresolved main-executable frames
  on Windows and Linux while preserving the RVA. Function
  extents come from PE exception data or ELF unwind metadata, with class/slot and
  semantic hints recovered from validated RTTI, vtables, thunks, and decoded
  string references.

### Changed

- Store profiles created by `--save-to-file` or upload-failure recovery under
  `plugins/spark/profiles/` instead of the plugin data root.
- Make Linux vtable guesses deterministic and reject conflicting class owners;
  decode string references only at real x86-64 instruction boundaries and only
  for functions present in the exported profile.

## [0.4.0][0.4.0] - 2026-07-31

### Added

- Maintain a fixed-capacity 15-minute history of completed ticks and one-second
  process/system CPU observations even when no profiler is running.
- Calculate independent 5-second, 10-second, 1-minute, 5-minute, and 15-minute TPS
  windows plus query-time MSPT distributions and rolling CPU usage.
- Populate profile metadata and per-second Viewer windows from the shared history,
  including exact time bounds, CPU, TPS, MSPT median/max, and low-cost player counts.
- Report real rolling TPS, MSPT distributions, and process/system CPU from
  `/spark tps`, with the available history duration shown during warm-up.

### Changed

- Share one cross-platform process CPU normalization rule and keep allocation,
  sorting, and percentile work out of the per-tick statistics path.
- Omit unavailable per-second entity and chunk fields instead of copying a final
  world snapshot or serializing zero as though it were an observation.
- Make `/spark health` use the shared performance snapshot and consistently report
  available process RSS/virtual memory, threads, physical memory, swap/page-file,
  disk, uptime, players, CPU, and OS resources on Windows and Linux.
- Track resource-field availability explicitly and omit failed system/profile
  queries rather than presenting synthetic zero values.

### Fixed

- Stop copying one TPS/CPU average across every rolling field or labeling MSPT
  means and maxima as median, minimum, and p95 values in profile metadata.
- Preserve the suspended thread's top instruction address on Windows when
  `StackWalk64` cannot unwind a caller, instead of discarding the whole sample.
- Report reserved/committed process virtual address space on Windows instead of
  incorrectly labeling private commit charge as virtual memory.
- Retry Windows hook removal within the existing bounded shutdown interval when
  a concurrently starting or exiting thread temporarily rejects context capture.

## [0.3.2][0.3.2] - 2026-07-30

### Added

- Support repeatable exact-name and case-insensitive full-match regular-expression
  thread selection for native allocation profiles, including dynamically created
  threads and `--thread *`.

### Changed

- Apply allocation thread matching in the safe aggregation path while retaining
  process-wide lifecycle tracking, so cross-thread realloc/free remains correct and
  allocator hooks never resolve names or run regular expressions.
- Separate monotonic allocation-thread identities from the bounded overflow viewer
  root so filtering remains correct after thread-ID reuse and beyond 256 roots.
- Label profile-selected allocation totals separately from process-wide hook,
  observed-byte, drop, and live/freed lifecycle diagnostics.

## [0.3.1][0.3.1] - 2026-07-30

### Added

- Sample native allocations across process threads with independent byte-sampling
  state, monotonic session thread identities, stable overflow merging, and
  per-thread spark Viewer roots.
- Patch supported allocator relocations in the Linux main executable and loaded
  ELF modules, with REL/RELA validation, RELRO permission restoration, periodic
  rescans, and unload-safe cleanup.
- Export distinct allocation hook, success, sampling-point, enqueue, drop,
  queue high-water, live-index, thread, module, and truncation metadata.

### Fixed

- Avoid Windows loader-TLS recursion by keeping allocation-hook thread state in a
  preallocated native TLS registry and bypassing hooks while a thread's static TLS
  vector is being constructed.
- Preserve sampled allocation identity across cross-thread free and realloc,
  including failure, zero-size, in-place, moved, and address-reuse cases.
- Use a bounded multi-producer allocation event queue on Linux, enforce the declared
  tick-event capacity on both native backends, and shard their live-allocation
  indexes.
- Suppress allocation sampling from aggregation, module scanning, export, and
  symbolization control paths.
- Quiesce active Windows detours before restoring allocator entry points during
  shutdown, including allocations originating in other loaded DLLs.

### Limitations

- Allocation coverage does not include static or private allocators,
  `VirtualAlloc`/`VirtualFree`, `mmap`/`munmap`, or module lifetimes that begin and
  end entirely between Linux rescans.

## [0.3.0][0.3.0] - 2026-07-19

### Added

- Add `/spark tickmonitor` with percentage-over-baseline and absolute tick-duration
  thresholds for detecting intermittent server tick spikes.
- Support `/spark profiler start --thread *` execution profiles across all BDS
  process threads, with dynamic thread discovery and per-thread viewer roots.
- Support repeatable exact-name and regular-expression thread selection for
  execution profiles, with matching thread-dumper metadata in viewer payloads.
- Include the running BDS executable's SHA-256 in every profile so offline
  binary analysis can select the exact matching server build without collecting
  the executable or server-private data.

### Fixed

- Weight execution samples by each thread's measured runnable time instead of the
  nominal interval, preserving viewer time semantics for sequential multi-thread sweeps.
- Detect idle Windows threads through `QueryThreadCycleTime` deltas so the default
  execution profiler skips their suspension and stack-walk overhead.
- Schedule matching execution threads round-robin with at most one stack-walk attempt
  per interval, bounding profiler overhead as the process thread count grows.

## [0.2.0][0.2.0] - 2026-07-18

### Added

- `/spark profiler start --alloc` on Windows, producing an uploaded spark
  `ALLOCATION` profile weighted by sampled UCRT allocation bytes.
- `/spark profiler start --alloc` on Linux x86-64 using atomic BDS ELF import-slot
  hooks for glibc allocation entry points.
- `/spark profiler start --alloc-live-only` for retained-allocation call trees and
  leak-candidate analysis by allocation stack and age.
- Byte-based allocation stack sampling with spark's default 524287-byte interval,
  a randomized systematic sampling phase, a preallocated hook event pool,
  deferred module/symbol resolution, per-second windows, and
  `--only-ticks-over` support.
- CMake FetchContent integration for funchook v1.1.3, avoiding an unavailable
  Conan Center package.

### Changed

- Reject ambiguous export-only UCRT symbol names during Windows symbolization and
  fall back to `module+0xRVA` instead of displaying unrelated nearest exports.
- Export allocation sample count, dropped sample count, estimated sampled byte
  weight, observed request bytes, interval, backend, and coverage through spark
  `extra_platform_metadata`.
- Report every candidate allocation entry point as active, aliased, missing, or
  unhookable in profiler status and exported platform metadata.
- Follow sampled allocations through realloc and free entry points across threads,
  reporting live/freed estimates and sampled allocation lifetimes on both native
  backends.
- Preserve the process's effective Linux allocator implementation, including
  `LD_PRELOAD` interposition, while redirecting BDS import slots.
- Harden native hook lifecycle handling: prepare and retain trampolines separately
  from entry-hook installation, retain disabled entry hooks between sessions,
  stabilize and verify thread suspension, retry thread restoration, and require
  complete entry-hook removal plus hook/trampoline quiescence before plugin unload.
  An unrecoverable cleanup failure terminates the process instead of pinning old
  plugin code or permitting an unsafe reload.
- Treat the allocation aggregator as a fallible service: failures disable capture,
  block partial-profile export, remain visible through profiler status, and allow
  a clean subsequent session after stop/cancel.
- Report failed allocation sessions explicitly, discard their incomplete data on
  stop/cancel/timeout, and confirm when the backend is ready to start again.
- Write valid gzip-compressed local profiles atomically and automatically save a
  local copy when upload fails, so completed captures are not lost to the network.
- Replace the finite repeated threshold table with constant-time systematic byte
  sampling using a fresh uniformly random phase per session. Allocation weights
  use successful requested sizes and every sampling point is attributed to the
  allocation that contains it.
- Expand native coverage to recalloc, aligned allocation families, internal UCRT
  base exports, and direct `HeapAlloc`/`HeapReAlloc`, while avoiding duplicate hooks
  for alias exports.
- Encapsulate Windows allocation backend state in the `AllocationSampler` instance
  instead of file-level mutable globals.
- Removed the temporary `/spark alloc start|info|stop|reset|test` validation
  commands and their allocation/free counter implementation.
- Stop sampling before collecting plugin/world export metadata so the profiler's
  own metadata allocations are not included in allocation profiles.

### Limitations

- Direct virtual-memory calls and custom allocator activity below its backing
  Windows heap allocation are not supported.

## [0.1.1][0.1.1] - 2026-07-16

### Fixed

- Build Linux plugins on Ubuntu 22.04 for compatibility with older glibc hosts.
- Upload profiles without requiring an external `curl` executable.

### Changed

- Upload Linux and Windows plugin binaries from every Build workflow run.
- Support manually building a selected Git ref for release testing.
- Generate release notes and changelog entries from one normalized release section.

## [0.1.0][0.1.0] - 2026-07-16

### Fixed

- Preserve `--save-to-file` for the entire profiler session, including manual
  `stop` and `upload` commands.
- Populate world, region, and chunk statistics from loaded chunks and actors.

### Added

- Native statistical sampling profiler for the Bedrock Dedicated Server thread,
  covering all BDS internal calls (not just plugins), with results uploaded to the
  spark web viewer.
- `/spark profiler start|stop|info|cancel` with flags `--interval`, `--timeout`,
  `--only-ticks-over`, `--save-to-file`, `--comment`, and `--include-sleeping`.
- `/spark tps` and `/spark health`.
- Linux backend: `SIGPROF` sampler with cpptrace async-signal-safe stack capture and
  `dladdr` symbolization (module + RVA fallback for stripped frames).
- Windows backend: `SuspendThread` + `StackWalk64` capture with PDB symbolization.

[Unreleased]: https://github.com/EndstoneMC/spark/compare/v0.4.1...HEAD
[0.4.1]: https://github.com/EndstoneMC/spark/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/EndstoneMC/spark/compare/v0.3.2...v0.4.0
[0.3.2]: https://github.com/EndstoneMC/spark/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/EndstoneMC/spark/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/EndstoneMC/spark/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/EndstoneMC/spark/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/EndstoneMC/spark/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/EndstoneMC/spark/releases/tag/v0.1.0
