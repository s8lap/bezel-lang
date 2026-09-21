// fix #3653: the ONE shared implementation of the per-file `runUnderAsan` /
// `runUnderASan` "compile under -fsanitize=address, link, run, classify the
// result" helper. #3633 (abdbe32) gave that helper's per-cause return codes a
// canonical shape and applied it identically to 40 tests/codegen/*_test.cpp
// files (39 of the 42 that matched `runUnderAsan|runUnderASan` at the time);
// this header is that canonical shape, extracted once instead of copied.
//
// RETURN-CODE CONTRACT (per #3633 / abdbe32, unchanged here):
//   -1  toolchain unavailable                          -> SKIP
//   -2  test PROGRAM failed to build (parse/sema/emit/emitObject)
//   -3  link failure under -fsanitize=address
//   -4  system() failure (fork/exec-level: OOM, disk, no fork slot, ...)
//    0  clean
//    1 / raw exit code  -- family-dependent, see each function below.
//
// #4504: -3/-4 are INFRASTRUCTURE failures, not memory-safety findings, but
// every caller compares the same int against 0 or an expected exit code, so
// `BEZEL_EXPECT_EQ(rc, 0)` fails IDENTICALLY for "LeakSanitizer found a leak"
// (rc==1) and "clang++ could not link the test program" (rc==-3) -- and the
// link command used to send both stdout and stderr to /dev/null, discarding
// the reason. The numeric CONTRACT above is unchanged (112 call sites depend
// on the exact values), but every -3/-4 now prints an `[ INFRA ]` marker
// carrying the link command's captured stderr (mirrors `noteInfraFailure`,
// tests/support/aot_support.hpp, and `sanitizerRuntimeMaster`'s `[ INFRA ]`
// marker in tests/native_test_harness.hpp) and marks the test failed
// directly, so an infra failure can no longer masquerade as a sanitizer
// finding at the assertion site. Family A's three failure branches (crash /
// sanitizer / wrong value) similarly each get a distinct `[ ASAN ]` marker
// naming which one fired, without changing what they return.
//
// THREE FAMILIES, NOT ONE. Diffing all 42 copies (#3653 investigation, see the
// commit body for the file-by-file table) found the copies do NOT all answer
// the same question — they split cleanly into three shapes used by disjoint
// sets of callers, plus a handful of genuinely one-off variants left as their
// own per-file copies (not converted here; see the commit body's exclusion
// list). Folding a Family-B caller onto Family A (or vice versa) would be a
// silent behavior change (checking / not checking the program's own exit
// value), which is exactly what this dedup must not do — hence three
// functions, not one with everything defaulted away.
//
// HEADER-ONLY + `inline`: every function below is defined in a header
// included from many tests/codegen/*.cpp translation units that all link
// into ONE test binary (bezel_tests_codegen). A non-`inline` free function
// here would be an ODR / multiple-definition link error.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "native_test_harness.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace bezel::test {

// Lower `source` through the real front end + codegen pipeline to an object
// file at `objPath`. Returns 0 on success, -2 on any front-end/codegen
// failure (parse, sema diagnostics, module emission, or object emission) --
// every one of the 35 converted per-file copies used exactly this sequence,
// inlined either directly in their `runUnderAsan*` body or via their own
// private `lowerToObject` helper.
inline int lowerToObjectForAsan(const std::string& source,
                                 const std::string& objPath) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -2;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return -2;
    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    if (!module) return -2;
    std::string err;
    if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
    return 0;
}

namespace asan_detail {

// Best-effort: read a small captured-output file in full and remove it.
// Returns "" on any failure to open/read (never throws) -- diagnostics
// degrade gracefully rather than crash the harness.
inline std::string readAndRemoveCapture(const std::string& path) {
    std::string out;
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
    }
    std::remove(path.c_str());
    return out;
}

// #4504: print an `[ INFRA ]` marker naming the failure mode and mark the
// CURRENT test failed directly, so a link/system() failure cannot be
// misread as a sanitizer finding regardless of which assertion macro the
// caller uses on the returned code. Mirrors `noteInfraFailure`
// (tests/support/aot_support.hpp:713) / `sanitizerRuntimeMaster`'s
// `[ INFRA ]` marker (tests/native_test_harness.hpp:284) -- same
// "print inline with BEZEL_EXPECT_* output, fail unconditionally" shape,
// extended here to the native-ASan-leg link/exec path those two don't cover.
inline void noteAsanInfraFailure(const char* tag, const std::string& reason) {
    std::cout << "    [ INFRA ] runUnderAsan(tag=\"" << (tag ? tag : "?")
              << "\"): " << reason << "\n"
              << "    [ INFRA ] this is an INFRASTRUCTURE failure "
                 "(link/exec), NOT a sanitizer finding -- see #4504.\n";
    std::cout.flush();
    ::bezel::test::currentTestFailed() = true;
}

