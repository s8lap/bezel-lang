# Per-area test binaries.
#
# For a long time every test *.cpp linked into ONE `bezel_tests` executable. At
# ~1.2k test TUs that binary reached ~370 MB and every change relinked all of it
# — in the main checkout and in every lane worktree, on the critical path of
# every scripts/agent_check.sh iteration and every gate. The suite is now split
# along the EXISTING tests/<area>/ directory structure into several executables
# (bezel_tests_<group>), so a test-only edit relinks only its own area and a
# lane can build and run just the area it works in
# (`scripts/agent_check.sh --area sema …`).
#
# `bezel_tests` SURVIVES as an aggregate target: `cmake --build build --target
# bezel_tests` still builds them all, so every caller that names the TARGET is
# unchanged. Callers that need a PATH read the generated MANIFEST instead:
#
#     build/tests/test_binaries.txt   one line per binary, largest first:
#                                     <group> <TAB> <abs path> <TAB> <#sources>
#
# The manifest is the single source of truth every runner (tests/run_parallel.sh,
# scripts/gate.sh, scripts/agent_check.sh, scripts/asan_sweep.sh,
# scripts/run-tests.sh) reads, so the set of binaries a runner accounts for is
# generated from the very list CMake built — a binary can never be silently
# omitted from a run.
#
# SAFETY. This suite is the project's authoritative correctness oracle
# (scripts/gate.sh), so the union of the per-area binaries must be EXACTLY the
# old single binary's test set. Two configure-time invariants enforce that, and
# both are FATAL_ERROR (never a warning — a warning scrolls past in a build log
# and the gate then goes green on a silently smaller suite):
#
#   (1) PARTITION COMPLETENESS — the per-group source lists are compared against
#       one flat glob of every *.cpp under the claimed directories; a source
#       assigned to no group, or to two, is a hard configure error. A test file
#       can therefore never silently stop being built, and no test can run twice.
#   (2) NO UNCLAIMED AREA — any tests/<dir>/ holding a *.cpp that no group claims
#       is a hard configure error. (Before the split such a directory was
#       silently invisible: the old glob named its directories one by one, so a
#       brand-new tests/<area>/ simply never got built.)
#
# To EXCLUDE a file, rename it *.cpp.disabled (self-documenting on disk) — see
# codegen/iterator_test.cpp.disabled (out of the suite for LLVM module state
# accumulation, commit 0112853; iterator_native_test.cpp supersedes it).
# CONFIGURE_DEPENDS is reliable under the Ninja preset.

# ── the split ───────────────────────────────────────────────────────────────
# One group per tests/ directory, except that directories too small to be worth
# their own copy of the compiler archive are folded into a neighbour: every test
# binary statically links libbezel_core.a (every test drives the real
# Driver::compileFile), so a group costs ~47 MB of link output whatever its size
# — that is the measured size of `bezelc`, which is exactly core+runtime and
# nothing else. Groups are listed LARGEST FIRST; that is also the order the
# runner launches them in, and longest-job-first keeps the tail short.
#
#   codegen    tests/codegen                  ~70% of the suite; on its own
#   ownership  tests/ownership
#   sema       tests/sema + tests/flow        flow/ is 3 CFG tests
#   frontend   tests/lexer + tests/parser
#   module     tests/module
#   support    tests/runtime + tests/support + the root-level tests/*.cpp
#
# tests/lit/ is NOT a group: it is the lit+FileCheck track and has no C++ TUs.
set(BEZEL_TEST_GROUPS codegen ownership sema frontend module support)
set(BEZEL_TEST_GROUP_codegen_DIRS   codegen)
set(BEZEL_TEST_GROUP_ownership_DIRS ownership)
set(BEZEL_TEST_GROUP_sema_DIRS      sema flow)
set(BEZEL_TEST_GROUP_frontend_DIRS  lexer parser)
set(BEZEL_TEST_GROUP_module_DIRS    module)
set(BEZEL_TEST_GROUP_support_DIRS   runtime support)
# The root-level tests/*.cpp other than the shared entry point join this group.
set(BEZEL_TEST_ROOT_GROUP support)
# The shared entry point (BEZEL_SHARD_* / BEZEL_TEST_FILTER / BEZEL_TEST_LIST /
# BEZEL_TEST_ENUMERATE) compiles into EVERY binary; it is not part of any
# group's partition.
set(BEZEL_TEST_ENTRY ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp)

