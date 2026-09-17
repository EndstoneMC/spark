# Architecture

Endstone Spark is a native statistical profiler for Minecraft Bedrock Dedicated Server (BDS). It samples native execution and allocation call stacks, aggregates them into spark-compatible profiles, and uploads or saves them. The plugin also maintains a 15-minute rolling history for TPS, MSPT, CPU, player counts, and world gauges.

## Source Tree

```
src/
  application/                # platform-independent business orchestration
    activity/                 #   /spark activity command
    command/                  #   command registry, sender interface
    health/                   #   /spark health command
    placeholder/              #   Spark placeholder formatting and dispatch
    profiler/                 #   profiler service, profile exporter
    tick_monitor/             #   /spark tickmonitor command
    spark_application.h/.cpp  #   central application container
    platform_capabilities.h   #   MainThreadDispatcher, ProfileMetadataProvider, ResultNotifier

  core/                       # platform-independent services (no Endstone includes)
    activity/                 #   bounded activity log
    command/                  #   flag/argument parsing
    config/                   #   TOML config, trusted-viewer state
    metadata/                 #   server.properties allowlist parser
    profiler/                 #   profiler orchestration, thread grouper
    recovery/                 #   crash-safe journal: writer, reader, player, watchdog
    stats/                    #   rolling statistics, tick monitor, system/ping/network stats
    util/                     #   base64, formatting, world region grouping
    ws/                       #   crypto (RSA2048), WebSocket protocol, live viewer socket

  native/                     # native backend (no Endstone includes)
    sampler/                  #   execution sampler, call tree, capture, thread selection
    symbol/                   #   symbolication, symbol guesser (DWARF + PE64)
    alloc/                    #   allocation hooks, bounded queue, thread filter
    python/                   #   bounded Python shadow-stack attribution support

  platform/
    endstone/                 # Endstone adapters and optional PAPI integration

  proto/                      # spark protobuf serialization
  net/                        # gzip, bytebin upload, WebSocket transport, profile persistence
  plugin.cpp                  # Endstone plugin lifecycle (thin bootstrap)
  spark_constants.h           # version string
```

## CMake Structure

Layered targets enforce dependency direction:

```
spark_profiling_time (static) <- monotonic clock and profiling-window alignment
                                NO Endstone dependency

spark_native (static)        <- native/sampler, native/symbol, native/alloc
                               links: spark_profiling_time, cpptrace, concurrentqueue,
                                      distorm
                               NO Endstone dependency

spark_core (static)          <- core/, proto/, net/
                               links: spark_native, zlib, curl, tomlplusplus, OpenSSL (Linux)
                               NO Endstone dependency

spark_application (static)   <- application/
                               links: spark_core
                               NO Endstone dependency

spark_papi_integration       <- platform/endstone/papi_integration
                               links: spark_application, public PAPI headers

spark (endstone_add_plugin)  <- platform/endstone/, plugin.cpp
                               links: spark_application, spark_papi_integration
                               Endstone API only here
```

Dependency direction (enforced by CMake target structure):

```
platform/endstone -> application -> core -> native -> profiling_time
```

## Key Components

### Application Layer (`application/`)

`SparkApplication` owns all platform-independent services and dispatches ticks and commands. `ProfilerService` manages profiler sessions, background profiling, live viewer connections, and background export. Three capability interfaces (`MainThreadDispatcher`, `ProfileMetadataProvider`, `ResultNotifier`) abstract platform dependencies.

### Execution Sampler (`native/sampler/`)

Captures native thread stacks at a bounded interval. Linux uses `SIGPROF` with cpptrace's safe raw-trace path; Windows suspends the target thread and walks it with `StackWalk64`, retrying `ResumeThread` up to 32 times after each capture. If all retries fail while the target remains alive, the process is terminated before BDS can remain suspended. Samples are enqueued to a lock-free queue and aggregated on a background thread.

### Allocation Profiler (`native/alloc/`)

Samples allocation stacks by requested bytes on Linux x86-64 by redirecting supported ELF allocator imports and on Windows x64 through Spark-owned `WindowsAllocationIatHooks` and process-lifetime Permanent-IAT gateways. The Windows backend patches supported UCRT and heap import slots to permanent gateways whose handler admission can be closed and drained before plugin unload; the gateways then fall through to the original allocator without retaining pointers into unloaded Spark code. Refresh rescans loaded modules so newly loaded importers receive the same ownership-checked IAT treatment. Hook callbacks enqueue bounded records for later processing. Live exports deep-copy cumulative aggregator state or rebuild a temporary retained tree from the authoritative live index without stopping hooks. The hook path is free of allocations, string construction, and unbounded containers.

### Symbol Guesser (`native/symbol/`)

Unresolved frames in the BDS main executable may receive conservative runtime guesses from unwind metadata, RTTI, vtables, thunks, and decoded string references. The guesser runs at export time (not on the sampling hot path) and produces deterministic labels that retain the original RVA and identify their evidence source.

### Statistics Service (`core/stats/`)

Maintains bounded rolling TPS, MSPT, CPU, player-count, and world-gauge histories independently of an active profile. `/spark tps`, `/spark health`, profile metadata, and per-second Viewer windows all read from this shared service.

### Crash Recovery (`core/recovery/`)

`RecoveryWriter` journals module, thread, sample, and tick records to segmented files via a bounded lock-free queue. `StallWatchdog` monitors the main-thread heartbeat on an independent thread and journals stall begin/end events. On startup, `RecoveryPlayer` replays an unclean supported session and exports a recovered profile.

### Live Viewer (`core/ws/`)

`ViewerSocket` manages a WebSocket connection to the spark live viewer, uploading initial sampler data and pushing payload IDs on window rotation. RSA2048-SHA256 signing (`Crypto`) authenticates viewer clients; `TrustedViewersState` persists approved public keys separately from the user-owned config file.

### Platform Adapter (`platform/endstone/`)

Thin Endstone implementations of the capability interfaces and `CommandSender`. `plugin.cpp` creates adapters and delegates to `SparkApplication`.

The optional PAPI adapter constructs a provider-owned `spark` expansion from
PAPI's public headers. It reads targeted rolling values on the server thread and
unregisters before Spark application teardown. A soft dependency gives PAPI first
enable order when installed; no PAPI binary is linked into Spark.

## Platform Safety

- Sampling stays off the BDS tick hot path except for the minimum bounded capture.
- Linux signal-handler code remains async-signal-safe.
- Windows thread suspension and stack walking restore target-thread state on normal and recoverable failure paths; a bounded retry failure is fatal and terminates the process before unload.
- Allocation hooks remain reentrancy-safe and never block allocator threads.
- Plugin shutdown uses bounded waits for health, export, viewer, and native backend work. If quiescence is not proven before a deadline, shutdown reports failure and the bootstrap aborts before unloading the plugin; this is a fail-closed policy, not a guarantee that timed-out work is safe to unload.

## Dependencies

Conan supplies cpptrace, concurrentqueue, zlib, expected-lite, libcurl, tomlplusplus, and nlohmann_json. Linux additionally requires OpenSSL for crypto. When `ENDSTONE_SPARK_BUILD_PLUGIN=ON`, CMake fetches Endstone's public plugin API and pinned public PAPI headers; plugin-off builds do not fetch either host SDK. CMake also directly fetches the pinned distorm revision used for strict x86-64 instruction-boundary decoding. Windows allocation hooking is implemented entirely by Spark-owned `WindowsAllocationIatHooks` and Permanent-IAT gateways.
