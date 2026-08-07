cmake_minimum_required(VERSION 3.23)
project(spark VERSION 0.4.1 LANGUAGES CXX)

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

# --- x86-64 instruction decoder and Windows hook backend ---
# funchook 1.1.3 is not available from Conan Center. Fetch the same upstream
# version used by Endstone. Its small distorm dependency provides the strict
# instruction-boundary decoder used by both native symbol guessers; Windows
# additionally links funchook itself for allocation profiling.
if (WIN32 OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    set(FUNCHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    if (WIN32)
        set(FUNCHOOK_BUILD_STATIC ON CACHE BOOL "" FORCE)
    else ()
        set(FUNCHOOK_BUILD_STATIC OFF CACHE BOOL "" FORCE)
    endif ()
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

# --- native backend: sampler, symbol guesser, allocation hooks (no Endstone) ---
add_library(spark_native STATIC
        src/native/alloc/allocation_thread_filter.cpp
        src/native/sampler/call_tree.cpp
        src/native/sampler/sampler.cpp
        src/native/sampler/thread_selector.cpp
        src/native/sampler/thread_info.cpp
        src/native/symbol/symbol_guess_dwarf.cpp
        src/native/symbol/symbol_guess_evidence.cpp
        src/native/symbol/symbolicate.cpp
        src/native/symbol/symbol_guess.cpp)
if (WIN32)
    target_sources(spark_native PRIVATE
            src/native/symbol/symbol_guess_windows.cpp
            src/native/sampler/capture_windows.cpp
            src/native/alloc/allocation_sampler_windows.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_sources(spark_native PRIVATE
            src/native/sampler/capture_linux.cpp
            src/native/alloc/allocation_sampler_linux.cpp
            src/native/alloc/elf_import_hooks.cpp)
else ()
    target_sources(spark_native PRIVATE
            src/native/sampler/capture_linux.cpp
            src/native/alloc/allocation_sampler_stub.cpp)
endif ()
target_include_directories(spark_native PUBLIC src)
target_link_libraries(spark_native PUBLIC cpptrace::cpptrace concurrentqueue::concurrentqueue
        Threads::Threads)
set_target_properties(spark_native PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(spark_native PRIVATE _GNU_SOURCE)
    target_compile_options(spark_native PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif ()
if (WIN32)
    target_include_directories(spark_native PRIVATE "${funchook_SOURCE_DIR}/distorm/include")
    target_link_libraries(spark_native PUBLIC dbghelp psapi advapi32 funchook-static distorm)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_include_directories(spark_native PRIVATE "${funchook_SOURCE_DIR}/distorm/include")
    target_link_libraries(spark_native PUBLIC distorm)
endif ()

# --- platform-independent core: profiler, stats, proto, net (no Endstone) ---
add_library(spark_core STATIC
        src/core/util/format.cpp
        src/core/command/arguments.cpp
        src/core/profiler/profiler.cpp
        src/core/stats/executable_hash.cpp
        src/core/stats/statistics_service.cpp
        src/core/stats/tick_monitor.cpp
        src/core/stats/system_stats.cpp
        src/proto/sampler_data.cpp
        src/net/gzip.cpp
        src/net/bytebin.cpp
        src/net/profile_file.cpp)
target_include_directories(spark_core PUBLIC src)
target_link_libraries(spark_core PUBLIC spark_native ZLIB::ZLIB CURL::libcurl)
set_target_properties(spark_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spark_core PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif ()

# --- the Endstone plugin (links only its own static core + Endstone headers) ---
endstone_add_plugin(spark
        src/platform/endstone/health_command.cpp
        src/platform/endstone/profiler_controller.cpp
        src/platform/endstone/server_info.cpp
        src/platform/endstone/tick_monitor_controller.cpp
        src/plugin.cpp)
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
    enable_testing()
    add_subdirectory(tests)
endif ()
