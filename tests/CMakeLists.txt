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

# fix #4530: glob each CLAIMED directory's `*.cpp` pattern EXACTLY ONCE at
# CONFIGURE time and reuse the result everywhere below, instead of the THREE
# separate `file(GLOB ... CONFIGURE_DEPENDS ...)` calls this used to make per
# directory (once here to populate `_d_srcs` for the truthy check below, once
# more inside the flat "everything" builder, and a third time inside the
# per-group builder). `VerifyGlobs.cmake_force` (a zero-input phony) forces
# ninja to re-run `cmake -P VerifyGlobs.cmake` on EVERY invocation regardless
# — that check-time triplication was already free there, because CMake's own
# glob-verification manager merges identical glob EXPRESSIONS into one watched
# entry (confirmed by inspecting a configured build's generated
# VerifyGlobs.cmake: each entry is annotated with every consuming variable/
# line, e.g. `_d_srcs`, `BEZEL_TEST_ALL_SOURCES` and `BEZEL_TEST_SOURCES_codegen`
# all cite the SAME single `tests/codegen/*.cpp` block). What the triplication
# actually tripled was CONFIGURE-TIME work: every full reconfigure (the
# `RERUN_CMAKE` this same force-check can trigger, per #4530's title) re-listed
# and re-processed each claimed directory's files three times over instead of
# once. Discovery semantics are unchanged — every claimed dir is still globbed,
# both FATAL_ERROR invariants below are unchanged, and a new file is picked up
# exactly as before (see the commit body for the before/after proof).
foreach(_d IN LISTS BEZEL_TEST_CLAIMED_DIRS)
    file(GLOB _bezel_claimed_srcs_${_d} CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/${_d}/*.cpp)
endforeach()

# Invariant (2): no unclaimed area.
file(GLOB _bezel_test_entries RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(_d IN LISTS _bezel_test_entries)
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${_d})
        if(_d IN_LIST BEZEL_TEST_CLAIMED_DIRS)
            # Already globbed above; reuse it instead of re-globbing (#4530).
            set(_d_srcs ${_bezel_claimed_srcs_${_d}})
        else()
            # An unclaimed dir (e.g. tests/lit/) — this IS the only place that
            # glob is needed, so it stays a fresh call.
            file(GLOB _d_srcs CONFIGURE_DEPENDS
                 ${CMAKE_CURRENT_SOURCE_DIR}/${_d}/*.cpp)
        endif()
        if(_d_srcs AND NOT _d IN_LIST BEZEL_TEST_CLAIMED_DIRS)
            message(FATAL_ERROR
                "Bezel: tests/${_d}/ contains test sources but no group in "
                "BEZEL_TEST_GROUPS claims it — those tests would NOT be built "
                "and the suite would silently shrink. Add '${_d}' to a "
                "BEZEL_TEST_GROUP_<g>_DIRS list in tests/CMakeLists.txt.")
        endif()
    endif()
endforeach()

# Root-level tests (everything but the shared entry point) — also the "root
# pattern" contribution to the flat "everything" list below, globbed once.
file(GLOB _bezel_root_srcs CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
set(BEZEL_TEST_ROOT_SOURCES ${_bezel_root_srcs})
list(REMOVE_ITEM BEZEL_TEST_ROOT_SOURCES ${BEZEL_TEST_ENTRY})

# The flat "everything" list invariant (1) is checked against — built from the
# already-globbed per-dir results above instead of re-globbing them (#4530).
set(BEZEL_TEST_ALL_SOURCES ${_bezel_root_srcs})
foreach(_d IN LISTS BEZEL_TEST_CLAIMED_DIRS)
    list(APPEND BEZEL_TEST_ALL_SOURCES ${_bezel_claimed_srcs_${_d}})
endforeach()

# Per-group source lists — reuse the already-globbed per-dir results (#4530).
set(_bezel_assigned "")
foreach(g IN LISTS BEZEL_TEST_GROUPS)
    set(BEZEL_TEST_SOURCES_${g} "")
    foreach(_d IN LISTS BEZEL_TEST_GROUP_${g}_DIRS)
        list(APPEND BEZEL_TEST_SOURCES_${g} ${_bezel_claimed_srcs_${_d}})
    endforeach()
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
#
# fix #3172: the PCH used to be owned by whichever group configured first
# (codegen, ~1,357 of ~2,094 TUs) and every other group REUSE_FROM'd it. Per
# `cmake --help-command target_precompile_headers`, the REUSE_FROM form "adds
# a dependency such that <target> will depend on <other_target>" — the WHOLE
# owner target, not just its .pch artifact — so `--area module` scheduled all
# of codegen's compile actions before its own ~140 (#3172, measured ~2,228
# codegen actions against 30 module ones). bezel_tests_pch below is a
# dedicated one-TU OBJECT library that owns the PCH instead: the REUSE_FROM
# dependency now lands on a target with exactly one (dummy, never-edited)
# source, so no other group's compile is dragged in by it.
#
# The same doc also says REUSE_FROM "requires the same set of compiler
# options, compiler flags and compiler definitions for both <target> and
# <other_target>" — so bezel_test_apply_common_compile_settings() below
# applies the include dirs, link usage-requirements (bezel_core, for the LLVM
# include dirs the 12 PCH headers need; bezel_warnings for -Wall/-Wextra
# /-Wpedantic) and compile definitions identically to bezel_tests_pch AND
# every group target, from ONE place, so they cannot drift apart.
function(bezel_test_apply_common_compile_settings tgt)
    target_include_directories(${tgt} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${tgt} PRIVATE bezel_core bezel_warnings)
    # fix #3172: -gline-tables-only, NOT -g0 — the AOT harness still needs a
    # symbolized backtrace out of a crashing forked test child (file:line),
    # and -gline-tables-only keeps that while dropping the full variable/type
    # debug info the RelWithDebInfo build type's plain -g would otherwise
    # emit for every one of the ~2,094 test TUs. This is a new flag, so it is
    # a ccache key change — paid once as a full test rebuild, a cache hit on
    # every build after. Orthogonal to the CMAKE_BUILD_TYPE=RelWithDebInfo /
    # -DNDEBUG decision above (#3013): that one is about optimization level
    # and live asserts; this one is only about how much debug info a TEST TU
    # carries. bezel_core — the compiler library actually under test — is
    # UNAFFECTED and keeps full `-g`: a crash inside the forked
    # Driver::compileFile child needs full frames, since that is the code
    # being tested, not the harness driving it.
    target_compile_options(${tgt} PRIVATE -gline-tables-only)
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
        BEZEL_RUNTIME_SRC_DIR="${CMAKE_SOURCE_DIR}/src/runtime"
        # fix #3627: the SAME defect class, one directory over. Four source-
        # scanning ratchet tests each derived the repo root from their own
        # __FILE__ (tests/support/repo_root.hpp); under the same ccache
        # base_dir rewrite this becomes unresolvable-from-the-worktree-root,
        # and (worse than the runtime case) a directory walk over an
        # unresolvable root silently visits zero files instead of erroring,
        # so a "zero hits expected" ratchet passes having scanned nothing.
        # Same fix: an absolute -D no path-rewriting wrapper touches.
        BEZEL_REPO_ROOT_DIR="${CMAKE_SOURCE_DIR}")
endfunction()

set(BEZEL_TEST_TARGETS "")

# The PCH owner: one dummy, never-edited TU, written into the BINARY dir (not
# tests/) so it is invisible to the tests/**/*.cpp glob and the two partition
# FATAL_ERROR invariants above.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/pch_owner.cpp "// #3172 PCH owner TU\n")
add_library(bezel_tests_pch OBJECT ${CMAKE_CURRENT_BINARY_DIR}/pch_owner.cpp)
bezel_test_apply_common_compile_settings(bezel_tests_pch)
# Precompiled header: the LLVM (+ a few ubiquitous std) headers dominate the
# ~209K preprocessed lines every test TU re-parses, and they are identical
# across all of them. Precompiling them once cuts the per-TU compile by
# ~40%. Only the HEAVY, STABLE headers are listed — deliberately NOT the
# bezel sema/codegen headers, which change often: putting a frequently-edited
# header in the PCH would invalidate it (and force a full test rebuild) on
# every such edit. ONE PCH is built (by this dedicated owner target) and
# REUSE_FROM'd by every group: per-target PCHs would mean N identical PCH
# compiles and, worse, a different `-include-pch` path per target — which is
# part of the ccache key.
# NOTE (machine-local, not tracked): ccache needs `sloppiness =
# pch_defines,time_macros` (~/.config/ccache/ccache.conf) to cache PCH
# builds — see the build-accelerators note in CLAUDE.md.
target_precompile_headers(bezel_tests_pch PRIVATE
    <cstddef> <cstdlib> <functional> <memory>
    <string> <string_view> <utility> <vector>
    <llvm/IR/LLVMContext.h>
    <llvm/IR/Module.h>
    <llvm/ExecutionEngine/Orc/LLJIT.h>
    <llvm/ExecutionEngine/Orc/ExecutionUtils.h>)

function(bezel_add_test_binary group)
    set(tgt bezel_tests_${group})
    add_executable(${tgt} ${BEZEL_TEST_ENTRY} ${BEZEL_TEST_SOURCES_${group}})
    bezel_test_apply_common_compile_settings(${tgt})
    target_precompile_headers(${tgt} REUSE_FROM bezel_tests_pch)
    # Force all bezel_runtime object files in, then export-dynamic so a
    # process-symbol-table lookup resolves bz_array_*, bz_dict_*, etc.
    target_link_options(${tgt} PRIVATE
        -Wl,--whole-archive $<TARGET_FILE:bezel_runtime> -Wl,--no-whole-archive
        -Wl,--export-dynamic)
    # $<TARGET_FILE:...> in link OPTIONS creates no dependency edge (unlike
    # target_link_libraries), so a fresh tree building one test target alone
    # never builds the archive; make the edge explicit.
    add_dependencies(${tgt} bezel_runtime)
    # fix #4386: CLI-surface tests (e.g. checked_contracts_4386_cli) exec the
    # real bezelc binary via BEZELC_PATH (set with $<TARGET_FILE:bezelc>);
    # the option produces no dependency edge of its own, so make the build
    # edge explicit.
    add_dependencies(${tgt} bezelc)
    # fix #4529 (follow-up to #2877): state the link's provenance instead of
    # leaving the harness to infer it from mtimes. The AOT bincache keys on
    # SHA-256(libbezel_core.a) for a binary that was linked from that archive,
    # and it decided "was it?" by comparing mtimes — which a no-op re-archive
    # (identical bytes, new timestamp; llvm-ar is deterministic here) inverts,
    # cooling the whole shared store and failing aot_cache_key's safety pin.
    # This step records the digest the link actually consumed, so the harness
    # can compare CONTENT. It runs on every link, so the record can never be
    # stale with respect to its binary. See aot_support.hpp's LINK-TIME
    # PROVENANCE comment.
    #
    # No BYPRODUCTS: `$<TARGET_FILE:${tgt}>` is rejected there ("No target
    # bezel_tests_<g>") because BYPRODUCTS of a POST_BUILD command is evaluated
    # without the owning target in scope, and the record is a 65-byte file that
    # is rewritten on every link — nothing downstream consumes it as a build
    # input, so ninja does not need to know about it.
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DBEZEL_CORE_LIB=$<TARGET_FILE:bezel_core>
                -DBEZEL_OUT=$<TARGET_FILE:${tgt}>.coreid
                -P ${CMAKE_CURRENT_SOURCE_DIR}/record_core_identity.cmake
        COMMENT "Recording libbezel_core.a identity for ${tgt} (#4529)"
        VERBATIM)
    set_property(TARGET ${tgt} APPEND PROPERTY
        ADDITIONAL_CLEAN_FILES $<TARGET_FILE:${tgt}>.coreid)
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
