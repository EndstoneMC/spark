# spark for Endstone

An implementation of the [spark](https://spark.lucko.me/) profiler for
[Endstone](https://github.com/EndstoneMC/endstone) — a native port of spark to the
Bedrock Dedicated Server. Find out where your server is actually spending its tick
time, in spark's own web viewer.

It is a **native statistical sampling profiler**: it periodically snapshots the BDS
server thread's real call stack, so it covers **all** of BDS's internal work (chunk
gen, entity ticking, redstone, pathfinding, …), not just plugin code — even though
the server binary is stripped. It produces genuine spark profiles, uploaded to
spark's bytebin and opened as an interactive flame graph at
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
* `--timeout <seconds>` — auto-stop and finalize after more than 10 seconds. Omit
  this flag to run until `stop` or `cancel` is issued.
* `--only-ticks-over <ms>` — retain samples only from ticks longer than the given
  positive whole number of milliseconds.
* `--comment <text>` — attach a note to the profile; quote text containing spaces.
* `--save-to-file` — write a `.sparkprofile` file instead of uploading it (open the
  file by dragging it into the spark viewer).
* `--thread <name>` — execution profiles only. Select a thread by case-insensitive
  exact name; repeat the flag to select multiple threads and quote names containing
  spaces.
* `--thread *` — execution profiles only. Select all BDS process threads and emit a
  separate viewer root for each sampled operating-system thread. It cannot be
  combined with another `--thread` or `--regex`.
* `--regex` — execution profiles only. Interpret each `--thread <pattern>` as a
  case-insensitive full-match regular expression; at least one pattern is required.
* `--include-sleeping` — execution profiles only. Also sample threads while they are
  idle. Without this flag, Linux task state and Windows per-thread CPU cycle deltas
  avoid capturing threads that did not run.
* `--alloc` — record sampled native allocation call stacks instead of execution time.
  Custom thread selectors are not supported.
* `--alloc-live-only` — record only sampled allocations retained at stop for leak
  analysis; this implies `--alloc` .

Multi-thread execution profiles treat the interval as a global stack-walk budget and
rotate fairly through matching threads. `/spark profiler stop` also accepts
`--save-to-file` and `--comment <text>`; values supplied at stop take effect for the
final output.

## How it works

* **Linux:** a dedicated sampler thread signals the server thread (`SIGPROF`) on the
  chosen interval; the handler captures the stack async-signal-safely via
  [cpptrace](https://github.com/jeremy-rifkin/cpptrace)'s `safe_generate_raw_trace`.
  Frames are resolved with `dladdr` (dynamic symbols) and fall back to
  `module+0xRVA` for the stripped BDS internals — which you can symbolicate offline
  against an IDA database or the Windows PDB.
* **Windows:** the sampler suspends the server thread and walks its context with
  `StackWalk64`; frames resolve against the shipped PDB (real names).
* Samples aggregate into a call tree, serialize to spark's protobuf, gzip, and
  either upload to bytebin or write a local `.sparkprofile` file. Symbolization and
  output processing run on a background thread so the server tick never stalls.
  Execution samples use the measured elapsed time between sampling points, excluding
  the target thread's own stack-walk suspension, so multi-thread sweeps retain correct
  time weights even when their effective cadence is longer than the requested interval.
* Every profile includes the SHA-256 of the running BDS executable, allowing an
  offline analyst to select the exact matching binary without receiving the
  server owner's executable, paths, configuration, or world data.

### Native allocation profiler

`--alloc` profiles successful native allocation requests made by the BDS server
thread. Samples are weighted by requested bytes using
a randomized fixed-byte interval (524287 bytes by default), then exported through
the same spark viewer, upload, and save paths as execution profiles.

`--alloc-live-only` follows sampled allocations through realloc and free calls,
including releases from other threads, and reports only allocations still live
when profiling stops. It is intended to identify retained-memory and leak
candidates; repeated profiles are needed to distinguish growth from legitimate
long-lived state.

Windows intercepts UCRT and process-heap allocation entry points with funchook.
Linux atomically redirects the BDS executable's glibc import slots without
rewriting allocator instructions. Direct virtual-memory calls, custom allocator
internals, and allocations made entirely by other threads are outside the current
profile scope.

Stack symbolization and call-tree aggregation run outside the hook path. A fixed
preallocated queue drops and reports excess samples instead of blocking the server
thread. Hooks remain disabled pass-throughs between sessions and are fully removed
after in-flight calls finish during plugin shutdown, allowing a clean plugin reload.

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
