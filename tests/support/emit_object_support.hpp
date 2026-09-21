#pragma once

// emit_object_support.hpp — the ONE `codegen::emitObjectFile` wrapper every
// hand-rolled test runner calls, so the whole-suite ASan sweep reaches it.
//
// ── THE HOLE THIS CLOSES (#3917) ───────────────────────────────────────────
//
// `scripts/gate.sh --full` puts the suite under generated-code AddressSanitizer
// by exporting `BEZEL_AOT_FORCE_SANITIZE=address` (#1822). Exactly one
// predicate reads that variable — `bezel::test::forceSanitizeAddress()` in
// support/force_sanitize.hpp — and three places consult it: `runAot`
// (support/aot_support.hpp), the hand-rolled `Driver::compileFile` runners
// (support/driver_run_support.hpp, the #4532/#3156 sibling of this header),
// and the shard-summary writer (test_main.cpp).
//
// A test that calls `bezel::codegen::emitObjectFile` DIRECTLY consults none of
// them. That call's fourth parameter, `sanitizeAddress`, is positional-
// defaulted to `false` (src/codegen/LlvmBackend.hpp:102), so the three-argument
// spelling — the natural way to write the call, and the spelling 99 sites in
// this tree use — silently opts the program out of the sweep. `--full` then
// compiles that program's generated code EXACTLY as the plain gate does.
//
// The consequence is not "slightly less coverage": it is a specific, named
// blind spot. These runners link an ASan-instrumented *C++ runtime* object
// (`native_test::asanRuntimeObject()`), which interposes the allocator — so
// LeakSanitizer still sees a LEAK, and `-fsanitize=address` still catches a
// double-free routed through the runtime's own `free`. But a use-after-free
// READ performed by the GENERATED Bezel code is invisible: no shadow-memory
// check is emitted on the program's own loads and stores, so a read of
// quarantined-but-still-mapped freed memory returns stale bytes and the
// program runs on. #3917 measured exactly that: widening
// `StringEscapeWalker::walkCallArgs`'s relaxation to make a #1747 row's
// program a genuine, reproducible use-after-free left that row reporting
// "clean", while the SAME program through `bezelc --sanitize=address` reported
//
//     ==ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8
//
// i.e. the rows in this family whose NAMES promise `no_uaf` / `no_crash` /
// `double_free`-freedom were, for that defect class, false negatives.
//
// ── THE FIX: A WRAPPER, NOT A DEFAULT ──────────────────────────────────────
//
// `codegen::emitObjectFile`'s default could not simply become the environment:
// `LlvmBackend` is COMPILER code and the shipping `bezelc` must not change
// behaviour because a test-harness variable happens to be set. The
// consultation therefore lives on the TEST side of the boundary, in the one
// call every hand-rolled runner makes — the same argument, and the same shape,
// as `compileFileForTest` in support/driver_run_support.hpp.
//
// Converting a call site is a rename and nothing else:
//
//     bezel::codegen::emitObjectFile(*module, objPath, err)
//  -> bezel::test::emitObjectFileForTest(*module, objPath, err)
//
// ── THE DIRECTION IS ONE-WAY ───────────────────────────────────────────────
//
// The wrapper only ever turns instrumentation ON. A runner that deliberately
// passes `sanitizeAddress=true` — because instrumented generated code is the
// whole point of that test — keeps it in a plain run, exactly as before. With
// `BEZEL_AOT_FORCE_SANITIZE` unset this is `codegen::emitObjectFile(m, p, e)`
// verbatim, so a normal run is byte-for-byte unchanged.
//
// ── ⚠ THE CALLER MUST LINK `-fsanitize=address` ────────────────────────────
//
// An ASan-instrumented object references `__asan_report_*` and registers an
// `asan.module_ctor`; linking it without the ASan runtime fails at link time.
// 84 of the 91 files in this family already pass `-fsanitize=address`
// unconditionally (they link `asanRuntimeObject()`, which is itself built that
// way), so for them the wrapper needs nothing further. A runner that links
// WITHOUT the runtime — or under a DIFFERENT sanitizer, e.g. the
// `-fsanitize=` + `san` parameterised helpers in async_test.cpp and
// async_618_domain_serial_test.cpp, where forcing ASan instrumentation into a
// TSan link would be actively wrong — must decide for itself: either add
// `forcedSanitizeLinkFlag()` below to its link line, or keep calling
// `codegen::emitObjectFile` and say why.
//
// ── ⚠ AND `exitcode=99` ALONE DOES NOT CLASSIFY THE FINDING ────────────────
//
// Instrumenting the object is necessary but not sufficient for a row whose
// name promises UAF-freedom. Most runners in this family spell
// `ASAN_OPTIONS=detect_leaks=1:exitcode=99` and then read the wait status —
// but `exitcode` is the status ASan uses for EVERY finding, a LeakSanitizer
// report and a heap-use-after-free alike. A runner that maps "99" to "leak"
// therefore reports a freshly-detected use-after-free as a leak, and a row
// asserting "not a hard error" passes anyway. `sanitizerFindingInReport`
// below classifies the captured stderr, which is what makes the newly
// available signal observable. See record_string_field_call_release_1747_test.
//
// Kept dependency-light on purpose: `LlvmBackend.hpp` (which every caller
// already includes — it is where `emitObjectFile` is declared) plus
// force_sanitize.hpp's two standard-library headers.

