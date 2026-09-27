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
# fix #3013: REFUSE AN UNPINNED BUILD TYPE.
#
# CMake leaves CMAKE_BUILD_TYPE empty by default, which means no -O2, no -g and
# — the part that matters — NO -DNDEBUG, so asserts are LIVE. Nothing passed the
# flag, so a bare `cmake -S . -B build` fell through to empty. Measured across
# every build tree on this machine on 2026-08-08:
#
#     55  CMAKE_BUILD_TYPE:STRING=            <- lanes
#      4  CMAKE_BUILD_TYPE:STRING=Debug
#      1  CMAKE_BUILD_TYPE:STRING=RelWithDebInfo   <- the gate
#
# So 55 of 59 lane build trees validated a SEMANTICALLY DIFFERENT bezelc than
# the gate accepts: asserts live in the lane, compiled out in the gate. A lane
# green and a gate green were not claims about the same program, and no existing
# check could see the split — the only symptom is a three-way CXX_FLAGS
# divergence nothing reads.
#
# DECISION (recorded per CLAUDE.md rule 4, since the spec does not settle it):
# converge the LANES UP to the gate's RelWithDebInfo, not the gate down to the
# lanes' cheap flags. Converging down was considered and declined — asserts-on /
# no-NDEBUG against Fedora's SHARED libLLVM, which is built without assertions,
# is an ABI question rather than a speed one, and it would change the oracle for
# all 16,482 tests to chase 7-12% on a component that is not the bottleneck.
#
# This is a FATAL_ERROR rather than a silent default on purpose: a default would
# fix the flags and hide the fact that a caller was not passing them, which is
# how the split survived unnoticed. Every configure site in this repo passes it.
if(NOT CMAKE_BUILD_TYPE)
    message(FATAL_ERROR
        "CMAKE_BUILD_TYPE is not set.\n"
        "An unpinned build type means no -O2, no -g and no -DNDEBUG, so asserts "
        "are live and this tree would compile a different bezelc than the gate "
        "validates (#3013).\n"
        "Configure with:  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo\n"
        "or use scripts/agent_check.sh / scripts/gate.sh, which pass it for you.")
endif()

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

# fix #3084: derive a build-time version string the driver prints on
# --version. The scaffold-era "0.0.0 (scaffold; no compilation implemented yet)"
# string survived long after the compiler became an AOT oracle; tooling and logs
# that record the version (the corpus README, gate telemetry) now say something
# false. `git describe --tags --long --dirty=-modified` is the canonical source:
# if the repo has tags it names the closest tag + commits-past + dirty suffix,
# and --long makes the "N commits past" part zero on a tagged commit. When the
# repo has NO tags (the case here, at the time of writing) --tags produces
# nothing and the call would fail; fall back to --tags --always so the output is
# at least the commit hash, and finally to a hard-coded constant so a build with
# no .git directory at all still produces a valid binary. Cache the result so
# the second execute_process call is skipped when the first succeeded.
execute_process(
    COMMAND git describe --tags --long --dirty=-modified
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE BEZEL_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE BEZEL_GIT_DESCRIBE_RC)
if(NOT BEZEL_GIT_DESCRIBE_RC EQUAL 0 OR NOT BEZEL_GIT_DESCRIBE)
    execute_process(
        COMMAND git describe --tags --always --long
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE BEZEL_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT BEZEL_GIT_DESCRIBE)
    set(BEZEL_GIT_DESCRIBE "0.0.0-unknown")
endif()
message(STATUS "Bezel: version ${BEZEL_GIT_DESCRIBE} (from git describe, fix #3084)")

