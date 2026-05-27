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
    LANGUAGES CXX)

# C++20 per CLAUDE.md's "Build toolchain" section.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Treating warnings as errors keeps the scaffold honest as it grows; off by
# default so a stray warning never blocks local work, on in CI.
option(BEZEL_WERROR "Treat compiler warnings as errors" OFF)

add_library(bezel_warnings INTERFACE)
target_compile_options(bezel_warnings INTERFACE -Wall -Wextra -Wpedantic)
if(BEZEL_WERROR)
    target_compile_options(bezel_warnings INTERFACE -Werror)
endif()

# --- Core library: every compilation phase ---------------------------------
# One static library aggregates all phases. Phase boundaries are expressed as
# directories (and namespaces), not separate packages; see CLAUDE.md and the
# "phase-aligned skeleton" decision. Sources are listed explicitly (no globs)
# so that adding a file is always a reviewable diff.
add_library(bezel_core STATIC
    src/support/Diagnostic.cpp
    src/lexer/Lexer.cpp
    src/parser/Parser.cpp
    src/sema/TypeChecker.cpp
    src/ownership/OwnershipChecker.cpp
    src/codegen/CodeGen.cpp
    src/driver/Driver.cpp)
target_include_directories(bezel_core PUBLIC src)
target_link_libraries(bezel_core PRIVATE bezel_warnings)

# --- Compiler driver executable --------------------------------------------
add_executable(bezelc src/main.cpp)
target_link_libraries(bezelc PRIVATE bezel_core bezel_warnings)

# --- Tests ------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
