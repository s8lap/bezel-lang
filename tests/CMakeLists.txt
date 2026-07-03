# Test executable. Phase tests are added to BEZEL_TEST_SOURCES as each milestone
# lands its spec-derived tests; the per-phase directories alongside this file
# are placeholders for them.
# Test sources are auto-discovered (stage 1 of #1414, amended per the
# adversarial review): every *.cpp under the test directories joins the suite.
# To EXCLUDE a file, rename it *.cpp.disabled (self-documenting on disk) —
# see codegen/iterator_test.cpp.disabled (out of the JIT suite for LLVM module
# state accumulation, commit 0112853; iterator_native_test.cpp supersedes it).
# CONFIGURE_DEPENDS is reliable under the Ninja preset.
file(GLOB BEZEL_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lexer/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sema/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ownership/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/flow/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/codegen/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/module/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/conformance/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/support/*.cpp)
list(LENGTH BEZEL_TEST_SOURCES BEZEL_TEST_SOURCE_COUNT)
message(STATUS "Bezel: bezel_tests auto-discovered ${BEZEL_TEST_SOURCE_COUNT} test sources")

add_executable(bezel_tests ${BEZEL_TEST_SOURCES})
target_include_directories(bezel_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
# Precompiled header: the LLVM (+ a few ubiquitous std) headers dominate the
# ~209K preprocessed lines every one of the ~770 test TUs re-parses, and they
# are identical across all of them (one target → one set of flags, so the PCH is
# always valid). Precompiling them once cuts the per-TU compile by ~40%. Only
# the HEAVY, STABLE headers are listed — deliberately NOT the bezel sema/codegen
# headers, which change often: putting a frequently-edited header in the PCH
# would invalidate it (and force a full test rebuild) on every such edit.
# NOTE (machine-local, not tracked): ccache needs `sloppiness =
# pch_defines,time_macros` (~/.config/ccache/ccache.conf) to cache PCH builds —
# see the build-accelerators note in CLAUDE.md.
target_precompile_headers(bezel_tests PRIVATE
    <cstddef> <cstdlib> <functional> <memory>
    <string> <string_view> <utility> <vector>
    <llvm/IR/LLVMContext.h>
    <llvm/IR/Module.h>
    <llvm/ExecutionEngine/Orc/LLJIT.h>
    <llvm/ExecutionEngine/Orc/ExecutionUtils.h>)
target_link_libraries(bezel_tests PRIVATE bezel_core bezel_warnings)
# Force all bezel_runtime object files into bezel_tests, then export-dynamic
# so the JIT (DynamicLibrarySearchGenerator::GetForCurrentProcess) can resolve
# bz_array_*, bz_dict_*, etc. from the process symbol table at JIT run time.
target_link_options(bezel_tests PRIVATE
    -Wl,--whole-archive $<TARGET_FILE:bezel_runtime> -Wl,--no-whole-archive
    -Wl,--export-dynamic)

add_test(NAME bezel_tests COMMAND bezel_tests)

# Fast full-suite run: the single bezel_tests binary honours
# BEZEL_SHARD_COUNT / BEZEL_SHARD_INDEX (test_main.cpp) to run a disjoint subset,
# so run_parallel.sh launches one process per core and runs the whole suite in a
# fraction of the serial wall-clock (≈186s serial → ≈25s on 16 shards) while
# still executing every test. This is the blessed way to run the full suite for
# a merge gate:  cmake --build build --target check
add_custom_target(check
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel.sh
    DEPENDS bezel_tests
    USES_TERMINAL
    COMMENT "Running full test suite in parallel shards (run_parallel.sh)")