# C++20 per CLAUDE.md's "Build toolchain" section.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit build/compile_commands.json at configure time. Required by the
# check-tidy static-analysis tier (tests/CMakeLists.txt) — clang-tidy needs a
# compilation database to know each TU's include paths and flags. Harmless
# otherwise: any clangd/IDE tooling picks it up too. Test-infra migration plan
# §2 / WS7 (see /var/tmp/bezel-scratch/impl-run-2026-07-12/aot-plan/ws7-static-analysis.md).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# --- Build accelerators (optional; used only if present) --------------------
# These speed up the heavy worktree-per-lane workflow without becoming hard
# dependencies: each is gated on find_program, so a plain checkout still builds.
#  * ccache: caches object compiles across builds AND across git worktrees
#    (shared ~/.ccache), so a fresh worktree's "clean" build is mostly cache
#    hits — the single biggest win for parallel lanes. Install with
#    `sudo dnf install ccache`; CMake picks it up automatically on reconfigure.
#    NOTE: LLVM-heavy objects are large, so the default 5G cache thrashes (it
#    evicts faster than it reuses). Raise it once with `ccache -M 40G` — this is
#    machine-local config (~/.config/ccache/ccache.conf), not tracked here.
#  * lld: a much faster linker than the default bfd/gold for the 60M test
#    binary, which is relinked on every change.
find_program(BEZEL_CCACHE ccache)
#  * #5010 RELOCATABLE ccache keys (cross-worktree hits). By default every
#    object's ccache key embeds its worktree's absolute path (the -c/-I
#    arguments, and the cwd via DWARF comp_dir), so a fresh worktree at the SAME
#    commit misses on every TU. When the build dir lives PHYSICALLY inside the
#    source tree (a real ./build, no symlink between them) we make the keys
#    tree-independent:
#      - ccache `base_dir=<tree>`, passed on the launcher command line
#        (`ccache KEY=VALUE compiler ...`), so it is per-build and never read
#        from or written to the user's global ccache.conf;
#      - `-ffile-prefix-map=<tree>=.` on every TU, so the OBJECT is
#        tree-independent too (DW_AT_comp_dir "./build", __FILE__ "./src/..."),
#        which is what makes serving tree A's object to tree B correct rather
#        than merely a hit (ccache logs "Relocating debuginfo from <tree> to .");
#      - per-tree absolute paths only in the TUs that need them (Driver.cpp for
#        the runtime archive; one generated TU for the test harness — see
#        tests/CMakeLists.txt), so those few TUs miss, correctly, and the rest
#        hit;
#      - `-Xclang -fno-pch-timestamp` on the test PCH (tests/CMakeLists.txt);
#      - the test PCH itself is NEVER served from ccache (#5073): a clang PCH
#        embeds its own tree's absolute input paths, so a PCH from tree A is
#        unusable once A is removed. It is compiled per tree, and its
#        consumers key on a content-derived, tree-independent `<pch>.sum`
#        (ccache option `pch_external_checksum=true`) instead of the PCH
#        bytes -- see tests/pch_sum_launcher.sh for the staleness argument.
#        NB it is a BOOLEAN OPTION, not a sloppiness value: ccache silently
#        ignores an unknown sloppiness word, which left every PCH'd TU
#        missing across trees. The launcher also passes
#        `sloppiness=pch_defines,time_macros`, which REPLACES the ccache.conf
#        value for this build (the two values CLAUDE.md requires there).
#    Measured (#4991, #5010): a PCH'd test TU 6-9 s cold -> ~1.4 s from the
#    cache of a sibling worktree.
#    WHY ONLY IN-TREE: base_dir rewrites paths relative to the compiler's
#    PHYSICAL cwd. With build/ a symlink to /mnt/fast/lane-builds/<name> the
#    rewritten paths still contain the lane name (no hit — #4991 measured 0/3),
#    and worse, the relative paths in the -MD depfiles are then joined by CMake
#    against the LOGICAL build dir, i.e. resolve to the wrong file: header edits
#    would stop triggering rebuilds. So the mode is OFF for that layout and
#    FATAL if forced on there.
#    DEBUGGING a relocatable build: debug paths are relative to the worktree
#    root. Start gdb from the worktree root (or `directory <tree>` /
#    `set substitute-path . <tree>`); addr2line/llvm-symbolizer/ASan print
#    `./build/../src/x.cpp:N`-style paths, which read correctly from the root.
set(_bezel_in_tree_build OFF)
file(RELATIVE_PATH _bezel_bin_rel "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
if(NOT _bezel_bin_rel STREQUAL "" AND NOT _bezel_bin_rel MATCHES "^\\.\\.(/|$)"
   AND NOT IS_ABSOLUTE "${_bezel_bin_rel}")
    file(REAL_PATH "${CMAKE_SOURCE_DIR}" _bezel_src_real)
    file(REAL_PATH "${CMAKE_BINARY_DIR}" _bezel_bin_real)
    # No symlink anywhere between the tree root and the build dir: the
    # physical and logical `..` chains then agree (see WHY ONLY IN-TREE).
    if(_bezel_bin_real STREQUAL "${_bezel_src_real}/${_bezel_bin_rel}")
        set(_bezel_in_tree_build ON)
    endif()
endif()
option(BEZEL_CCACHE_RELOCATABLE
    "Tree-independent ccache keys (#5010); default ON for an in-tree build dir"
    ${_bezel_in_tree_build})
if(BEZEL_CCACHE)
    if(BEZEL_CCACHE_RELOCATABLE AND NOT _bezel_in_tree_build)
        # A cached ON from an earlier layout must not survive a move to a
        # symlinked/out-of-tree build dir: that combination mis-resolves
        # depfiles (see WHY ONLY IN-TREE above).
        message(FATAL_ERROR
            "BEZEL_CCACHE_RELOCATABLE=ON requires the build dir to be a real "
            "directory inside the source tree (${CMAKE_BINARY_DIR} is not). "
            "Reconfigure with -DBEZEL_CCACHE_RELOCATABLE=OFF.")
    endif()
    if(BEZEL_CCACHE_RELOCATABLE)
        set(BEZEL_CCACHE_RELOCATABLE_LAUNCHER
            "${BEZEL_CCACHE}" "base_dir=${CMAKE_SOURCE_DIR}"
            "sloppiness=pch_defines,time_macros" "pch_external_checksum=true")
        # #5073: pch_external_checksum is what lets PCH'd test TUs hit across
        # trees. An old ccache without the option would ignore it (correct,
        # but every PCH consumer misses), so say so instead of degrading
        # silently.
        execute_process(COMMAND "${BEZEL_CCACHE}" -k pch_external_checksum
            RESULT_VARIABLE _bezel_ccache_extsum_rc OUTPUT_QUIET ERROR_QUIET)
        if(NOT _bezel_ccache_extsum_rc EQUAL 0)
            message(WARNING "ccache (${BEZEL_CCACHE}) does not know "
                "pch_external_checksum: test TUs using the PCH will not hit "
                "the cache across worktrees (#5073).")
        endif()
        # The real path of the tree, for tests/pch_sum_launcher.sh (#5073).
        set(BEZEL_SOURCE_DIR_REAL "${_bezel_src_real}")
        set(CMAKE_CXX_COMPILER_LAUNCHER ${BEZEL_CCACHE_RELOCATABLE_LAUNCHER})
        set(CMAKE_C_COMPILER_LAUNCHER ${BEZEL_CCACHE_RELOCATABLE_LAUNCHER})
        add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
        if(NOT _bezel_src_real STREQUAL CMAKE_SOURCE_DIR)
            # The tree reached through a symlinked ancestor (e.g. a worktrees
            # dir that points at the NVMe): clang may see either spelling.
            add_compile_options("-ffile-prefix-map=${_bezel_src_real}=.")
        endif()
        message(STATUS "Bezel: using ccache (${BEZEL_CCACHE}), relocatable "
                       "keys (base_dir + prefix map, #5010)")
    else()
        set(CMAKE_CXX_COMPILER_LAUNCHER "${BEZEL_CCACHE}")
        set(CMAKE_C_COMPILER_LAUNCHER "${BEZEL_CCACHE}")
        message(STATUS "Bezel: using ccache (${BEZEL_CCACHE})")
    endif()
elseif(BEZEL_CCACHE_RELOCATABLE)
    set(BEZEL_CCACHE_RELOCATABLE OFF CACHE BOOL "" FORCE)
endif()
#    mold is preferred when available (faster than lld on the large test-binary
#    link and on incremental relinks); lld is the fallback, then the default.
find_program(BEZEL_MOLD ld.mold)
find_program(BEZEL_LLD ld.lld)
if(BEZEL_MOLD)
    add_link_options(-fuse-ld=mold)
    message(STATUS "Bezel: using mold linker")
elseif(BEZEL_LLD)
    add_link_options(-fuse-ld=lld)
    message(STATUS "Bezel: using lld linker")
endif()
#  * split DWARF: the RelWithDebInfo build carries full `-g` debug info, and
#    everything links statically into one 300M+ test binary that is relinked on
#    every change. With -gsplit-dwarf the debug info goes into separate .dwo
#    files the linker does NOT touch, so links are far faster and the binary far
#    smaller, with no loss of debuggability (gdb reads the .dwo in place). Gated
#    on a debug-info build type and clang/gcc support, like the accelerators
#    above; a plain Release build is unaffected.
if(CMAKE_BUILD_TYPE MATCHES "Deb" AND
   (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    add_compile_options(-gsplit-dwarf)
    message(STATUS "Bezel: using split DWARF (-gsplit-dwarf)")
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
# Prefer the single monolithic shared library (libLLVM-NN.so) over the static
# component archives when LLVM is built shared (Fedora's llvmNN is): linking one
# .so instead of pulling+deduping ~hundreds of MB of static archives makes the
# test/compiler binaries an order of magnitude smaller and the link far faster
# (and incremental relinks near-instant). The in-process LLJIT is unaffected —
# codegen and the JIT share the one libLLVM, and the JIT still resolves the
# Bezel `bz_*` runtime symbols via the test binary's own --whole-archive +
# --export-dynamic. Falls back to the static components if no shared build.
if(LLVM_LINK_LLVM_DYLIB OR TARGET LLVM)
    set(BEZEL_LLVM_LIBS LLVM)
    message(STATUS "Bezel: linking shared libLLVM (dylib)")
else()
    llvm_map_components_to_libnames(BEZEL_LLVM_LIBS
        core orcjit native nativecodegen target codegen mc
        # object: the cross-module import loader (§2.5, issue #328 inc 3) reads a
        # dependency's embedded `.bezel.meta` section via the LLVM Object API.
        object)
    message(STATUS "Bezel: linking static LLVM components")
endif()
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
add_library(bezel_runtime STATIC
    src/runtime/bezel_runtime_collections.cpp
    src/runtime/bezel_runtime_decimal_guid.cpp
    src/runtime/bezel_runtime_time.cpp
    src/runtime/bezel_runtime_fileio.cpp
    src/runtime/bezel_runtime_shared_weak.cpp
    src/runtime/bezel_runtime_concurrency.cpp
    src/runtime/bezel_runtime_events.cpp
    src/runtime/bezel_runtime_strings.cpp
    src/runtime/bezel_runtime_encoding.cpp
    src/runtime/bezel_runtime_refcounting.cpp
    src/runtime/bezel_runtime_itable.cpp
    src/runtime/bezel_runtime_serialization.cpp
    src/runtime/bezel_runtime_sync.cpp)
target_include_directories(bezel_runtime PUBLIC src/runtime)
# bezel_runtime uses POSIX threads; suppress LLVM-specific warnings.
target_compile_options(bezel_runtime PRIVATE -std=c++20 -pthread)
target_link_libraries(bezel_runtime PRIVATE pthread)

add_library(bezel_core STATIC
    src/common/AwaitScan.cpp
    src/common/BuiltinRegistry.cpp
    src/common/OwnershipFacts.cpp
    src/support/Diagnostic.cpp
    src/lexer/Lexer.cpp
    src/parser/Parser.cpp
    src/parser/Parser_Decls.cpp
    src/parser/Parser_Stmts.cpp
    src/parser/Parser_Exprs.cpp
    src/sema/TypeChecker.cpp
    src/sema/ConstFold.cpp
    src/sema/SymbolCollection.cpp
    src/sema/TypeResolution.cpp
    src/sema/TypeResolution_Names.cpp
    src/sema/TypeResolution_Checks.cpp
    src/sema/ExprTypecheck.cpp
    src/sema/SubstArgs.cpp
    src/sema/ExprTypecheck_Exprs.cpp
    src/sema/ExprTypecheck_Calls.cpp
    src/sema/DefiniteAssignment.cpp
    src/sema/StaticInitCycles.cpp
    src/sema/Sendability.cpp
    src/sema/EntryPoint.cpp
    src/sema/PureChecker.cpp
    src/flow/CFG.cpp
    src/ownership/OwnershipChecker.cpp
    src/ownership/Ownership.cpp
    src/ownership/Ownership_Checks.cpp
    src/codegen/CodeGen.cpp
    src/codegen/LlvmBackend.cpp
    src/codegen/LlvmBackend_Setup.cpp
    src/codegen/LlvmBackend_Disposal.cpp
    src/codegen/LlvmBackend_ElementPolicy.cpp
    src/codegen/LlvmBackend_String.cpp
    src/codegen/LlvmBackend_Layout.cpp
    src/codegen/LlvmBackend_Assign.cpp
    src/codegen/LlvmBackend_Iterators.cpp
    src/codegen/LlvmBackend_Async.cpp
    src/codegen/LlvmBackend_Stmts.cpp
    src/codegen/LlvmBackend_Exprs.cpp
    src/codegen/LlvmBackend_Exprs2.cpp
    src/codegen/LlvmBackend_Calls.cpp
    src/codegen/LlvmBackend_Calls2.cpp
    src/codegen/LlvmBackend_Calls3.cpp
    src/codegen/LlvmBackend_Serialization.cpp
    src/module/ModuleMetadata.cpp
    src/module/ModuleMetadataBuilder.cpp
    src/module/ModuleLoader.cpp
    src/driver/Driver.cpp
    src/driver/ScopedAwaitBind.cpp)
target_include_directories(bezel_core PUBLIC src)
# LLVM headers as SYSTEM so their warnings don't trip our -Wall/-Werror.
target_include_directories(bezel_core SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_options(bezel_core PUBLIC ${BEZEL_LLVM_DEFS})
target_link_libraries(bezel_core PRIVATE bezel_warnings)
target_link_libraries(bezel_core PUBLIC ${BEZEL_LLVM_LIBS})

# --- Compiler driver executable --------------------------------------------
add_executable(bezelc src/main.cpp)
target_link_libraries(bezelc PRIVATE bezel_core bezel_runtime bezel_warnings)
# fix #3084: inject the build-time version into the driver so --version prints
# a real string (git describe output, falling back to "0.0.0-unknown") instead
# of the scaffold-era constant. The string is a configure-time substitution, so
# a rebuild after a commit produces a different version with no code change.
target_compile_definitions(bezelc PRIVATE
    BEZEL_VERSION="${BEZEL_GIT_DESCRIBE}")
# Teach the driver the path to libbezel_runtime.a so it can pass it to clang
# when compiling standalone Bezel programs (generator expression → literal path).
# NOTE: Driver.cpp is compiled as part of bezel_core, so the definition must
# be added to bezel_core's Driver.cpp (not bezelc which only compiles main.cpp).
# #5010: scoped to the ONE source that reads it, not the whole target — a
# per-worktree absolute path on the command line is part of the ccache key, so
# on all 52 bezel_core TUs it made every one of them miss in a fresh worktree.
set_source_files_properties(src/driver/Driver.cpp PROPERTIES
    COMPILE_DEFINITIONS "BEZEL_RUNTIME_LIB_PATH=\"$<TARGET_FILE:bezel_runtime>\"")

# --- Tests ------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
