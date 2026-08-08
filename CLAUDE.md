# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Endstone Spark is a native statistical profiler plugin for Minecraft Bedrock Dedicated Server (BDS). It samples native execution and allocation call stacks on Windows and Linux, aggregates them into spark-compatible profiles, and uploads them to or opens them with the standard spark viewer.

The plugin must remain safe inside a long-running server process. Sampling and allocator-hook paths have stricter constraints than ordinary plugin code: they must be bounded, avoid blocking, and defer symbolization, aggregation, compression, and network I/O to safe background or export-time code.

## Build Commands

### Prerequisites

- CMake 3.23+
- Ninja
- Conan 2
- Python 3.12 for release-tool tests
- Windows: LLVM clang-cl 20, Visual Studio Build Tools, and the Windows SDK
- Linux: Clang 20 with libc++ and libc++abi

The repository ships `.conan2/profiles/default`, which selects clang-cl on Windows and Clang/libc++ on Linux. Do not run `conan profile detect` over this file.

### Install dependencies with Conan

```shell
python -m pip install "conan>=2,<3"
conan install . --build=missing
```

### Build with generated presets

After Conan generates the presets:

```shell
cmake --preset conan-relwithdebinfo
cmake --build --preset conan-relwithdebinfo
```

### Build with an explicit build directory

```shell
cmake -S . -B build -G Ninja \
  "-DCMAKE_TOOLCHAIN_FILE=build/RelWithDebInfo/generators/conan_toolchain.cmake" \
  "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
cmake --build build
```

On Windows, run the commands from an environment where clang-cl can find the MSVC toolchain and Windows SDK.

## Testing

### Complete CTest suite

Preset build:

```shell
ctest --test-dir build/RelWithDebInfo --output-on-failure
```

Explicit `build` directory:

```shell
ctest --test-dir build --output-on-failure
```

The suite includes the offline profiler self-test, shared evidence-policy tests, and platform-specific native symbol-guesser tests.

### Focused tests

```shell
./build/RelWithDebInfo/spark_selftest --seconds=1
./build/RelWithDebInfo/spark_selftest --statistics-only
./build/RelWithDebInfo/spark_selftest --allocation-only
python tests/test_release_changelog.py
```

Use the corresponding `.exe` paths on Windows. `spark_allocation_benchmark` is a benchmark tool, not a correctness test.

## Code Style

### C++

Endstone uses **clang-format** and **clang-tidy** for code quality enforcement.

**Style Guidelines:**

- Based on Microsoft style with Stroustrup braces
- Naming conventions:
  - Classes/Structs/Enums: `CamelCase`
  - Methods: `camelBack`
  - Private/protected members: `lower_case_` (trailing underscore)
  - Local variables/parameters: `lower_case`
  - Macros: `UPPER_CASE`

### Python

**Configuration:**

- Line length: 120 characters

### Comments (all languages)
- Keep comments terse and human. Default to no comment; when one is warranted, one short line.
- No multi-line explanations, rationale, design-decision narration, or parenthetical asides.
- Do not leave "LLM notes" — comments that explain why a change was made, reference the development process, or restate what the code plainly does.
- Match the comment density and verbosity of the surrounding or original code (e.g. a port stays as terse as its upstream).

## Submitting Changes

### Commit Message Guidelines

Follow conventional commits format:

- `feat:` for new features
- `fix:` for bug fixes
- `docs:` for documentation changes
- `style:` for code style changes (formatting, etc.)
- `refactor:` for code refactoring
- `test:` for adding or updating tests
- `chore:` for maintenance tasks

Example:

```
feat: improve labeling of unresolved BDS frames

Detect function extents in stripped Windows and Linux BDS executables
Append best-effort RTTI or string-based hints to unresolved frames
Display guesses alongside the original module-relative address
Leave successfully symbolicated frames and non-BDS modules untouched
```

### Platform safety

- Sampling must stay off the BDS tick hot path except for the minimum bounded capture operation.
- Linux signal-handler code must remain async-signal-safe.
- Windows thread suspension and stack walking must always restore target-thread state on every exit path.
- Allocation hooks must remain reentrancy-safe and must never block allocator threads.
- Plugin shutdown must wait only within bounded intervals and must not leave callbacks, hooks, or background work referring to unloaded code.

## Architecture

### Source Structure

- `src/plugin.cpp` - Endstone plugin lifecycle and command dispatch (thin bootstrap)
- `src/application/` - platform-independent business orchestration: command registry, profiler service, profile exporter, health, activity, and tick-monitor commands, platform capability interfaces
- `src/core/` - platform-independent services: profiler, statistics, command parsing, config (TOML), recovery journal, activity log, WebSocket/crypto, server-properties metadata, utilities
- `src/native/` - native backend: execution sampler, symbol guesser, allocation hooks
- `src/platform/endstone/` - thin Endstone platform adapters: command sender, thread dispatcher, metadata provider (including world gauges and ping), result notifier
- `src/proto/` - spark protobuf serialization
- `src/net/` - gzip compression, bytebin upload, WebSocket transport, and local profile persistence
- `proto/` - upstream spark protocol references
- `tests/` - offline, synthetic, and platform-specific tests
- `tools/` - release changelog tooling and profile evaluator
- `docs/` - architecture and phase status documentation

### Key Components