#include <string>
#include <string_view>

#include "codegen/LlvmBackend.hpp"      // codegen::emitObjectFile
#include "support/force_sanitize.hpp"   // bezel::test::forceSanitizeAddress

namespace bezel::test {

// ---------------------------------------------------------------------------
// emitObjectFileForTest — what a hand-rolled runner calls instead of
// `bezel::codegen::emitObjectFile(…)`.
//
// The signature mirrors `codegen::emitObjectFile` exactly, including the
// positional default, so converting a call site is a rename. The sweep is
// OR-ed in, never OR-ed out.
// ---------------------------------------------------------------------------
inline bool emitObjectFileForTest(llvm::Module& module,
                                  const std::string& objectPath,
                                  std::string& error,
                                  bool sanitizeAddress = false) {
    return codegen::emitObjectFile(module, objectPath, error,
                                   sanitizeAddress || forceSanitizeAddress());
}

// ---------------------------------------------------------------------------
// forcedSanitizeLinkFlag — for a runner whose link line does NOT already carry
// `-fsanitize=address`.
//
// Returns `"-fsanitize=address "` (trailing space, so it concatenates into a
// command line) under the sweep and `""` otherwise, so a plain run's link
// command is byte-for-byte unchanged. A runner that already passes the flag
// unconditionally — the overwhelming majority — needs none of this.
// ---------------------------------------------------------------------------
inline const char* forcedSanitizeLinkFlag() {
    return forceSanitizeAddress() ? "-fsanitize=address " : "";
}

// ---------------------------------------------------------------------------
// sanitizerFindingInReport — classify a program's captured stderr.
//
// `ASAN_OPTIONS=...:exitcode=N` makes EVERY sanitizer finding exit with N, so
// the wait status alone cannot tell a LeakSanitizer report from a
// heap-use-after-free. The report text can: LSan's headline is
// `ERROR: LeakSanitizer: detected memory leaks`, every ASan hard error is
// `ERROR: AddressSanitizer: <kind>`.
//
//   kNone      — no sanitizer finding in the report.
//   kLeak      — a LeakSanitizer report and nothing else.
//   kHardError — an AddressSanitizer finding (use-after-free, double-free,
//                heap-buffer-overflow, …). Takes precedence: a run that both
//                faults and leaks is a hard error.
// ---------------------------------------------------------------------------
enum class SanitizerFinding { kNone, kLeak, kHardError };

inline SanitizerFinding sanitizerFindingInReport(std::string_view report) {
    if (report.find("ERROR: AddressSanitizer") != std::string_view::npos)
        return SanitizerFinding::kHardError;
    if (report.find("ERROR: LeakSanitizer") != std::string_view::npos)
        return SanitizerFinding::kLeak;
    return SanitizerFinding::kNone;
}

}  // namespace bezel::test
