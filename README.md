# endstone-spark

A [spark](https://spark.lucko.me/)-style sampling profiler for
[Endstone](https://github.com/EndstoneMC/endstone) — find out where your Bedrock
Dedicated Server is actually spending its tick time.

Unlike a plugin-only timings system, endstone-spark is a **native statistical
sampling profiler**: it periodically snapshots the BDS server thread's real call
stack, so it covers **all** of BDS's internal work (chunk gen, entity ticking,
redstone, pathfinding, …), not just plugin code — even though the server binary is
stripped. Results are uploaded to spark's own viewer and open as an interactive
flame graph at `https://spark.lucko.me/<id>`.

> The profile format and the web viewer are spark's. endstone-spark produces
> spark-compatible data; all credit for the viewer and format goes to
> [lucko/spark](https://github.com/lucko/spark).

## Commands

| Command | Description |
| --- | --- |
| `/spark profiler start` | Start profiling the server thread (background). |
| `/spark profiler stop` | Stop, upload, and print the viewer link. |
| `/spark profiler info` | Show status of the running profiler. |
| `/spark profiler cancel` | Stop without uploading. |
| `/spark tps` | Ticks-per-second and tick duration (MSPT). |
| `/spark health` | TPS/MSPT plus process memory, threads, uptime. |

Permission: `endstone.command.spark` (operators by default).

### `/spark profiler start` flags

- `--interval <ms>` — sampling interval (default `4`).
- `--timeout <seconds>` — auto-stop and upload after N seconds.
- `--only-ticks-over <ms>` — only record ticks longer than this.
- `--save-to-file` — write a `.sparkprofile` instead of uploading (open it by
  dragging it into the spark viewer).
- `--comment <text>` — attach a note to the profile.
- `--include-sleeping` — also sample while the server thread is idle between ticks
  (off by default, since the inter-tick sleep would otherwise dominate a
  wall-clock profile).

## How it works

- **Linux:** a dedicated sampler thread signals the server thread (`SIGPROF`) on the
  chosen interval; the handler captures the stack async-signal-safely via
  [cpptrace](https://github.com/jeremy-rifkin/cpptrace)'s `safe_generate_raw_trace`.
  Frames are resolved with `dladdr` (dynamic symbols) and fall back to
  `module+0xRVA` for the stripped BDS internals — which you can symbolicate offline
  against an IDA database or the Windows PDB.
- **Windows:** the sampler suspends the server thread and walks its context with
  `StackWalk64`; frames resolve against the shipped PDB (real names).
- Samples aggregate into a call tree, serialize to spark's protobuf, gzip, and
  upload to bytebin. Symbolization and upload run on a background thread so the
  server tick never stalls.

## Building

Requires the same toolchain Endstone uses: **Clang 18+ with libc++**, CMake 3.23+,
Ninja, and Conan 2.

```shell
pip install conan
conan install . --build=missing -pr:a <endstone-conan-profile>
cmake --preset conan-relwithdebinfo
cmake --build --preset conan-relwithdebinfo
```

The plugin is emitted as `endstone_spark.so` (Linux) / `endstone_spark.dll`
(Windows). Drop it in your server's `plugins/` directory.

> **Toolchain / ABI note.** A C++ Endstone plugin shares the process's libc++ with
> the Endstone runtime. Build endstone-spark with a libc++ that is ABI-compatible
> with the Endstone build you deploy into (in practice: the same major Clang the
> Endstone release was built with). Mismatched libc++ can corrupt objects passed
> across the plugin/runtime boundary.

## License

GPLv3, matching spark, whose profile format and viewer this builds on. See
[LICENSE](LICENSE).
