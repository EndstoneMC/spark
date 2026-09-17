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
| `/spark profiler open`         | Open a live, auto-updating spark viewer for the running profile. |
| `/spark profiler trust-viewer --id <client id>` | Approve a pending live viewer client. |
| `/spark tps`                    | Show rolling TPS, MSPT distributions, and CPU usage.      |
| `/spark ping`                  | Show player ping RTT statistics (min/median/p95/max).    |
| `/spark health`                 | Add process and host resources to the performance report.  |
| `/spark health --upload`        | Upload a health report to the spark viewer.                |
| `/spark activity`               | Show recent profiler and health report activity.           |
| `/spark tickmonitor`            | Report ticks that exceed a duration or baseline change.   |

By default, stopping a profiler uploads the generated profile to spark's bytebin
and prints the viewer link. With `--save-to-file`, the profile is written locally
under `plugins/spark/profiles/` as a `.sparkprofile` file instead. If an upload
fails, Spark automatically preserves the raw protobuf profile in the same directory
and reports the local path.

Permission: `endstone.command.spark` (operators by default) is the umbrella
permission. Per-command permissions are also available: `spark.profiler`,
`spark.tps`, `spark.ping`, `spark.health`, `spark.activity`, and
`spark.tickmonitor`.

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
bedrock_server.0x116d77e (str?: Level - tick redstone)() tentative runtime guess
bedrock_server.0x123456 (vtable?: Level::<virtual>)()   tentative runtime guess
bedrock_server.0x654321()                               unresolved RVA
```

A resolved PDB or dynamic symbol replaces the RVA completely. Runtime guesses
retain the RVA and name their evidence source: `rtti` is a verified runtime type,
`vtable` is a class and virtual-table slot, `str` is a referenced semantic string,
and `thunk` is a verified jump wrapper. A `?` after the source, such as `str?:`
or `vtable?:`, means the evidence is useful but cannot identify an exact member.
Linux string evidence remains tentative even when its semantic score is high.
Conflicting or unsafe evidence, including ambiguous lambda ownership, is omitted
rather than displayed as tentative. Instruction-based guesses use validated
instruction boundaries.

Profiles may contain one root per selected native thread. Execution-profile
weights are elapsed sampled microseconds; allocation-profile weights are sampled
requested bytes. The metadata pages report the available BDS hash and version, loaded
plugins, configured interval and filters, TPS/MSPT/CPU windows, and any sampling,
queue, unwind, or allocation-hook drops that make a profile incomplete.
With `--not-combined`, threads retain independent complete call trees even when
they have the same name.

On CPython 3.12+, sampled Python plugin frames include the filename leaf and
CodeId without exporting server-owner directory paths. Native frame identities
and available line data are retained. If the public Python runtime API is
unavailable, or code registration fails, Spark continues with native-only
sampling and records the support state, reason, and native/Python attribution
counters in export diagnostics. After a registration failure, Spark stops
admitting new Python symbols for the rest of that profiling session. CPython
3.11 remains native-only; see [Python function attribution](docs/python-function-attribution.md)
for the runtime and fallback details.

### `/spark tps` and `/spark health`

`/spark tps` reads the same profiler-independent history used by exported
profiles. It reports TPS over 5 seconds, 10 seconds, 1 minute, 5 minutes, and
15 minutes; MSPT mean/minimum/median/p95/maximum over 10 seconds, 1 minute, and
5 minutes; and process/system CPU over 10 seconds, 1 minute, and 15 minutes.
Until enough server history exists, each label uses the data actually available
and the command explicitly reports the shorter history span.

`/spark health` includes that report, then adds server uptime and players plus
available process RSS, physical memory, disk, CPU/OS details, and active
per-interface network throughput (RX/TX bytes per second, 15-minute rolling
mean). Resource-query failures are omitted instead of being displayed as zero.
`/spark health show --memory` adds process virtual memory, process thread count,
and swap/page-file details. `--network` includes interfaces whose current rate
is zero as well as active interfaces. On Windows, the virtual-memory value is
the process's reserved or committed address space; swap/page-file usage follows
Windows commit-limit semantics. On Linux, process RSS and virtual memory come
from `/proc/self/statm` (with process thread data from `/proc/self/status`),
while host physical memory and swap values come from `/proc/meminfo`.

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

### Live viewer

`/spark profiler open` opens a real-time spark viewer while an execution or
allocation profiler is running. It connects to the spark WebSocket relay,
uploads sampler data every minute and sends standalone rolling statistics every
10 seconds, then displays the viewer URL in chat. Sampler rotations follow the
globally aligned profiling windows. A
normal `--alloc` viewer is cumulative from session start; an
`--alloc-live-only` viewer shows sampled allocations retained at each update.
The viewer stays live until the profiler is stopped, cancelled, or times out.
Relay connection, compression, and uploads run asynchronously; command
completion and failures are reported back on the server thread without waiting
for a network timeout.
Viewer uploads support cancellation and compression runs in bounded steps. Before
any new profiling session starts, the previous timer thread must exit and the
previous viewer must retire within a shared 500 ms budget. If either is still
stopping, the command reports an error asking you to retry. An ordinary viewer
close timeout does not terminate the server. During plugin shutdown, Spark uses
bounded quiescence checks and aborts before unloading if cleanup cannot be
proven complete.

When the automatic background profiler is enabled, a valid foreground start
pauses it. An invalid start leaves it running. Explicitly stopping and exporting
the foreground profile restarts the background profiler after export completes;
cancel or timeout leaves it paused until Spark is reloaded.

When a client connects to the live viewer, the server checks the client's
public key against the trusted viewer list in `trusted-viewers.json`. Trusted clients
receive immediate access to the live sampler data. Untrusted clients receive an
UNTRUSTED response and their public key is held pending. Use
`/spark profiler trust-viewer --id <client id>` to approve a pending client;
the key is then persisted in `trusted-viewers.json` and the client receives an ACCEPTED
response with access to the data stream.

### `/spark profiler start` flags

* `--interval <value>` — execution interval in milliseconds (default `4`, maximum
  `1000`), or allocation interval in bytes with `--alloc` (default `524287`).
* `--timeout <seconds>` — auto-stop and finalize after the specified number of
  seconds, which must be greater than `10`. Omit this flag to run until `stop` or
  `cancel` is issued.
* `--only-ticks-over <ms>` — retain samples only from ticks longer than the given
  positive whole number of milliseconds. Samples from the unfinished tick at
  finalization are excluded and reported separately from incomplete-data drops.
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
* `--alloc-live-only` — record only sampled allocations currently retained for
  leak analysis; this implies `--alloc`.

Numeric flags are parsed by absolute magnitude. `--interval 0` selects the
execution default of 4 ms or the allocation default of 524287 bytes; after this
normalization, the existing positive validation rules still apply (execution
interval `1`-`1000` ms, allocation interval `1`-`524287` bytes, timeout over
10 seconds, and `--only-ticks-over` greater than 0 ms).

Multi-thread execution profiles treat the interval as a global stack-walk budget and
rotate fairly through matching threads. `/spark profiler stop` also accepts
`--save-to-file` and `--comment <text>`; values supplied at stop take effect for the
final output.

## PlaceholderAPI integration

When [Endstone PAPI](https://github.com/EndstoneMC/papi) is installed, Spark
registers the optional `spark` expansion and serves the same rolling data used by
`/spark tps`. PAPI is not required: Spark starts and profiles normally when the
service is absent or inactive. Unknown parameters and values without a usable
sample remain unresolved.

| Placeholder | Value |
| --- | --- |
| `{spark:tps}` | TPS for 5s, 10s, 1m, 5m, and 15m |
| `{spark:tps_5s}`, `{spark:tps_10s}`, `{spark:tps_1m}`, `{spark:tps_5m}`, `{spark:tps_15m}` | One TPS window |
| `{spark:tickduration}` | MSPT min/median/p95/max for the latest 200 and 1200 ticks |
| `{spark:tickduration_10s}`, `{spark:tickduration_1m}` | One 200-tick or 1200-tick MSPT distribution |
| `{spark:cpu_system}` | System CPU for 10s, 1m, and 15m |
| `{spark:cpu_system_10s}`, `{spark:cpu_system_1m}`, `{spark:cpu_system_15m}` | One system CPU window |
| `{spark:cpu_process}` | Spark/BDS process CPU for 10s, 1m, and 15m |
| `{spark:cpu_process_10s}`, `{spark:cpu_process_1m}`, `{spark:cpu_process_15m}` | One process CPU window |

The output preserves Java spark's precision, ordering, percent signs, over-target
TPS marker, and Minecraft color codes. These placeholders are player-independent.

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
  frames use evidence-tagged guesses recovered from PE exception data, decoded MSVC
  RTTI names, vtables, thunks, and bounded string references. String evidence is
  emitted as tentative `str?:` labels; rejected, shared, and
  budget-limited references are reported in export metadata. A failed caller
  unwind therefore shortens the sample instead of discarding it. The sampler
  retries `ResumeThread` up to 32 times after each suspension; if every retry
  fails and the target has not exited, it terminates the process rather than
  leaving BDS suspended.
* Samples aggregate into per-thread call trees and serialize to spark's protobuf.
  Bytebin uploads are gzip-compressed; local `.sparkprofile` files under
  `plugins/spark/profiles/` contain raw protobuf.
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
  CPU, the latest low-cost player count, and rolling entity/chunk gauges maintained
  from platform events and bounded reconciliation without a per-second world scan.
* Profiles include the SHA-256 of the running BDS executable when available,
  allowing an offline analyst to select the exact matching binary without
  receiving the executable contents.

### Native allocation profiler

`--alloc` profiles successful native allocation requests across process threads on
Linux x86-64 and Windows x64. Linux redirects supported ELF allocator imports;
Windows redirects supported allocator IAT slots through Spark-owned process-lifetime
Permanent-IAT gateways whose callbacks fall back safely after Spark-owned handlers
are detached.
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

`--alloc-live-only` follows sampled allocations through realloc and free calls on both supported platforms,
including releases from other threads, and reports only allocations still live
at export time. This applies to each Live Viewer update and the final stopped
profile. It is intended to identify retained-memory and leak candidates;
repeated profiles are needed to distinguish growth from legitimate long-lived
state.

Linux atomically redirects supported allocator relocations in the main executable
and loaded ELF modules, including Endstone, native plugins, and Python. Allocator
providers must be provably part of the main executable's startup `DT_NEEDED`
dependency closure; dynamically loaded custom providers outside that closure are
unsupported. Preloading a provider alone does not make it eligible.
Loaded modules are rescanned at session start
and every five seconds while profiling; unloaded modules are recognized before
restoration so stale slots are never written.

Linux allocation gateways live in permanent anonymous memory, independently of
Spark plugin unload. Allocation profiling adds no permanent pin on Spark itself;
execution profiling and external loader references may still keep the plugin loaded.
Unsupported host unwinders or incompatible resident gateways prevent allocation-hook
admission. These restrictions do not disable execution profiling.

The gateway arena has a limit of 256 groups over the server process lifetime,
not 256 simultaneous profiling sessions. Published groups are never reused after
retirement. Exhausting this capacity or encountering incompatible resident
gateway code requires a server process restart. Reloading the plugin does not
replace permanent gateway code.

On Windows x64, Spark redirects supported UCRT and heap allocation imports through
Spark-owned Permanent-IAT gateways. Shutdown first closes gateway admission, drains
callbacks already admitted into Spark, clears Spark-owned handlers, and restores IAT
slots still owned by the gateway. Stale or ownership-uncertain slots remain safe
because process-lifetime gateway code falls back to the original allocator without
calling unloaded Spark plugin code.

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
Allocation coverage is limited to the listed allocator entry points/imports. Static CRT
copies, inlined or private allocators, arenas and object pools that do not reach a
covered entry point, direct virtual-memory APIs, and memory mappings that bypass the
covered allocator families are not sampled. A Linux module loaded and unloaded
entirely between rescans can escape coverage.

## Crash recovery

When BDS crashes or is forcibly killed during an active profiling session
(execution or allocation, foreground or background), in-memory profile data is
normally lost. Spark mitigates this by writing a crash-safe recovery journal
during profiling.

During each session, the sampler aggregation thread writes compact records
(module definitions, thread definitions, samples, and tick events) to a
segmented journal file under `plugins/spark/profiles/recovery/`. The journal
uses CRC32-validated records (via zlib) so that a truncated tail from an
unclean shutdown is recoverable up to the last complete record.
Journal flushes, segment rotation, and explicit flush requests use durable writes.
Recovery remains limited to records persisted before the interruption; sampling
and queue losses are still reported as incomplete data.

On the next plugin startup, spark replays an unclean supported session into a
call tree, runs normal symbolization, and saves a `.sparkprofile` file under
`plugins/spark/profiles/`. Cleanly ended sessions are discarded, and allocation
live-only sessions are not recovered because the journal lacks free/realloc
lifecycle state. The journal is removed after a successful save and retained if
the save fails.

A separate watchdog thread monitors a monotonic heartbeat updated every server
tick. If the main thread stops ticking for more than 5 seconds, stall
begin/end events are recorded in the journal so that recovered profiles retain
evidence of the stall. The watchdog never calls Endstone APIs and never stops
the profiler, ensuring that stall evidence is preserved for diagnosis.

## Configuration

Spark reads a `config.toml` file from the plugin data directory on startup. If the
file does not exist, spark writes one with default values and explanatory comments.
The file is user-owned: spark never rewrites it during normal operation. Missing
fields use their defaults. Any invalid value makes Spark report the configuration
error and use all defaults for that startup while preserving the file byte-for-byte.
Unknown fields are silently ignored.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `viewerUrl` | string | `"https://spark.lucko.me/"` | HTTP(S) base URL for the spark viewer. |
| `bytebinUrl` | string | `"https://spark-usercontent.lucko.me/"` | HTTP(S) bytebin endpoint for profiles and health reports. |
| `bytesocksHost` | string | `"spark-usersockets.lucko.me"` | Live-viewer WebSocket host with an optional port, but no scheme or path. |
| `backgroundProfiler` | bool | `true` | Whether to auto-start a background execution profiler. |
| `backgroundProfilerInterval` | int | `10` | Background sampling interval in milliseconds (`1`-`1000`). |
| `backgroundProfilerThreadGrouper` | string | `"by-pool"` | Thread grouping mode: `by-pool`, `by-name`, or `as-one`. |
| `backgroundProfilerThreadDumper` | string | `"default"` | Thread selection: `default` (server thread) or `all`. |
| `allocationRateMetrics` | bool | `true` | Keep the count-only native allocation-rate counter active when no allocation profile owns the hooks. It starts lazily when the first server main-thread ID is observed, records process-wide bytes on ticks, and resumes after an explicit allocation profile is exported. `false` disables this persistent counter only; explicit `--alloc` profiling remains available. |
| `serverPropertiesAdditionalKeys` | string | `""` | Comma-separated, administrator-reviewed `server.properties` keys appended to the built-in safe allowlist. Up to 64 unique keys are accepted, each at most 128 characters and limited to letters, digits, `-`, `_`, and `.`. Known-sensitive names and names containing password, passcode, token, secret, credential, or private-key remain blocked. |
| `disableResponseBroadcast` | bool | `false` | Restrict result notifications to the originating player. |

The native plugin also accepts the Java-compatible environment variables
`SPARK_VIEWERURL`, `SPARK_BYTEBINURL`, `SPARK_BYTESOCKSHOST`,
`SPARK_BACKGROUNDPROFILER`, `SPARK_BACKGROUNDPROFILERINTERVAL`,
`SPARK_BACKGROUNDPROFILERTHREADGROUPER`, `SPARK_BACKGROUNDPROFILERTHREADDUMPER`,
`SPARK_ALLOCATIONRATEMETRICS`,
and `SPARK_DISABLERESPONSEBROADCAST`. Environment values override TOML values
in memory and are not written to `config.toml`. Boolean values follow Java's
`Boolean.parseBoolean` behavior; invalid interval text leaves the TOML value
unchanged, while endpoint, thread-mode, and out-of-range interval values make
startup reject the configuration. `serverPropertiesAdditionalKeys` has no
environment-variable counterpart.

The optional Python attribution diagnostic switch `SPARK_PYTHON_ATTRIBUTION_MODE`
accepts `auto` (the default), `off`, or `shadow-only`. `off` keeps profiling
native-only; `shadow-only` keeps the Python shadow stack active for diagnostics.
Unknown values are ignored with a warning.

Trusted viewer public keys are stored separately in `trusted-viewers.json` (a
JSON array of base64-encoded X.509 keys). The `trust-viewer` command appends to
this file without touching `config.toml`.

## Building

> CMake fetches the pinned distorm revision directly for strict instruction-boundary
> decoding in both x86-64 symbol guessers. Linux allocation profiling uses atomic ELF
> import-slot redirection. Windows allocation profiling uses Spark-owned IAT redirection
> through process-lifetime Permanent-IAT gateways.

The platform requirements are CMake 3.29 or newer on both platforms. The
minimum supported compiler is Clang 18 or clang-cl 18; CI currently tests Clang
20 and clang-cl 20:

* **Linux:** Clang 18 or newer, libc++, Ninja, and Conan 2.
  The default Linux test suite also builds a statically linked symbol fixture;
  make Clang's `libc++.a` and `libc++abi.a` archives available.
* **Windows:** LLVM clang-cl 18 or newer, Visual Studio Build Tools, the Windows SDK,
  Ninja, and Conan 2. clang-cl must target the MSVC ABI.

Python tooling and native Python runtime tests require Python 3.12 or newer with
its shared runtime. Set `SPARK_TEST_LIBPYTHON` (or pass
`-DSPARK_TEST_LIBPYTHON:FILEPATH=...`) to the matching `python312.dll` on Windows,
or to the matching `.so` from Python's `LIBDIR`/`LDLIBRARY` on Linux. For
example, the CI discovery commands are equivalent to:

```powershell
$env:SPARK_TEST_LIBPYTHON = python -c "import sys; from pathlib import Path; print(Path(sys.base_prefix) / f'python{sys.version_info.major}{sys.version_info.minor}.dll')"
```

```shell
export SPARK_TEST_LIBPYTHON="$(python -c 'import sysconfig; from pathlib import Path; print(Path(sysconfig.get_config_var("LIBDIR")) / sysconfig.get_config_var("LDLIBRARY"))')"
```

An optional Linux-only CPython 3.11 fallback test is enabled separately with
`-DSPARK_TEST_LIBPYTHON_311:FILEPATH=...`; it does not replace the required
3.12-or-newer runtime. If only the plugin is needed, pass
`-DENDSTONE_SPARK_BUILD_SELFTEST=OFF`. To build the offline self-tests without
fetching Endstone or PAPI, pass `-DENDSTONE_SPARK_BUILD_PLUGIN=OFF` instead.

Install Conan, resolve the dependencies, then configure CMake directly with the
generated toolchain file:

```shell
pip install conan

