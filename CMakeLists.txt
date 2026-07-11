cmake_minimum_required(VERSION 3.23)
project(spark VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENDSTONE_SPARK_BUILD_SELFTEST "Build the offline sampler self-test tool" ON)

# --- dependencies (from conan) ---
find_package(cpptrace REQUIRED)
find_package(concurrentqueue REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# --- Endstone: header-only API + endstone_add_plugin(), fetched standalone ---
# Endstone's root CMake returns right after its include/ target when consumed as a
# subproject, so this only pulls the headers, not the server. For local iteration,
# pass -DFETCHCONTENT_SOURCE_DIR_ENDSTONE=<path> to reuse an existing checkout.
include(FetchContent)
FetchContent_Declare(endstone
        GIT_REPOSITORY https://github.com/EndstoneMC/endstone.git
        GIT_TAG v0.11)
FetchContent_MakeAvailable(endstone)

# --- platform-independent profiler core (no Endstone dependency) ---
add_library(spark_core STATIC
        src/proto/sampler_data.cpp
        src/net/gzip.cpp
        src/net/bytebin.cpp
        src/sampler/call_tree.cpp
        src/sampler/symbolicate.cpp
        src/sampler/sampler.cpp
        src/sampler/profiler.cpp
        src/stats/system_stats.cpp)
if (WIN32)
    target_sources(spark_core PRIVATE src/sampler/capture_windows.cpp)
else ()
    target_sources(spark_core PRIVATE src/sampler/capture_linux.cpp)
endif ()
target_include_directories(spark_core PUBLIC src)
target_link_libraries(spark_core PUBLIC cpptrace::cpptrace concurrentqueue::concurrentqueue ZLIB::ZLIB Threads::Threads)
set_target_properties(spark_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Keep our vendored cpptrace/zlib symbols local to the .so that embeds spark_core so
# they never interpose with Endstone core's own (differently-built) cpptrace copy.
target_compile_options(spark_core PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
if (WIN32)
    target_link_libraries(spark_core PUBLIC dbghelp)
endif ()

# --- the Endstone plugin (links only its own static core + Endstone headers) ---
endstone_add_plugin(spark
        src/plugin.cpp
        src/command/arguments.cpp)
target_include_directories(spark PRIVATE src)
target_link_libraries(spark PRIVATE spark_core)
target_compile_options(spark PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
# Localize every symbol from the static archives (zlib, cpptrace, libunwind, libc++)
# so they can't interpose with Endstone core's own copies in the BDS process. Only
# init_endstone_plugin (default visibility via the macro) stays exported.
if (UNIX)
    target_link_options(spark PRIVATE "-Wl,--exclude-libs,ALL")
endif ()

# --- offline self-test (no BDS needed) ---
if (ENDSTONE_SPARK_BUILD_SELFTEST)
    add_executable(spark_selftest tools/selftest.cpp)
    target_link_libraries(spark_selftest PRIVATE spark_core)
endif ()