// Link `objPath` against the ASan runtime, run the resulting `exe` under
// `asanOptions`, and hand back the raw `system()` status. Returns 0 when the
// run happened (status is meaningful), -3 on link failure, -4 if `system()`
// itself failed to run the program at all. Every family's cleanup discipline
// (remove the .o and the exe, whatever happens) is identical, so it lives
// here once too. `tag` identifies the caller for the #4504 `[ INFRA ]`
// marker; it is diagnostic-only and does not affect the return contract.
inline int linkAndRunUnderAsan(const std::string& rtObj,
                                const std::string& objPath,
                                const std::string& exe,
                                const std::string& asanOptions, int& status,
                                const char* tag = nullptr) {
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    // #4504: capture the link command's stderr instead of discarding it
    // (was `> /dev/null 2>&1`), so a link failure's cause is diagnosable in
    // one read instead of indistinguishable from a sanitizer finding.
    std::string linkErrPath = exe + ".link.stderr";
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2> " + linkErrPath;
    if (std::system(linkCmd.c_str()) != 0) {
        std::string linkErr = readAndRemoveCapture(linkErrPath);
        cleanup();
        noteAsanInfraFailure(
            tag, "link failed (`clang++ -fsanitize=address` exited "
                 "nonzero); captured stderr:\n" +
                     (linkErr.empty() ? std::string("    (empty)") : linkErr));
        return -3;
    }
    std::remove(linkErrPath.c_str());  // link succeeded: nothing to keep
    std::string runCmd =
        "ASAN_OPTIONS=" + asanOptions + " " + exe + " > /dev/null 2>&1";
    status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) {
        noteAsanInfraFailure(
            tag, "system() failed to invoke the test program "
                 "(fork/exec-level failure: OOM, disk, no fork slot, ...)");
        return -4;
    }
    return 0;
}

}  // namespace asan_detail