conan install . --build=missing

cmake -S . -B build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=build/RelWithDebInfo/generators/conan_toolchain.cmake" "-DCMAKE_BUILD_TYPE=RelWithDebInfo"

cmake --build build
```

To build the common profiler, application, native, protobuf, and network layers
without fetching Endstone or PAPI, add `-DENDSTONE_SPARK_BUILD_PLUGIN=OFF` to
the configure command. The default is `ON` and retains the plugin build.
The plugin build pins the official Endstone v0.11.11 public API and the pinned
public PlaceholderAPI headers.

For full Linux CTest coverage, including production-sampler, unload, and legacy
tests, leave `-DENDSTONE_SPARK_GATEWAY_SAMPLER_TESTS=ON` enabled (it is the
default). Set it to `OFF` only when those isolated gateway sampler tests are not
needed.

With self-test tools enabled, Linux `spark_selftest --allocation-only` exercises exact,
regex, multiple, dynamic, and no-match allocation thread selection, cross-thread
free/realloc and live-only lifecycles, session reuse, thread overflow, and bounded
queue/index pressure. `spark_allocation_benchmark` prints repeatable CSV medians for
unprofiled and disabled-hook baselines, default/4 KiB intervals,
single/four-thread, live-only, and forced saturation cases.
`spark_selftest --statistics-only` deterministically verifies independent TPS and
CPU windows, true MSPT median/p95 calculations, partial-history spans, and exact
per-second profile boundaries. The default self-test also decodes key rolling and
window fields from the generated current-protocol payload. On Windows,
`spark_windows_allocation_backend_test` exercises Permanent-IAT installation,
late-module refresh, real sampled call trees, repeat sessions, and harmless post-shutdown
allocator pass-through.

On Linux, the bundled profile selects libunwind because the SIGPROF sampler
requires cpptrace's async-signal-safe unwinding path. Windows does not use
libunwind; cpptrace uses its native Windows backend while spark captures stacks
with StackWalk64.

On Windows, copy `build/endstone_spark.dll` into your server's `plugins/`
directory.

On Linux, copy `build/endstone_spark.so` into the server's `plugins/` directory:

```text
plugins/
  endstone_spark.so
```

No helper library is required.
Restart the full BDS process when replacing an older development Permanent-IAT
gateway with gateway ABI 3 or changing the gateway's code identity. ABI 3
preserves its published state offsets for compatible rediscovery; incompatible
resident registry code is not reused.

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
