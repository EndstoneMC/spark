# Changelog

All notable changes to endstone-spark are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-07-11

### Added

- Native statistical sampling profiler for the Bedrock Dedicated Server thread,
  covering all BDS internal calls (not just plugins), with results uploaded to the
  spark web viewer.
- `/spark profiler start|stop|info|cancel` with flags `--interval`, `--timeout`,
  `--only-ticks-over`, `--save-to-file`, `--comment`, and `--include-sleeping`.
- `/spark tps` and `/spark health`.
- Linux backend: `SIGPROF` sampler with cpptrace async-signal-safe stack capture and
  `dladdr` symbolization (module + RVA fallback for stripped frames).
- Windows backend: `SuspendThread` + `StackWalk64` capture with PDB symbolization.

[Unreleased]: https://github.com/EndstoneMC/endstone-spark/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/EndstoneMC/endstone-spark/releases/tag/v0.1.0
