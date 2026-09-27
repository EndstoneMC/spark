# Development

This guide covers building and validating Spark. The [README](../README.md) describes installation and user-facing commands; [Architecture](architecture.md) describes the code boundaries.

## Prerequisites

All builds require CMake 3.29 or newer, Ninja, Conan 2, and a Python 3 interpreter. Python tooling and native Python runtime tests require Python 3.12 or newer. The repository's `.conan2/profiles/default` selects the supported compiler and standard library; keep that profile intact and do not run `conan profile detect` over it.

| Platform | Toolchain requirements |
| --- | --- |
| Linux x86-64 | Clang 18 or newer, libc++, and the static `libc++.a` and `libc++abi.a` archives used by symbol-fixture tests. |
| Windows x64 | LLVM clang-cl 18 or newer, Visual Studio Build Tools, and the Windows SDK. clang-cl must target the MSVC ABI. |

CI currently uses Clang 20 and clang-cl 20. Native Python attribution tests need the matching shared CPython 3.12+ runtime. CMake also fetches the pinned distorm decoder for supported x86-64 builds. The default plugin build fetches Endstone's public plugin API and public PAPI headers.

## Configure and build

Install Conan dependencies, configure with the generated toolchain file, and build:

```shell
python -m pip install "conan>=2,<3"
conan install . --build=missing
cmake -S . -B build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=build/RelWithDebInfo/generators/conan_toolchain.cmake" "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
cmake --build build
```

By default this builds the plugin and offline self-test tools. To build the offline tools without fetching Endstone or PAPI, add `-DENDSTONE_SPARK_BUILD_PLUGIN=OFF` to the CMake configure command. To build only the plugin, add `-DENDSTONE_SPARK_BUILD_SELFTEST=OFF`.

To run the Python attribution integration test, make the matching shared runtime available during configuration. Set `SPARK_TEST_LIBPYTHON` to its path or pass `-DSPARK_TEST_LIBPYTHON:FILEPATH=<path>` to CMake. Windows uses `python312.dll` (or the matching newer runtime); Linux uses the shared library reported by Python's `LIBDIR` and `LDLIBRARY` settings. The remaining native tests can run without this optional runtime.

The C++ Endstone plugin must match the ABI expected by the Endstone server. Use the matching compiler ABI and C++ runtime: libc++ on Linux and the MSVC runtime on Windows. C++ types cross the plugin boundary, so incompatible runtimes can corrupt objects.

## Tests

Run the configured CTest suite after building:

```shell
ctest --test-dir build --output-on-failure
```

The suite includes offline service and profiler tests, synthetic native fixtures, and platform-specific sampler and symbol-guesser tests. On Linux, `ENDSTONE_SPARK_GATEWAY_SAMPLER_TESTS` is enabled by default and adds isolated allocation-gateway sampler coverage.

Useful focused checks are:

```shell
./build/spark_selftest --seconds=1
./build/spark_selftest --statistics-only
./build/spark_selftest --allocation-only
python tests/test_release_changelog.py
```

On Windows, run the corresponding `build/spark_selftest.exe` commands. The allocation benchmark is for performance measurements, not correctness checks.

## Contribution pointers

- Keep Endstone calls in `src/platform/endstone/`; put application behavior in `src/application/` and reusable services in `src/core/`.
- Preserve the bounded and nonblocking rules for sampling and allocator hooks. See [Architecture](architecture.md) before changing native or lifecycle code.
- Keep profile output compatible with spark and limit metadata to approved fields. Do not add server paths, secrets, arbitrary configuration, or executable contents.
- Add a `CHANGELOG.md` entry for user-visible behavior. Keep implementation-only refactoring out of the changelog.
- Follow the repository's clang-format style for C++. Python files use a 120-character line limit. Use conventional commit subjects such as `fix: ...`, `feat: ...`, and `docs: ...`.
- Keep generated profiles, build output, BDS binaries, PDBs, crash dumps, logs, and local deployment configuration out of commits.
