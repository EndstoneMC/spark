if (MSVC)
    add_compile_options(/UNDEBUG)
else ()
    add_compile_options(-UNDEBUG)
endif ()

add_executable(spark_selftest
        selftest.cpp)
target_link_libraries(spark_selftest PRIVATE spark_application)
set_target_properties(spark_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(spark_allocation_benchmark allocation_benchmark.cpp)
target_compile_definitions(
        spark_allocation_benchmark PRIVATE
        SPARK_ALLOCATION_BENCHMARK_CURRENT)
target_link_libraries(spark_allocation_benchmark PRIVATE spark_core)
set_target_properties(spark_allocation_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(spark_symbol_guess_evidence_test native/symbol/symbol_guess_evidence_test.cpp)
target_link_libraries(spark_symbol_guess_evidence_test PRIVATE spark_core)
set_target_properties(spark_symbol_guess_evidence_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_symbol_guess_evidence_test COMMAND spark_symbol_guess_evidence_test)

add_executable(spark_ping_statistics_test core/stats/ping_statistics_test.cpp)
target_link_libraries(spark_ping_statistics_test PRIVATE spark_core)
set_target_properties(spark_ping_statistics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_ping_statistics_test COMMAND spark_ping_statistics_test)

add_executable(spark_network_monitor_test core/stats/network_monitor_test.cpp)
target_link_libraries(spark_network_monitor_test PRIVATE spark_core)
set_target_properties(spark_network_monitor_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_network_monitor_test COMMAND spark_network_monitor_test)

add_executable(spark_placeholder_statistics_test core/stats/placeholder_statistics_test.cpp)
target_link_libraries(spark_placeholder_statistics_test PRIVATE spark_core)
set_target_properties(spark_placeholder_statistics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_placeholder_statistics_test COMMAND spark_placeholder_statistics_test)

add_executable(spark_activity_log_test core/activity/activity_log_test.cpp)
target_link_libraries(spark_activity_log_test PRIVATE spark_core)
set_target_properties(spark_activity_log_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_activity_log_test COMMAND spark_activity_log_test)

add_executable(spark_activity_command_test application/activity/activity_command_test.cpp)
target_link_libraries(spark_activity_command_test PRIVATE spark_application)
set_target_properties(spark_activity_command_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_activity_command_test COMMAND spark_activity_command_test)

add_executable(spark_placeholder_resolver_test application/placeholder/spark_placeholder_resolver_test.cpp)
target_link_libraries(spark_placeholder_resolver_test PRIVATE spark_application)
set_target_properties(spark_placeholder_resolver_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_placeholder_resolver_test COMMAND spark_placeholder_resolver_test)

add_executable(spark_papi_integration_test platform/endstone/papi_integration_test.cpp)
target_link_libraries(spark_papi_integration_test PRIVATE spark_papi_integration)
set_target_properties(spark_papi_integration_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_papi_integration_test COMMAND spark_papi_integration_test)

add_executable(spark_thread_grouper_test core/profiler/thread_grouper_test.cpp)
target_link_libraries(spark_thread_grouper_test PRIVATE spark_core)
set_target_properties(spark_thread_grouper_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_thread_grouper_test COMMAND spark_thread_grouper_test)

add_executable(spark_native_plugin_attribution_test core/profiler/native_plugin_attribution_test.cpp)
target_link_libraries(spark_native_plugin_attribution_test PRIVATE spark_core)
set_target_properties(spark_native_plugin_attribution_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_native_plugin_attribution_test COMMAND spark_native_plugin_attribution_test)

add_executable(spark_config_test core/config/spark_config_test.cpp)
target_link_libraries(spark_config_test PRIVATE spark_core)
set_target_properties(spark_config_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_config_test COMMAND spark_config_test)

add_executable(spark_trusted_viewers_test core/config/trusted_viewers_test.cpp)
target_link_libraries(spark_trusted_viewers_test PRIVATE spark_core)
set_target_properties(spark_trusted_viewers_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_trusted_viewers_test COMMAND spark_trusted_viewers_test)

add_executable(spark_crypto_test core/ws/crypto_test.cpp)
target_link_libraries(spark_crypto_test PRIVATE spark_core)
set_target_properties(spark_crypto_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_crypto_test COMMAND spark_crypto_test)

add_executable(spark_websocket_lifecycle_test net/websocket_lifecycle_test.cpp)
target_link_libraries(spark_websocket_lifecycle_test PRIVATE spark_core)
set_target_properties(spark_websocket_lifecycle_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_websocket_lifecycle_test COMMAND spark_websocket_lifecycle_test)

add_executable(spark_stall_watchdog_test core/recovery/stall_watchdog_test.cpp)
target_link_libraries(spark_stall_watchdog_test PRIVATE spark_core)
set_target_properties(spark_stall_watchdog_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_stall_watchdog_test COMMAND spark_stall_watchdog_test)

add_executable(spark_journal_test core/recovery/journal_test.cpp)
target_link_libraries(spark_journal_test PRIVATE spark_core)
set_target_properties(spark_journal_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_journal_test COMMAND spark_journal_test)

add_executable(spark_recovery_save_failure_test application/recovery/recovery_save_failure_test.cpp)
target_link_libraries(spark_recovery_save_failure_test PRIVATE spark_application)
set_target_properties(spark_recovery_save_failure_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_recovery_save_failure_test COMMAND spark_recovery_save_failure_test)

add_executable(spark_recovery_corrupt_test application/recovery/recovery_corrupt_test.cpp)
target_link_libraries(spark_recovery_corrupt_test PRIVATE spark_application)
set_target_properties(spark_recovery_corrupt_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_recovery_corrupt_test COMMAND spark_recovery_corrupt_test)

add_executable(spark_profiler_upload_alias_test application/command/profiler_upload_alias_test.cpp)
target_link_libraries(spark_profiler_upload_alias_test PRIVATE spark_application)
set_target_properties(spark_profiler_upload_alias_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_upload_alias_test COMMAND spark_profiler_upload_alias_test)

add_executable(spark_world_region_test core/util/world_region_test.cpp)
target_link_libraries(spark_world_region_test PRIVATE spark_core)
set_target_properties(spark_world_region_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_world_region_test COMMAND spark_world_region_test)

add_executable(spark_server_properties_test core/metadata/server_properties_test.cpp)
target_link_libraries(spark_server_properties_test PRIVATE spark_core)
set_target_properties(spark_server_properties_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_server_properties_test COMMAND spark_server_properties_test)

add_executable(spark_symbol_guess_dwarf_test native/symbol/symbol_guess_dwarf_test.cpp)
target_link_libraries(spark_symbol_guess_dwarf_test PRIVATE spark_core)
set_target_properties(spark_symbol_guess_dwarf_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_symbol_guess_dwarf_test COMMAND spark_symbol_guess_dwarf_test)

if (WIN32)
    add_executable(spark_symbol_guess_windows_test native/symbol/symbol_guess_windows_test.cpp)
    target_link_libraries(spark_symbol_guess_windows_test PRIVATE spark_core)
    set_target_properties(spark_symbol_guess_windows_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_symbol_guess_windows_test COMMAND spark_symbol_guess_windows_test)

    add_library(spark_windows_allocation_fixture MODULE
            native/alloc/windows_allocation_fixture.cpp)
    set_target_properties(spark_windows_allocation_fixture PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(spark_selftest spark_windows_allocation_fixture)
    target_compile_definitions(
            spark_selftest PRIVATE
            SPARK_WINDOWS_ALLOCATION_FIXTURE_PATH="$<TARGET_FILE:spark_windows_allocation_fixture>")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_executable(spark_symbol_guess_linux_test native/symbol/symbol_guess_linux_test.cpp)
    target_link_libraries(spark_symbol_guess_linux_test PRIVATE spark_core)
    set_target_properties(spark_symbol_guess_linux_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_symbol_guess_linux_test COMMAND spark_symbol_guess_linux_test)

    add_library(spark_hidden_symbol_fixture MODULE native/symbol/hidden_symbol_fixture.cpp)
    target_compile_options(spark_hidden_symbol_fixture PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    set_target_properties(spark_hidden_symbol_fixture PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(spark_hidden_symbol_test native/symbol/hidden_symbol_test.cpp)
    target_link_libraries(spark_hidden_symbol_test PRIVATE spark_core ${CMAKE_DL_LIBS})
    target_compile_definitions(
            spark_hidden_symbol_test PRIVATE
            SPARK_HIDDEN_SYMBOL_FIXTURE_PATH="$<TARGET_FILE:spark_hidden_symbol_fixture>")
    add_dependencies(spark_hidden_symbol_test spark_hidden_symbol_fixture)
    set_target_properties(spark_hidden_symbol_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_hidden_symbol_test COMMAND spark_hidden_symbol_test)

    add_library(spark_elf_hook_fixture MODULE native/alloc/elf_hook_fixture.cpp)
    set_target_properties(spark_elf_hook_fixture PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(spark_selftest spark_elf_hook_fixture)
    target_compile_definitions(
            spark_selftest PRIVATE
            _GNU_SOURCE
            SPARK_ELF_HOOK_FIXTURE_PATH="$<TARGET_FILE:spark_elf_hook_fixture>")
endif ()

add_test(NAME spark_selftest COMMAND spark_selftest --seconds=1)

# --- Python tests (no compilation needed) ---
add_test(NAME spark_architecture_boundaries_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_architecture_boundaries.py)

add_test(NAME spark_release_changelog_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_release_changelog.py)

add_test(NAME spark_profile_evaluator_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_profile_evaluator.py)

add_test(NAME spark_workflow_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_workflows.py)
