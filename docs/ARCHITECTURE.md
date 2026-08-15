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

  platform/
    endstone/                 # Endstone adapters and optional PAPI integration

  proto/                      # spark protobuf serialization
  net/                        # gzip, bytebin upload, WebSocket transport, profile persistence
  plugin.cpp                  # Endstone plugin lifecycle (thin bootstrap)
  spark_constants.h           # version string
```

## CMake Structure

Four layered targets enforce dependency direction:

```
spark_native (static)        <- native/sampler, native/symbol, native/alloc
                               links: cpptrace, concurrentqueue, distorm, funchook (Windows)
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
platform/endstone -> application -> core -> native -> (external libs)
```

## Key Components

### Application Layer (`application/`)

`SparkApplication` owns all platform-independent services and dispatches ticks and commands. `ProfilerService` manages profiler sessions, background profiling, live viewer connections, and background export. Three capability interfaces (`MainThreadDispatcher`, `ProfileMetadataProvider`, `ResultNotifier`) abstract platform dependencies.

### Execution Sampler (`native/sampler/`)

Captures native thread stacks at a bounded interval. Linux uses `SIGPROF` with cpptrace's safe raw-trace path; Windows suspends the target thread and walks it with `StackWalk64`. Samples are enqueued to a lock-free queue and aggregated on a background thread.

### Allocation Profiler (`native/alloc/`)

Samples allocation stacks by requested bytes. Windows uses funchook for supported UCRT and heap entry points; Linux redirects supported ELF allocator imports. Hook callbacks enqueue bounded records for later processing. Live exports deep-copy cumulative aggregator state or rebuild a temporary retained tree from the authoritative live index without stopping hooks. The hook path is free of allocations, string construction, and unbounded containers.

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
- Windows thread suspension and stack walking always restore target-thread state.
- Allocation hooks remain reentrancy-safe and never block allocator threads.
- Plugin shutdown waits within bounded intervals and does not leave callbacks, hooks, or background work referring to unloaded code.

## Dependencies

Conan supplies cpptrace, concurrentqueue, zlib, expected-lite, libcurl, and tomlplusplus. Linux additionally requires OpenSSL for crypto. CMake fetches Endstone's public plugin API, pinned public PAPI headers, and funchook `v1.1.3`; funchook's bundled distorm decoder is used by the x86-64 symbol guessers on both supported platforms, while the funchook hook library itself is linked only on Windows.
