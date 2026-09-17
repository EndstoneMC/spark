cmake_minimum_required(VERSION 3.29)
project(spark VERSION 0.5.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENDSTONE_SPARK_BUILD_SELFTEST "Build the offline sampler self-test tool" ON)
option(ENDSTONE_SPARK_BUILD_PLUGIN "Build the Endstone plugin and host adapters" ON)
option(ENDSTONE_SPARK_GATEWAY_TEST_ONLY "Build only the standalone Linux allocation gateway tests" OFF)
option(ENDSTONE_SPARK_GATEWAY_SAMPLER_TESTS "Build isolated Linux allocation sampler tests" ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(spark_gateway_sources
            src/native/alloc/linux_allocation_gateway.cpp
            src/native/alloc/linux_allocation_gateway_abi.h
            src/native/alloc/linux_permanent_gateway_registry.cpp
            src/native/alloc/linux_permanent_gateway_registry.h
            src/native/alloc/linux_permanent_gateway_unwind.cpp
            src/native/alloc/linux_permanent_gateway_unwind.h
            src/native/alloc/linux_gateway_identity.h
            src/native/alloc/linux_loader_handle.h
            src/native/alloc/linux_elf_admission.h
            src/native/alloc/linux_allocation_gateway_client.cpp
            src/native/alloc/linux_allocation_gateway_client.h
            CMakeLists.txt)
    set(spark_gateway_identity "${CMAKE_CXX_COMPILER_ID}:${CMAKE_CXX_COMPILER_VERSION}:${CMAKE_CXX_FLAGS}:${CMAKE_BUILD_TYPE}:256:1792:8:O2:hidden:no-tail-call:unwind")
    string(APPEND spark_gateway_identity ":${CMAKE_CXX_COMPILER_TARGET}:${CMAKE_SYSROOT}:${CMAKE_SHARED_LINKER_FLAGS}")
    foreach (configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        string(APPEND spark_gateway_identity ":${CMAKE_CXX_FLAGS_${configuration}}:${CMAKE_SHARED_LINKER_FLAGS_${configuration}}")
    endforeach ()
    foreach (source IN LISTS spark_gateway_sources)
        file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" source_hash)
        string(APPEND spark_gateway_identity ":${source_hash}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    endforeach ()
    string(SHA256 spark_gateway_hash "${spark_gateway_identity}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/spark_gateway_compatibility.h"
            CONTENT "#define SPARK_GATEWAY_COMPATIBILITY \"${spark_gateway_hash}\"\n#define SPARK_GATEWAY_EXPECTED_CAPACITY 256\n")
    find_package(cpptrace REQUIRED)
    find_package(libunwind REQUIRED CONFIG)
    find_path(SPARK_LIBUNWIND_INCLUDE_DIR libunwind.h
            HINTS "${libunwind_PACKAGE_FOLDER_RELWITHDEBINFO}/include"
                  "${libunwind_PACKAGE_FOLDER_RELEASE}/include"
                  "${libunwind_PACKAGE_FOLDER_DEBUG}/include" REQUIRED NO_DEFAULT_PATH)
    add_library(spark_linux_permanent_gateway STATIC
            src/native/alloc/linux_allocation_gateway.cpp
            src/native/alloc/linux_allocation_gateway_client.cpp
            src/native/alloc/linux_permanent_gateway_registry.cpp
            src/native/alloc/linux_permanent_gateway_unwind.cpp)
    target_include_directories(spark_linux_permanent_gateway PUBLIC src
            PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" "${SPARK_LIBUNWIND_INCLUDE_DIR}")
    target_compile_definitions(spark_linux_permanent_gateway PRIVATE _GNU_SOURCE)
    target_compile_options(spark_linux_permanent_gateway PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    target_link_libraries(spark_linux_permanent_gateway PUBLIC libunwind::unwind ${CMAKE_DL_LIBS} pthread)
    set_target_properties(spark_linux_permanent_gateway PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif ()

if (ENDSTONE_SPARK_GATEWAY_TEST_ONLY)
    if (NOT TARGET spark_linux_permanent_gateway)
        message(FATAL_ERROR "The standalone allocation gateway tests require Linux x86-64")
    endif ()
    enable_testing()
    add_subdirectory(tests/native/alloc/linux_gateway)
    return()
endif ()

# --- dependencies (from conan) ---
find_package(cpptrace REQUIRED)
find_package(concurrentqueue REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(nlohmann_json REQUIRED)
if (NOT WIN32)
    find_package(OpenSSL REQUIRED)
endif ()
find_package(Threads REQUIRED)

include(FetchContent)

# --- x86-64 instruction decoder ---
# Spark uses distorm directly for strict instruction-boundary decoding in both
# supported x86-64 native symbol guessers.
if (WIN32 OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    enable_language(C)

    if (NOT TARGET distorm)
        FetchContent_Declare(distorm_source
                GIT_REPOSITORY https://github.com/gdabah/distorm.git
                GIT_TAG 7a02caa1a936f0a653fc75f1aaea9bd3fa654603)
        FetchContent_MakeAvailable(distorm_source)

        set(distorm_src_dir "${distorm_source_SOURCE_DIR}/src")
        add_library(distorm STATIC
                "${distorm_src_dir}/decoder.c"
                "${distorm_src_dir}/distorm.c"
                "${distorm_src_dir}/instructions.c"
                "${distorm_src_dir}/insts.c"
                "${distorm_src_dir}/mnemonics.c"
                "${distorm_src_dir}/operands.c"
                "${distorm_src_dir}/prefix.c"
                "${distorm_src_dir}/textdefs.c")
        set_target_properties(distorm PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_include_directories(distorm PUBLIC "${distorm_source_SOURCE_DIR}/include")

        if (MSVC)
            set_source_files_properties("${distorm_src_dir}/prefix.c" PROPERTIES COMPILE_OPTIONS "/wd4819")
        elseif (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(distorm PRIVATE -fvisibility=hidden)
        endif ()
    endif ()
endif ()

if (ENDSTONE_SPARK_BUILD_PLUGIN)
    # --- Endstone: header-only API + endstone_add_plugin(), fetched standalone ---
    # Endstone's root CMake returns right after its include/ target when consumed as a
    # subproject, so this only pulls the headers, not the server. For local iteration,
    # pass -DFETCHCONTENT_SOURCE_DIR_ENDSTONE=<path> to reuse an existing checkout.
    set(ENDSTONE_SPARK_ENDSTONE_GIT_REPOSITORY "https://github.com/EndstoneMC/endstone.git" CACHE STRING
            "Repository providing the Endstone public plugin API")
    set(ENDSTONE_SPARK_ENDSTONE_GIT_TAG "37b395378d91d6d20f1c52bf9d79dbd20e152458" CACHE STRING
            "Git ref providing the Endstone v0.11.11 public plugin API")
    FetchContent_Declare(endstone
            GIT_REPOSITORY ${ENDSTONE_SPARK_ENDSTONE_GIT_REPOSITORY}
            GIT_TAG ${ENDSTONE_SPARK_ENDSTONE_GIT_TAG})
    FetchContent_MakeAvailable(endstone)

    # PAPI is optional at runtime; only its public headers are fetched.
    set(ENDSTONE_SPARK_PAPI_GIT_REPOSITORY "https://github.com/EndstoneMC/papi.git" CACHE STRING
            "Repository providing the public Endstone PAPI headers")
    set(ENDSTONE_SPARK_PAPI_GIT_TAG "3bc3dbf99010e7af09b967d913b5b6a7e182e989" CACHE STRING
            "Git ref providing the public Endstone PAPI headers")
    FetchContent_Declare(endstone_papi_headers
            GIT_REPOSITORY ${ENDSTONE_SPARK_PAPI_GIT_REPOSITORY}
            GIT_TAG ${ENDSTONE_SPARK_PAPI_GIT_TAG})
    FetchContent_MakeAvailable(endstone_papi_headers)
    add_library(spark_papi_headers INTERFACE)
    target_include_directories(spark_papi_headers INTERFACE "${endstone_papi_headers_SOURCE_DIR}/include")
    target_link_libraries(spark_papi_headers INTERFACE endstone::endstone)

    if (WIN32)
        add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
        enable_language(RC)
        configure_file(src/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc" @ONLY)
    endif ()
endif ()

# --- shared profiling time primitives (no native or core dependencies) ---
add_library(spark_profiling_time STATIC
        src/core/util/monotonic_time.cpp
        src/core/profiler/profiling_window.cpp)
target_include_directories(spark_profiling_time PUBLIC src)
set_target_properties(spark_profiling_time PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- opt-in CI lifecycle diagnostics (no native, core, or Endstone dependencies) ---
add_library(spark_diagnostics STATIC
        src/native/diagnostics/ci_diagnostics.cpp)
target_include_directories(spark_diagnostics PUBLIC src)
target_link_libraries(spark_diagnostics PUBLIC Threads::Threads)
set_target_properties(spark_diagnostics PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- native backend: sampler, symbol guesser, allocation hooks (no Endstone) ---
add_library(spark_native STATIC
        src/native/alloc/allocation_profile_aggregation.cpp
        src/native/alloc/allocation_thread_filter.cpp
        src/native/sampler/call_tree.cpp
        src/native/sampler/sampler.cpp
        src/native/sampler/sampler_aggregation.cpp
        src/native/sampler/thread_selector.cpp
        src/native/sampler/thread_info.cpp
        src/native/symbol/symbol_guess_dwarf.cpp
        src/native/symbol/symbol_guess_evidence.cpp
        src/native/symbol/symbolicate.cpp
        src/native/symbol/symbol_guess.cpp)
if (WIN32)
    target_compile_definitions(spark_native PRIVATE NOMINMAX)
    target_sources(spark_native PRIVATE
            src/native/symbol/dbghelp_manager.cpp
            src/native/symbol/symbol_guess_windows.cpp
            src/native/symbol/symbol_guess_windows_support.cpp
            src/native/symbol/symbol_guess_windows_core.cpp
            src/native/symbol/symbol_guess_windows_rtti.cpp
            src/native/symbol/symbol_guess_windows_strings.cpp
            src/native/symbol/symbol_guess_windows_misc.cpp
            src/native/sampler/capture_windows.cpp
            src/native/alloc/allocation_sampler_windows.cpp
            src/native/alloc/windows_iat_hooks.cpp
            src/native/alloc/windows_iat_hooks_windows.cpp
            src/native/alloc/windows_permanent_iat_gateway.cpp
            src/native/alloc/windows_permanent_iat_gateway_registry.cpp
            src/native/alloc/windows_allocation_iat_hooks.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_sources(spark_native PRIVATE
            src/native/sampler/capture_linux.cpp
            src/native/alloc/allocation_sampler_linux.cpp
            src/native/alloc/elf_import_hooks.cpp)
    target_include_directories(spark_native PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(spark_native PUBLIC spark_linux_permanent_gateway)
else ()
    target_sources(spark_native PRIVATE
            src/native/sampler/capture_linux.cpp
            src/native/alloc/allocation_sampler_stub.cpp)
endif ()
target_include_directories(spark_native PUBLIC src)
target_link_libraries(spark_native PUBLIC spark_profiling_time spark_diagnostics cpptrace::cpptrace concurrentqueue::concurrentqueue
        Threads::Threads)
set_target_properties(spark_native PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(spark_native PRIVATE _GNU_SOURCE)
    target_compile_options(spark_native PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif ()
if (WIN32)
    target_link_libraries(spark_native PUBLIC dbghelp psapi advapi32 distorm)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_link_libraries(spark_native PUBLIC distorm ${CMAKE_DL_LIBS})
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(spark_native PUBLIC ${CMAKE_DL_LIBS})
endif ()

# --- platform-independent core: profiler, stats, proto, net (no Endstone) ---
add_library(spark_core STATIC
        src/core/config/spark_config.cpp
        src/core/config/spark_config_environment.cpp
        src/core/config/trusted_viewers.cpp
        src/core/recovery/journal_reader.cpp
        src/core/recovery/recovery_player.cpp
        src/core/recovery/recovery_writer.cpp
        src/core/recovery/stall_watchdog.cpp
        src/core/util/base64.cpp
        src/core/util/format.cpp
        src/core/util/state_file.cpp
        src/core/util/world_region.cpp
        src/core/metadata/behavior_packs.cpp
        src/core/metadata/server_properties.cpp
        src/core/activity/activity_log.cpp
        src/core/command/arguments.cpp
        src/core/profiler/profiler.cpp
        src/core/profiler/native_attribution.cpp
        src/core/profiler/profiler_export.cpp
        src/core/profiler/profiler_recovery.cpp
        src/core/stats/executable_hash.cpp
        src/core/stats/metrics_history.cpp
        src/core/stats/statistics_service.cpp
        src/core/stats/statistics_service_queries.cpp
        src/core/stats/statistics_profile_windows.cpp
        src/core/stats/ping_statistics.cpp
        src/core/stats/network_monitor.cpp
        src/core/stats/tick_monitor.cpp
        src/core/stats/system_stats.cpp
        src/core/ws/ws_proto.cpp
        src/core/ws/viewer_socket_lifecycle.cpp
        src/core/ws/viewer_socket_send.cpp
        src/core/ws/viewer_socket_receive.cpp
        src/proto/health_data.cpp
        src/proto/metrics_proto.cpp
        src/proto/sampler_data.cpp
        src/proto/statistics_proto.cpp
        src/net/gzip.cpp
        src/net/bytebin.cpp
        src/net/profile_file.cpp
        src/net/websocket.cpp)
target_include_directories(spark_core PUBLIC src)
if (WIN32)
    target_sources(spark_core PRIVATE
            src/core/stats/network_monitor_windows.cpp
            src/core/ws/crypto_cng.cpp)
    target_link_libraries(spark_core PUBLIC spark_diagnostics spark_native ZLIB::ZLIB CURL::libcurl tomlplusplus::tomlplusplus
        nlohmann_json::nlohmann_json bcrypt)
else ()
    target_sources(spark_core PRIVATE
            src/core/stats/network_monitor_linux.cpp
            src/core/ws/crypto.cpp)
    target_link_libraries(spark_core PUBLIC spark_diagnostics spark_native ZLIB::ZLIB CURL::libcurl tomlplusplus::tomlplusplus
        nlohmann_json::nlohmann_json OpenSSL::Crypto)
endif ()
if (WIN32)
    target_compile_definitions(spark_core PUBLIC NOMINMAX _WIN32_WINNT=0x0600 NTDDI_VERSION=0x06000000)
    target_link_libraries(spark_core PUBLIC iphlpapi)
endif ()
set_target_properties(spark_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spark_core PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif ()

# --- application layer: command dispatch, session orchestration, export (no Endstone) ---
add_library(spark_application STATIC
        src/application/activity/activity_command.cpp
        src/application/command/command_registry.cpp
        src/application/command/profiler_action_resolver.cpp
        src/application/health/health_command.cpp
        src/application/health/health_command_report.cpp
        src/application/health/health_report.cpp
        src/application/monitoring/monitoring_schedule.cpp
        src/application/health/health_dashboard.cpp
        src/application/health/health_dashboard_worker.cpp
        src/application/health/health_dashboard_connection.cpp
        src/application/placeholder/spark_placeholder_resolver.cpp
        src/application/profiler/profile_exporter.cpp
        src/application/profiler/live_statistics_payload.cpp
        src/application/profiler/live_viewer_schedule.cpp
        src/application/profiler/profiler_open_orchestrator.cpp
        src/application/profiler/profiler_service.cpp
        src/application/profiler/profiler_service_commands.cpp
        src/application/profiler/profiler_start_options.cpp
        src/application/profiler/profiler_timeout.cpp
        src/application/profiler/viewer_update_worker.cpp
        src/application/spark_application.cpp
        src/application/tick_monitor/tick_monitor_command.cpp)
target_include_directories(spark_application PUBLIC src)
target_link_libraries(spark_application PUBLIC spark_core)
set_target_properties(spark_application PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spark_application PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif ()

if (ENDSTONE_SPARK_BUILD_PLUGIN)
    add_library(spark_papi_integration STATIC
        src/platform/endstone/papi_integration.cpp)
    target_include_directories(spark_papi_integration PUBLIC src)
    target_link_libraries(spark_papi_integration PUBLIC spark_application spark_papi_headers)
    set_target_properties(spark_papi_integration PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(spark_papi_integration PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    endif ()

# --- the Endstone plugin (links its application layer + Endstone adapters) ---
    endstone_add_plugin(spark
        src/platform/endstone/adapters.cpp
        src/platform/endstone/world_gauge_provider.cpp
        src/platform/endstone/native_plugin_attribution.cpp
        src/plugin.cpp)
    target_include_directories(spark PRIVATE src)
    target_link_libraries(spark PRIVATE spark_application spark_papi_integration)
    target_compile_definitions(spark PRIVATE ENDSTONE_SPARK_ENDSTONE_API_0_11=1)
    if (TARGET spark_linux_permanent_gateway)
        install(TARGETS spark LIBRARY DESTINATION . COMPONENT spark_linux_runtime)
        add_custom_target(spark_linux_archive
                COMMAND ${CMAKE_COMMAND}
                        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
                        "-DCONFIG=$<CONFIG>"
                        "-DOUTPUT_DIR=$<TARGET_FILE_DIR:spark>"
                        "-DPYTHON=${Python3_EXECUTABLE}"
                        "-DVERIFIER=${CMAKE_CURRENT_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageLinuxRuntime.cmake"
                DEPENDS spark VERBATIM)
    endif ()
    if (WIN32)
        target_sources(spark PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
    endif ()
# Localize every symbol from the static archives (zlib, cpptrace, libunwind, libc++)
# so they can't interpose with Endstone core's own copies in the BDS process. Only
# init_endstone_plugin (default visibility via the macro) stays exported.
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(spark PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
        target_link_options(spark PRIVATE "-Wl,--exclude-libs,ALL")
        if (TARGET spark_linux_permanent_gateway)
            add_custom_command(TARGET spark POST_BUILD
                    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py"
                            elf "$<TARGET_FILE:spark>" VERBATIM)
        endif ()
    endif ()
endif ()

# --- offline self-test (no BDS needed) ---
if (ENDSTONE_SPARK_BUILD_SELFTEST)
    enable_testing()
    add_subdirectory(tests)
endif ()
