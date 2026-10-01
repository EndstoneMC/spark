# Using Spark

Endstone Spark profiles native Bedrock Dedicated Server work and opens the result
in the standard [spark viewer](https://spark.lucko.me/). It can sample the server
thread or selected process threads, including native work such as chunk
generation, entity ticking, redstone, and pathfinding.

For installation, configuration, and upgrade guidance, see the [README](../README.md)
and [Configuration](configuration.md).

## Install

Download `endstone_spark.dll` on Windows or `endstone_spark.so` on Linux from the
[latest GitHub Release](https://github.com/EndstoneMC/spark/releases/latest) and
place it directly in the server's `plugins/` directory. Start or restart BDS to
load Spark. Fully restart the server when upgrading; reloading may not fully apply
the update.

## Commands and permissions

| Command | Description |
| --- | --- |
| `/spark profiler start [flags]` | Start an execution or allocation profile. |
| `/spark profiler stop [flags]` | Stop and export the current profile. |
| `/spark profiler upload` | Alias for `profiler stop`. |
| `/spark profiler info` | Show the current profile's status and sample counts. |
| `/spark profiler cancel` | Stop without creating a profile. |
| `/spark profiler open` | Open a live viewer for a running profile. |
| `/spark profiler trust-viewer --id <client id>` | Approve a pending live viewer client. |
| `/spark tps` | Show rolling TPS, MSPT distributions, and CPU usage. |
| `/spark ping [--player <name>]` | Show ping summaries or query a player. |
| `/spark health` | Open the live health dashboard. |
| `/spark health show [--memory] [--network]` | Show the local performance and resource report. |
| `/spark health upload` | Upload a health report to the spark viewer. |
| `/spark health trust-viewer --id <client id>` | Approve a pending dashboard client. |
| `/spark activity [--page <number>]` | Show recent profile and health report activity. |
| `/spark tickmonitor [flags]` | Report ticks that exceed a threshold. |

Aliases include `/spark sampler` for `profiler`, `/spark cpu` for `tps`,
`/spark healthreport` and `/spark ht` for `health`, `/spark activitylog` and
`/spark log` for `activity`, and `/spark tickmonitoring` for `tickmonitor`.
Health also accepts `/spark health dashboard`, `/spark health --upload`, and
`/spark health upload`.

`endstone.command.spark` and Java-compatible `spark` are umbrella permissions,
granted to operators by default. Per-command permissions are `spark.profiler`,
`spark.tps`, `spark.ping`, `spark.health`, `spark.activity`, and
`spark.tickmonitor`.

## Start and stop a profile

By default, an execution profile samples the server thread every 4 ms. Stopping
uploads the profile to spark's bytebin and prints a viewer link. If the upload
fails, Spark saves the raw profile locally and reports its path. To save locally
without uploading, use `--save-to-file`; files are written under
`plugins/spark/profiles/` and can be opened by dragging them into the viewer.

| Flag | Effect and limits |
| --- | --- |
| `--interval <value>` | Execution sampling interval in milliseconds (default `4`, range `1`–`1000`); with `--alloc`, allocation sampling interval in bytes (default `524287`, range `1`–`2147483647`). |
| `--timeout <seconds>` | Stop and export automatically after a whole number of seconds greater than `10`. Omit it to run until stopped or cancelled. |
| `--only-ticks-over <ms>` | Keep samples only from completed ticks strictly longer than this positive whole-number threshold. The unfinished tick at finalization is excluded. |
| `--comment <text>` | Add a profile note. Quote text containing spaces. A value supplied to `stop` is used for the final profile. |
| `--save-to-file` | Save a `.sparkprofile` file under `plugins/spark/profiles/` instead of uploading. Can also be supplied to `stop`. |
| `--thread <name>` | Select a process thread by case-insensitive exact name. Repeat to select multiple threads; quote names with spaces. |
| `--thread *` | Select all process threads. Root grouping still follows the selected grouping mode. Cannot be combined with another `--thread` or `--regex`. |
| `--regex` | Treat each `--thread` value as a case-insensitive, full-match regular expression. Requires at least one `--thread`. |
| `--not-combined` | Export each sampled thread as its own root instead of grouping threads by pool name. |
| `--combine-all` | Merge sampled threads into one root instead of grouping by pool name. Cannot be combined with `--not-combined`. |
| `--ignore-sleeping` | Execution profiles only: skip threads detected as idle. Sleeping threads are included by default. |
| `--alloc` | Profile sampled native allocation call stacks and requested bytes instead of execution time. |
| `--alloc-live-only` | Implies `--alloc`; export only sampled allocations still live when the profile stops. |

Numeric flag values are interpreted by absolute magnitude. A zero interval selects
the mode's default. Values are rounded to the nearest whole interval after parsing.
Execution interval is capped at `1000` ms; allocation interval is capped at
`2147483647` bytes. Timeout must be greater than 10 seconds, and
`--only-ticks-over` must be greater than zero.

Execution profiles sample the server thread unless thread selection is supplied.
Allocation profiles include all covered process threads by default. Thread names
and regular expressions use full-name matching without regard to case. The
execution interval is a shared stack-walk budget when several threads are selected,
so each selected thread may be sampled less often than the requested interval.

## Read a profile

Open the URL printed after an upload. For a locally saved profile, open
[spark.lucko.me](https://spark.lucko.me/) and drag the `.sparkprofile` file into
the page. In the call tree and flame graph, callers appear above callees. **Total**
is the inclusive sampled time or bytes attributed to a frame and its children;
**Self** is attributed to that frame alone. Percentages are shares of the selected
thread or root.

Execution profiles weight samples by elapsed sampled microseconds. Allocation
profiles weight them by sampled requested bytes. Profiles can contain a root per
selected thread. The default grouping combines threads by pool name;
`--not-combined` keeps them separate and `--combine-all` merges them into one root.
The metadata pages include available BDS hash and version, loaded plugins,
configuration and filters, rolling statistics, and sampling, unwind, queue, or
allocation-hook drops that can make the profile incomplete.

Resolved native symbols are shown directly. If a frame is unresolved, Spark may
show a tentative runtime guess while retaining its address, for example:

```text
bedrock_server.Level::_subTick()                       resolved symbol
bedrock_server.0x116d77e (str?: Level - tick redstone)() tentative runtime guess
bedrock_server.0x123456 (vtable?: Level::<virtual>)()   tentative runtime guess
bedrock_server.0x654321()                               unresolved RVA
```

Labels such as `rtti`, `vtable`, `str`, and `thunk` identify the evidence used;
`?` marks incomplete evidence. Existing PDB or dynamic symbols take precedence.
Conflicting or unsafe guesses are omitted. On CPython 3.12 and newer, profiles can
also include Python plugin frames without exporting server-owner directory paths.
If Python attribution is unavailable, Spark continues with native sampling. See
[Python function attribution](python-function-attribution.md) for runtime details.

## Allocation profiles

The native allocation profiler is supported on Windows x64 and Linux x86-64. It
samples successful native allocation requests by approximate byte interval.
Normal `--alloc` profiles record sampled allocations during the session. The
`--alloc-live-only` mode follows sampled allocations through free and realloc and
keeps only those still live at export; use repeated profiles to distinguish growth
from legitimate long-lived memory.

Coverage depends on allocator calls reaching supported entry points. Windows hooks
supported UCRT and process-heap imports. Linux patches supported dynamic ELF
imports for `malloc`, `calloc`, `realloc`, `reallocarray`, `aligned_alloc`, and
`posix_memalign`. Linux supports the default glibc allocator and compatible
jemalloc or mimalloc loaded at process startup with `LD_PRELOAD`; the effective
allocator must route through the covered C interfaces. jemalloc requires
`opt.zero_realloc=free`. Custom allocators and allocator modules loaded later with
`dlopen` are unsupported.

Static CRT copies, inlined allocators, pools that bypass covered entry points,
direct virtual-memory calls, and memory mappings are not sampled. A Linux module
loaded and unloaded between module scans can also escape coverage. Profile metadata
reports hook coverage, capacity, and dropped or filtered samples.

## Live viewer and trusted clients

Run `/spark profiler open [--comment <text>]` while an execution or allocation
profile is running. The optional comment is attached to the live viewer profile.
Spark uploads sampler data about once a minute and standalone rolling statistics
every 10 seconds, then prints a live viewer URL. The viewer remains available until
the profiler is stopped, cancelled, or times out. A normal `--alloc` viewer shows
session allocations cumulatively; an `--alloc-live-only` viewer shows allocations
still retained at each update.

When the viewer connects, Spark checks its public key against
`trusted-viewers.json`. A trusted client receives data immediately. An unknown
client is held pending; approve it with `/spark profiler trust-viewer --id <client
id>`, using the ID shown by the viewer. Health dashboard clients use the same
trust list and can be approved with `/spark health trust-viewer --id <client id>`.
The approval is saved for later sessions. See [Configuration](configuration.md)
for the trust file location and format.

The automatic background profiler is enabled by default. A valid foreground start
pauses it. Stopping and exporting the foreground profile restarts it after export;
cancelling or timing out leaves it paused until Spark is reloaded. If a previous
profiler timer or viewer is still closing, a new profiling session may ask you to
retry.

## TPS, health, ping, and tick monitor

`/spark tps` reports TPS for 5 seconds, 10 seconds, 1 minute, 5 minutes, and
15 minutes; MSPT mean, minimum, median, 95th percentile, and maximum for 10 seconds,
1 minute, and 5 minutes; and process and system CPU for 10 seconds, 1 minute, and
15 minutes. Early after startup, longer windows use the available history and the
command reports that span.

`/spark health show` includes those statistics, uptime, player count, and available
process RSS, physical memory, disk, CPU/OS details, and per-interface network rates.
Resource-query failures are omitted instead of shown as zero. `--memory` adds
process virtual memory and thread count plus swap or page-file details.
`--network` includes interfaces whose current rate is zero. On Windows, virtual
memory is reserved or committed process address space and page-file usage follows
commit-limit semantics. On Linux, process memory and thread data come from `/proc`,
and physical memory and swap data come from `/proc/meminfo`.

`/spark health` opens the live dashboard. `/spark health upload` uploads a report
with statistics, platform and system resources, time-window history, and the plugin
list, then prints the viewer link.

`/spark ping` shows the current min/median/95th-percentile/max ping summary and the
rolling 15-minute average of each poll's median. Use `--player <name>` to query one
player; matching ignores case. Ping is polled every 10 seconds and is also included
in exported profile metadata when available.

`/spark tickmonitor` measures a baseline over 120 ticks (about six seconds), then
reports ticks more than 100% above the baseline. Use `--threshold <percent>` for a
different positive percentage, or `--threshold-tick <ms>` for a positive absolute
duration. These flags cannot be used together. Run the command again to disable the
monitor.

## Activity log

`/spark activity` lists recent profile uploads, saved profiles, and health reports,
including who started them, when they completed, and the resulting URL or file
path. The log is stored in `activity.json` in the plugin data folder. Each page
shows four entries; use `--page <number>` to view another page. URL entries expire
after 60 days; saved-file entries do not expire.

## PlaceholderAPI

When [Endstone PAPI](https://github.com/EndstoneMC/papi) is installed, Spark
registers an optional `spark` expansion. PAPI is not required for Spark to start or
profile. Unknown parameters and values without usable samples remain unresolved.

| Placeholder | Value |
| --- | --- |
| `{spark:tps}` | TPS windows: 5s, 10s, 1m, 5m, and 15m. |
| `{spark:tps_5s}`, `{spark:tps_10s}`, `{spark:tps_1m}`, `{spark:tps_5m}`, `{spark:tps_15m}` | One TPS window. |
| `{spark:tickduration}` | MSPT min/median/p95/max for the latest 200 and 1200 ticks. |
| `{spark:tickduration_10s}`, `{spark:tickduration_1m}` | One tick-duration window. |
| `{spark:cpu_system}` | System CPU for 10s, 1m, and 15m. |
| `{spark:cpu_system_10s}`, `{spark:cpu_system_1m}`, `{spark:cpu_system_15m}` | One system CPU window. |
| `{spark:cpu_process}` | BDS process CPU for 10s, 1m, and 15m. |
| `{spark:cpu_process_10s}`, `{spark:cpu_process_1m}`, `{spark:cpu_process_15m}` | One process CPU window. |

Output follows Java spark's precision, ordering, color codes, and over-target TPS
marker. These placeholders do not depend on a particular player.

## Crash recovery

Spark journals an active execution or allocation profile under
`plugins/spark/profiles/recovery/`. If BDS crashes or is forcibly killed, Spark
replays the last unclean supported session on the next startup and saves a recovered
`.sparkprofile` under `plugins/spark/profiles/`. Recovery includes only records
persisted before the interruption, so sampling or queue losses can still make the
profile incomplete. Cleanly ended sessions are discarded. Allocation
`--alloc-live-only` sessions cannot be recovered because the journal does not
contain the allocation free/realloc lifecycle. The recovery journal is removed
after a successful save and retained if saving fails.

Crash recovery covers BDS process crashes and forced termination. Durability across
operating-system crashes or power loss is not guaranteed. If metadata snapshots or
journal pruning keep failing, recovery journaling is disabled for the current session
while profiling and the server continue, and the operator is notified. If cleanup
fails, a sibling replay block prevents the same journal directory generation from
being replayed again; a successful fresh-writer purge clears the block before a new
journal is created.

An independent watchdog records stall begin/end events if the server main thread
stops ticking for more than five seconds. This helps identify stalls in a recovered
profile.

## License

Spark is GPLv3, matching the upstream spark project whose profile format and viewer
this plugin uses. See [LICENSE](../LICENSE).
