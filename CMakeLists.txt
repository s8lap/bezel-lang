# Bezel compiler — top-level build configuration.
#
# The compiler is split into one directory per compilation phase under src/,
# aligned to the language spec (docs/Bezel_Language_Spec_*.md). See README.md
# for the phase -> spec-section map.

cmake_minimum_required(VERSION 3.20)

# Build toolchain: clang / clang++, per CLAUDE.md's "Build toolchain" section.
# The compiler is set here — before project(), where CMake runs compiler
# detection — so a plain `cmake -B build` uses clang without a separate
# toolchain file. The guards leave any -DCMAKE_*_COMPILER=... override in place,
# so the build stays configurable for environments where clang lives elsewhere.
if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang CACHE FILEPATH "C compiler")
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++ CACHE FILEPATH "C++ compiler")
endif()

project(bezel
    VERSION 0.0.0
    DESCRIPTION "The Bezel programming language compiler"
    # C is enabled (alongside CXX) because LLVM's package config runs C
    # feature checks (FindFFI / FindLibEdit) during find_package(LLVM).
    LANGUAGES C CXX)

# C++20 per CLAUDE.md's "Build toolchain" section.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Treating warnings as errors keeps the scaffold honest as it grows; off by
# default so a stray warning never blocks local work, on in CI.
option(BEZEL_WERROR "Treat compiler warnings as errors" OFF)

add_library(bezel_warnings INTERFACE)
target_compile_options(bezel_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    # Trigraphs were removed in C++17 (we build at C++20). Clang still warns
    # on `??=` etc. as a portability hint, which is moot here and would force
    # `"\?\?="`-style escapes throughout the lexer's punctuator match tables.
    -Wno-trigraphs)
if(BEZEL_WERROR)
    target_compile_options(bezel_warnings INTERFACE -Werror)
endif()

# --- LLVM (codegen backend) -------------------------------------------------
# Decision #147: the back-end targets LLVM IR via the C++ API. LLVMConfig.cmake
# lives under a versioned prefix on Fedora (/usr/lib64/llvmNN/...), so we ask
# llvm-config where it is rather than hardcoding a path; an explicit
# -DLLVM_DIR=... override still wins.
execute_process(COMMAND llvm-config --cmakedir
    OUTPUT_VARIABLE BEZEL_LLVM_CMAKEDIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
find_package(LLVM REQUIRED CONFIG HINTS "${BEZEL_LLVM_CMAKEDIR}")
message(STATUS "Bezel: using LLVM ${LLVM_PACKAGE_VERSION} (cmake dir ${LLVM_DIR})")
# Components needed: IR construction (core), the ORC JIT used by codegen tests,
# and the native target (with its codegen/asm-printer) for JIT execution and
# eventual object emission.
llvm_map_components_to_libnames(BEZEL_LLVM_LIBS
    core orcjit native nativecodegen target codegen mc)
# LLVM_DEFINITIONS is a space-separated flag string (e.g. -D_GNU_SOURCE …);
# pass it as options, not as compile-definitions (which would re-prefix -D).
separate_arguments(BEZEL_LLVM_DEFS NATIVE_COMMAND "${LLVM_DEFINITIONS}")

# --- Core library: every compilation phase ---------------------------------
# One static library aggregates all phases. Phase boundaries are expressed as
# directories (and namespaces), not separate packages; see CLAUDE.md and the
# "phase-aligned skeleton" decision. Sources are listed explicitly (no globs)
# so that adding a file is always a reviewable diff.
# --- Bezel runtime library -------------------------------------------------
# A static C++ library that implements the Bezel standard library's native
# layer: Array<T>, Dictionary<K,V>, File I/O, DateTime, Random, Task scheduler.
# Linked into the compiler driver (for standalone executables) and available
# to the JIT test harness via DynamicLibrarySearchGenerator.
add_library(bezel_runtime STATIC src/runtime/bezel_runtime.cpp)
target_include_directories(bezel_runtime PUBLIC src/runtime)
# bezel_runtime uses POSIX threads; suppress LLVM-specific warnings.
target_compile_options(bezel_runtime PRIVATE -std=c++20 -pthread)
target_link_libraries(bezel_runtime PRIVATE pthread)

add_library(bezel_core STATIC
    src/support/Diagnostic.cpp
    src/lexer/Lexer.cpp
    src/parser/Parser.cpp
    src/parser/Parser_Decls.cpp
    src/parser/Parser_Stmts.cpp
    src/parser/Parser_Exprs.cpp
    src/sema/TypeChecker.cpp
    src/sema/SymbolCollection.cpp
    src/sema/TypeResolution.cpp
    src/sema/ExprTypecheck.cpp
    src/sema/DefiniteAssignment.cpp
    src/flow/CFG.cpp
    src/ownership/OwnershipChecker.cpp
    src/ownership/Ownership.cpp
    src/codegen/CodeGen.cpp
    src/codegen/LlvmBackend.cpp
    src/codegen/LlvmBackend_Setup.cpp
    src/codegen/LlvmBackend_String.cpp
    src/codegen/LlvmBackend_Layout.cpp
    src/codegen/LlvmBackend_Assign.cpp
    src/codegen/LlvmBackend_Iterators.cpp
    src/codegen/LlvmBackend_Stmts.cpp
    src/codegen/LlvmBackend_Exprs.cpp
    src/codegen/LlvmBackend_Calls.cpp
    src/driver/Driver.cpp)
target_include_directories(bezel_core PUBLIC src)
# LLVM headers as SYSTEM so their warnings don't trip our -Wall/-Werror.
target_include_directories(bezel_core SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_options(bezel_core PUBLIC ${BEZEL_LLVM_DEFS})
target_link_libraries(bezel_core PRIVATE bezel_warnings)
target_link_libraries(bezel_core PUBLIC ${BEZEL_LLVM_LIBS})

# --- Compiler driver executable --------------------------------------------
add_executable(bezelc src/main.cpp)
target_link_libraries(bezelc PRIVATE bezel_core bezel_runtime bezel_warnings)
# Teach the driver the path to libbezel_runtime.a so it can pass it to clang
# when compiling standalone Bezel programs (generator expression → literal path).
# NOTE: Driver.cpp is compiled as part of bezel_core, so the definition must
# be added to bezel_core (not bezelc which only compiles main.cpp).
target_compile_definitions(bezel_core PRIVATE
    BEZEL_RUNTIME_LIB_PATH="$<TARGET_FILE:bezel_runtime>")

# --- Tests ------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
