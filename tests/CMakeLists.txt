add_executable(spark_selftest
        selftest.cpp)
target_link_libraries(spark_selftest PRIVATE spark_core)
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
