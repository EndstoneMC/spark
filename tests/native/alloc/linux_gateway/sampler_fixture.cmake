find_package(cpptrace REQUIRED)
find_package(concurrentqueue REQUIRED)
set(sampler_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/sampler-runtime")
add_library(spark_linux_sampler_fixture SHARED sampler_fixture.cpp
        "${CMAKE_SOURCE_DIR}/src/native/alloc/allocation_sampler_linux.cpp"
        "${CMAKE_SOURCE_DIR}/src/native/alloc/allocation_profile_aggregation.cpp"
        "${CMAKE_SOURCE_DIR}/src/native/alloc/allocation_thread_filter.cpp"
        "${CMAKE_SOURCE_DIR}/src/native/alloc/elf_import_hooks.cpp"
        "${CMAKE_SOURCE_DIR}/src/native/sampler/call_tree.cpp"
        "${CMAKE_SOURCE_DIR}/src/native/sampler/thread_selector.cpp"
        "${CMAKE_SOURCE_DIR}/src/native/sampler/thread_info.cpp"
        "${CMAKE_SOURCE_DIR}/src/core/profiler/profiling_window.cpp"
        "${CMAKE_SOURCE_DIR}/src/core/util/monotonic_time.cpp")
target_include_directories(spark_linux_sampler_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/tests"
        "${CMAKE_SOURCE_DIR}/src/core/profiler" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(spark_linux_sampler_fixture PRIVATE _GNU_SOURCE SPARK_ALLOCATION_LIFECYCLE_TESTING SPARK_GATEWAY_HANDLE_TESTING)
target_compile_options(spark_linux_sampler_fixture PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_options(spark_linux_sampler_fixture PRIVATE "-Wl,--exclude-libs,ALL")
target_link_libraries(spark_linux_sampler_fixture PRIVATE spark_linux_permanent_gateway cpptrace::cpptrace concurrentqueue::concurrentqueue ${CMAKE_DL_LIBS} pthread)
set_target_properties(spark_linux_sampler_fixture PROPERTIES PREFIX "" OUTPUT_NAME endstone_spark
        LIBRARY_OUTPUT_DIRECTORY "${sampler_fixture_root}")
add_executable(spark_linux_sampler_dso_test sampler_dso_test.cpp)
add_library(spark_linux_loader_blocker SHARED loader_blocker.cpp)
target_include_directories(spark_linux_sampler_dso_test PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/tests")
target_link_libraries(spark_linux_sampler_dso_test PRIVATE concurrentqueue::concurrentqueue ${CMAKE_DL_LIBS} pthread)
target_link_options(spark_linux_sampler_dso_test PRIVATE -rdynamic)
target_compile_definitions(spark_linux_sampler_dso_test PRIVATE _GNU_SOURCE
        SPARK_SAMPLER_FIXTURE="$<TARGET_FILE:spark_linux_sampler_fixture>"
        SPARK_LOADER_BLOCKER="$<TARGET_FILE:spark_linux_loader_blocker>")
add_dependencies(spark_linux_sampler_dso_test spark_linux_sampler_fixture spark_linux_loader_blocker)
foreach (mode churn aggregator_exit start_failures loader_block event_block final_block handle_fault hook_block tls_block creation_held errno preload)
    add_test(NAME spark_linux_sampler_dso_${mode} COMMAND spark_linux_sampler_dso_test ${mode})
    set_tests_properties(spark_linux_sampler_dso_${mode} PROPERTIES TIMEOUT 90)
endforeach ()
set(SPARK_TEST_JEMALLOC "" CACHE FILEPATH "jemalloc shared library for Linux preload tests")
set(SPARK_TEST_MIMALLOC "" CACHE FILEPATH "mimalloc shared library for Linux preload tests")
foreach (allocator jemalloc mimalloc)
    string(TOUPPER "${allocator}" allocator_upper)
    if (SPARK_TEST_${allocator_upper})
        add_test(NAME spark_linux_sampler_${allocator}_preload
                COMMAND ${CMAKE_COMMAND} -E env
                        "LD_PRELOAD=${SPARK_TEST_${allocator_upper}}"
                        "SPARK_EXPECT_ALLOCATOR=${SPARK_TEST_${allocator_upper}}"
                        $<TARGET_FILE:spark_linux_sampler_dso_test> preload)
        set_tests_properties(spark_linux_sampler_${allocator}_preload PROPERTIES TIMEOUT 90)
        add_test(NAME spark_linux_sampler_${allocator}_errno
                COMMAND ${CMAKE_COMMAND} -E env "LD_PRELOAD=${SPARK_TEST_${allocator_upper}}"
                        $<TARGET_FILE:spark_linux_sampler_dso_test> errno)
        set_tests_properties(spark_linux_sampler_${allocator}_errno PROPERTIES TIMEOUT 90)
    endif ()
endforeach ()
if (SPARK_TEST_JEMALLOC)
    add_test(NAME spark_linux_sampler_jemalloc_zero_realloc_rejected
            COMMAND ${CMAKE_COMMAND} -E env "LD_PRELOAD=${SPARK_TEST_JEMALLOC}"
                    "SPARK_EXPECT_ALLOCATOR=${SPARK_TEST_JEMALLOC}"
                    "MALLOC_CONF=zero_realloc:alloc"
                    $<TARGET_FILE:spark_linux_sampler_dso_test> preload_zero_realloc)
    set_tests_properties(spark_linux_sampler_jemalloc_zero_realloc_rejected PROPERTIES TIMEOUT 90)
endif ()
foreach (mode snapshot_admitted_stop snapshot_admitted_shutdown snapshot_aggregate_stop snapshot_aggregate_shutdown
        snapshot_restore_stop snapshot_restore_shutdown request_stop_loader)
    add_test(NAME spark_linux_sampler_dso_${mode} COMMAND spark_linux_sampler_dso_test ${mode})
    set_tests_properties(spark_linux_sampler_dso_${mode} PROPERTIES TIMEOUT 45)
endforeach ()

get_target_property(sampler_test_sources spark_linux_sampler_fixture SOURCES)
list(REMOVE_ITEM sampler_test_sources sampler_fixture.cpp)
add_library(spark_linux_sampler_test_backend STATIC ${sampler_test_sources})
target_include_directories(spark_linux_sampler_test_backend PUBLIC "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/tests"
        PRIVATE "${CMAKE_SOURCE_DIR}/src/core/profiler" "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(spark_linux_sampler_test_backend PUBLIC _GNU_SOURCE SPARK_ALLOCATION_LIFECYCLE_TESTING)
target_link_libraries(spark_linux_sampler_test_backend PUBLIC spark_linux_permanent_gateway cpptrace::cpptrace concurrentqueue::concurrentqueue ${CMAKE_DL_LIBS} pthread)
foreach (test lifecycle diagnostics count_only)
    add_executable(spark_linux_existing_${test}_test "${CMAKE_SOURCE_DIR}/tests/native/alloc/allocation_${test}_test.cpp"
            legacy_imports.cpp)
    target_link_libraries(spark_linux_existing_${test}_test PRIVATE spark_linux_sampler_test_backend)
    target_link_options(spark_linux_existing_${test}_test PRIVATE "-Wl,-z,relro,-z,now")
    set_target_properties(spark_linux_existing_${test}_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_linux_existing_${test} COMMAND spark_linux_existing_${test}_test)
    set_tests_properties(spark_linux_existing_${test} PROPERTIES TIMEOUT 120)
endforeach ()
