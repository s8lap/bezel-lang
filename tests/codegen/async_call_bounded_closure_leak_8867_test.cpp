// fix #8867 (§28.14 *Call-bounded × deferred activation*, shape (1) "Frame
// residence" / §28.9 / §30.1, v0.359): a lambda literal passed to a PLAIN
// (call-bounded, §28.14) function-typed parameter of an ASYNC method leaked
// its 64 B closure object on every call. The sync twin did not.
//
// THE DEFECT. §28.14's call-bounded default: "A function-typed parameter
// declared *without* `scoped` is call-bounded … the callee receives the
// callback for the duration of the call only — or, when the call mints a
// deferred-activation frame, for that frame's life." The async callee's frame
// IS such a deferred-activation frame (§28.14 shape (1): "A callee whose call
// mints a deferred-activation frame (… an `async` callee's task; …) holds a
// plain function-typed parameter in that frame … The bound is the frame's life
// (K6.1): a task's settlement (§30.1)"), so the callee must hold the
// callback — and the CALLER must drop the producer's +1.
//
// The compiler did neither. `prepareCallClosureArgRelease`'s call-bounded arm
// is gated on `!isAsyncMethod(ms)`, so the async callee never got the
// call-site release its sync twin gets; and the async method's frame copied
// the parameter as a RAW pointer (`emitFrameCaptureStores` — no closure
// retain), so nobody held the closure either. The producer's +1 was nobody's
// to drop ⇒ the closure leaked, once per call.
//
// MEASURED on the base (r30-c1 @ fbe542d4d), the issue's own repro verbatim:
// "SUMMARY: AddressSanitizer: 12800 byte(s) leaked in 200 allocation(s)" —
// 200 × 64 B, allocated by `calloc` in `bz$async$M.C$Main$Resume`. The
// program's own output was correct (2400) — only the memory was not.
//
// THE FIX. Two halves, the arrangement the async ITERATOR already has
// (#2609: the frame takes its own +1 on the closure cell at construction and
// drops it at Dispose, and the call site releases the producer's +1 through
// its own `rel.syncIter` arm):
//   * the CALLEE: `emitAsyncMethod` retains the plain function-typed formal
//     into the task frame at entry and drops it at the task's terminal edge
//     (success or fault — mutually exclusive, §30.5), so the callback stays
//     alive until the task settles;
//   * the CALLER: `prepareCallClosureArgRelease` admits a PLAIN (non-
//     iterator) async callee into the call-bounded release, so the producer's
//     +1 is dropped right after the call.
// The two halves are keyed on the SAME predicate (the callee's own parameter
// symbol), so they cannot drift.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED module and LeakSanitizer at exit. Every row asserts
// the program's own output and exit value as well as the leak count, so a
// silent zero-fill cannot pass. The rows LOOP (200 iterations inside the
// program, the issue's own repro), so the leak cannot hide under the
// sanitizer's noise — and the callback is INVOKED inside the callee, so a
// fix that released the closure without the frame's hold would show up as a
// heap-use-after-free here rather than as a silent pass.
//
// FAIL-WITHOUT / PASS-WITH. Both rows are RED on the base (the closure
// leaks) and GREEN with the fix.

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

// THE REGRESSION ROW: the issue's own repro, verbatim. A lambda literal
// passed to a PLAIN function-typed parameter of an ASYNC method; the callee
// awaits (so its frame outlives the call) and INVOKES the callback after the
// suspension's preparation.
//
// RED on the base: "12800 byte(s) leaked in 200 allocation(s)" — one 64 B
// closure per call. GREEN with the fix.
BEZEL_TEST(async_call_bounded_closure_leak_8867,
           async_callee_call_bounded_closure_released_spec_28_14_28_9) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Sum(xs: Array<int>, h: (int) -> int): "
        "                                          Task<int> "
        "    var t := 0; "
        "    for var y in xs do "
        "      t := t + h(y); "
        "    end; "
        "    await Task.Delay(0); "
        "    return t; "
        "  end Sum; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var a := 0; "
        "    for var k in 0..<200 do "
        "      a := a + await Sum(xs, lambda(v: int): int => v * 2); "
        "    end; "
        "    Console.WriteLine(a); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(source);
    expectClean(r, 0, "2400\n");
}

// THE FAULT EDGE: the async callee THROWS after invoking the callback. The
// task settles by FAULT, and §30.5 makes the fault and success edges
// mutually exclusive terminal states — so the frame's +1 must be dropped on
// the fault edge too, exactly once. Without the fault-edge drop the closure
// leaks on every faulted call; with the drop it is released exactly once.
//
// This row is what keeps the fix from being a HALF fix: the success edge's
// release alone would leave the fault edge leaking, and the row's leak
// assertion is what makes that observable.
//
// RED on the base (the closure leaks — the async callee never got the
// call-site release), GREEN with the fix.
BEZEL_TEST(async_call_bounded_closure_leak_8867,
           async_callee_call_bounded_closure_released_on_fault_spec_30_5) {
    const std::string source =
        "module M; "
        "class C "
        "  public static async method Sum(xs: Array<int>, h: (int) -> int): "
        "                                          Task<int> "
        "    var t := 0; "
        "    for var y in xs do "
        "      t := t + h(y); "
        "    end; "
        "    await Task.Delay(0); "
        "    throw new ArgumentException(\"boom\"); "
        "  end Sum; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var a := 0; "
        "    for var k in 0..<200 do "
        "      try "
        "        a := a + await Sum(xs, lambda(v: int): int => v * 2); "
        "      catch var e: ArgumentException => "
        "        a := a + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(a); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(source);
    expectClean(r, 0, "200\n");
}

}  // namespace
