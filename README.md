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

| Command                           | Description                                               |
| --------------------------------- | --------------------------------------------------------- |
| `/spark profiler start [flags]` | Start profiling selected native threads (background).     |
| `/spark profiler start --alloc` | Profile native allocation call stacks.                    |
| `/spark profiler stop`          | Stop profiling and finalize the profile.                  |
| `/spark profiler info`          | Show status of the running profiler.                      |
| `/spark profiler cancel`        | Stop profiling without generating a profile.              |
| `/spark tps`                    | Show rolling TPS, MSPT distributions, and CPU usage.      |
| `/spark ping`                  | Show player ping RTT statistics (min/median/p95/max).    |
| `/spark health`                 | Add process and host resources to the performance report.  |
| `/spark health --upload`        | Upload a health report to the spark viewer.                |
| `/spark activity`               | Show recent profiler and health report activity.           |
| `/spark tickmonitor`            | Report ticks that exceed a duration or baseline change.   |

By default, stopping a profiler uploads the generated profile to spark's bytebin
and prints the viewer link. With `--save-to-file`, the profile is written locally
under `plugins/spark/profiles/` as a `.sparkprofile` file instead. If an upload
fails, Spark automatically preserves the compressed profile in the same directory
and reports the local path.

Permission: `endstone.command.spark` (operators by default).

### Viewing and reading a profile

