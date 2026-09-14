# Python function attribution

Spark can enrich its existing statistical CPU samples with the currently executing Python call chain when Endstone embeds CPython 3.12 or newer.

## Runtime model

The implementation deliberately separates Python execution tracking from CPU accounting:

1. PEP 669 `sys.monitoring` lifecycle callbacks maintain a per-native-thread shadow stack of monotonic `PythonCodeId` values.
2. Code-object metadata is resolved on the cold registration path. Sampling never dereferences Python frames or objects.
3. The native statistical sampler captures its normal native stack first, then attempts a bounded seqlock snapshot of the matching Python shadow stack.
4. A concurrent/inconsistent Python snapshot is dropped without dropping the native CPU sample.
5. Export converts Python code IDs to explicit `[Python] <module>` pseudo frames with qualname, source filename and first line while preserving the existing Spark protobuf/viewer schema.

The sampler does not acquire the GIL, call the Python C API, take a normal mutex, perform filesystem work, or symbolize Python objects.

The shadow stack is bounded to 256 visible frames per Python thread. Deeper execution remains safe: overflow depth is counted and diagnostics expose the maximum observed depth and truncation events. A process-wide fixed table supports up to 256 distinct native thread IDs per profiling session. A slot is retained for the rest of that session after its thread exits or its ID stops producing events; slots are reclaimed only by the next session reset.

## Lifecycle events

Production monitoring enables only the PEP 669 events needed to describe Python execution-frame lifetime:

- enter/re-enter: `PY_START`, `PY_RESUME`, `PY_THROW`
- leave/suspend: `PY_RETURN`, `PY_YIELD`, `PY_UNWIND`

`LINE`, `INSTRUCTION` and the legacy `PyEval_SetProfile` API are not used.

At profiler start, Python stacks that are already executing are bootstrapped once from public runtime APIs so late attach does not begin from an empty stack. Python threads created after monitoring starts build their shadow stacks naturally from subsequent PEP 669 lifecycle events; no private frame-layout access or sampler-side Python call is required.

Monitoring is enabled only for an active CPU profiler session. Export freezes the attribution state before monitoring is disabled, so code metadata remains valid through serialization.

## Python version policy

- Python 3.12, 3.13 and 3.14: PEP 669 function attribution is supported.
- Python 3.11: Spark keeps the existing native profiler and reports Python function attribution as unavailable with a diagnostic requiring Python 3.12 or newer.

Spark does not inspect `_PyInterpreterFrame`, use private `PyThreadState` layout, replace the eval frame (PEP 523), patch CPython, or add a second 3.11 profiling state machine.

## Attribution metadata

Each registered code object records:

- source filename
- module
- function name and qualified name
- `co_firstlineno`
- category: Endstone plugin, Endstone framework/binding, stdlib, external dependency, or unknown
- Endstone plugin source/entry-point name when applicable

The full Python caller chain is retained when execution enters stdlib or a dependency, so a plugin remains visible as the originating caller.

## Recovery behavior

Synthetic Python frames use profiling-session-local `PythonCodeId` values. They are therefore retained in the live/completed profile tree and normal viewer export, but deliberately removed from the crash-recovery journal before persistence. A report reconstructed after a process crash remains a valid native Spark profile rather than exporting Python IDs whose metadata registry no longer exists after restart.

## Diagnostics

Export metadata includes backend/version/support state, event counts, registered threads, maximum and overflow depth, snapshot attempts/failures, attributed/native-only samples, native/Python boundary misses, thread mismatches, unknown IDs, code-category counts, cache hits/misses and monitoring callback failures.

If public CPython APIs cannot be resolved, or a code object cannot be registered,
Spark keeps native sampling available and exports a native-only/support-state
diagnostic. After a registration failure, the session rejects new Python symbols
while retaining already registered metadata. The counters are intended both for
production diagnosis and for the real-BDS validation and performance harnesses.