1. **Execution sampler:** Captures selected native thread stacks at a bounded interval. Linux uses `SIGPROF` with cpptrace's safe raw-trace path; Windows suspends a target thread and walks it with the native stack APIs.
2. **Allocation profiler:** Samples allocation stacks by requested bytes. Windows uses funchook for supported UCRT and heap entry points; Linux redirects supported ELF allocator imports. Hook callbacks enqueue bounded records for later processing.
3. **Profiler pipeline:** Aggregates samples into per-thread call trees, attaches statistics and platform metadata, serializes the spark protobuf, compresses it, then uploads or writes a `.sparkprofile` file.
4. **Symbolization:** Normal platform symbols have priority. Unresolved frames in the BDS main executable may receive conservative runtime guesses from unwind metadata, RTTI, vtables, thunks, and decoded string references. Guesses retain the RVA and identify their evidence source.
5. **Statistics service:** Maintains bounded rolling TPS, MSPT, CPU, player-count, and world-gauge histories independently of an active profile.
6. **Application layer:** Platform-independent business orchestration in `src/application/`. `SparkApplication` owns all services and dispatches ticks and commands. `ProfilerService` manages profiler sessions, background profiling, live viewer connections, and exports. Three focused capability interfaces (`MainThreadDispatcher`, `ProfileMetadataProvider`, `ResultNotifier`) abstract platform dependencies without a god-Platform.
7. **Platform adapters:** `src/platform/endstone/` provides thin Endstone implementations of the capability interfaces and `CommandSender`. `plugin.cpp` remains a thin bootstrap responsible for registration and lifecycle wiring.
8. **Crash recovery:** `RecoveryWriter` journals module, thread, sample, and tick records to segmented files via a bounded lock-free queue. On startup, `RecoveryPlayer` replays the journal to reconstruct and export a profile from a crashed or stalled session.
9. **Stall watchdog:** `StallWatchdog` runs on an independent thread, monitoring the main-thread heartbeat. It fires stall-begin and stall-end callbacks to trigger recovery without calling Endstone APIs or stopping the profiler.
10. **Live viewer:** `ViewerSocket` manages a WebSocket connection to the spark live viewer, uploading initial sampler data and pushing payload IDs on window rotation. A dedicated worker thread moves gzip and HTTP upload off the main thread.
11. **Trusted viewer / crypto:** RSA2048-SHA256 signing and verification (`Crypto`) authenticates live viewer clients. `TrustedViewersState` persists approved public keys separately from the user-owned config file.
12. **Activity log:** `ActivityLog` maintains a bounded circular log of profiler and health-report outputs, persisted as JSON.
13. **Persistent configuration:** `SparkConfig` loads and writes a TOML config file with safe defaults. `TrustedViewersState` manages trusted viewer keys in a separate JSON file.
14. **Network and ping statistics:** `NetworkMonitor` polls per-interface RX/TX counters with rolling averages. `PingStatistics` collects player ping on a fixed interval with a rolling median.
15. **Server metadata:** `server_properties` parses `server.properties` with a strict allowlist, emitting only safe performance-relevant keys as JSON for the viewer's `server_configurations` field.

### Dependencies

Conan supplies cpptrace, concurrentqueue, zlib, expected-lite, libcurl, and tomlplusplus. Linux additionally requires OpenSSL for crypto. CMake fetches Endstone's public plugin API and funchook `v1.1.3`; funchook's bundled distorm decoder is used by the x86-64 symbol guessers on both supported platforms, while the funchook hook library itself is linked only on Windows.

## Native Symbol Guessing

- Treat the running executable as the only product-time source of guessed symbols. Debug databases, PDBs, IDA databases, and build-specific RVA tables must never become runtime dependencies.
- Prefer a raw RVA over a plausible but unsupported label.
- Existing PDB, export, or dynamic-symbol results must never be replaced or annotated by guesses.
- Only unresolved frames proven to belong to the main executable are eligible.
- Use `type: message` for strong evidence and `type?: message` for useful but incomplete evidence. Conflicting evidence returns no label.
- Keep label selection deterministic across scan order, process runs, and ASLR bases.
- Test parsers and decoders with synthetic public fixtures. Do not copy BDS executable fragments, private symbols, or analysis exports into the repository.

## Profile Compatibility and Privacy

- Preserve compatibility with the spark protobuf schema and viewer behavior documented in `README.md`.
- Profile metadata may include the BDS executable hash and version, strict allowlisted `server.properties` fields, and aggregate world entity/chunk counts. It must not include the level seed, credentials, secrets, tokens, passwords, private keys, server-owner paths, arbitrary configuration, or executable contents.
- Never commit generated `.sparkprofile` files, BDS binaries, PDBs, IDA databases, crash dumps, benchmark output, logs, or local deployment configuration.

## Release Workflow

- `develop` is the integration branch and is built on Windows and Linux by `.github/workflows/build.yml`.
- `CHANGELOG.md` follows Keep a Changelog and contains one non-empty `Unreleased` section before a release.
- Release versions follow Semantic Versioning.
- `.github/workflows/release.yml` can be dispatched from `main` with a version or triggered by pushing a `vX.Y.Z` tag. A release requires the selected ref, `main`, and `develop` to point to the same commit.
- The release workflow updates `CMakeLists.txt`, `src/spark_constants.h`, `src/plugin.cpp`, and `CHANGELOG.md`; creates the release commit and tag; builds both platform artifacts; and uploads them to the GitHub release.
- Do not manually duplicate version changes that the release workflow owns.

## Git Conventions

- Preserve unrelated working-tree changes and stage only files that belong to the current task.
- Do not commit generated build output, local presets, profiles, deployment files, or research artifacts.
- Keep commits focused and use short imperative subjects such as `fix: ...`, `feat: ...`, `test: ...`, and `docs: ...`.
- Never add a `Co-Authored-By` line for Claude.
- Document user-visible behavior in `CHANGELOG.md`; omit implementation-only refactoring notes.
- Prefix breaking user-facing changes with `**BREAKING**:` under `Changed` or `Removed`.
