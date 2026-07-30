# spark for Endstone

An implementation of the [spark](https://spark.lucko.me/) profiler for
[Endstone](https://github.com/EndstoneMC/endstone) — a native port of spark to the
Bedrock Dedicated Server. Find out where your server is actually spending its tick
time, in spark's own web viewer.

It is a **native statistical sampling profiler**: execution profiles periodically
snapshot selected BDS process threads (the server thread by default), covering native
work such as chunk generation, entity ticking, redstone, and pathfinding, not just
plugin code — even though the server binary is stripped. It produces genuine spark
profiles, uploaded to spark's bytebin and opened as an interactive flame graph at
`https://spark.lucko.me/<id>`.

> This is spark, ported to Endstone. The profile format, protocol, and web viewer
> are spark's — all credit for those goes to
> [lucko/spark](https://github.com/lucko/spark).

## Commands

| Command                           | Description                                             |
| --------------------------------- | ------------------------------------------------------- |
| `/spark profiler start [flags]` | Start profiling selected native threads (background).   |
| `/spark profiler start --alloc` | Profile native allocation call stacks.                  |
| `/spark profiler stop`          | Stop profiling and finalize the profile.                |
| `/spark profiler info`          | Show status of the running profiler.                    |
| `/spark profiler cancel`        | Stop profiling without generating a profile.            |
| `/spark tps`                    | Show ticks-per-second and tick duration (MSPT).         |
| `/spark health`                 | Show TPS/MSPT plus process memory, threads, and uptime. |
| `/spark tickmonitor`            | Report ticks that exceed a duration or baseline change. |

By default, stopping a profiler uploads the generated profile to spark's bytebin
and prints the viewer link. With `--save-to-file`, the profile is written locally
as a `.sparkprofile` file instead. If an upload fails, Spark automatically
preserves the compressed profile in its data folder and reports the local path.

Permission: `endstone.command.spark` (operators by default).

### `/spark tickmonitor`

Run `/spark tickmonitor` to establish a 120-tick baseline and report ticks whose
duration is more than 100% above it. Use `--threshold <percent>` to change the
relative threshold, or `--threshold-tick <ms>` to use an absolute tick duration.
Run the command again to disable the monitor.

### `/spark profiler start` flags

* `--interval <value>` — execution interval in milliseconds (default `4`, maximum
  `1000`), or allocation interval in bytes with `--alloc` (default `524287`).
* `--timeout <seconds>` — auto-stop and finalize after the specified number of
  seconds, which must be greater than `10`. Omit this flag to run until `stop` or
  `cancel` is issued.
* `--only-ticks-over <ms>` — retain samples only from ticks longer than the given
  positive whole number of milliseconds.
* `--comment <text>` — attach a note to the profile; quote text containing spaces.
* `--save-to-file` — write a `.sparkprofile` file instead of uploading it (open the
  file by dragging it into the spark viewer).
* `--thread <name>` — select a thread by case-insensitive exact name; repeat the
  flag to select multiple threads and quote names containing spaces. This works for
  execution and allocation profiles.
* `--thread *` — select all BDS process threads and emit separate viewer roots. It
  is equivalent to allocation mode's default all-thread selection and cannot be
  combined with another `--thread` or `--regex`.
* `--regex` — interpret each `--thread <pattern>` as a case-insensitive full-match
  regular expression; at least one pattern is required. This works for execution
  and allocation profiles.
* `--include-sleeping` — execution profiles only. Also sample threads while they are
  idle. Without this flag, Linux task state and Windows per-thread CPU cycle deltas
  avoid capturing threads that did not run.
* `--alloc` — record sampled native allocation call stacks instead of execution time.
* `--alloc-live-only` — record only sampled allocations retained at stop for leak
  analysis; this implies `--alloc`.

Multi-thread execution profiles treat the interval as a global stack-walk budget and
rotate fairly through matching threads. `/spark profiler stop` also accepts
`--save-to-file` and `--comment <text>`; values supplied at stop take effect for the
final output.

## How it works

* **Linux:** a dedicated sampler thread signals one selected target (`SIGPROF`) per
  interval; the handler captures the stack async-signal-safely via
  [cpptrace](https://github.com/jeremy-rifkin/cpptrace)'s `safe_generate_raw_trace`.
  Frames are resolved with `dladdr` (dynamic symbols) and fall back to
  `module+0xRVA` for the stripped BDS internals — which you can symbolicate offline
  against an IDA database or the Windows PDB.
* **Windows:** the sampler suspends one selected target per interval and walks its
  context with `StackWalk64`; frames resolve against the shipped PDB (real names).
* Samples aggregate into per-thread call trees, serialize to spark's protobuf,
  gzip, and either upload to bytebin or write a local `.sparkprofile` file.
  Symbolization and output processing run on a background thread so the server
  tick never stalls.
  Execution samples use the measured elapsed time between sampling points, excluding
  the target thread's own stack-walk suspension, so multi-thread sweeps retain correct
  time weights even when their effective cadence is longer than the requested interval.
* Every profile includes the SHA-256 of the running BDS executable, allowing an
  offline analyst to select the exact matching binary without receiving the
  server owner's executable, paths, configuration, or world data.

### Native allocation profiler

`--alloc` profiles successful native allocation requests across process threads.
Every thread has an independent randomized byte-sampling phase and a non-reused
session identity, so short-lived threads and operating-system thread-ID reuse do
not merge unrelated stacks. Samples are weighted by requested bytes using a
fixed-byte interval (524287 bytes by default) and appear as separate thread roots
in the same spark viewer used by execution profiles.

Without `--thread`, allocation profiles include all covered process threads.
Exact-name and regular-expression selectors use the same case-insensitive,
full-name matching rules as execution profiles, including threads created while
profiling. Allocation hooks still sample and maintain lifecycle state process-wide;
the safe aggregator resolves the allocation-origin thread name and excludes
non-matching samples before building the call tree. Consequently, no regular
expression, string construction, or thread-name query runs in an allocator hook,
and a free or realloc on an unselected thread can still retire an allocation
created by a selected thread.

`--alloc-live-only` follows sampled allocations through realloc and free calls,
including releases from other threads, and reports only allocations still live
when profiling stops. It is intended to identify retained-memory and leak
candidates; repeated profiles are needed to distinguish growth from legitimate
long-lived state.

Windows intercepts process-wide UCRT and process-heap entry points with funchook.
Linux atomically redirects supported allocator relocations in the main executable
and loaded ELF modules, including Endstone, native plugins, and Python when they
import the effective libc allocator. Loaded modules are rescanned at session start
and every five seconds while profiling; unloaded modules are recognized before
restoration so stale slots are never written.

Stack symbolization and call-tree aggregation run outside the hook path. A fixed
preallocated queue drops and reports excess samples instead of blocking allocator
threads. Live records, thread roots, module entries, pending samples, and call-tree
nodes are also capped; exported metadata reports capacities, high-water marks,
overflow merging, drops, hook coverage, and whether the profile is incomplete.
Profile sample/byte totals reflect samples accepted after thread and tick filters;
hook, observed-byte, sampling-point, live/freed lifecycle, and drop diagnostics are
explicitly labeled process-wide. If an allocation-origin thread exits before its
name can be read, a named selector fails closed for that identity rather than
attributing it using a possibly reused operating-system thread ID.
Hooks remain disabled pass-throughs between sessions and are fully removed after
in-flight calls finish during plugin shutdown, allowing a clean plugin reload.

Coverage is limited to the listed allocator entry points/imports. Static CRT
copies, inlined or private allocators, arenas and object pools that do not reach a
covered entry point, `VirtualAlloc`/`VirtualFree`, and `mmap`/`munmap` are not
sampled. A Linux module loaded and unloaded entirely between rescans can escape
coverage.

## Building

> Windows allocation profiler: CMake fetches and statically builds upstream funchook `v1.1.3`; it is not a Conan requirement. Linux uses atomic ELF import-slot redirection and does not link funchook.

The platform requirements are:

* **Linux:** Clang, libc++, Ninja, and Conan 2.
* **Windows:** LLVM clang-cl, Visual Studio Build Tools, the Windows SDK,
  Ninja, and Conan 2. clang-cl must target the MSVC ABI.

Install Conan, resolve the dependencies, then configure CMake directly with the
generated toolchain file:

```shell
pip install conan

conan install . --build=missing

cmake -S . -B build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=build/RelWithDebInfo/generators/conan_toolchain.cmake" "-DCMAKE_BUILD_TYPE=RelWithDebInfo"

cmake --build build
```

With self-test tools enabled, `spark_selftest --allocation-only` exercises exact,
regex, multiple, dynamic, and no-match allocation thread selection, cross-thread
free/realloc and live-only lifecycles, session reuse, thread overflow, and bounded
queue/index pressure. `spark_allocation_benchmark` prints repeatable CSV medians for
unprofiled and disabled-hook baselines, default/4 KiB intervals,
single/four-thread, live-only, and forced saturation cases.

On Linux, the bundled profile selects libunwind because the SIGPROF sampler
requires cpptrace's async-signal-safe unwinding path. Windows does not use
libunwind; cpptrace uses its native Windows backend while spark captures stacks
with StackWalk64.

The plugin is emitted as `build/endstone_spark.so` (Linux) /
`build/endstone_spark.dll` (Windows). Drop it in your server's `plugins/`
directory.

> **Toolchain / ABI note.** A C++ Endstone plugin must use the runtime ABI expected
> by the Endstone build it is loaded into. Match its compiler, compiler ABI, C++
> standard, and standard library/runtime. On Linux, use an ABI-compatible libc++;
> on Windows, use clang-cl with the matching MSVC runtime. Do not mix incompatible
> STL or runtime ABIs: every C++ type crossing the Endstone plugin boundary must
> have the same ABI on both sides. A mismatch can corrupt objects passed across the
> plugin boundary.

## License

GPLv3, matching spark, whose profile format and viewer this builds on. See
[LICENSE](LICENSE).