// ---------------------------------------------------------------------------
// Family A ("value" shape) -- mirrors array_reverse_chain_466_test.cpp, the
// worked example in #3633 (abdbe32). Checks BOTH ASan-cleanliness AND the
// program's own exit value against `expectedExitCode`; a raw (non-ASan-
// trapped) crash is a failure too (`!WIFEXITED` -> 1), which is what
// distinguishes this family from Family B below.
// `asanOptions` defaults to the leak+corruption posture every Family-A
// caller used; two callers (capture_cell_scoped_call_2334_test.cpp,
// taskscope_asan_2334_test.cpp) pass their own to select a different
// ASan/LSan posture per test.
// ---------------------------------------------------------------------------
inline int runUnderAsanValue(
        const std::string& source, int expectedExitCode, const char* tag,
        const std::string& asanOptions = "detect_leaks=1:exitcode=99") {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable: SKIP
    auto dir = wtScratchDir(tag);
    std::string stem = unique_stem(tag);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    int lowered = lowerToObjectForAsan(source, objPath);
    if (lowered != 0) return lowered;  // -2
    int status = 0;
    int rc = asan_detail::linkAndRunUnderAsan(rtObj, objPath, exe, asanOptions,
                                              status, tag);
    if (rc != 0) return rc;  // -3 / -4 (already reported via [ INFRA ], #4504)
    // #4504: name which of the three causes fired -- all three still return
    // `1`, unchanged, but the log now says which one so a wrong-value defect
    // is never mistaken for a sanitizer report or vice versa.
    if (!WIFEXITED(status)) {
        std::cout << "    [ ASAN ] runUnderAsanValue(tag=\"" << tag
                   << "\"): program did not exit normally (crashed/aborted, "
                      "raw wait status=" << status << ") -- see #4504.\n";
        return 1;                                    // crashed/aborted
    }
    int code = WEXITSTATUS(status);
    if (code == 99) {
        std::cout << "    [ ASAN ] runUnderAsanValue(tag=\"" << tag
                   << "\"): sanitizer reported a problem (exitcode=99) -- "
                      "see #4504.\n";
        return 1;                                    // sanitizer reported a problem
    }
    if (code != expectedExitCode) {
        std::cout << "    [ ASAN ] runUnderAsanValue(tag=\"" << tag
                   << "\"): wrong exit value (expected " << expectedExitCode
                   << ", got " << code << ") -- see #4504.\n";
        return 1;                                    // wrong value -> failure
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Family B ("sanitizer-only" shape) -- mirrors owned_drain_639_test.cpp, the
// most common copy (25 of the 35 converted files). Does NOT check the
// program's own exit value, only whether ASan itself reported a problem via
// its distinct `exitcode=99`. Matching every one of the 25 originals
// byte-for-byte: a raw crash NOT trapped by ASan's own handler is not
// distinguished from a clean run here (falls through to `return 0`) -- that
// is EXISTING behavior across all 25 copies, not introduced by this dedup.
// `detectLeaks` (default true, matching the 21 copies that had no such
// parameter at all) toggles `detect_leaks=0` for the two copies
// (cts_register_timer_1413, linked_cts_810) and the one caller
// (serialization_roundtrip_1390_1397, forced false) that varied it.
// ---------------------------------------------------------------------------
inline int runUnderAsan(const std::string& source, const char* tag,
                        bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable: SKIP
    auto dir = wtScratchDir(tag);
    std::string stem = unique_stem(tag);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    int lowered = lowerToObjectForAsan(source, objPath);
    if (lowered != 0) return lowered;  // -2
    std::string asanOptions = std::string("detect_leaks=") +
                              (detectLeaks ? "1" : "0") + ":exitcode=99";
    int status = 0;
    int rc = asan_detail::linkAndRunUnderAsan(rtObj, objPath, exe, asanOptions,
                                              status, tag);
    if (rc != 0) return rc;  // -3 / -4 (already reported via [ INFRA ], #4504)
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) {
        std::cout << "    [ ASAN ] runUnderAsan(tag=\"" << tag
                   << "\"): sanitizer reported a problem (exitcode=99) -- "
                      "see #4504.\n";
        return 1;                                                  // sanitizer hit
    }
    return 0;                                                      // clean
}

// ---------------------------------------------------------------------------
// Family C ("exit-code passthrough" shape) -- mirrors
// owned_arg_temp_unwind_993_test.cpp / owned_param_borrow_arg_unwind_994_test.cpp
// (byte-identical to each other) and encoding_test.cpp (identical modulo the
// `detectLeaks` toggle). Returns the PROGRAM's own exit code when ASan is
// clean -- these pins care about the exact value the program legitimately
// returns, not just pass/fail -- and 99 on any sanitizer report OR an
// untrapped crash. `abort_on_error=0` (present in all three originals, unlike
// Family A/B) keeps ASan from raising SIGABRT on its own report so the
// distinct `exitcode=99` is reachable via a normal exit. `detectLeaks`
// defaults to false, matching the two copies that had no such parameter
// (these pins target UAF/double-free/corruption, not leaks); encoding_test.cpp
// is the one caller that varies it explicitly both ways.
// ---------------------------------------------------------------------------
inline int runUnderAsanExitCode(const std::string& source, const char* tag,
                                bool detectLeaks = false) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable: SKIP
    auto dir = wtScratchDir(tag);
    std::string stem = unique_stem(tag);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    int lowered = lowerToObjectForAsan(source, objPath);
    if (lowered != 0) return lowered;  // -2
    std::string asanOptions = std::string("detect_leaks=") +
                              (detectLeaks ? "1" : "0") +
                              ":exitcode=99:abort_on_error=0";
    int status = 0;
    int rc = asan_detail::linkAndRunUnderAsan(rtObj, objPath, exe, asanOptions,
                                              status, tag);
    if (rc != 0) return rc;  // -3 / -4 (already reported via [ INFRA ], #4504)
    if (!WIFEXITED(status)) {
        std::cout << "    [ ASAN ] runUnderAsanExitCode(tag=\"" << tag
                   << "\"): program did not exit normally (crashed/aborted, "
                      "raw wait status=" << status << "); reporting as 99 "
                      "(corruption) -- see #4504.\n";
        return 99;  // crash/abort == corruption
    }
    return WEXITSTATUS(status);
}

}  // namespace bezel::test