# ── discovery ───────────────────────────────────────────────────────────────
set(BEZEL_TEST_CLAIMED_DIRS "")
foreach(g IN LISTS BEZEL_TEST_GROUPS)
    list(APPEND BEZEL_TEST_CLAIMED_DIRS ${BEZEL_TEST_GROUP_${g}_DIRS})
endforeach()

# Invariant (2): no unclaimed area.
file(GLOB _bezel_test_entries RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(_d IN LISTS _bezel_test_entries)
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${_d})
        file(GLOB _d_srcs CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/${_d}/*.cpp)
        if(_d_srcs AND NOT _d IN_LIST BEZEL_TEST_CLAIMED_DIRS)
            message(FATAL_ERROR
                "Bezel: tests/${_d}/ contains test sources but no group in "
                "BEZEL_TEST_GROUPS claims it — those tests would NOT be built "
                "and the suite would silently shrink. Add '${_d}' to a "
                "BEZEL_TEST_GROUP_<g>_DIRS list in tests/CMakeLists.txt.")
        endif()
    endif()
endforeach()

# The flat "everything" list invariant (1) is checked against.
set(_bezel_all_globs ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
foreach(_d IN LISTS BEZEL_TEST_CLAIMED_DIRS)
    list(APPEND _bezel_all_globs ${CMAKE_CURRENT_SOURCE_DIR}/${_d}/*.cpp)
endforeach()
file(GLOB BEZEL_TEST_ALL_SOURCES CONFIGURE_DEPENDS ${_bezel_all_globs})

# Root-level tests (everything but the shared entry point).
file(GLOB BEZEL_TEST_ROOT_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(REMOVE_ITEM BEZEL_TEST_ROOT_SOURCES ${BEZEL_TEST_ENTRY})

# Per-group source lists.
set(_bezel_assigned "")
foreach(g IN LISTS BEZEL_TEST_GROUPS)
    set(_g_globs "")
    foreach(_d IN LISTS BEZEL_TEST_GROUP_${g}_DIRS)
        list(APPEND _g_globs ${CMAKE_CURRENT_SOURCE_DIR}/${_d}/*.cpp)
    endforeach()
    file(GLOB BEZEL_TEST_SOURCES_${g} CONFIGURE_DEPENDS ${_g_globs})
    if(g STREQUAL BEZEL_TEST_ROOT_GROUP)
        list(APPEND BEZEL_TEST_SOURCES_${g} ${BEZEL_TEST_ROOT_SOURCES})
    endif()
    list(APPEND _bezel_assigned ${BEZEL_TEST_SOURCES_${g}})
endforeach()

# Invariant (1): partition completeness — no source assigned twice, and the
# union (plus the shared entry point) is exactly the flat glob.
set(_bezel_dedup ${_bezel_assigned})
list(REMOVE_DUPLICATES _bezel_dedup)
list(LENGTH _bezel_assigned _n_assigned)
list(LENGTH _bezel_dedup _n_dedup)
if(NOT _n_assigned EQUAL _n_dedup)
    message(FATAL_ERROR
        "Bezel: a test source is claimed by TWO groups (${_n_assigned} "
        "assignments, ${_n_dedup} distinct sources) — it would run twice and "
        "the shard accounting would double-count. Fix the "
        "BEZEL_TEST_GROUP_<g>_DIRS lists in tests/CMakeLists.txt.")
endif()
set(_bezel_union ${_bezel_assigned} ${BEZEL_TEST_ENTRY})
list(SORT _bezel_union)
set(_bezel_expected ${BEZEL_TEST_ALL_SOURCES})
list(SORT _bezel_expected)
if(NOT _bezel_union STREQUAL _bezel_expected)
    set(_missing ${_bezel_expected})
    list(REMOVE_ITEM _missing ${_bezel_union})
    message(FATAL_ERROR
        "Bezel: the per-group test partition does NOT cover every discovered "
        "test source — these would silently stop being built: ${_missing}")
endif()
list(LENGTH BEZEL_TEST_ALL_SOURCES BEZEL_TEST_SOURCE_COUNT)
message(STATUS
    "Bezel: test suite auto-discovered ${BEZEL_TEST_SOURCE_COUNT} sources "
    "(${_n_dedup} partitioned across ${BEZEL_TEST_GROUPS} + 1 shared entry point)")

# ── one executable per group ────────────────────────────────────────────────
# Everything below is identical for every group; only the source list differs.
# It lives in a function so the targets cannot drift apart — a group compiled
# with different flags would be a different oracle.
set(BEZEL_TEST_TARGETS "")
set(BEZEL_TEST_PCH_OWNER "")
function(bezel_add_test_binary group)
    set(tgt bezel_tests_${group})
    add_executable(${tgt} ${BEZEL_TEST_ENTRY} ${BEZEL_TEST_SOURCES_${group}})
    target_include_directories(${tgt} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src)
    # Precompiled header: the LLVM (+ a few ubiquitous std) headers dominate the
    # ~209K preprocessed lines every test TU re-parses, and they are identical
    # across all of them. Precompiling them once cuts the per-TU compile by
    # ~40%. Only the HEAVY, STABLE headers are listed — deliberately NOT the
    # bezel sema/codegen headers, which change often: putting a frequently-edited
    # header in the PCH would invalidate it (and force a full test rebuild) on
    # every such edit. ONE PCH is built (by the first group) and REUSE_FROM'd by
    # the rest: per-target PCHs would mean N identical PCH compiles and, worse, a
    # different `-include-pch` path per target — which is part of the ccache key.
    # NOTE (machine-local, not tracked): ccache needs `sloppiness =
    # pch_defines,time_macros` (~/.config/ccache/ccache.conf) to cache PCH
    # builds — see the build-accelerators note in CLAUDE.md.
    if(BEZEL_TEST_PCH_OWNER)
        target_precompile_headers(${tgt} REUSE_FROM ${BEZEL_TEST_PCH_OWNER})
    else()
        target_precompile_headers(${tgt} PRIVATE
            <cstddef> <cstdlib> <functional> <memory>
            <string> <string_view> <utility> <vector>
            <llvm/IR/LLVMContext.h>
            <llvm/IR/Module.h>
            <llvm/ExecutionEngine/Orc/LLJIT.h>
            <llvm/ExecutionEngine/Orc/ExecutionUtils.h>)
        set(BEZEL_TEST_PCH_OWNER ${tgt} PARENT_SCOPE)
    endif()
    target_link_libraries(${tgt} PRIVATE bezel_core bezel_warnings)
    # Teach the AOT harness's content-addressed link-cache
    # (support/aot_support.hpp) the path to libbezel_runtime.a, so it can fold
    # the runtime archive's identity into the cache key (constraint (b)/(d)): a
    # runtime rebuilt WITHOUT relinking the test binaries must invalidate cached
    # link products. Mirrors the same definition on bezel_core (which is where
    # Driver.cpp reads it to drive the actual link). BEZEL_CORE_LIB_PATH is the
    # k2 compiler-identity refinement (#2100): the cache keys on the COMPILER
    # archive, not on the test binary — so a test-only relink no longer
    # invalidates the cache, AND every per-area binary shares one cache (they
    # all embed the same compiler). Soundness guard + fallback live in
    # aot_support.hpp combinedIdentity(); its STALE-CORE guard already handles
    # the new "only one area was relinked against a newer core.a" case by
    # falling back to the conservative whole-binary identity.
    target_compile_definitions(${tgt} PRIVATE
        BEZEL_RUNTIME_LIB_PATH="$<TARGET_FILE:bezel_runtime>"
        BEZEL_CORE_LIB_PATH="$<TARGET_FILE:bezel_core>"
        # fix #3084: the bezelc --version regression test (tests/support/
        # bezelc_version_test.cpp) needs to exec the driver binary to check
        # the output. Same generator expression as the lit track uses; not on
        # a separate target so a test-only build misses nothing.
        BEZELC_PATH="$<TARGET_FILE:bezelc>"
        # The ABSOLUTE path to src/runtime, for native_test_harness.hpp's
        # runtimeSourceDir(). It used to derive that path from the header's own
        # __FILE__, which is only absolute when the compiler is invoked with an
        # absolute -I -- and a path-REWRITING compiler wrapper breaks exactly
        # that assumption. ccache's `base_dir` rewrites every absolute path
        # under it into one relative to the compiler's cwd (the CMake build
        # dir) and passes the REWRITTEN argv to the real compiler, so __FILE__
        # became `../../../../../home/.../tests/native_test_harness.hpp`. That
        # resolves from the build dir and nowhere else; gate.sh runs the test
        # binaries from the WORKTREE ROOT (`cd $(git rev-parse --show-toplevel)`,
        # gate.sh:71), where it does not resolve at all -- so
        # runtimeSourceFiles() came back empty, asanRuntimeObject() returned "",
        # and the ASan leg of 123 test files silently degraded (a handful FAILED
        # loudly; the rest treat "" as "toolchain missing -> SKIP" and passed
        # VACUOUSLY). A -D value is not a path argument, so no wrapper rewrites
        # it; this is the only channel that is absolute by construction.
        BEZEL_RUNTIME_SRC_DIR="${CMAKE_SOURCE_DIR}/src/runtime")
    # Force all bezel_runtime object files in, then export-dynamic so a
    # process-symbol-table lookup resolves bz_array_*, bz_dict_*, etc.
    target_link_options(${tgt} PRIVATE
        -Wl,--whole-archive $<TARGET_FILE:bezel_runtime> -Wl,--no-whole-archive
        -Wl,--export-dynamic)
    # $<TARGET_FILE:...> in link OPTIONS creates no dependency edge (unlike
    # target_link_libraries), so a fresh tree building one test target alone
    # never builds the archive; make the edge explicit.
    add_dependencies(${tgt} bezel_runtime)
    add_test(NAME ${tgt} COMMAND ${tgt})
    set(BEZEL_TEST_TARGETS ${BEZEL_TEST_TARGETS} ${tgt} PARENT_SCOPE)
endfunction()

foreach(g IN LISTS BEZEL_TEST_GROUPS)
    bezel_add_test_binary(${g})
endforeach()

# Aggregate target: `--target bezel_tests` keeps meaning "build the whole
# suite", so every existing caller (CMakePresets.json, scripts/gate.sh,
# scripts/agent_check.sh, scripts/asan_sweep.sh, scripts/run-tests.sh) is
# unchanged. It is a custom target, NOT a file — anything that needs a path
# reads the manifest below.
add_custom_target(bezel_tests)
add_dependencies(bezel_tests ${BEZEL_TEST_TARGETS})

# ── the manifest ────────────────────────────────────────────────────────────
# Generated, never hand-maintained: <group> <TAB> <path> <TAB> <#sources>, in
# BEZEL_TEST_GROUPS order (largest first). file(GENERATE) resolves the
# $<TARGET_FILE:...> generator expressions to concrete paths at generate time.
# The source count is only the runner's launch-ordering weight — the
# drop-detection accounting keys on each binary's OWN reported registry total,
# never on this number.
set(_bezel_manifest "")
foreach(g IN LISTS BEZEL_TEST_GROUPS)
    list(LENGTH BEZEL_TEST_SOURCES_${g} _n)
    string(APPEND _bezel_manifest
           "${g}\t$<TARGET_FILE:bezel_tests_${g}>\t${_n}\n")
endforeach()
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/test_binaries.txt
     CONTENT "${_bezel_manifest}")

# Fast full-suite run: each binary honours BEZEL_SHARD_COUNT / BEZEL_SHARD_INDEX
# (test_main.cpp) to run a disjoint round-robin subset of ITS OWN registry, so
# run_parallel.sh slices every binary and feeds the slices through one bounded
# queue — the whole suite in a fraction of the serial wall-clock while still
# executing every test. This is the blessed way to run the full suite for a
# merge gate:  cmake --build build --target check
add_custom_target(check
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel.sh
    DEPENDS bezel_tests
    USES_TERMINAL
    COMMENT "Running full test suite in parallel shards (run_parallel.sh)")

# ── lit + FileCheck diagnostic test track (tests/lit/) ───────────────────────
# ADDITIVE and COMPLEMENTARY to the gtest suite above: `lit` drives the real
# `bezelc` binary over the `.bzl` files in tests/lit/ and matches diagnostics
# with LLVM's FileCheck. See tests/lit/README.md for the rationale (per-process
# isolation; the full pipeline always runs, so an ownership-pass reject can't be
# missed by a sema-only harness; no per-test C++ compile).
#
# This is NOT part of the default build (`all`) and is a SEPARATE target.
# `lit` is a dev-time-only pip dependency and FileCheck comes from the system
# LLVM; if EITHER is missing the target is simply skipped with a status message
# — the ordinary build never depends on it and never breaks because of it.
find_program(BEZEL_LIT NAMES lit lit.py llvm-lit)
find_program(BEZEL_FILECHECK NAMES FileCheck FileCheck-21
    HINTS ${LLVM_TOOLS_BINARY_DIR})
# LLVM's `not` (exit-status inverter) is required by the reject tests — see
# tests/lit/lit.cfg.py for why. It ships alongside FileCheck.
find_program(BEZEL_NOT NAMES not
    HINTS ${LLVM_TOOLS_BINARY_DIR})
if(BEZEL_LIT AND BEZEL_FILECHECK AND BEZEL_NOT)
    # Values fed into lit.site.cfg.py.in. BEZELC_PATH is a generator expression
    # (resolved at build/generate time, not configure time); the others are
    # plain paths known at configure time.
    set(BEZELC_PATH "$<TARGET_FILE:bezelc>")
    set(FILECHECK_PATH "${BEZEL_FILECHECK}")
    set(NOT_PATH "${BEZEL_NOT}")
    set(LIT_MAIN_CFG "${CMAKE_CURRENT_SOURCE_DIR}/lit/lit.cfg.py")
    set(LIT_EXEC_ROOT "${CMAKE_CURRENT_BINARY_DIR}/lit")
    # Two-phase generation (mirrors LLVM's configure_lit_site_cfg): configure_file
    # substitutes the @VAR@ text but leaves the $<TARGET_FILE:bezelc> genex
    # verbatim; file(GENERATE) then resolves that genex into a concrete path.
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/lit/lit.site.cfg.py.in
        ${CMAKE_CURRENT_BINARY_DIR}/lit/lit.site.cfg.py.gen
        @ONLY)
    file(GENERATE
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lit/lit.site.cfg.py
        INPUT ${CMAKE_CURRENT_BINARY_DIR}/lit/lit.site.cfg.py.gen)
    add_custom_target(check-lit
        COMMAND ${BEZEL_LIT} -sv ${CMAKE_CURRENT_BINARY_DIR}/lit
        DEPENDS bezelc
        USES_TERMINAL
        COMMENT "Running Bezel lit + FileCheck diagnostic tests (tests/lit/)")
    message(STATUS
        "Bezel: lit diagnostic track enabled "
        "(lit=${BEZEL_LIT}, FileCheck=${BEZEL_FILECHECK}, not=${BEZEL_NOT}); "
        "target: check-lit")
else()
    message(STATUS
        "Bezel: lit diagnostic track DISABLED — lit, FileCheck, or not not found "
        "(target check-lit unavailable; ordinary build unaffected)")
endif()

# ── check-tidy: clang-tidy static-analysis tier (ADVISORY) ──────────────────
# Mirrors check-lit above: an ADDITIONAL, OPTIONAL target that is NOT part of
# `all` and skips gracefully if its tools are absent. Runs clang-analyzer-* +
# bugprone-* (see the repo-root .clang-tidy) over the compiler's own C++ under
# src/ via scripts/check_tidy.sh, which sanitizes
# ${CMAKE_BINARY_DIR}/compile_commands.json (CMAKE_EXPORT_COMPILE_COMMANDS is
# ON at the top level — see root CMakeLists.txt) and wraps run-clang-tidy.
#
# ADVISORY, not gate-blocking: test-infra migration plan §2 / WS7
# (/var/tmp/bezel-scratch/impl-run-2026-07-12/aot-plan/ws7-static-analysis.md)
# measured a ~70-80% false-positive rate among substantive findings on this
# ASan-hardened codebase, so this target (and the gate.sh stage that calls it)
# always exits 0 on findings — see check_tidy.sh's header comment for the
# promotion-to-blocking plan. This target runs the FULL sweep (all TUs, ~4-5
# min wall at -j16 per WS7 §2.3 — heavier than a clean build); it's the
# consolidation/CI mode. The changed-TUs mode used in the gate cycle
# (WS7 §2.2) isn't expressed as a build-system target — a custom target's
# COMMAND is fixed at configure time and forwarding extra args portably across
# generators (Ninja per the CMakePresets.json preset, but plain `cmake -B
# build` falls back to the platform default) isn't clean — so gate.sh instead
# invokes scripts/check_tidy.sh directly with `--changed`; both paths share
# the same script and .clang-tidy config.
find_program(BEZEL_CLANG_TIDY NAMES clang-tidy)
find_program(BEZEL_RUN_CLANG_TIDY NAMES run-clang-tidy run-clang-tidy.py)
if(BEZEL_CLANG_TIDY AND BEZEL_RUN_CLANG_TIDY)
    add_custom_target(check-tidy
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_tidy.sh ${CMAKE_BINARY_DIR} --full
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running clang-tidy static-analysis tier over src/ (advisory; see .clang-tidy)")
    message(STATUS
        "Bezel: clang-tidy static-analysis track enabled "
        "(clang-tidy=${BEZEL_CLANG_TIDY}, run-clang-tidy=${BEZEL_RUN_CLANG_TIDY}); "
        "target: check-tidy (ADVISORY, not gate-blocking)")
else()
    message(STATUS
        "Bezel: clang-tidy static-analysis track DISABLED — clang-tidy or "
        "run-clang-tidy not found (target check-tidy unavailable; ordinary "
        "build unaffected)")
endif()
