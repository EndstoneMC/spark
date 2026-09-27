# Configuration

Spark reads `config.toml` from its plugin data directory when the server starts.
When the file is missing, Spark creates it with the defaults below. The file is
user-owned: Spark does not rewrite it during normal operation. Missing fields use
their defaults and unknown fields are ignored.

If the TOML is malformed, a field has the wrong type, or a value is invalid, Spark
reports the configuration error and uses defaults for that startup. It leaves the
file byte-for-byte unchanged. Environment overrides are applied in memory and are
never written to the file.

## Options

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `viewerUrl` | string | `"https://spark.lucko.me/"` | HTTP(S) base URL for the spark viewer. |
| `bytebinUrl` | string | `"https://spark-usercontent.lucko.me/"` | HTTP(S) base URL for profile and health report uploads. |
| `bytesocksHost` | string | `"spark-usersockets.lucko.me"` | Live-viewer WebSocket host, optionally with a port. Do not include a scheme, path, query, or fragment. |
| `backgroundProfiler` | boolean | `true` | Enable the automatic background execution profiler. |
| `backgroundProfilerInterval` | integer | `10` | Background execution sampling interval in milliseconds, from `1` through `1000`. |
| `backgroundProfilerThreadGrouper` | string | `"by-pool"` | Group background profile threads by `by-pool`, `by-name`, or `as-one`. |
| `backgroundProfilerThreadDumper` | string | `"default"` | Profile the server thread (`default`) or all process threads (`all`). |
| `allocationRateMetrics` | boolean | `true` | Keep the count-only native allocation-rate counter active outside allocation profiles. It starts after Spark identifies the server thread and resumes after a foreground allocation profile is exported. `false` disables this counter; explicit `--alloc` profiles still work. |
| `serverPropertiesAdditionalKeys` | string | `""` | Comma-separated extra `server.properties` keys to include in profile metadata after administrator review. |
| `disableResponseBroadcast` | boolean | `false` | Restrict result notifications to the player who requested them. |

The background profiler starts automatically when enabled. A valid foreground
profile pauses it; explicitly stopping and exporting the foreground profile
restarts it after export completes. A cancelled or timed-out foreground session
leaves it paused until Spark is reloaded.

### Additional `server.properties` keys

Use `serverPropertiesAdditionalKeys` only for values that have been reviewed as
safe to share in a profile. Spark accepts at most 64 unique keys; each key can be
up to 128 characters and may contain only letters, digits, `-`, `_`, and `.`.
Separate keys with commas. Known sensitive names such as server/world identity,
seeds, and debugger settings remain blocked, as do names containing `password`,
`passcode`, `token`, `secret`, `credential`, or `private-key`. This option has no
environment-variable override.

## Environment overrides

These Java-compatible environment variables override the matching TOML value for
the current process:

| Environment variable | TOML key |
| --- | --- |
| `SPARK_VIEWERURL` | `viewerUrl` |
| `SPARK_BYTEBINURL` | `bytebinUrl` |
| `SPARK_BYTESOCKSHOST` | `bytesocksHost` |
| `SPARK_BACKGROUNDPROFILER` | `backgroundProfiler` |
| `SPARK_BACKGROUNDPROFILERINTERVAL` | `backgroundProfilerInterval` |
| `SPARK_BACKGROUNDPROFILERTHREADGROUPER` | `backgroundProfilerThreadGrouper` |
| `SPARK_BACKGROUNDPROFILERTHREADDUMPER` | `backgroundProfilerThreadDumper` |
| `SPARK_ALLOCATIONRATEMETRICS` | `allocationRateMetrics` |
| `SPARK_DISABLERESPONSEBROADCAST` | `disableResponseBroadcast` |

Boolean variables follow Java's `Boolean.parseBoolean` behavior: only
case-insensitive `true` enables the option; any other present value means `false`.
An unparseable background interval is ignored, leaving the TOML value in effect.
Valid interval values must be between `1` and `1000`. Invalid endpoints, an
out-of-range interval, or an unsupported background thread mode make Spark reject
the configuration for that startup and use defaults. Viewer and bytebin endpoints
must be HTTP(S) base URLs; endpoints cannot contain credentials, query strings, or
fragments. The WebSocket setting must be a host with an optional port.

Python attribution also has a process-level diagnostic switch,
`SPARK_PYTHON_ATTRIBUTION_MODE`. It accepts `auto` (default), `off`, or
`shadow-only`. `off` keeps profiles native-only; `shadow-only` keeps the Python
shadow stack active for diagnostics. Unknown values are ignored with a warning.
See [Python function attribution](python-function-attribution.md) for details.

## Trusted live-viewer clients

Approved public keys are stored separately from `config.toml` in
`trusted-viewers.json` as a JSON array of base64-encoded X.509 public keys. The
`/spark profiler trust-viewer --id <client id>` and
`/spark health trust-viewer --id <client id>` commands add a pending viewer client
to this file. Trust applies to both the profiler live viewer and health dashboard.
