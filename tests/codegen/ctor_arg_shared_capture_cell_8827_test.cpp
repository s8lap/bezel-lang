// fix #8827 (§27.5 / §28.3 / §28.9, v0.359): TWO lambda literals passed as
// two arguments of ONE constructor (or of ONE call) lose the FIRST lambda's
// capture-cell record, so the shared capture cell is never reclaimed.
//
// THE DEFECT (issue #8827, verbatim repro):
//
//   module T;
//   public class K
//     public constructor(f: () -> int, g: () -> int)
//       Console.WriteLine((f() + g()).ToString());
//     end;
//   end;
//   public static class Program
//     public static method Main(args: Array<string>): int
//       var n: int := 41;
//       var k := new K(lambda() => n, lambda() => n + 1);   // prints 83
//       return 0;
//     end;
//   end;
//
// `Emitter::lowerLambda` (src/codegen/LlvmBackend_Exprs2.cpp) cleared the
// pending capture-cell channel (`lastLambdaCapturedCells_`) at the START of
// EVERY lambda lowering, so the channel held only the LAST lambda's cells.
// A site that lowers TWO lambda literals — two arguments of one constructor —
// loses the FIRST lambda's entry to the SECOND lambda's clear(); the site's
// consume (`Emitter::consumeLastLambdaCapturedCells`, called from `lowerNew`'s
// `ctorClosureArgs` block and from `releaseCallClosureArgs` on the method-call
// path) then sees an EMPTY vector and registers NO scope-end `boxedCellFree`
// for that cell. The capture cell is then reclaimed by nobody.
//
// MEASURED on the base (r30-c1 @ 082e6eb73), with the issue's own repro
// looped 200 times (the captured local declared inside the loop, so the cell
// is fresh per iteration):
//
//   SUMMARY: AddressSanitizer: 800 byte(s) leaked in 200 allocation(s).
//
// — exactly 4 B (one `int` capture cell) per construction. The METHOD-CALL
// twin of the issue's control leaks the same way (796 B / 199 allocations), so
// the issue's "the static-method twin is clean" control was a FALSE NEGATIVE:
// in the one-shot spelling the leaked cell pointer survives in the (dead but
// LeakSanitizer-scanned) stack slot of `Main`, which keeps the block
// "reachable"; with `LSAN_OPTIONS=use_stacks=0` the one-shot twin reports the
// same 4 B.
//
// THE FIX. The pending-cell channel is no longer cleared by `lowerLambda`: a
// capture cell's record belongs to the CELL, not to whichever lambda happened
// to be lowered last. No duplicate entry can arise, because the channel's
// producer is gated on `!alreadyBoxed` — the capture that CREATES a cell
// records it, and a later capture of the same cell re-uses it and records
// nothing. A late free is safe: an entry exists only for a capture the
// pre-scan classified as NON-escaping, so the capturing closure is released at
// its own site — before any later consume — and its envdtor has already run.
//
// THE SPEC. §27.5 (v0.359): "Captured variables are heap-allocated in a
// closure object". §28.3 (v0.359): "Captures share one cell (§27.5) and the
// analysis follows no invocation". §28.9 (v0.359): function-typed values are
// runtime-managed — "Runtime management governs the function value itself —
// the heap-allocated closure object that backs it". One shared cell, reclaimed
// exactly once; which site performs the reclaim is an implementation choice
// (the scope-end `boxedCellFree` the site's consume registers).
//
// ORACLE. `runAotChecked` — the checked AOT path with AddressSanitizer over
// the GENERATED module and LeakSanitizer at exit. Every row asserts the
// program's own output and exit value as well as the leak count, so a silent
// zero-fill cannot pass, and every row LOOPS 200 times so the leak cannot hide
// under the sanitizer's noise.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor the runtime's §28.9 over-release check
// fired. `runAotChecked` reports an abort and any sanitizer report as
// `ran == false`.
void didNotCrash(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    didNotCrash(r, exitCode, out);
    assertNoLeak(r);
}

