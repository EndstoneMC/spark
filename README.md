# Spark for Endstone

Spark is a native profiler for Minecraft Bedrock Dedicated Server running on
[Endstone](https://github.com/EndstoneMC/endstone). It helps you find what is
using server tick time by sampling execution and allocation call stacks, then
showing the results in the spark web viewer.

Spark uses the profile format, protocol, and viewer from
[lucko/spark](https://github.com/lucko/spark). Credit for those belongs to the
upstream spark project.

## Install

Download the latest release for your platform:

- Windows: [`endstone_spark.dll`](https://github.com/EndstoneMC/spark/releases/latest)
- Linux: [`endstone_spark.so`](https://github.com/EndstoneMC/spark/releases/latest)

Place the file directly in the server's `plugins/` directory, then fully restart
the server to load Spark.

## Quick start

Run these commands in the server console or in game as an operator:

```text
/spark profiler start
/spark profiler info
/spark profiler stop
```

The profiler runs in the background until you stop it. `info` shows its current
status. When `stop` finishes, Spark uploads the profile and prints a link to the
viewer. Open that link to explore the call tree and flame graph.

## Features

- Sample native server execution, including work outside plugin code.
- Profile native allocations with `/spark profiler start --alloc`.
- View rolling server statistics with `/spark tps` and resource reports with
  `/spark health show`.
- Open a live spark viewer while profiling with `/spark profiler open`.

## Documentation

For server owners and operators:

- [Using Spark](docs/using-spark.md)
- [Configuration](docs/configuration.md)

For developers and contributors:

- [Development](docs/development.md)
- [Architecture](docs/architecture.md)
- [Behavior pack metadata](docs/behavior-pack-metadata.md)
- [Python function attribution](docs/python-function-attribution.md)

## License

Spark for Endstone is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE).
