// fix #9004 / §27.5 (v0.359): a local captured by a lambda INSIDE a loop that
// the same loop WRITES between iterations — the closure read a stale or
// diverged value.
//
// THE DEFECT. §27.5: "Captured variables are heap-allocated in a closure
// object … Modifications to a captured variable inside the lambda are visible
// outside, and vice versa" — ONE cell per binding. `boxLocalSlot` lowers the
// enclosing body's statements in order, so the accesses to the captured local
// that come BEFORE the capture site were emitted against the local's
// pre-boxing alloca, while every access emitted AFTER it goes through the cell
// holder. In straight-line code the two agree (the pre-capture accesses run
// before the cell exists). In a LOOP they do not: the same pre-capture access
// runs again on the next iteration, AFTER the cell exists, and then reads and
// writes the pre-boxing alloca while the closure reads and writes the cell.
//
// Two modes were reachable (`boxLocalSlot`'s strategy choice):
//   * by-CELL (guarded) — one cell, reused every iteration: the closure read
//     the cell's first-pass value forever (the issue's repros 1 and 2);
//   * by-VALUE (fresh) — a new cell per iteration, seeded from the pre-boxing
//     alloca: the accumulated value was reset every pass (the issue's repro 3).
// The mode was chosen by "is there a store to the slot in the capture's own
// basic block", which fires for an ACCUMULATION (`k := k + i`) as well as for
// a re-initialisation.
//
// THE FIX. (1) The by-CELL mode keeps the cell in step with the loop: the
// accesses the enclosing body already emitted against the pre-boxing alloca
// are redirected to the cell holder's address — the same funnel the
// post-capture accesses use — so the loop's write and the closure's read hit
// the ONE cell the binding has. (2) The mode choice no longer answers
// "by-VALUE" for an accumulating local: §27.5 gives one cell per binding, so
// an accumulating local takes the by-CELL mode.
//
// MEASURED (ASan/LSan, `bezelc --sanitize=address`, the issue's repros
// verbatim): repro 1 printed 12 (expected 40), repro 2 printed 21 (expected
// 77), repro 3 printed 108 and leaked 16 B in 4 allocations (expected 248).
// With the fix: 40, 77, 248, and no leak.
//
// ORACLE. `runAot` for the three value rows (the issue's literal repros: the
// program's own stdout and exit code are asserted exactly) and
// `runAotChecked` for the leak row (the checked AOT path instruments the
// GENERATED module with AddressSanitizer and runs LeakSanitizer at exit; the
// row loops the shape 200 times so a per-iteration cell leak cannot hide
// under the sanitizer's noise, and asserts the leak count is zero).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
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

// The program ran to completion, with the program's own exit code and the
// exact expected output.
void expectOutput(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
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
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// THE ISSUE'S REPRO 1, verbatim: a conditional capture in a `while` loop, the
// loop writing the captured local between iterations.
//
// Hand trace: k = 3, 4, 6, 9, 13, 18 across i = 0..5; the lambda runs at
// i = 0, 2, 4 and reads 3, 6, 13, so s = 22 and s + k = 22 + 18 = 40.
// (Base: 12 — the closure read the cell's first-pass value every time.)
BEZEL_TEST(loop_capture_shared_cell_9004,
           while_conditional_capture_sees_the_loop_write) {
    auto r = runAot(
        "module M; public class C "
        "  public static method Main(): int "
        "    var k := 3; var s := 0; "
        "    var i := 0; "
        "    while i < 6 do "
        "      k := k + i; "
        "      if i mod 2 = 0 then var h := lambda() => k; s := s + h(); end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(s + k); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    expectOutput(r, 0, "40\n");
}

// THE ISSUE'S REPRO 2, verbatim: the capture after a conditional `break` in a
// `for` loop.
//
// Hand trace: s = 3 + 4 + 6 + 9 + 13 + 18 = 53, then i = 6 gives k = 24 and
// breaks: 53 + 24 = 77. (Base: 21.)
BEZEL_TEST(loop_capture_shared_cell_9004,
           for_break_capture_sees_the_loop_write) {
    auto r = runAot(
        "module M; public class C "
        "  public static method Main(): int "
        "    var k := 3; var s := 0; "
        "    for var i in 0..<10 do "
        "      k := k + i; "
        "      if i = 6 then break; end; "
        "      var h := lambda() => k; s := s + h(); "
        "    end; "
        "    Console.WriteLine(s + k); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    expectOutput(r, 0, "77\n");
}

// THE ISSUE'S REPRO 3, verbatim: a lambda that MUTATES the captured local, in
// a loop that also reassigns it.
//
// Hand trace: k = 13, 24, 36, 49, 63 after each call; s accumulates those
// results: 13 + 24 + 36 + 49 + 63 = 185, and the method returns 185 + 63 =
// 248. (Base: 108, plus a 16 B / 4-allocation capture-cell leak — the by-VALUE
// mode allocated one cell per iteration and reclaimed only the last.)
BEZEL_TEST(loop_capture_shared_cell_9004,
           lambda_mutation_shares_the_cell_with_the_loop) {
    auto r = runAot(
        "module M; public class C "
        "  public static method Main(): int "
        "    var k := 3; var s := 0; "
        "    for var i in 0..<5 do "
        "      k := k + i; "
        "      var h := lambda() => do k := k + 10; return k; end; "
        "      s := s + h(); "
        "    end; "
        "    Console.WriteLine(s + k); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    expectOutput(r, 0, "248\n");
}

// THE ISSUE'S REPRO 3 over 200 CALLS: the capture cell is reclaimed, and the
// value is the by-CELL one on every call.
//
// Base: 21600 (108 per call) and "SUMMARY: AddressSanitizer: 3200 byte(s)
// leaked in 800 allocation(s)". With the fix: 49600 (248 per call) and no
// leak.
BEZEL_TEST(loop_capture_shared_cell_9004,
           no_capture_cell_leaks_over_200_calls) {
    auto r = runAotChecked(
        "module M; public class C "
        "  public static method Run(): int "
        "    var k := 3; var s := 0; "
        "    for var i in 0..<5 do "
        "      k := k + i; "
        "      var h := lambda() => do k := k + 10; return k; end; "
        "      s := s + h(); "
        "    end; "
        "    return s + k; "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<200 do total := total + C.Run(); end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    expectOutput(r, 0, "49600\n");
    assertNoLeak(r);
}

}  // namespace