// The issue's repro, LOOPED 200 times. The captured local `n` is declared
// INSIDE the loop body, so each iteration boxes a FRESH capture cell and the
// leak is 4 B per iteration (200 cells over 200 iterations) — the one-shot
// spelling of the issue's program hides the leak from LeakSanitizer, because
// the leaked cell pointer survives in the (dead but scanned) stack slot of
// `Main` and the block stays "reachable".
//
// The constructor body is the issue's own: it CALLS both lambdas, so the row
// also pins that the two closures still read the shared cell correctly (the
// printed value is the sum of the two captured reads).
const char* kCtorTwoSharedCaptures =
    "module T; "
    "public class K "
    "  public constructor(f: () -> int, g: () -> int) "
    "    Console.WriteLine((f() + g()).ToString()); "
    "  end; "
    "end; "
    "public static class Program "
    "  public static method Main(args: Array<string>): int "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      var n: int := 41; "
    "      var k := new K(lambda() => n, lambda() => n + 1); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end; "
    "end; ";

// The issue's control, as a static method: TWO lambda arguments sharing ONE
// capture. The issue recorded this shape as clean; MEASURED on the base it
// leaks the same shared cell (796 B / 199 allocations over 200 iterations;
// 4 B with `LSAN_OPTIONS=use_stacks=0` in the one-shot spelling), because the
// method-call path consumes the same pending channel.
const char* kMethodTwoSharedCaptures =
    "module T; "
    "public static class Program "
    "  public static method M(f: () -> int, g: () -> int) "
    "    Console.WriteLine((f() + g()).ToString()); "
    "  end; "
    "  public static method Main(args: Array<string>): int "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      var n: int := 41; "
    "      M(lambda() => n, lambda() => n + 1); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end; "
    "end; ";

// The GENERAL shape of the same defect: TWO lambda arguments capturing TWO
// DIFFERENT locals. The first lambda's cell has no second capture to
// re-record it, so the pending record the clear destroyed is the only one —
// and it is lost the same way (MEASURED on the base: 800 B / 200
// allocations). This row reads TWO distinct captured values, so it cannot pass
// on a mis-read of the arguments.
const char* kCtorTwoDistinctCaptures =
    "module T; "
    "public class K "
    "  public constructor(f: () -> int, g: () -> int) "
    "    Console.WriteLine((f() + g()).ToString()); "
    "  end; "
    "end; "
    "public static class Program "
    "  public static method Main(args: Array<string>): int "
    "    var i: int := 0; "
    "    while i < 200 do "
    "      var n: int := 41; "
    "      var m: int := 7; "
    "      var k := new K(lambda() => n, lambda() => m); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end; "
    "end; ";

// The output each program prints: one line per iteration.
std::string repeatedLine(const std::string& line, int times) {
    std::string out;
    for (int i = 0; i < times; ++i) {
        out += line;
        out += "\n";
    }
    return out;
}

}  // namespace

// ===========================================================================
// A — the issue's repro, looped 200x. The two constructor arguments share ONE
// capture cell; the cell must be reclaimed once, at the captured local's scope
// end. RED on the base (800 B / 200 allocations), GREEN with the fix.
// ===========================================================================
BEZEL_TEST(ctor_arg_shared_capture_cell_8827,
           two_ctor_arg_lambdas_sharing_a_capture_reclaim_the_shared_cell) {
    auto r = runAotChecked(kCtorTwoSharedCaptures);
    expectClean(r, 0, repeatedLine("83", 200));
}

// ===========================================================================
// B — the issue's static-method control, looped 200x. The issue recorded this
// shape as clean; MEASURED on the base it leaks the same shared cell, because
// the method-call path consumes the same pending channel. RED on the base
// (796 B / 199 allocations), GREEN with the fix.
// ===========================================================================
BEZEL_TEST(ctor_arg_shared_capture_cell_8827,
           two_method_arg_lambdas_sharing_a_capture_reclaim_the_shared_cell) {
    auto r = runAotChecked(kMethodTwoSharedCaptures);
    expectClean(r, 0, repeatedLine("83", 200));
}

// ===========================================================================
// C — the general shape of the same defect: two constructor arguments
// capturing TWO DIFFERENT locals, so the first cell's pending record has no
// second capture to re-record it. RED on the base (800 B / 200 allocations),
// GREEN with the fix.
// ===========================================================================
BEZEL_TEST(ctor_arg_shared_capture_cell_8827,
           two_ctor_arg_lambdas_capturing_distinct_locals_reclaim_both_cells) {
    auto r = runAotChecked(kCtorTwoDistinctCaptures);
    expectClean(r, 0, repeatedLine("48", 200));
}
