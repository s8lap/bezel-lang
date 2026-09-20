#pragma once

// driver_run_support.hpp — the ONE `Driver::compileFile` wrapper every
// hand-rolled test runner calls, so the whole-suite ASan sweep reaches it.
//
// ── THE HOLE THIS CLOSES ───────────────────────────────────────────────────
//
// `scripts/gate.sh --full` puts the suite under generated-code AddressSanitizer
// by exporting `BEZEL_AOT_FORCE_SANITIZE=address` (#1822). That variable was
// honoured in exactly ONE place: `runAot` (support/aot_support.hpp). But ~200
// tests do not go through `runAot` — they hand-roll a `Driver::compileFile` call
// plus `std::system(exe)` because they need two modules (a `compileObject`'d
// library plus an importer), or a captured stderr, or a dependency vector. Every
// one of those was INVISIBLE to the ASan oracle: `--full` compiled and ran their
// programs exactly as the plain gate did, so neither gate could see a leak in
// any of them.
//
// That is not hypothetical. A 962-byte cross-module leak shipped in the
// 2026-08-11 run and was caught by a human code reviewer rather than by either
// gate, because the test covering it was one of these runners. The established
// per-file remedy (`opts.sanitizeAddress = true;` — see
// tests/module/xmod_object_tostring_producer_residual_2867_test.cpp) works, but
// it is a PER-FILE flag: 15 of ~200 runners carried it and nothing made the
// other ~185 wrong. A per-file flag is the same drift channel that produced the
// bug it was added to catch.
//
// So the flag stops being per-file. `compileFileForTest(driver, …)` is a
// drop-in for the `driver.compileFile(…)` call it replaces, consulting the
// environment exactly as `runAot` does through the single predicate in
// support/force_sanitize.hpp. Normal (unset) runs are byte-for-byte unchanged —
// the wrapper's only other effect is to pass `codegen::CodegenOptions{}`
// explicitly, which is what the 2- and 3-argument `Driver::compileFile`
// overloads already do themselves (driver/Driver.cpp). Under `--full` every
// hand-rolled runner's program is now instrumented and linked
// `-fsanitize=address`, and LeakSanitizer forces the process exit status to 1 on
// a leak — so a runner's existing `BEZEL_EXPECT_EQ(r, 0)` becomes the leak
// assertion, with no per-test change.
//
// ── WHY A WRAPPER RATHER THAN A DEFAULT ────────────────────────────────────
//
// `codegen::CodegenOptions::sanitizeAddress` could not simply default to the
// environment: `CodegenOptions` is COMPILER code, and the shipping `bezelc` must
// not change behaviour because a test-harness variable happens to be set. The
// consultation therefore lives on the TEST side of the boundary, in the one call
// every test runner makes.
//
// ── THE DIRECTION IS ONE-WAY ───────────────────────────────────────────────
//
// The wrapper only ever turns the sanitizer ON. A test that deliberately sets
// `opts.sanitizeAddress = true` — a leak regression test, whose whole point it
// is — keeps it in a plain run, exactly as before.
//
// ── WHY `compileObject` IS NOT WRAPPED ─────────────────────────────────────
//
// `Driver::compileObject` DERIVES the library build's options from `opts`
// (fix #4488 — so the per-build checked modes of §28.2/§22.16 reach a `-c`
// artifact) but still forces `sanitizeAddress` off: it is an executable-build
// option that needs the final link to add the ASan runtime, and that path does
// not link — stated at driver/Driver.hpp. Wrapping `compileObject` in this
// header would therefore imply a coverage it cannot deliver. Leak
// detection still covers the dependency's allocations regardless, because
// linking the ASan runtime into the EXECUTABLE interposes `malloc`
// process-wide; only redzone instrumentation (out-of-bounds, use-after-free) is
// limited to the importer's own emitted code.

#include <string>
#include <vector>

#include "codegen/LlvmBackend.hpp"      // codegen::CodegenOptions
#include "driver/Driver.hpp"            // bezel::Driver / Dependency / CompileResult
#include "support/force_sanitize.hpp"   // bezel::test::forceSanitizeAddress

namespace bezel::test {

// Apply the sweep to a caller's options. One-way: never clears a bit the caller
// set deliberately.
inline codegen::CodegenOptions withForcedSanitize(codegen::CodegenOptions opts) {
    if (forceSanitizeAddress()) opts.sanitizeAddress = true;
    return opts;
}

// ---------------------------------------------------------------------------
// compileFileForTest — what a hand-rolled runner calls instead of
// `driver.compileFile(…)`.
//
// The overload set mirrors `Driver::compileFile` exactly (2-, 3-, and
// 4-argument forms), so converting a call site is moving the receiver into the
// first argument and nothing else. The 2-argument form covers
// `compileFile(path, "")` — a front-end-only run where the sanitizer bit is
// inert, but the uniform spelling still matters: a reader must not have to work
// out which form honours the sweep.
// ---------------------------------------------------------------------------

inline CompileResult compileFileForTest(Driver& d, const std::string& path,
                                        const std::string& outputPath = "") {
    return d.compileFile(path, outputPath, {},
                         withForcedSanitize(codegen::CodegenOptions{}));
}

inline CompileResult compileFileForTest(Driver& d, const std::string& path,
                                        const std::string& outputPath,
                                        const std::vector<Dependency>& deps) {
    return d.compileFile(path, outputPath, deps,
                         withForcedSanitize(codegen::CodegenOptions{}));
}

inline CompileResult compileFileForTest(Driver& d, const std::string& path,
                                        const std::string& outputPath,
                                        const std::vector<Dependency>& deps,
                                        const codegen::CodegenOptions& opts) {
    return d.compileFile(path, outputPath, deps, withForcedSanitize(opts));
}

}  // namespace bezel::test
