cmake_minimum_required(VERSION 3.23)
project(spark VERSION 0.3.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENDSTONE_SPARK_BUILD_SELFTEST "Build the offline sampler self-test tool" ON)

# --- dependencies (from conan) ---
find_package(cpptrace REQUIRED)
find_package(concurrentqueue REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)

# --- Windows hook backend ---
# funchook 1.1.3 is not available from Conan Center. Fetch the same upstream
# version used by Endstone and build only its static library. Its distorm
# dependency is a git submodule, so recursive submodule checkout is required.
if (WIN32)
    set(FUNCHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(FUNCHOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_INSTALL OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_DISASM distorm CACHE STRING "" FORCE)
    FetchContent_Declare(funchook
            GIT_REPOSITORY https://github.com/kubo/funchook.git
            GIT_TAG v1.1.3
            GIT_SHALLOW TRUE
            GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(funchook)
endif ()

# --- Endstone: header-only API + endstone_add_plugin(), fetched standalone ---
# Endstone's root CMake returns right after its include/ target when consumed as a
# subproject, so this only pulls the headers, not the server. For local iteration,
# pass -DFETCHCONTENT_SOURCE_DIR_ENDSTONE=<path> to reuse an existing checkout.
FetchContent_Declare(endstone
        GIT_REPOSITORY https://github.com/EndstoneMC/endstone.git
        GIT_TAG v0.11)
FetchContent_MakeAvailable(endstone)

# --- platform-independent profiler core (no Endstone dependency) ---
add_library(spark_core STATIC
        src/proto/sampler_data.cpp
        src/net/gzip.cpp
        src/net/bytebin.cpp
        src/net/profile_file.cpp
        src/alloc/allocation_thread_filter.cpp
        src/sampler/call_tree.cpp
        src/sampler/symbolicate.cpp
        src/sampler/sampler.cpp
        src/sampler/profiler.cpp
        src/sampler/thread_selector.cpp
        src/sampler/thread_info.cpp
        src/stats/executable_hash.cpp
        src/stats/tick_monitor.cpp
        src/stats/system_stats.cpp)
if (WIN32)
    target_sources(spark_core PRIVATE
            src/sampler/capture_windows.cpp
            src/alloc/allocation_sampler_windows.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_sources(spark_core PRIVATE
            src/sampler/capture_linux.cpp
            src/alloc/allocation_sampler_linux.cpp
            src/alloc/elf_import_hooks.cpp)
else ()
    target_sources(spark_core PRIVATE
            src/sampler/capture_linux.cpp
            src/alloc/allocation_sampler_stub.cpp)
endif ()
target_include_directories(spark_core PUBLIC src)
target_link_libraries(spark_core PUBLIC cpptrace::cpptrace concurrentqueue::concurrentqueue ZLIB::ZLIB CURL::libcurl
        Threads::Threads)
set_target_properties(spark_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Keep our vendored cpptrace/zlib symbols local to the .so that embeds spark_core so
# they never interpose with Endstone core's own (differently-built) cpptrace copy.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(spark_core PRIVATE _GNU_SOURCE)
    target_compile_options(spark_core PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif ()
if (WIN32)
    target_link_libraries(spark_core PUBLIC dbghelp psapi advapi32 funchook-static)
endif ()

# --- the Endstone plugin (links only its own static core + Endstone headers) ---
endstone_add_plugin(spark
        src/plugin.cpp
        src/command/arguments.cpp)
target_include_directories(spark PRIVATE src)
target_link_libraries(spark PRIVATE spark_core)
# Localize every symbol from the static archives (zlib, cpptrace, libunwind, libc++)
# so they can't interpose with Endstone core's own copies in the BDS process. Only
# init_endstone_plugin (default visibility via the macro) stays exported.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spark PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    target_link_options(spark PRIVATE "-Wl,--exclude-libs,ALL")
endif ()

# --- offline self-test (no BDS needed) ---
if (ENDSTONE_SPARK_BUILD_SELFTEST)
    add_executable(spark_selftest tools/selftest.cpp src/command/arguments.cpp)
    target_link_libraries(spark_selftest PRIVATE spark_core)
    add_executable(spark_allocation_benchmark tools/allocation_benchmark.cpp)
    target_compile_definitions(
            spark_allocation_benchmark PRIVATE
            SPARK_ALLOCATION_BENCHMARK_CURRENT)
    target_link_libraries(spark_allocation_benchmark PRIVATE spark_core)
    if (WIN32)
        add_library(spark_windows_allocation_fixture MODULE
                tools/windows_allocation_fixture.cpp)
        add_dependencies(spark_selftest spark_windows_allocation_fixture)
        target_compile_definitions(
                spark_selftest PRIVATE
                SPARK_WINDOWS_ALLOCATION_FIXTURE_PATH="$<TARGET_FILE:spark_windows_allocation_fixture>")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        add_library(spark_elf_hook_fixture MODULE tools/elf_hook_fixture.cpp)
        add_dependencies(spark_selftest spark_elf_hook_fixture)
        target_compile_definitions(
                spark_selftest PRIVATE
                _GNU_SOURCE
                SPARK_ELF_HOOK_FIXTURE_PATH="$<TARGET_FILE:spark_elf_hook_fixture>")
    endif ()
endif ()
