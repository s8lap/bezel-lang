// fix #8978 (§27.5 / §28.9, v0.359): a CAPTURED function-typed local that is
// REASSIGNED from inside the capturing closure's own body leaks its 8-byte
// capture cell — the no-reassignment twin is clean.
//
// THE DEFECT (issue #8978, verbatim repro):
//
//   module M;
//   public class C
//     public static method Run(): int
//       var f := lambda() => 1;
//       var h := lambda() => do
//         f := lambda() => 2;
//         return 2;
//       end;
//       return h();
//     end Run;
//     public static method Main(): int
//       var total := 0;
//       for var j in 0..<200 do total := total + C.Run(); end;
//       Console.WriteLine(total);
//       return 0;
//     end Main;
//   end C;
//
// MEASURED on the base (r30-c1 @ 8ccf7ff06 + the LLVM-23 build fix), with the
// issue's own repro verbatim and `bezelc --sanitize=address`:
//
//   SUMMARY: AddressSanitizer: 1600 byte(s) leaked in 200 object(s)
//
// — one DIRECT leak of 8 B per call, allocated by `calloc` in `bz$M.C$Run`:
// the BOXED CAPTURE CELL, not the closure value. The issue's own control (the
// same program with a READ of `f` instead of a REASSIGNMENT of it) is clean.
//
// ROOT CAUSE. `Emitter::consumeLastLambdaCapturedCells`
// (src/codegen/LlvmBackend_Setup.cpp) SKIPS the scope-end `boxedCellFree`
// registration for a captured-cell local that is also REASSIGNED, on the
// premise that the envdtor is then the cell's sole owner (#4434 / #4261-FORWARD).
// The skip's oracle is "the envdtor frees the cell", and the envdtor frees the
// cell on exactly TWO routes (#4261-FORWARD's own comment):
//
//   * #694's sole-escaping-holder arm, gated on
//     `singleCaptureLocals_.count(key) && definitelyEscapingLambdas_.count(lam)`
//     (src/codegen/LlvmBackend_Exprs2.cpp), and
//   * #3426's shared-cell arm, gated on `isSharedCellEnvdtorFreeOwner(key, &lam)`.
//
// The skip spelled the first of those as `singleCaptureLocals_.count(key)` —
// WITHOUT the `definitelyEscapingLambdas_` conjunct — while #4509's own
// refcount-arming code (src/codegen/LlvmBackend_Exprs2.cpp) spells the SAME
// test correctly. The dropped conjunct is the whole defect: the sole capturer
// here is a closure that is CALLED in the same frame, so it does not escape and
// the envdtor never frees the cell. And the #4509 refcount route is declined
// for a function-typed capture (`contentsAlsoManaged`), so nothing else frees
// the cell either: it leaks, 8 B per call.
//
// WHY THE NO-REASSIGNMENT TWIN IS CLEAN. The skip is gated on
// `closureLocalCapturedCellReassigned_`, which `ClosureEscapeWalker` sets only
// when it observes a reassignment target inside a lambda body. Without the
// reassignment the skip does not fire and the scope-end `boxedCellFree`
// registers — the cell is reclaimed exactly as in the twin.
//
// THE SPEC. §27.5 (v0.359): "Captured variables are heap-allocated in a closure
// object" — the capture cell is a heap object owned by the closure.
// §28.9 (v0.359): "Runtime management governs the function value itself — the
// heap-allocated closure object that backs it" — runtime management covers that
// object's memory, so the capture cell is reclaimed with the closure, exactly as
// the no-reassignment twin does. Which site performs the reclaim is an
// implementation choice (§28.9 disclaims the refcounting implementation); the
// requirement is that it happens EXACTLY ONCE.
//
// ORACLE. `runAotChecked` — the checked AOT path with AddressSanitizer over
// the GENERATED module and LeakSanitizer at exit. Every row asserts the
// program's own output and exit value as well as the leak count, so a silent
// zero-fill cannot pass, and the row LOOPS 200 times (the issue's own
// amplification) so the leak cannot hide under the sanitizer's noise.

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

// THE ISSUE'S REPRO, VERBATIM. `f` is a function-typed local, captured by the
// single closure `h`, and REASSIGNED from inside `h`'s body; `h` is called in
// the same frame, so it does not escape.
//
// The program prints the sum of 200 calls of `Run`, each of which returns 2.
const char* kIssueRepro =
    "module M; "
    "public class C "
    "  public static method Run(): int "
    "    var f := lambda() => 1; "
    "    var h := lambda() => do "
    "      f := lambda() => 2; "
    "      return 2; "
    "    end; "
    "    return h(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 0..<200 do total := total + C.Run(); end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// THE BOUNDARY CONTROL: the same shape, but the captured local is READ AFTER
// the capturing closure has returned (`f()` after `h()`). This is what pins
// WHERE the cell may be reclaimed: the cell must still be readable after the
// capturing env's envdtor has run, so the reclaim cannot happen at the
// capture site or in the envdtor — only at the captured local's own scope end.
//
// Each call returns `h() + f()` = 2 + 2 = 4 (the reassignment made `f` return
// 2), so the program prints 200 * 4 = 800.
const char* kReadAfterClosureReturned =
    "module M; "
    "public class C "
    "  public static method Run(): int "
    "    var f: () -> int := lambda() => 1; "
    "    var h := lambda() => do "
    "      f := lambda() => 2; "
    "      return 2; "
    "    end; "
    "    var r := h(); "
    "    return r + f(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 0..<200 do total := total + C.Run(); end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C; ";

}  // namespace

// ===========================================================================
// A — THE REGRESSION ROW. The issue's repro, verbatim. RED on the base (the
// 8-byte capture cell leaks on every call: 1600 B / 200 allocations), GREEN
// with the fix.
// ===========================================================================
BEZEL_TEST(captured_cell_reassigned_closure_leak_8978,
           reassigned_captured_closure_local_reclaims_its_capture_cell) {
    auto r = runAotChecked(kIssueRepro);
    expectClean(r, 0, "400\n");
}

// ===========================================================================
// B — THE BOUNDARY CONTROL. The captured local is read AFTER the capturing
// closure returned, so the cell must still be alive then: the reclaim belongs
// to the captured local's scope end, not to the capture site and not to the
// envdtor. A fix that reclaimed the cell too early would turn this row into a
// heap-use-after-free.
// ===========================================================================
BEZEL_TEST(captured_cell_reassigned_closure_leak_8978,
           reassigned_captured_closure_local_is_readable_after_the_closure_returns) {
    auto r = runAotChecked(kReadAfterClosureReturned);
    expectClean(r, 0, "800\n");
}
