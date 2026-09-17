if (MSVC)
    add_compile_options(/UNDEBUG)
else ()
    add_compile_options(-UNDEBUG)
endif ()

set(SPARK_SELFTEST_SOURCES
        selftest.cpp
        selftest_internal.cpp
        selftest_support.cpp
        selftest_thread_name.cpp
        selftest_basic.cpp
        selftest_capture.cpp
        selftest_sampler.cpp
        selftest_live.cpp
        selftest_viewer.cpp
        selftest_async.cpp
        selftest_statistics.cpp
        selftest_allocation_selection.cpp
        selftest_allocation_sampling.cpp)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    list(APPEND SPARK_SELFTEST_SOURCES
            selftest_allocation_common.cpp
            selftest_allocation_pressure.cpp
            selftest_allocation_lifecycle.cpp
            selftest_allocation_linux.cpp)
endif ()
add_executable(spark_selftest ${SPARK_SELFTEST_SOURCES})
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

add_executable(spark_ci_diagnostics_test core/ci_diagnostics_test.cpp)
target_link_libraries(spark_ci_diagnostics_test PRIVATE spark_diagnostics)
set_target_properties(spark_ci_diagnostics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_ci_diagnostics_test COMMAND spark_ci_diagnostics_test)

add_executable(spark_symbol_guess_evidence_test native/symbol/symbol_guess_evidence_test.cpp)
target_link_libraries(spark_symbol_guess_evidence_test PRIVATE spark_core)
set_target_properties(spark_symbol_guess_evidence_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_symbol_guess_evidence_test COMMAND spark_symbol_guess_evidence_test)

add_executable(spark_sampler_bounded_queue_test native/sampler/bounded_queue_test.cpp)
target_link_libraries(spark_sampler_bounded_queue_test PRIVATE spark_core)
set_target_properties(spark_sampler_bounded_queue_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_sampler_bounded_queue_test COMMAND spark_sampler_bounded_queue_test)

add_executable(spark_sampler_bounded_aggregation_test native/sampler/bounded_aggregation_test.cpp)
target_link_libraries(spark_sampler_bounded_aggregation_test PRIVATE spark_core)
set_target_properties(spark_sampler_bounded_aggregation_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_sampler_bounded_aggregation_test COMMAND spark_sampler_bounded_aggregation_test)

