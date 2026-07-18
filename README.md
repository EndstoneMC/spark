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
| `/spark profiler start --alloc` | Profile native allocation call stacks on Windows.       |
| `/spark profiler stop`          | Stop profiling and finalize the profile.                |
| `/spark profiler info`          | Show status of the running profiler.                    |
| `/spark profiler cancel`        | Stop profiling without generating a profile.            |
| `/spark tps`                    | Show ticks-per-second and tick duration (MSPT).         |
| `/spark health`                 | Show TPS/MSPT plus process memory, threads, and uptime. |

By default, stopping a profiler uploads the generated profile to spark's bytebin
and prints the viewer link. With `--save-to-file`, the profile is written locally
as a `.sparkprofile` file instead. If an upload fails, Spark automatically
preserves the compressed profile in its data folder and reports the local path.

Permission: `endstone.command.spark` (operators by default).

### Native allocation profiler (Windows)

`/spark profiler start --alloc` starts an allocation profile and uses the same
stop, save, compression, upload, and spark viewer path as an execution profile.
The call tree is weighted in allocated bytes, and the protobuf is marked with
`sampler_mode = ALLOCATION`, so the viewer renders memory values rather than
execution time. The default sampling interval matches upstream spark: 524287
bytes (approximately 512 KiB).

`/spark profiler start --alloc-live-only` runs the retained variant. Its call
tree contains only sampled allocations that are still live when the profile
stops, weighted by estimated retained bytes. This is intended for leak analysis:
the oldest/largest retained stacks are candidates, while repeated profiles are
still required to distinguish a leak from legitimate long-lived state.

The current native backend reflects BDS constraints rather than a JVM:

* funchook intercepts the public UCRT `malloc`, `calloc`, `realloc`, `recalloc`,
  aligned allocation families, and the corresponding internal base exports when
  they are available. Direct `HeapAlloc` and `HeapReAlloc` calls are also sampled;
  alias exports are detected so the same entry address is not hooked twice;
* only allocations made by the BDS server thread are included, matching the
  current profiler target; direct `VirtualAlloc`, custom pool internals, and other
  threads remain outside the reported rate. Static/private CRT allocations that
  ultimately use the Windows process heap are visible at the `HeapAlloc` boundary;
* successful allocation requests are sampled by requested bytes. Each session
  chooses a random byte phase and then samples at the configured fixed interval;
  every crossing contributes one interval of estimated allocation weight to the
  allocation that actually contains that sampling point;
* stack capture uses `RtlCaptureStackBackTrace`; symbolization is deferred until
  the profile stops;
* sampled allocations are followed through `realloc`, `free`, aligned free, and
  Windows heap release paths even when ownership moves to another thread. The
  profile metadata reports freed/live sampled bytes and lifetime diagnostics;
  normal `--alloc` profiles remain weighted by allocation traffic.

The funchook trampoline set is prepared once and retained across profiler
sessions. Entry hooks are installed lazily when the first allocation profile
starts, remain as disabled pass-throughs between sessions, and are removed with
their trampolines during plugin shutdown. Hook patching uses a stabilized thread
snapshot and restores every suspend count with checked retries. Shutdown also
waits until no thread is executing a Spark hook or trampoline before destroying
the backend, so a successful plugin reload leaves no allocator entry point or
in-flight callback referencing the old DLL. If that cleanup cannot be proven,
Spark terminates the process rather than unloading unsafe code or pinning the old
plugin for the process lifetime.

The byte sampler uses a uniformly random phase that is reset for every profile,
then counts fixed-interval crossings in constant time. This gives an unbiased
estimate for each allocation, and a single allocation crossing multiple sampling
points receives the corresponding multiple of the interval. The allocation
aggregator is treated as a fallible
service: an internal failure disables capture, is reported by `profiler info`,
and prevents export of a partial profile while leaving the backend restartable.

A fixed preallocated event pool is used in the hook path; when it is exhausted,
samples are dropped and reported by `/spark profiler info`. Captured/dropped
sample counts, estimated sampled byte weight, observed request bytes, backend
coverage, and the byte interval are embedded in the uploaded spark metadata under
`extra_platform_metadata`.

DbgHelp may only have a DLL export table for Windows runtime modules. Since
`SymFromAddr` returns the nearest preceding symbol, private UCRT startup routines
can otherwise be mislabeled as unrelated exports. The exporter now accepts UCRT
names only when DbgHelp can prove the address belongs to that symbol; ambiguous
frames fall back to `ucrtbase.dll.0x<RVA>`.

### `/spark profiler start` flags

* `--interval <value>` — execution interval in milliseconds (default `4`),
  or allocation interval in bytes with `--alloc` (default `524287`).
* `--alloc` — record sampled native allocation call stacks instead of execution time
  (Windows only in the current backend).
* `--alloc-live-only` — record only sampled allocations retained at stop for leak
  analysis; this implies `--alloc` (Windows only).
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

> Windows allocation profiler: CMake fetches and statically builds upstream funchook `v1.1.3`; it is not a Conan requirement.


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