Open the URL printed when an uploaded profile finishes. For `--save-to-file`,
open [spark.lucko.me](https://spark.lucko.me/) and drag the `.sparkprofile` file
from `plugins/spark/profiles/` into the page. The viewer's call tree and flame
graph show callers above callees. **Total** is the inclusive sampled time or
bytes attributed to a node and all of its children; **Self** is work attributed
to that frame itself. Percentages are shares of the selected thread/root, not a
probability that a symbol name is correct.

Native frames use the following forms:

```text
bedrock_server.Level::_subTick()                       resolved symbol
bedrock_server.0x116d77e (str: Level - tick redstone)() strong runtime guess
bedrock_server.0x123456 (vtable?: Level::<virtual>)()   tentative runtime guess
bedrock_server.0x654321()                               unresolved RVA
```

A resolved PDB or dynamic symbol replaces the RVA completely. Runtime guesses
retain the RVA and name their evidence source: `rtti` is a verified runtime type,
`vtable` is a class and virtual-table slot, `str` is a referenced semantic string,
and `thunk` is a verified jump wrapper. A `?` after the source, such as `str?:`
or `vtable?:`, means the evidence is useful but cannot identify an exact member.
Conflicting or unsafe evidence is omitted rather than displayed as tentative.

Profiles may contain one root per selected native thread. Execution-profile
weights are elapsed sampled microseconds; allocation-profile weights are sampled
requested bytes. The metadata pages report the BDS hash and version, loaded
plugins, configured interval and filters, TPS/MSPT/CPU windows, and any sampling,
queue, unwind, or allocation-hook drops that make a profile incomplete.

### `/spark tps` and `/spark health`

`/spark tps` reads the same profiler-independent history used by exported
profiles. It reports TPS over 5 seconds, 10 seconds, 1 minute, 5 minutes, and
15 minutes; MSPT mean/minimum/median/p95/maximum over 10 seconds, 1 minute, and
5 minutes; and process/system CPU over 10 seconds, 1 minute, and 15 minutes.
Until enough server history exists, each label uses the data actually available
and the command explicitly reports the shorter history span.

`/spark health` includes that report, then adds server uptime and players plus
available process RSS, virtual address space, thread count, physical memory,
swap/page-file, disk, CPU/OS details, and per-interface network throughput
(RX/TX bytes per second, 15-minute rolling mean). Resource-query failures are
omitted instead of being displayed as zero. On Windows, the virtual-memory value
is the process's reserved or committed address space; swap/page-file usage
follows Windows commit limit semantics. On Linux, these values use `VmSize` and
`/proc/meminfo`.

`/spark health --upload` generates a spark `HealthData` protobuf containing the
same statistics, platform metadata, system resources, 15-minute time-window
history, and plugin list, then uploads it to the spark viewer. The viewer link
is printed in chat.

### `/spark activity`

`/spark activity` shows a paginated list of recent profiler uploads, saved
profiles, and health report uploads. Each entry records who triggered it, when
it happened, and the resulting URL or file path. The log is persisted in
`activity.json` in the plugin data folder across server restarts. URL entries
expire after 60 days; file entries are kept indefinitely. Use `--page <number>`
to navigate beyond the first page (4 entries per page).

### `/spark ping`

`/spark ping` reports player ping RTT statistics: the current snapshot (min,
median, p95, max) and the rolling 15-minute average of the median across all
online players. Use `--player <name>` to query a specific player
(case-insensitive). Ping is polled every 10 seconds and the rolling average is
also included in exported profile metadata.

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
* `--save-to-file` — write a `.sparkprofile` file under
  `plugins/spark/profiles/` instead of uploading it (open the file by dragging it
  into the spark viewer).
* `--thread <name>` — select a thread by case-insensitive exact name; repeat the
  flag to select multiple threads and quote names containing spaces. This works for
  execution and allocation profiles.
* `--thread *` — select all BDS process threads and emit separate viewer roots. It
  is equivalent to allocation mode's default all-thread selection and cannot be
  combined with another `--thread` or `--regex`.
* `--regex` — interpret each `--thread <pattern>` as a case-insensitive full-match
  regular expression; at least one pattern is required. This works for execution
  and allocation profiles.
* `--not-combined` — export each sampled thread as a separate viewer root instead of
  grouping threads by pool name.
* `--combine-all` — merge all sampled threads into a single viewer root instead of
  grouping by pool name.
* `--ignore-sleeping` — execution profiles only. Skip threads that are idle
  (Linux task state or Windows per-thread CPU cycle deltas). Without this flag,
  sleeping threads are included in the sample set.
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
  Frames are resolved with `dladdr` (dynamic symbols). Unresolved frames in the
  stripped BDS main executable retain `module+0xRVA` and may receive evidence-tagged
  class/slot or string guesses recovered from ELF unwind metadata, Itanium RTTI,
  vtables, and decoded instructions. Matching Linux debug data or an IDA database
  can replace those RVAs offline; Windows PDB addresses are not interchangeable.
* **Windows:** the sampler suspends one selected target per interval, retains its
  current instruction address, and walks callers with `StackWalk64`; frames resolve
  against the shipped PDB (real names). Without a PDB, unresolved main-executable
  frames use evidence-tagged guesses recovered from PE exception data, MSVC RTTI,
  vtables, thunks, and decoded string references. A failed caller unwind therefore
  shortens the sample instead of discarding it.
* Samples aggregate into per-thread call trees, serialize to spark's protobuf,
  gzip, and either upload to bytebin or write a local `.sparkprofile` file under
  `plugins/spark/profiles/`.
  Symbolization and output processing run on a background thread so the server
  tick never stalls.
  Execution samples use the measured elapsed time between sampling points, excluding
  the target thread's own stack-walk suspension, so multi-thread sweeps retain correct
  time weights even when their effective cadence is longer than the requested interval.
* A profiler-independent statistics service continuously retains up to 15 minutes
  of completed ticks and one-second process/system CPU observations in fixed-capacity
  ring buffers. Tick recording does not allocate or sort; rolling TPS, MSPT
  percentiles, and CPU averages are calculated only when a snapshot is requested.
  Profile metadata and Viewer time windows are derived from this same history.
  `/spark tps` and `/spark health` also read this snapshot, so commands and
  profiles cannot silently use different TPS, MSPT, or CPU definitions.
  Per-second windows include exact time bounds, tick count/rate, MSPT median/max,
  CPU, and the latest low-cost player count. Entity and chunk histories are omitted
  because obtaining them would require a main-thread world scan every second.
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
Windows retries fresh suspended-thread snapshots for up to 30 seconds when a
thread is concurrently starting or exiting, without patching while any context
cannot be inspected.

Coverage is limited to the listed allocator entry points/imports. Static CRT
copies, inlined or private allocators, arenas and object pools that do not reach a
covered entry point, `VirtualAlloc`/`VirtualFree`, and `mmap`/`munmap` are not
sampled. A Linux module loaded and unloaded entirely between rescans can escape
coverage.

## Building

> CMake fetches upstream funchook `v1.1.3` because its bundled distorm decoder is
> used by both x86-64 symbol guessers. The Windows allocation profiler also links
> funchook itself; Linux allocation profiling still uses atomic ELF import-slot
> redirection and does not link the funchook hook library.

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
`spark_selftest --statistics-only` deterministically verifies independent TPS and
CPU windows, true MSPT median/p95 calculations, partial-history spans, and exact
per-second profile boundaries. The default self-test also decodes key rolling and
window fields from the generated current-protocol payload.

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
