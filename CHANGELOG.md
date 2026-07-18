# Changelog

All notable changes to endstone-spark are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

- Native allocation profiling, sampled lifetimes, and retained/live analysis cover
  the selected server thread on Windows and Linux x86-64.
- Custom thread selection, direct virtual-memory calls, and custom allocator
  activity below its backing Windows heap allocation are not yet supported.

## [0.1.1] - 2026-07-16

### Fixed

- Build Linux plugins on Ubuntu 22.04 for compatibility with older glibc hosts.
- Upload profiles without requiring an external `curl` executable.

### Changed

- Upload Linux and Windows plugin binaries from every Build workflow run.
- Support manually building a selected Git ref for release testing.
- Generate release notes and changelog entries from one normalized release section.

## [0.1.0] - 2026-07-16

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

[Unreleased]: https://github.com/EndstoneMC/spark/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/EndstoneMC/spark/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/EndstoneMC/spark/releases/tag/v0.1.0
