// fix #8950 (§28.14 *Call-bounded × deferred activation*, shape (1) "Frame
// residence" / §27.5 / §28.9, v0.359): a CAPTURING lambda literal passed to a
// PLAIN (call-bounded, §28.14) function-typed parameter of an ASYNC method
// leaked its captured CELL — 8 B per call, for the life of the process.
//
// THE DEFECT. §28.14's frame residence makes the callee's frame hold the
// callback until the task's settlement (§30.1): "A callee whose call mints a
// deferred-activation frame (… an `async` callee's task; …) holds a plain
// function-typed parameter in that frame, and frame residence is not an escape
// sink … The bound is the frame's life (K6.1): a task's settlement (§30.1)."
// #8867 gave the async callee that hold (the frame retains the closure at
// entry and drops it at the task's terminal edge) and gave the call site the
// matching release of the PRODUCER's +1.
//
// The CAPTURED CELL was left with no owner at all:
//
//   * the call site cannot consume it at the call — the callee's frame still
//     holds the closure across the call, and the closure's envdtor READS the
//     cell to release the captured content, so freeing the cell there would
//     be a use-after-free (`releaseCallClosureArgs` declines the async arm's
//     cell consumption for exactly this reason);
//   * the scope-end free is disarmed — the capture is classified ESCAPING
//     (the pre-scan's `isCallBoundedClosureArgPosition` declines an async
//     callee), so the captured local is not in `cellFreeEligible_`;
//   * the envdtor did not free the cell — the envdtor's cell-free route needs
//     a DEFINITELY-escaping lambda, and a lambda merely PASSED AS AN ARGUMENT
//     is not one.
//
// ⇒ the cell outlived every holder: 8 B per call.
//
// THE FIX. The captured cell is COUNTED, on #4509's protocol: the env
// announces itself with `bz_cell_retain` at its capture-store and drops that
// reference with `bz_cell_release` in its envdtor, and the declaring scope
// drops the frame's implicit reference with a `Disposal::cellRelease`. The
// cell is then freed by whichever of the two lets go LAST — which is exactly
// §28.14's frame-residence bound (the task's settlement vs. the frame's scope
// end), and which is why neither a call-site free (too early: the envdtor
// reads the cell) nor a scope-end free (too early: an un-awaited task settles
// later) is sound.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. The row asserts
// the program's own output and exit value as well as the leak count, so a
// silent zero-fill cannot pass, and it LOOPS (200 iterations inside the
// program, the issue's own repro), so the 8 B/call leak cannot hide under the
// sanitizer's noise.
//
// FAIL-WITHOUT / PASS-WITH. Both rows are RED on the base (r30-c1 @ 1d2763251):
// "SUMMARY: AddressSanitizer: 1600 byte(s) leaked in 200 allocation(s)" —
// 8 B per call — and GREEN with the fix. The second row puts the lambda
// literal in the SECOND argument position (the first argument being a call),
// which is what makes the suite catch a gate that clears the callee symbol
// instead of restoring it: that mistake leaves the FIRST row green.

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

// THE REGRESSION ROW: the issue's own repro, verbatim. A CAPTURING lambda
// literal passed to a PLAIN function-typed parameter of an ASYNC method; the
// callee awaits (so its frame outlives the call — §28.14's frame residence)
// and INVOKES the callback after the suspension's preparation.
//
// The captured local is a `string`, so it needs a CELL: the cell is the 8 B
// heap block that holds the captured value by reference. (A captured `int`
// needs no cell — it is captured by value — and the sync twin of this shape
// consumes its cells at the call site, so neither leaks.)
//
// RED on the base (r30-c1 @ 1d2763251): "1600 byte(s) leaked in 200
// allocation(s)" — one 8 B cell per call. GREEN with the fix.
BEZEL_TEST(async_call_bounded_capture_cell_leak_8950,
           async_callee_capturing_lambda_cell_reclaimed_spec_28_14_27_5_28_9) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Later(h: (int) -> int): Task<int> "
        "    await Task.Delay(1); "
        "    return h(5); "
        "  end Later; "
        "  public static async method Main(): Task<int> "
        "    var a := 0; "
        "    for var k in 0..<200 do "
        "      var s := \"x\" + k.ToString(); "
        "      a := a + await Later(lambda(v: int): int => v + s.Length); "
        "    end; "
        "    Console.WriteLine(a); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(source);
    // 200 iterations of (5 + s.Length), where s is "x" + k.ToString(): s.Length
    // is 2 for k = 0..9, 3 for k = 10..99 and 4 for k = 100..199, so the sum is
    // 10*7 + 90*8 + 100*9 = 70 + 720 + 900 = 1690. The exact value is asserted
    // so a silent zero-fill cannot pass.
    expectClean(r, 0, "1690\n");
}

// THE SECOND ROW — the SAME shape with the lambda literal in the SECOND
// argument position, the first argument being a CALL.
//
// It pins the gate's own robustness: the callee symbol the gate reads is
// installed for the duration of the argument lowering, and an argument that
// is itself a call runs a WHOLE inner call — which installs the INNER callee
// there. If the inner call cleared the symbol instead of restoring it, the
// outer call's LATER arguments would read "no callee" and the gate would
// silently decline: the cell would leak 8 B per call and the row above would
// still be green. So this row is what makes the suite catch that.
//
// RED on the base (r30-c1 @ 1d2763251): "1600 byte(s) leaked in 200
// allocation(s)". GREEN with the fix.
BEZEL_TEST(async_call_bounded_capture_cell_leak_8950,
           async_callee_capturing_lambda_in_a_later_argument_cell_reclaimed) {
    const std::string source =
        "module M; "
        "class C "
        "  public static method Tag(i: int): string "
        "    return \"t\"; "
        "  end Tag; "
        "  public static async method Later(tag: string, h: (int) -> int): Task<int> "
        "    await Task.Delay(1); "
        "    return h(5); "
        "  end Later; "
        "  public static async method Main(): Task<int> "
        "    var a := 0; "
        "    for var k in 0..<200 do "
        "      var s := \"x\" + k.ToString(); "
        "      a := a + await Later(Tag(k), lambda(v: int): int => v + s.Length); "
        "    end; "
        "    Console.WriteLine(a); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(source);
    // Same 1690 as the row above: `Later` returns h(5) and the lambda's value
    // is 5 + s.Length, exactly as there.
    expectClean(r, 0, "1690\n");
}

}  // namespace
