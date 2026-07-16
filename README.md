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

| Command                         | Description                                             |
| ------------------------------- | ------------------------------------------------------- |
| `/spark profiler start [flags]` | Start profiling the server thread (background).         |
| `/spark profiler stop`          | Stop profiling and finalize the profile.                |
| `/spark profiler info`          | Show status of the running profiler.                    |
| `/spark profiler cancel`        | Stop profiling without generating a profile.            |
| `/spark tps`                    | Show ticks-per-second and tick duration (MSPT).         |
| `/spark health`                 | Show TPS/MSPT plus process memory, threads, and uptime. |

By default, stopping a profiler uploads the generated profile to spark's bytebin
and prints the viewer link. With `--save-to-file`, the profile is written locally
as a `.sparkprofile` file instead.

Permission: `endstone.command.spark` (operators by default).

### `/spark profiler start` flags

* `--interval <ms>` — sampling interval (default `4`).
* `--timeout <seconds>` — auto-stop and finalize after N seconds.
* `--only-ticks-over <ms>` — only record ticks longer than this.
* `--save-to-file` — write a `.sparkprofile` file instead of uploading
  (open it by dragging it into the spark viewer).
* `--comment <text>` — attach a note to the profile.
* `--include-sleeping` — also sample while the server thread is idle between ticks
  (off by default, since the inter-tick sleep would otherwise dominate a
  wall-clock profile).

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

## Building

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
