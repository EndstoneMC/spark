# Architecture

Endstone Spark is a native statistical profiler for Minecraft Bedrock Dedicated Server (BDS). It samples native execution and allocation call stacks, aggregates them into spark-compatible profiles, and uploads or saves them. The plugin also maintains a 15-minute rolling statistics history for TPS, MSPT, and CPU reporting.

## Source Tree

```
src/
  core/                       # platform-independent services (no Endstone includes)
    command/
      arguments.h/.cpp        # flag parsing for /spark subcommands
    profiler/
      profiler.h/.cpp         # profiler orchestration (Sampler + AllocationSampler)
      profile_mode.h          # execution vs allocation mode enum
    stats/
      statistics_service.h/.cpp  # rolling TPS/MSPT/CPU history (fixed capacity)
      tick_monitor.h/.cpp        # tick monitoring state machine
      system_stats.h/.cpp        # process/host resource stats
      executable_hash.h/.cpp     # BDS executable SHA256
    util/
      format.h/.cpp           # Minecraft-colored formatting helpers

  native/                     # native backend subsystem (no Endstone includes)
    sampler/
      sampler.h/.cpp          # execution sampler (SIGPROF / thread suspend)
      call_tree.h/.cpp        # call-tree aggregation
      capture.h               # platform stack-capture interface
      capture_linux.cpp       # Linux SIGPROF + cpptrace raw trace
      capture_windows.cpp     # Windows SuspendThread + StackWalk64
      thread_selector.h/.cpp  # thread name matching
      thread_info.h/.cpp      # thread name lookup
      types.h                 # FrameKey, Sample, ModuleTable
    symbol/
      symbolicate.h/.cpp      # frame resolution + guess fallback
      symbol_guess.h/.cpp     # symbol guesser core
      symbol_guess_dwarf.h/.cpp       # FDE-based function extent guessing
      symbol_guess_evidence.h/.cpp    # evidence accumulation (RTTI, strings)
      symbol_guess_linux.h            # Linux guesser internals
      symbol_guess_windows.h/.cpp     # Windows guesser
    alloc/
      allocation_sampler.h            # PImpl allocation sampler interface
      allocation_sampler_linux.cpp    # Linux ELF import redirection hooks
      allocation_sampler_windows.cpp  # Windows funchook-based UCRT/heap hooks
      allocation_sampler_stub.cpp     # fallback stub
      allocation_thread_filter.h/.cpp # thread filtering for allocation origin
      bounded_event_queue.h           # lock-free bounded queue
      byte_sampler.h                  # byte threshold sampling
      elf_import_hooks.h/.cpp         # Linux ELF import hook implementation

  platform/
    endstone/                 # thin Endstone adapter
      profiler_controller.h/.cpp     # /spark profiler command handler + export lifecycle
      health_command.h/.cpp          # /spark tps + /spark health
      tick_monitor_controller.h/.cpp # /spark tickmonitor
      server_info.h/.cpp             # Endstone version/plugins/world gathering

  proto/                      # spark protobuf serialization
    proto_writer.h
    sampler_data.h/.cpp
  net/                        # gzip, bytebin upload, local profile persistence
    bytebin.h/.cpp
    gzip.h/.cpp
    profile_file.h/.cpp

  plugin.cpp                  # Endstone plugin lifecycle + command dispatch (thin)
  spark_constants.h           # version string

tests/
  selftest.cpp                # offline integration self-test
  allocation_benchmark.cpp    # allocation diagnostic benchmark
  native/
    symbol/                   # symbol guesser tests
    alloc/                    # allocation fixture modules
  test_profile_evaluator.py   # profile evaluation tool tests
  test_release_changelog.py   # release changelog tooling tests

tools/
  profile_evaluator.py        # profile evaluation CLI
  release_changelog.py        # deterministic changelog generation
```

## CMake Structure

Three layered static libraries enforce dependency direction:

```
spark_native (static)     <- native/sampler, native/symbol, native/alloc
                            links: cpptrace, concurrentqueue, distorm, funchook (Windows)
                            NO Endstone dependency

spark_core (static)       <- core/, proto/, net/
                            links: spark_native, zlib, curl
                            NO Endstone dependency

spark (endstone_add_plugin) <- platform/endstone/, plugin.cpp
                              links: spark_core
                              Endstone API only here
```

