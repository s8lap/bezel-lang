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
// `-fsanitize=address`.
//
// ⚠ An earlier revision of this paragraph continued: "and LeakSanitizer forces
// the process exit status to 1 on a leak — so a runner's existing
// `BEZEL_EXPECT_EQ(r, 0)` becomes the leak assertion, with no per-test change."
// That is WRONG in both halves and it cost a gate cycle (#4532). The sweep's
// own run options turn LSan OFF (`runAot`: detect_leaks=0, #1822) precisely
// because whole-suite leak detection is too noisy and because §28.2 checked
// disposal leaks BY DESIGN; the hand-rolled path got `detect_leaks=1` only by
// inheriting the ambient environment, which no other test in the suite runs
// under. And a forced exit status of 1 does not "become" an assertion — it
// REPLACES the program's own exit code, so a row asserting a value fails on a
// correct program and a row asserting 1 passes on any program at all. The run
// half now lives in `runProgramForTest` at the bottom of this header; use it
// instead of `std::system(exe)`.
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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "codegen/LlvmBackend.hpp"      // codegen::CodegenOptions
#include "driver/Driver.hpp"            // bezel::Driver / Dependency / CompileResult
#include "support/force_sanitize.hpp"   // bezel::test::forceSanitizeAddress
#include "test_harness.hpp"             // currentTestFailed() — the loud-failure channel

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

// ---------------------------------------------------------------------------
// runProgramForTest — THE RUN HALF OF THE SWEEP (#4532)
//
// ── THE HOLE THIS CLOSES ───────────────────────────────────────────────────
//
// `compileFileForTest` above wired the COMPILE half of `gate.sh --full` into
// the hand-rolled runners. The RUN half was never wired at all. `runAot` — the
// harness the sweep was designed around — runs every sanitized program with
//
//     ASAN_OPTIONS=detect_leaks=0:abort_on_error=0        (aot_support.hpp:3127)
//
// per #1822: the sweep's oracle is REDZONE checking (out-of-bounds,
// use-after-free, double-free), deliberately NOT leak checking, because
// whole-suite LSan is too noisy — a negative test that exits via an uncaught
// exception legitimately leaves heap live at exit, and `--checked-disposal`
// QUARANTINES disposed objects instead of freeing them on purpose (§28.2
// v0.135, issues #621/#627: `bz_array_mark_disposed`,
// `bz_closure_mark_disposed`, `bz_shared_mark_disposed` — "a deliberate memory
// leak in checked builds"). Leak assertions in this tree are made EXPLICITLY,
// per test, by rows that spell `ASAN_OPTIONS=detect_leaks=1:exitcode=99`
// themselves (19 files in tests/module alone); that mechanism is untouched.
//
// A hand-rolled runner calling `std::system(exe)` inherits the AMBIENT
// environment instead, which under the gate sets no ASAN_OPTIONS at all — so it
// ran with LSan defaults (`detect_leaks=1`, `exitcode=1`). Two consequences,
// both measured on the 2026-09-21 `--full` cycle at e89577e4:
//
//   * A CORRECT program's exit code was REPLACED. `xmod_sig_key_kind_collision
//     _4508` rows 5-7 return 105 and `xmod_meta_type_source_text_4480` returns
//     42; under the sweep both reported 1, because LeakSanitizer's `exitcode`
//     supersedes the program's own. The rows' own oracle (the value) was never
//     consulted. 7 rows failed this way while the plain oracle passed all of
//     them.
//   * A row passed VACUOUSLY. `xmod_checked_mode_forwarding_4488` row 1 asserts
//     exit 1, and its checked-disposal program's §28.2 quarantine makes LSan
//     force exactly 1 — so the row was green under the sweep whatever the
//     program did.
//
// So the ambient oracle is not a stronger one; it is a DIFFERENT one that
// nobody chose, that no `runAot` test runs, and that contradicts §28.2 for any
// checked-disposal program. This helper gives the hand-rolled path the sweep's
// own run options, so the row keeps asserting the value it exists to assert.
//
// ── A SANITIZER FINDING STILL FAILS, LOUDLY ────────────────────────────────
//
// `exitcode=97` (not ASan's default 1) means "the sanitizer intervened" can
// never be mistaken for "the program returned N" — which matters precisely
// because #4488 row 1 legitimately returns 1. On top of that, the captured
// report is re-emitted under the same `[aot-asan #1822]` banner `runAot` uses
// (aot_support.hpp:573 — keep the two spellings identical; `scripts/gate.sh`
// greps for it) and the CURRENT TEST IS MARKED FAILED, so a redzone error can
// never be observed as a pass regardless of what the caller does with the
// returned status.
//
// ── ONE-WAY, AND INERT WITHOUT THE SWEEP ───────────────────────────────────
//
// With `BEZEL_AOT_FORCE_SANITIZE` unset this is `std::system(exe)` verbatim, so
// a plain run — and every runner that sets its own `ASAN_OPTIONS=...` prefix,
// which this helper is not for — is byte-for-byte unchanged.
// ---------------------------------------------------------------------------