add_executable(spark_allocation_bounded_event_queue_test native/alloc/bounded_event_queue_test.cpp)
target_include_directories(spark_allocation_bounded_event_queue_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_target_properties(spark_allocation_bounded_event_queue_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_allocation_bounded_event_queue_test COMMAND spark_allocation_bounded_event_queue_test)

add_executable(spark_allocation_quiescence_test native/alloc/allocation_quiescence_test.cpp)
target_include_directories(spark_allocation_quiescence_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_target_properties(spark_allocation_quiescence_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_allocation_quiescence_test COMMAND spark_allocation_quiescence_test)

if (WIN32 OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    function(spark_lifecycle_sources target output)
        get_target_property(target_sources ${target} SOURCES)
        set(absolute_sources)
        foreach (source IN LISTS target_sources)
            if (IS_ABSOLUTE "${source}")
                list(APPEND absolute_sources "${source}")
            else ()
                list(APPEND absolute_sources "${CMAKE_SOURCE_DIR}/${source}")
            endif ()
        endforeach ()
        set(${output} "${absolute_sources}" PARENT_SCOPE)
    endfunction()

    spark_lifecycle_sources(spark_native SPARK_NATIVE_LIFECYCLE_SOURCES)
    add_library(spark_native_lifecycle_test STATIC ${SPARK_NATIVE_LIFECYCLE_SOURCES})
    target_compile_definitions(spark_native_lifecycle_test PUBLIC SPARK_ALLOCATION_LIFECYCLE_TESTING=1)
    target_include_directories(spark_native_lifecycle_test PUBLIC "${CMAKE_SOURCE_DIR}/src"
            PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(spark_native_lifecycle_test PUBLIC spark_profiling_time spark_diagnostics
            cpptrace::cpptrace concurrentqueue::concurrentqueue Threads::Threads)
    set_target_properties(spark_native_lifecycle_test PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (WIN32)
        target_compile_definitions(spark_native_lifecycle_test PRIVATE NOMINMAX)
        target_link_libraries(spark_native_lifecycle_test PUBLIC dbghelp psapi advapi32 distorm)
    else ()
        target_compile_definitions(spark_native_lifecycle_test PRIVATE _GNU_SOURCE)
        target_include_directories(spark_native_lifecycle_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
        if (TARGET spark_linux_permanent_gateway)
            target_link_libraries(spark_native_lifecycle_test PUBLIC spark_linux_permanent_gateway)
        endif ()
        target_compile_options(spark_native_lifecycle_test PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
        target_link_libraries(spark_native_lifecycle_test PUBLIC distorm ${CMAKE_DL_LIBS})
    endif ()

    spark_lifecycle_sources(spark_core SPARK_CORE_LIFECYCLE_SOURCES)
    add_library(spark_core_lifecycle_test STATIC ${SPARK_CORE_LIFECYCLE_SOURCES})
    target_compile_definitions(spark_core_lifecycle_test PUBLIC SPARK_ALLOCATION_LIFECYCLE_TESTING=1)
    target_include_directories(spark_core_lifecycle_test PUBLIC "${CMAKE_SOURCE_DIR}/src"
            PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    set_target_properties(spark_core_lifecycle_test PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (WIN32)
        target_compile_definitions(spark_core_lifecycle_test PUBLIC NOMINMAX _WIN32_WINNT=0x0600
                NTDDI_VERSION=0x06000000)
        target_link_libraries(spark_core_lifecycle_test PUBLIC spark_diagnostics spark_native_lifecycle_test
                ZLIB::ZLIB CURL::libcurl tomlplusplus::tomlplusplus nlohmann_json::nlohmann_json bcrypt iphlpapi)
    else ()
        target_compile_options(spark_core_lifecycle_test PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
        target_link_libraries(spark_core_lifecycle_test PUBLIC spark_diagnostics spark_native_lifecycle_test
                ZLIB::ZLIB CURL::libcurl tomlplusplus::tomlplusplus nlohmann_json::nlohmann_json OpenSSL::Crypto)
    endif ()

    add_executable(spark_allocation_lifecycle_test native/alloc/allocation_lifecycle_test.cpp)
    target_compile_definitions(spark_allocation_lifecycle_test PRIVATE SPARK_ALLOCATION_LIFECYCLE_TESTING=1)
    target_include_directories(spark_allocation_lifecycle_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(spark_allocation_lifecycle_test PRIVATE spark_native_lifecycle_test)
    set_target_properties(spark_allocation_lifecycle_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_allocation_lifecycle_test COMMAND spark_allocation_lifecycle_test)
    set_tests_properties(spark_allocation_lifecycle_test PROPERTIES TIMEOUT 60)

    add_executable(spark_allocation_diagnostics_test native/alloc/allocation_diagnostics_test.cpp)
    target_compile_definitions(spark_allocation_diagnostics_test PRIVATE SPARK_ALLOCATION_LIFECYCLE_TESTING=1)
    target_include_directories(spark_allocation_diagnostics_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(spark_allocation_diagnostics_test PRIVATE spark_native_lifecycle_test)
    set_target_properties(spark_allocation_diagnostics_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_allocation_diagnostics_test COMMAND spark_allocation_diagnostics_test)
    set_tests_properties(spark_allocation_diagnostics_test PROPERTIES TIMEOUT 60)

    spark_lifecycle_sources(spark_application SPARK_APPLICATION_LIFECYCLE_SOURCES)
    add_library(spark_application_lifecycle_test STATIC ${SPARK_APPLICATION_LIFECYCLE_SOURCES})
    target_compile_definitions(spark_application_lifecycle_test PUBLIC SPARK_ALLOCATION_LIFECYCLE_TESTING=1)
    target_include_directories(spark_application_lifecycle_test PUBLIC "${CMAKE_SOURCE_DIR}/src"
            PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(spark_application_lifecycle_test PUBLIC spark_core_lifecycle_test)
    set_target_properties(spark_application_lifecycle_test PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(spark_application_lifecycle_test PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    endif ()
endif ()

add_executable(spark_stable_shard_snapshot_test native/alloc/stable_shard_snapshot_test.cpp)
target_include_directories(spark_stable_shard_snapshot_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_target_properties(spark_stable_shard_snapshot_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_stable_shard_snapshot_test COMMAND spark_stable_shard_snapshot_test)

add_executable(spark_allocation_count_only_test native/alloc/allocation_count_only_test.cpp)
target_link_libraries(spark_allocation_count_only_test PRIVATE spark_core)
set_target_properties(spark_allocation_count_only_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_allocation_count_only_test COMMAND spark_allocation_count_only_test)

add_executable(spark_allocation_profile_aggregation_test native/alloc/allocation_profile_aggregation_test.cpp)
target_link_libraries(spark_allocation_profile_aggregation_test PRIVATE spark_core)
set_target_properties(spark_allocation_profile_aggregation_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_allocation_profile_aggregation_test COMMAND spark_allocation_profile_aggregation_test)

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

add_executable(spark_network_monitor_lifecycle_test core/stats/network_monitor_lifecycle_test.cpp)
target_link_libraries(spark_network_monitor_lifecycle_test PRIVATE spark_core)
set_target_properties(spark_network_monitor_lifecycle_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_network_monitor_lifecycle_test COMMAND spark_network_monitor_lifecycle_test)

add_executable(spark_double_rolling_average_test core/stats/double_rolling_average_test.cpp)
target_link_libraries(spark_double_rolling_average_test PRIVATE spark_core)
set_target_properties(spark_double_rolling_average_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_double_rolling_average_test COMMAND spark_double_rolling_average_test)

add_executable(spark_arguments_test core/command/arguments_test.cpp)
target_link_libraries(spark_arguments_test PRIVATE spark_core)
set_target_properties(spark_arguments_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_arguments_test COMMAND spark_arguments_test)

add_executable(spark_placeholder_statistics_test core/stats/placeholder_statistics_test.cpp)
target_link_libraries(spark_placeholder_statistics_test PRIVATE spark_core)
set_target_properties(spark_placeholder_statistics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_placeholder_statistics_test COMMAND spark_placeholder_statistics_test)

add_executable(spark_metrics_history_test core/stats/metrics_history_test.cpp)
target_link_libraries(spark_metrics_history_test PRIVATE spark_core)
set_target_properties(spark_metrics_history_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_metrics_history_test COMMAND spark_metrics_history_test)

add_executable(spark_proto_statistics_test proto/statistics_proto_test.cpp)
target_link_libraries(spark_proto_statistics_test PRIVATE spark_core)
set_target_properties(spark_proto_statistics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_proto_statistics_test COMMAND spark_proto_statistics_test)

add_executable(spark_sampler_statistics_test proto/sampler_statistics_test.cpp)
target_link_libraries(spark_sampler_statistics_test PRIVATE spark_core)
set_target_properties(spark_sampler_statistics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_sampler_statistics_test COMMAND spark_sampler_statistics_test)

add_executable(spark_proto_health_data_test proto/health_data_test.cpp)
target_link_libraries(spark_proto_health_data_test PRIVATE spark_core)
set_target_properties(spark_proto_health_data_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_proto_health_data_test COMMAND spark_proto_health_data_test)

add_executable(spark_proto_metrics_test proto/metrics_proto_test.cpp)
target_link_libraries(spark_proto_metrics_test PRIVATE spark_core)
set_target_properties(spark_proto_metrics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_proto_metrics_test COMMAND spark_proto_metrics_test)

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

add_executable(spark_monitoring_schedule_test application/monitoring/monitoring_schedule_test.cpp)
target_link_libraries(spark_monitoring_schedule_test PRIVATE spark_application)
set_target_properties(spark_monitoring_schedule_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_monitoring_schedule_test COMMAND spark_monitoring_schedule_test)

add_executable(spark_profiler_action_resolver_test application/command/profiler_action_resolver_test.cpp)
target_link_libraries(spark_profiler_action_resolver_test PRIVATE spark_application)
set_target_properties(spark_profiler_action_resolver_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_action_resolver_test COMMAND spark_profiler_action_resolver_test)

add_executable(spark_viewer_update_worker_test application/profiler/viewer_update_worker_test.cpp)
target_link_libraries(spark_viewer_update_worker_test PRIVATE spark_application)
set_target_properties(spark_viewer_update_worker_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_viewer_update_worker_test COMMAND spark_viewer_update_worker_test)

add_executable(spark_live_viewer_schedule_test application/profiler/live_viewer_schedule_test.cpp)
target_link_libraries(spark_live_viewer_schedule_test PRIVATE spark_application)
set_target_properties(spark_live_viewer_schedule_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_live_viewer_schedule_test COMMAND spark_live_viewer_schedule_test)

add_executable(spark_live_statistics_payload_test application/profiler/live_statistics_payload_test.cpp)
target_link_libraries(spark_live_statistics_payload_test PRIVATE spark_application)
set_target_properties(spark_live_statistics_payload_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_live_statistics_payload_test COMMAND spark_live_statistics_payload_test)

add_executable(spark_profiler_timeout_test application/profiler/profiler_timeout_test.cpp)
target_link_libraries(spark_profiler_timeout_test PRIVATE spark_application)
set_target_properties(spark_profiler_timeout_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_timeout_test COMMAND spark_profiler_timeout_test)
add_test(NAME spark_profiler_timeout_subprocess_test
        COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/application/profiler/profiler_timeout_subprocess.py
        $<TARGET_FILE:spark_profiler_timeout_test>)
set_tests_properties(spark_profiler_timeout_subprocess_test PROPERTIES
        TIMEOUT 15 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(spark_profiler_timeout_integration_test application/profiler/profiler_timeout_integration_test.cpp)
target_link_libraries(spark_profiler_timeout_integration_test PRIVATE spark_application)
set_target_properties(spark_profiler_timeout_integration_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_timeout_integration_test COMMAND spark_profiler_timeout_integration_test)
set_tests_properties(spark_profiler_timeout_integration_test PROPERTIES TIMEOUT 20)

add_executable(spark_profiler_service_failure_test application/profiler/profiler_service_failure_test.cpp)
target_link_libraries(spark_profiler_service_failure_test PRIVATE spark_application)
set_target_properties(spark_profiler_service_failure_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_service_failure_test COMMAND spark_profiler_service_failure_test)

if (WIN32 OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    add_executable(spark_profiler_export_lifecycle_test application/profiler/profiler_export_lifecycle_test.cpp)
    target_compile_definitions(spark_profiler_export_lifecycle_test PRIVATE SPARK_ALLOCATION_LIFECYCLE_TESTING=1)
    target_include_directories(spark_profiler_export_lifecycle_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(spark_profiler_export_lifecycle_test PRIVATE spark_application_lifecycle_test)
    set_target_properties(spark_profiler_export_lifecycle_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_profiler_export_lifecycle_test COMMAND spark_profiler_export_lifecycle_test)
    set_tests_properties(spark_profiler_export_lifecycle_test PROPERTIES TIMEOUT 30)
    add_test(NAME spark_profiler_export_lifecycle_subprocess_test
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/application/profiler/profiler_export_lifecycle_subprocess.py
                    $<TARGET_FILE:spark_profiler_export_lifecycle_test>)
    set_tests_properties(spark_profiler_export_lifecycle_subprocess_test PROPERTIES TIMEOUT 15)
endif ()

add_executable(spark_health_dashboard_test
        application/health/health_dashboard_test.cpp
        application/health/health_dashboard_test_support.cpp)
target_link_libraries(spark_health_dashboard_test PRIVATE spark_application)
set_target_properties(spark_health_dashboard_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_health_dashboard_test COMMAND spark_health_dashboard_test)

add_executable(spark_health_dashboard_concurrency_test
        application/health/health_dashboard_concurrency_test.cpp
        application/health/health_dashboard_test_support.cpp)
target_link_libraries(spark_health_dashboard_concurrency_test PRIVATE spark_application)
set_target_properties(spark_health_dashboard_concurrency_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_health_dashboard_concurrency_test COMMAND spark_health_dashboard_concurrency_test)

add_executable(spark_health_command_routing_test application/health/health_command_routing_test.cpp)
target_link_libraries(spark_health_command_routing_test PRIVATE spark_application)
set_target_properties(spark_health_command_routing_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_health_command_routing_test COMMAND spark_health_command_routing_test)

add_executable(spark_health_command_failure_test application/health/health_command_failure_test.cpp)
target_link_libraries(spark_health_command_failure_test PRIVATE spark_application)
set_target_properties(spark_health_command_failure_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_health_command_failure_test COMMAND spark_health_command_failure_test)

add_executable(spark_health_report_test application/health/health_report_test.cpp)
target_link_libraries(spark_health_report_test PRIVATE spark_application)
set_target_properties(spark_health_report_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_health_report_test COMMAND spark_health_report_test)

add_executable(spark_profiler_start_options_test application/profiler/profiler_start_options_test.cpp)
target_link_libraries(spark_profiler_start_options_test PRIVATE spark_application)
set_target_properties(spark_profiler_start_options_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_start_options_test COMMAND spark_profiler_start_options_test)

add_executable(spark_profiler_open_orchestrator_test application/profiler/profiler_open_orchestrator_test.cpp)
target_link_libraries(spark_profiler_open_orchestrator_test PRIVATE spark_application)
set_target_properties(spark_profiler_open_orchestrator_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_open_orchestrator_test COMMAND spark_profiler_open_orchestrator_test)

add_executable(spark_placeholder_resolver_test application/placeholder/spark_placeholder_resolver_test.cpp)
target_link_libraries(spark_placeholder_resolver_test PRIVATE spark_application)
set_target_properties(spark_placeholder_resolver_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_placeholder_resolver_test COMMAND spark_placeholder_resolver_test)

if (ENDSTONE_SPARK_BUILD_PLUGIN)
    add_executable(spark_papi_integration_test platform/endstone/papi_integration_test.cpp)
    target_link_libraries(spark_papi_integration_test PRIVATE spark_papi_integration)
    set_target_properties(spark_papi_integration_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_papi_integration_test COMMAND spark_papi_integration_test)
endif ()

add_executable(spark_platform_capabilities_compile_test application/platform_capabilities_compile_test.cpp)
target_include_directories(spark_platform_capabilities_compile_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_target_properties(spark_platform_capabilities_compile_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_platform_capabilities_compile_test COMMAND spark_platform_capabilities_compile_test)

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

add_executable(spark_profiler_export_test core/profiler/profiler_export_test.cpp)
target_link_libraries(spark_profiler_export_test PRIVATE spark_core)
set_target_properties(spark_profiler_export_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_export_test COMMAND spark_profiler_export_test)

if (WIN32 OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    add_executable(spark_profiler_recovery_lifecycle_test core/profiler/profiler_recovery_lifecycle_test.cpp)
    target_compile_definitions(spark_profiler_recovery_lifecycle_test PRIVATE SPARK_ALLOCATION_LIFECYCLE_TESTING=1)
    target_include_directories(spark_profiler_recovery_lifecycle_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(spark_profiler_recovery_lifecycle_test PRIVATE spark_core_lifecycle_test)
    set_target_properties(spark_profiler_recovery_lifecycle_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_profiler_recovery_lifecycle_test COMMAND spark_profiler_recovery_lifecycle_test)
    set_tests_properties(spark_profiler_recovery_lifecycle_test PROPERTIES TIMEOUT 60)
endif ()

add_executable(spark_included_ticks_test core/profiler/included_ticks_test.cpp)
target_link_libraries(spark_included_ticks_test PRIVATE spark_core)
set_target_properties(spark_included_ticks_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_included_ticks_test COMMAND spark_included_ticks_test)

add_executable(spark_config_test
        core/config/spark_config_test.cpp
        core/config/spark_config_load_test.cpp
        core/config/spark_config_persistence_test.cpp
        core/config/spark_config_environment_test.cpp)
target_link_libraries(spark_config_test PRIVATE spark_core)
set_target_properties(spark_config_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_config_test COMMAND spark_config_test)

add_executable(spark_trusted_viewers_test core/config/trusted_viewers_test.cpp)
target_link_libraries(spark_trusted_viewers_test PRIVATE spark_core)
set_target_properties(spark_trusted_viewers_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_trusted_viewers_test COMMAND spark_trusted_viewers_test)

add_executable(spark_crypto_test core/ws/crypto_test.cpp core/ws/crypto_test_keys.cpp)
target_link_libraries(spark_crypto_test PRIVATE spark_core)
set_target_properties(spark_crypto_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_crypto_test COMMAND spark_crypto_test)

add_executable(spark_ws_proto_test core/ws/ws_proto_test.cpp core/ws/crypto_test_keys.cpp)
target_link_libraries(spark_ws_proto_test PRIVATE spark_core)
set_target_properties(spark_ws_proto_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_ws_proto_test COMMAND spark_ws_proto_test)

add_executable(spark_websocket_lifecycle_test
        net/websocket_lifecycle_test.cpp
        net/websocket_lifecycle_test_support.cpp
        net/websocket_viewer_lifecycle_test.cpp
        core/ws/crypto_test_keys.cpp)
target_link_libraries(spark_websocket_lifecycle_test PRIVATE spark_core)
set_target_properties(spark_websocket_lifecycle_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_websocket_lifecycle_test COMMAND spark_websocket_lifecycle_test)

add_executable(spark_stall_watchdog_test core/recovery/stall_watchdog_test.cpp)
target_link_libraries(spark_stall_watchdog_test PRIVATE spark_core)
set_target_properties(spark_stall_watchdog_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_stall_watchdog_test COMMAND spark_stall_watchdog_test)

add_executable(spark_journal_test
        core/recovery/journal_test_main.cpp
        core/recovery/journal_test_support.cpp
        core/recovery/journal_format_test.cpp
        core/recovery/journal_writer_reader_test.cpp
        core/recovery/recovery_player_test.cpp
        core/recovery/recovery_rolling_test.cpp)
target_link_libraries(spark_journal_test PRIVATE spark_core)
set_target_properties(spark_journal_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_journal_test COMMAND spark_journal_test)

add_executable(spark_journal_reader_bounds_test core/recovery/journal_reader_bounds_test.cpp)
target_link_libraries(spark_journal_reader_bounds_test PRIVATE spark_core)
set_target_properties(spark_journal_reader_bounds_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_journal_reader_bounds_test COMMAND spark_journal_reader_bounds_test)

add_executable(spark_recovery_writer_queue_test core/recovery/recovery_writer_queue_test.cpp)
target_link_libraries(spark_recovery_writer_queue_test PRIVATE spark_core)
set_target_properties(spark_recovery_writer_queue_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_recovery_writer_queue_test COMMAND spark_recovery_writer_queue_test)

add_executable(spark_journal_format_bounds_test core/recovery/journal_format_bounds_test.cpp)
target_link_libraries(spark_journal_format_bounds_test PRIVATE spark_core)
set_target_properties(spark_journal_format_bounds_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_journal_format_bounds_test COMMAND spark_journal_format_bounds_test)

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

add_executable(spark_command_registry_test application/command/command_registry_test.cpp)
target_link_libraries(spark_command_registry_test PRIVATE spark_application)
set_target_properties(spark_command_registry_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_command_registry_test COMMAND spark_command_registry_test)

add_executable(spark_world_region_test core/util/world_region_test.cpp)
target_link_libraries(spark_world_region_test PRIVATE spark_core)
set_target_properties(spark_world_region_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_world_region_test COMMAND spark_world_region_test)

add_executable(spark_world_region_bounds_test core/util/world_region_bounds_test.cpp)
target_link_libraries(spark_world_region_bounds_test PRIVATE spark_core)
set_target_properties(spark_world_region_bounds_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_world_region_bounds_test COMMAND spark_world_region_bounds_test)

add_executable(spark_monotonic_time_test core/util/monotonic_time_test.cpp)
target_link_libraries(spark_monotonic_time_test PRIVATE spark_core)
set_target_properties(spark_monotonic_time_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_monotonic_time_test COMMAND spark_monotonic_time_test)

add_executable(spark_server_properties_test core/metadata/server_properties_test.cpp)
target_link_libraries(spark_server_properties_test PRIVATE spark_core)
set_target_properties(spark_server_properties_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_server_properties_test COMMAND spark_server_properties_test)

add_executable(spark_server_properties_json_test core/metadata/server_properties_json_test.cpp)
target_link_libraries(spark_server_properties_json_test PRIVATE spark_core)
set_target_properties(spark_server_properties_json_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_server_properties_json_test COMMAND spark_server_properties_json_test)

add_executable(spark_behavior_packs_test core/metadata/behavior_packs_test.cpp)
target_link_libraries(spark_behavior_packs_test PRIVATE spark_core)
set_target_properties(spark_behavior_packs_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_behavior_packs_test COMMAND spark_behavior_packs_test)

add_executable(spark_symbol_guess_dwarf_test native/symbol/symbol_guess_dwarf_test.cpp)
target_link_libraries(spark_symbol_guess_dwarf_test PRIVATE spark_core)
set_target_properties(spark_symbol_guess_dwarf_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_symbol_guess_dwarf_test COMMAND spark_symbol_guess_dwarf_test)

add_executable(spark_recovery_writer_durability_test core/recovery/recovery_writer_durability_test.cpp)
target_link_libraries(spark_recovery_writer_durability_test PRIVATE spark_core)
set_target_properties(spark_recovery_writer_durability_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_recovery_writer_durability_test COMMAND spark_recovery_writer_durability_test --durability)
add_test(NAME spark_recovery_writer_flush_test COMMAND spark_recovery_writer_durability_test --flush)
set_tests_properties(spark_recovery_writer_durability_test spark_recovery_writer_flush_test PROPERTIES TIMEOUT 60)

add_executable(spark_python_shadow_stack_test native/python/python_shadow_stack_test.cpp)
target_link_libraries(spark_python_shadow_stack_test PRIVATE spark_core Threads::Threads)
set_target_properties(spark_python_shadow_stack_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_python_shadow_stack_test COMMAND spark_python_shadow_stack_test)

set(SPARK_TEST_LIBPYTHON "$ENV{SPARK_TEST_LIBPYTHON}" CACHE FILEPATH "Python >= 3.12 shared runtime for native tests")
set(SPARK_TEST_LIBPYTHON_311 "" CACHE FILEPATH "Optional Python 3.11 shared runtime for fallback tests")
foreach (runtime SPARK_TEST_LIBPYTHON SPARK_TEST_LIBPYTHON_311)
    if (${runtime})
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c
                "import ctypes,sys; lib=ctypes.CDLL(sys.argv[1]); lib.Py_GetVersion.restype=ctypes.c_char_p; print(lib.Py_GetVersion().decode().split()[0])"
                "${${runtime}}" OUTPUT_VARIABLE runtime_version OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE runtime_result)
        if (NOT runtime_result EQUAL 0 OR
                (runtime STREQUAL "SPARK_TEST_LIBPYTHON" AND runtime_version VERSION_LESS "3.12") OR
                (runtime STREQUAL "SPARK_TEST_LIBPYTHON_311" AND NOT runtime_version MATCHES "^3\\.11\\."))
            message(FATAL_ERROR "${runtime} does not name the required Python runtime: ${runtime_version}")
        endif ()
    endif ()
endforeach ()
set(SPARK_PYTHON_RUNTIME_TESTS python_admission_privacy_test)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SPARK_PYTHON_RUNTIME_TESTS python_reload_identity_test python_attribution_runtime_test)
endif ()
foreach (fixture IN LISTS SPARK_PYTHON_RUNTIME_TESTS)
    add_executable(spark_${fixture} native/python/${fixture}.cpp)
    target_link_libraries(spark_${fixture} PRIVATE spark_core ${CMAKE_DL_LIBS})
    set_target_properties(spark_${fixture} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if (fixture STREQUAL "python_admission_privacy_test")
        add_test(NAME spark_${fixture} COMMAND spark_${fixture})
    else ()
        add_test(NAME spark_${fixture} COMMAND spark_${fixture} --expect-mode=monitoring)
    endif ()
    set_tests_properties(spark_${fixture} PROPERTIES TIMEOUT 180
            ENVIRONMENT "SPARK_TEST_LIBPYTHON=${SPARK_TEST_LIBPYTHON}" LABELS "python-monitoring")
endforeach ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SPARK_TEST_LIBPYTHON_311)
    add_test(NAME spark_python_attribution_fallback_test
            COMMAND spark_python_attribution_runtime_test --expect-mode=fallback)
    set_tests_properties(spark_python_attribution_fallback_test PROPERTIES TIMEOUT 180
            ENVIRONMENT "SPARK_TEST_LIBPYTHON=${SPARK_TEST_LIBPYTHON_311}" LABELS "python-3.11-fallback")
endif ()

add_executable(spark_windows_iat_hooks_test native/alloc/windows_iat_hooks_test.cpp)
if (WIN32)
    target_link_libraries(spark_windows_iat_hooks_test PRIVATE spark_core)
else ()
    target_sources(spark_windows_iat_hooks_test PRIVATE "${CMAKE_SOURCE_DIR}/src/native/alloc/windows_iat_hooks.cpp")
    target_include_directories(spark_windows_iat_hooks_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
endif ()
set_target_properties(spark_windows_iat_hooks_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_windows_iat_hooks_test COMMAND spark_windows_iat_hooks_test)

if (WIN32)
    foreach (fixture windows_iat_provider windows_iat_consumer windows_permanent_iat_backend_client
            windows_permanent_iat_backend_plugin)
        add_library(${fixture} SHARED native/alloc/${fixture}.cpp)
        set_target_properties(${fixture} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    endforeach ()
    target_link_libraries(windows_iat_consumer PRIVATE windows_iat_provider)
    target_link_libraries(windows_permanent_iat_backend_plugin PRIVATE spark_core)
    add_library(windows_iat_gateway_test_handler SHARED native/alloc/windows_gateway_test_handler.cpp)
    set_target_properties(windows_iat_gateway_test_handler PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

    foreach (fixture windows_iat_native_test windows_permanent_iat_backend_reload_test
            windows_permanent_iat_gateway_registry_test windows_permanent_iat_gateway_test
            windows_permanent_iat_gateway_unload_test windows_permanent_iat_gateway_unwind_test
            windows_permanent_iat_gateway_seh_test)
        add_executable(spark_${fixture} native/alloc/${fixture}.cpp)
        target_link_libraries(spark_${fixture} PRIVATE spark_core Threads::Threads)
        set_target_properties(spark_${fixture} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
        add_test(NAME spark_${fixture} COMMAND spark_${fixture})
        set_tests_properties(spark_${fixture} PROPERTIES TIMEOUT 180
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:spark_${fixture}>")
    endforeach ()
    add_dependencies(spark_windows_iat_native_test windows_iat_consumer windows_iat_provider)
    add_dependencies(spark_windows_permanent_iat_backend_reload_test
            windows_permanent_iat_backend_client windows_permanent_iat_backend_plugin)
    add_dependencies(spark_windows_permanent_iat_gateway_unload_test windows_iat_gateway_test_handler)

    add_executable(spark_symbol_guess_windows_test
            native/symbol/symbol_guess_windows_test.cpp
            native/symbol/symbol_guess_windows_test_support.cpp
            native/symbol/symbol_guess_windows_test_core.cpp
            native/symbol/symbol_guess_windows_test_rtti.cpp
            native/symbol/symbol_guess_windows_test_strings.cpp
            native/symbol/symbol_guess_windows_test_bounds.cpp
            native/symbol/symbol_guess_windows_test_misc.cpp
            native/symbol/symbol_guess_windows_test_evaluator.cpp)
    target_link_libraries(spark_symbol_guess_windows_test PRIVATE spark_core)
    set_target_properties(spark_symbol_guess_windows_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_symbol_guess_windows_test COMMAND spark_symbol_guess_windows_test)
    set_tests_properties(spark_symbol_guess_windows_test PROPERTIES TIMEOUT 30)

    add_custom_target(spark_windows_pe_evaluator_test DEPENDS spark_symbol_guess_windows_test)
    add_test(NAME spark_windows_pe_evaluator_test
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/native/symbol/test_windows_pe_evaluator.py
            --executable $<TARGET_FILE:spark_symbol_guess_windows_test>)
    set_tests_properties(spark_windows_pe_evaluator_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    add_executable(spark_windows_allocation_last_error_test
            native/alloc/windows_allocation_last_error_test.cpp
            "${CMAKE_SOURCE_DIR}/src/native/alloc/allocation_sampler_windows.cpp")
    target_compile_definitions(spark_windows_allocation_last_error_test PRIVATE
            SPARK_ALLOCATION_LAST_ERROR_TESTING=1 NOMINMAX)
    target_include_directories(spark_windows_allocation_last_error_test PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(spark_windows_allocation_last_error_test PRIVATE spark_native)
    set_target_properties(spark_windows_allocation_last_error_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_windows_allocation_last_error_test COMMAND spark_windows_allocation_last_error_test)

    add_library(spark_windows_allocation_fixture SHARED native/alloc/windows_allocation_fixture.cpp)
    set_target_properties(spark_windows_allocation_fixture PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            OUTPUT_NAME "spark_windows_allocation_fixture")

    add_executable(spark_windows_allocation_backend_test
            native/alloc/windows_allocation_backend_test.cpp)
    add_dependencies(spark_windows_allocation_backend_test spark_windows_allocation_fixture)
    target_link_libraries(spark_windows_allocation_backend_test PRIVATE spark_core)
    set_target_properties(spark_windows_allocation_backend_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_windows_allocation_backend_test COMMAND spark_windows_allocation_backend_test)

    add_executable(spark_windows_allocation_heap_test native/alloc/windows_allocation_heap_test.cpp)
    add_dependencies(spark_windows_allocation_heap_test spark_windows_allocation_fixture)
    target_include_directories(spark_windows_allocation_heap_test PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(spark_windows_allocation_heap_test PRIVATE spark_core_lifecycle_test)
    set_target_properties(spark_windows_allocation_heap_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_windows_allocation_heap_test COMMAND spark_windows_allocation_heap_test)
    set_tests_properties(spark_windows_allocation_heap_test PROPERTIES TIMEOUT 20)

    add_executable(spark_windows_allocation_new_handler_test native/alloc/windows_allocation_new_handler_test.cpp)
    add_dependencies(spark_windows_allocation_new_handler_test spark_windows_allocation_fixture)
    target_include_directories(spark_windows_allocation_new_handler_test PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(spark_windows_allocation_new_handler_test PRIVATE spark_core_lifecycle_test)
    set_target_properties(spark_windows_allocation_new_handler_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_windows_allocation_new_handler_test COMMAND spark_windows_allocation_new_handler_test)
    set_tests_properties(spark_windows_allocation_new_handler_test PROPERTIES TIMEOUT 10)

    add_executable(spark_windows_allocation_stop_test native/alloc/windows_allocation_stop_test.cpp)
    target_link_libraries(spark_windows_allocation_stop_test PRIVATE spark_core)
    set_target_properties(spark_windows_allocation_stop_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_windows_allocation_stop_test COMMAND spark_windows_allocation_stop_test)

    add_executable(spark_capture_windows_lifecycle_test native/sampler/capture_windows_lifecycle_test.cpp)
    target_link_libraries(spark_capture_windows_lifecycle_test PRIVATE spark_core)
    set_target_properties(spark_capture_windows_lifecycle_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_capture_windows_lifecycle_test COMMAND spark_capture_windows_lifecycle_test)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    enable_language(ASM)
    add_subdirectory(native/alloc/linux_gateway)
    foreach (fixture spark_allocation_lifecycle_test spark_allocation_diagnostics_test
            spark_allocation_count_only_test spark_profiler_export_test spark_profiler_recovery_lifecycle_test)
        target_link_options(${fixture} PRIVATE "LINKER:-z,now")
    endforeach ()
    if (TARGET spark_linux_sampler_churn_test)
        target_link_options(spark_linux_sampler_churn_test PRIVATE "LINKER:-z,now")
    endif ()
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

    add_executable(spark_symbol_guess_linux_fixture
            native/symbol/symbol_guess_linux_fixture.cpp
            native/symbol/symbol_guess_linux_fixture.S)
    target_link_libraries(spark_symbol_guess_linux_fixture PRIVATE spark_core ${CMAKE_DL_LIBS})
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "The Linux symbol fixture requires Clang's libc++ and libc++abi static archives")
    endif ()
    execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -stdlib=libc++ -print-file-name=libc++.a
            OUTPUT_VARIABLE SPARK_FIXTURE_LIBCXX_STATIC
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE SPARK_FIXTURE_LIBCXX_RESULT)
    execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -stdlib=libc++ -print-file-name=libc++abi.a
            OUTPUT_VARIABLE SPARK_FIXTURE_LIBCXXABI_STATIC
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE SPARK_FIXTURE_LIBCXXABI_RESULT)
    if (NOT SPARK_FIXTURE_LIBCXX_RESULT EQUAL 0 OR NOT IS_ABSOLUTE "${SPARK_FIXTURE_LIBCXX_STATIC}" OR
        NOT EXISTS "${SPARK_FIXTURE_LIBCXX_STATIC}")
        unset(SPARK_FIXTURE_LIBCXX_STATIC CACHE)
        unset(SPARK_FIXTURE_LIBCXX_STATIC)
        find_file(SPARK_FIXTURE_LIBCXX_STATIC NAMES libc++.a PATHS ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES}
                NO_DEFAULT_PATH)
    endif ()
    if (NOT SPARK_FIXTURE_LIBCXXABI_RESULT EQUAL 0 OR NOT IS_ABSOLUTE "${SPARK_FIXTURE_LIBCXXABI_STATIC}" OR
        NOT EXISTS "${SPARK_FIXTURE_LIBCXXABI_STATIC}")
        unset(SPARK_FIXTURE_LIBCXXABI_STATIC CACHE)
        unset(SPARK_FIXTURE_LIBCXXABI_STATIC)
        find_file(SPARK_FIXTURE_LIBCXXABI_STATIC NAMES libc++abi.a PATHS ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES}
                NO_DEFAULT_PATH)
    endif ()
    if (NOT EXISTS "${SPARK_FIXTURE_LIBCXX_STATIC}" OR NOT EXISTS "${SPARK_FIXTURE_LIBCXXABI_STATIC}")
        message(FATAL_ERROR "The Linux symbol fixture requires static libc++.a and libc++abi.a")
    endif ()
    target_compile_options(spark_symbol_guess_linux_fixture PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
    set(SPARK_FIXTURE_STATIC_LINK_GROUP
            "$<LINK_GROUP:RESCAN,spark_core,spark_native,spark_diagnostics,spark_profiling_time,spark_linux_permanent_gateway,cpptrace::cpptrace,distorm,${SPARK_FIXTURE_LIBCXX_STATIC},${SPARK_FIXTURE_LIBCXXABI_STATIC}>")
    target_link_libraries(spark_symbol_guess_linux_fixture PRIVATE
            "${SPARK_FIXTURE_STATIC_LINK_GROUP}"
            ${CMAKE_DL_LIBS})
    target_compile_options(spark_symbol_guess_linux_fixture PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-O2>
            $<$<COMPILE_LANGUAGE:CXX>:-fPIE>
            $<$<COMPILE_LANGUAGE:CXX>:-fvisibility=hidden>)
    target_link_options(spark_symbol_guess_linux_fixture PRIVATE -stdlib=libc++ -nostdlib++ -pie)
    set_target_properties(spark_symbol_guess_linux_fixture PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    find_program(SPARK_STRIP_EXECUTABLE NAMES strip llvm-strip)
    if (SPARK_STRIP_EXECUTABLE)
        add_test(NAME spark_symbol_guess_linux_fixture_test
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/native/symbol/test_linux_symbol_fixture.py
                --executable $<TARGET_FILE:spark_symbol_guess_linux_fixture>
                --strip-tool ${SPARK_STRIP_EXECUTABLE})
    else ()
        message(STATUS "Skipping spark_symbol_guess_linux_fixture_test: no strip tool found")
    endif ()

    add_library(spark_elf_hook_fixture MODULE native/alloc/elf_hook_fixture.cpp)
    set_target_properties(spark_elf_hook_fixture PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(spark_selftest spark_elf_hook_fixture)
    target_compile_definitions(
            spark_selftest PRIVATE
            _GNU_SOURCE
            SPARK_ELF_HOOK_FIXTURE_PATH="$<TARGET_FILE:spark_elf_hook_fixture>")
    target_compile_definitions(spark_profiler_export_test PRIVATE _GNU_SOURCE)

    add_executable(spark_capture_linux_lifecycle_test native/sampler/capture_linux_lifecycle_test.cpp)
    target_compile_definitions(spark_capture_linux_lifecycle_test PRIVATE _GNU_SOURCE)
    target_link_libraries(spark_capture_linux_lifecycle_test PRIVATE spark_core ${CMAKE_DL_LIBS})
    set_target_properties(spark_capture_linux_lifecycle_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_capture_linux_lifecycle_test COMMAND spark_capture_linux_lifecycle_test)

    add_library(spark_capture_linux_module_fixture MODULE native/sampler/capture_linux_module_fixture.cpp)
    target_include_directories(spark_capture_linux_module_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(spark_capture_linux_module_fixture PRIVATE _GNU_SOURCE)
    target_link_libraries(spark_capture_linux_module_fixture PRIVATE cpptrace::cpptrace Threads::Threads ${CMAKE_DL_LIBS})
    target_link_options(spark_capture_linux_module_fixture PRIVATE -Wl,--wrap=dlopen -Wl,--exclude-libs,ALL)
    set_target_properties(spark_capture_linux_module_fixture PROPERTIES
            CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    foreach (kind pie nonpie)
        set(driver spark_capture_linux_module_${kind}_test)
        add_executable(${driver} native/sampler/capture_linux_module_test.cpp)
        target_compile_definitions(${driver} PRIVATE _GNU_SOURCE
                SPARK_CAPTURE_MODULE_FIXTURE_PATH="$<TARGET_FILE:spark_capture_linux_module_fixture>")
        target_link_libraries(${driver} PRIVATE spark_native Threads::Threads ${CMAKE_DL_LIBS})
        add_dependencies(${driver} spark_capture_linux_module_fixture)
        set_target_properties(${driver} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
        if (kind STREQUAL "pie")
            target_compile_options(${driver} PRIVATE -fPIE)
            target_link_options(${driver} PRIVATE -pie)
        else ()
            target_compile_options(${driver} PRIVATE -fno-pie)
            target_link_options(${driver} PRIVATE -no-pie)
        endif ()
        add_test(NAME ${driver} COMMAND ${driver})
        add_test(NAME ${driver}_relative COMMAND "./$<TARGET_FILE_NAME:${driver}>")
        set_tests_properties(${driver}_relative PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:${driver}>")
    endforeach ()
    add_test(NAME spark_capture_linux_module_dso_success_test COMMAND spark_capture_linux_module_pie_test --dso-success)
    add_test(NAME spark_capture_linux_module_dso_failure_test COMMAND spark_capture_linux_module_pie_test --dso-failure)
    add_test(NAME spark_selftest_relative COMMAND "./$<TARGET_FILE_NAME:spark_selftest>" --seconds=1)
    set_tests_properties(spark_selftest_relative PROPERTIES WORKING_DIRECTORY "$<TARGET_FILE_DIR:spark_selftest>")
endif ()

add_test(NAME spark_selftest COMMAND spark_selftest --seconds=1)

# --- Python tests (no compilation needed) ---
add_test(NAME spark_architecture_boundaries_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_architecture_boundaries.py)

add_test(NAME spark_release_changelog_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_release_changelog.py)

add_test(NAME spark_release_workflow_inputs_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_release_workflow_inputs.py)
set_tests_properties(spark_release_workflow_inputs_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_test(NAME spark_profile_evaluator_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_profile_evaluator.py)

add_test(NAME spark_workflow_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_workflows.py)

add_test(NAME spark_plugin_lifecycle_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_plugin_lifecycle.py)