Dependency direction (enforced by CMake target structure):

```
platform/endstone -> core -> native -> (external libs)
                    |         |
                  proto/    net/  (part of core)
```

## Key Components

### Execution Sampler (`native/sampler/`)

Captures native thread stacks at a bounded interval. Linux uses `SIGPROF` with cpptrace's safe raw-trace path; Windows suspends the target thread and walks it with `StackWalk64`. Samples are enqueued to a lock-free bounded queue and aggregated on a background thread.

### Symbol Guesser (`native/symbol/`)

Unresolved frames in the BDS main executable may receive conservative runtime guesses from unwind metadata, RTTI, vtables, thunks, and decoded string references. The guesser runs at export time (not on the sampling hot path) and produces deterministic labels that retain the original RVA and identify their evidence source.

### Allocation Profiler (`native/alloc/`)

Samples allocation stacks by requested bytes. Windows uses funchook for supported UCRT and heap entry points; Linux redirects supported ELF allocator imports. Hook callbacks enqueue bounded records for later processing. The hook path is free of allocations, string construction, and unbounded containers.

### Statistics Service (`core/stats/`)

Maintains bounded rolling TPS, MSPT, CPU, and system-resource histories independently of an active profile. `/spark tps`, `/spark health`, profile metadata, and per-second Viewer windows all read from this shared service.

### Profiler (`core/profiler/`)

Orchestrates the execution sampler and allocation sampler. Manages start/stop lifecycle, auto-stop on timeout, and tick-based auto-stop on slow ticks.

### Platform Adapter (`platform/endstone/`)

Thin Endstone adapter: `plugin.cpp` handles lifecycle and command dispatch; `ProfilerController` owns the profiler and export thread; `HealthCommands` formats TPS/MSPT/CPU reports; `TickMonitorController` manages tick monitoring; `server_info` gathers Endstone version, plugins, world, and player data for profile metadata.

## Lifecycle

```
EndstoneSparkPlugin (RAII owner)
  ├── StatisticsService           # started in onEnable, stopped in onDisable
  ├── ProfilerController          # owns Profiler + export thread
  │     ├── Profiler
  │     │     ├── Sampler         # execution sampling
  │     │     └── AllocationSampler # allocation sampling
  │     └── export_thread_        # joined in shutdown()
  ├── HealthCommands              # formats /spark tps and /spark health
  ├── TickMonitorController       # owns TickMonitor state machine
  └── tick_task_                  # Endstone scheduler task (per-tick)
```

## Command Flow

```
Endstone onCommand(sender, args)
  -> "tps"       -> HealthCommands::cmdTps
  -> "health"    -> HealthCommands::cmdHealth
  -> "tickmonitor" -> TickMonitorController::cmdTickMonitor
  -> "profiler"  -> ProfilerController::cmdProfiler
  -> (empty)     -> sendHelp
```

## Tick Flow

```
Endstone scheduler -> onServerTick()
  -> StatisticsService::onTick(mspt)
  -> TickMonitorController::onTick(mspt)
  -> ProfilerController::onTick(mspt) -> auto-stop check
```

## Profiler Export Flow

```
ProfilerController::finishProfiler()  [main thread]
  -> Profiler::stopSampling()         [joins sampler threads]
  -> gatherServerInfo() + gatherWorldInfo()  [Endstone API, main thread]
  -> export_thread_                    [background]
    -> Profiler::exportData()
    -> gzip + upload/save
    -> scheduler.runTask() -> announceResult()  [main thread hop]
```

## Platform Safety

- Sampling stays off the BDS tick hot path except for the minimum bounded capture.
- Linux signal-handler code remains async-signal-safe.
- Windows thread suspension and stack walking always restore target-thread state.
- Allocation hooks remain reentrancy-safe and never block allocator threads.
- Plugin shutdown waits within bounded intervals and does not leave callbacks, hooks, or background work referring to unloaded code.

## Dependencies

Conan supplies cpptrace, concurrentqueue, zlib, expected-lite, and libcurl. CMake fetches Endstone's public plugin API and funchook `v1.1.3`; funchook's bundled distorm decoder is used by the x86-64 symbol guessers on both platforms, while the funchook hook library itself is linked only on Windows.
