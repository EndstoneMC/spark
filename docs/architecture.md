# Architecture

Endstone Spark samples native execution and allocation activity in Bedrock Dedicated Server (BDS), then exports profiles in spark's format. Its design keeps host integration at the edge and keeps sampling work bounded because the plugin runs inside a long-lived server process.

## Main boundaries

| Area | Responsibility |
| --- | --- |
| `src/plugin.cpp` | Registers the plugin and connects it to Endstone during load and shutdown. |
| `src/platform/endstone/` | Adapts Endstone commands, ticks, metadata, and notifications to Spark's interfaces. The optional PAPI integration lives here too. |
| `src/application/` | Coordinates commands and services, including profiler sessions, exports, health reports, and tick monitoring. |
| `src/core/` | Implements platform-independent profiling, statistics, configuration, recovery, metadata parsing, and viewer state. |
| `src/native/` | Captures native stacks, records allocation samples, and resolves symbols. It does not call Endstone. |
| `src/proto/`, `src/net/` | Serialize spark messages and handle compression, uploads, WebSockets, and local profile files. |

CMake follows the same dependency direction: the Endstone plugin and adapters use the application layer, which uses core services, which use the native backend. Lower layers do not depend on Endstone. `SparkApplication` is the main application container; its focused interfaces let the platform adapters provide server-thread dispatch, profile metadata, and result notifications.

## Profile flow

The sampler captures selected BDS threads at a configured interval. It places bounded records on queues for aggregation outside the capture path. The profiler and exporter build call trees, add available statistics and allowlisted server metadata, and serialize spark protobuf data. The network layer uploads compressed data or writes the raw profile locally. Symbol guesses run during export and only annotate unresolved frames proven to belong to the BDS executable.

Statistics are collected independently of a profile session. The application uses those rolling histories for commands and exports. Recovery journaling records supported session data so startup can recover a profile after an unclean shutdown.

## Safety rules

- Keep tick-thread work small. Sampling, allocation callbacks, and watchdog code must not perform export, symbolization, compression, or network I/O.
- Keep capture and hook paths bounded. Linux signal-handler code must remain async-signal-safe; allocator hooks must be reentrancy-safe and must not block allocator threads.
- Restore suspended Windows threads on every recoverable exit. If a live target cannot be resumed after the bounded retries, terminate the process before it can remain suspended.
- Treat plugin shutdown as safe only after work has quiesced. Waits are bounded; if quiescence cannot be proven, abort before unloading the plugin.
- Preserve spark protobuf and viewer compatibility. Export only approved metadata; do not include credentials, server-owner paths, arbitrary configuration, or executable contents.

For build prerequisites, commands, tests, and contribution pointers, see [Development](development.md). For the behavior-pack metadata fallback and its migration constraints, see [Behavior pack metadata](behavior-pack-metadata.md).