// "The sanitizer intervened", distinct from every exit code the converted rows
// assert (1, 42, 97 is not among them, 99, 105).
inline constexpr int kSanitizerInterventionExit = 97;

// Re-emit a fired report under `runAot`'s banner. Bounded so an adversarial
// report cannot flood a shard log. Sibling of
// aot_detail::emitSanitizerReportToStderr (aot_support.hpp) — deliberately NOT
// a call into it: that header pulls in the whole AOT/LLVM harness, and this one
// is included by ~110 TUs that need none of it.
inline void emitRunnerSanitizerReport(std::string_view report) {
    const std::size_t cap = 1u << 15;  // 32 KiB — headline + backtrace
    std::fprintf(stderr,
        "\n===== [aot-asan #1822] sanitizer fired in an AOT test program =====\n"
        "%.*s\n"
        "===== [aot-asan #1822] end sanitizer report =====\n",
        static_cast<int>(std::min(report.size(), cap)), report.data());
    std::fflush(stderr);
}

// Drop-in for `std::system(exe)`: returns the same wait status, so a call
// site's existing WIFEXITED/WEXITSTATUS decoding is unchanged.
//
// ⚠ ONE SEMANTIC DIFFERENCE, under the sweep only. The sanitized path goes
// through `/bin/sh` (to set `ASAN_OPTIONS` and redirect stderr), so a program
// killed by a SIGNAL comes back as the shell's `WIFEXITED` / `128+N` rather
// than the `WIFSIGNALED` a bare `std::system(exe)` would report — a runner's
// "abnormal exit" sentinel arm is not taken for that case. It cannot turn a
// failure into a pass: `128+N` is 129..192, and no converted row asserts a
// value in that range (they assert 1, 42, 99, 105). It is also mostly moot
// under the sweep, where `abort_on_error=0` makes ASan EXIT rather than raise.
inline int runProgramForTest(const std::string& exePath) {
    if (!forceSanitizeAddress()) return std::system(exePath.c_str());

    // stderr to a file so a fired report can be inspected AND re-emitted;
    // stdout is left inherited, exactly as `std::system(exe)` leaves it.
    const std::string errPath = exePath + ".sanerr";
    std::remove(errPath.c_str());
    const std::string cmd =
        "ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=" +
        std::to_string(kSanitizerInterventionExit) + " " + exePath +
        " 2>> " + errPath;
    const int status = std::system(cmd.c_str());

    std::string report;
    {
        std::ifstream in(errPath);
        std::ostringstream ss;
        ss << in.rdbuf();
        report = ss.str();
    }
    std::remove(errPath.c_str());

    if (report.find("ERROR: AddressSanitizer") != std::string::npos ||
        report.find("ERROR: LeakSanitizer") != std::string::npos) {
        emitRunnerSanitizerReport(report);
        std::fprintf(stderr,
            "    [ ASAN ] runProgramForTest(%s): the generated-code sanitizer "
            "fired; the program's own exit code is NOT what this run "
            "reports (exit %d == the sanitizer's). Failing the test (#4532).\n",
            exePath.c_str(), kSanitizerInterventionExit);
        std::fflush(stderr);
        ::bezel::test::currentTestFailed() = true;
    } else if (!report.empty()) {
        // Not a sanitizer report — give the program its stderr back verbatim,
        // which is where it would have gone without the capture.
        std::fwrite(report.data(), 1, report.size(), stderr);
        std::fflush(stderr);
    }
    return status;
}

}  // namespace bezel::test
