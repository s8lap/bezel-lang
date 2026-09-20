// Codegen — §21.3 (while) / §21.4 (for), v0.279 (issue #4471): an
// outer-scope local assigned by `local := local + <other local>` inside a
// loop body must produce the correct cumulative value across all iteration
// counts, including values that exceed the 8-bit Unix exit-code range.
//
// Spec authority:
//   §21.3 The `while` statement — body executes until the condition is
//          false; each iteration reads and writes locals in declaration
//          scope. An outer-scope local is NOT closure-captured; the
//          read-then-store path goes through the local's stack slot, not
//          any cell-indirection.
//   §21.4 The `for` statement — iterates an iterable source; the loop
//          variable is bound per iteration.
//   §28.3 Local variables, ownership, and borrows — locals declared
//          outside a closure are stack-resident; the only path that
//          forces box/cell indirection is closure CAPTURE of the local,
//          which `a := a + i` in a non-lambda loop body does not.
//
// Why this test uses the stdout channel: a Bezel `Main` whose value
// exceeds 255 does not round-trip through the 8-bit Unix exit status
// (`WEXITSTATUS` masks to 0..255). #4471 reported "wrong sums past ~20
// iterations" because the test loop observed 1225 % 256 = 201, 4950 % 256
// = 86, etc. — the values were correct in `%eax`; the SHELL truncated
// them. `runAotOut` reads the printed value from the program's stdout
// and bypasses that 8-bit ceiling entirely. See aot_support.hpp's
// `runAotOut` docstring for the full rationale.
//
// Independence from #4130 / #4116 / #4117 / #4210 / #4261 — these tests
// pin the codegen for plain outer locals with no closure capture; the
// `boxLocalSlot` / `boxedStorage` / `cellVars_` paths of #4116 do NOT
// engage (the locals are never captured into a lambda), so the IR is
// the straightforward `load slot; load slot; sadd; store slot` shape.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// ── helpers ────────────────────────────────────────────────────────────────

// Build a Main whose loop runs the body `iters` times and prints `a` to
// stdout, then returns 0. The program prints the FULL i32 value of `a`,
// which a single runAotOut call reads back as a decimal string.
//
// The loop body is intentionally textually identical to the issue repro
// (`a := a + i; i := i + 1;`) — this is the shape that #4471 claimed
// regressed past ~20 iters, so an `iters`-value of 50 covers the alleged
// boundary by ~30 iters of headroom on either side.
//
// `return 0;` (not `return a;`) so the exit code is uninformative and
// the test reads its truth from stdout only — the value channel that
// bypasses the 8-bit exit-code ceiling.
std::string whileAccumSrc(int iters) {
    return
        "module M4471; class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    var i := 0; "
        "    while i < " + std::to_string(iters) + " do "
        "      a := a + i; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(a); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// Same shape, but the loop variable is bound by the for-statement. The
// issue also tested `for var i in 0..N do a := a + i; end;`, so this
// pins the for-loop codegen path independently.
std::string forAccumSrc(int upperExclusive) {
    return
        "module M4471; class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    for var i in 0.." + std::to_string(upperExclusive) + " do "
        "      a := a + i; "
        "    end; "
        "    Console.WriteLine(a); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// The expected cumulative sums are pinned directly in the test bodies
// (literals like "1225", "1275"). Closed ranges per spec §29.1.

}  // namespace

// ── §21.3 / #4471: outer-local accumulator while-loop ─────────────────────

// 5 iterations → sum 0..4 = 10. The smallest count the issue reported
// as "correct" — pins the baseline of the count curve.
BEZEL_TEST(codegen_outer_local_accumulator_loop_4471,
           while_loop_5_iters_value_via_stdout) {
    // per §21.3, v0.279: 5 iterations → sum 0..4 = 10
    BEZEL_EXPECT_EQ(runAotOut(whileAccumSrc(5)), "10");
}

// 25 iterations → sum 0..24 = 300. The smallest count the issue
// reported as "broken" — pins the alleged boundary on the CORRECT side
// of it. A codegen defect would surface as any value other than "300".
BEZEL_TEST(codegen_outer_local_accumulator_loop_4471,
           while_loop_25_iters_value_via_stdout) {
    // per §21.3, v0.279: 25 iterations → sum 0..24 = 300
    BEZEL_EXPECT_EQ(runAotOut(whileAccumSrc(25)), "300");
}

// The exact reproducer from #4471's "Repro" block. 50 iters: 1225.
// Exit code 201 = 1225 % 256 (the "wrong sum" reported). Stdout value
// is the correct 1225. A regression in the codegen would make the
// stdout value anything OTHER than 1225 (typically a partial-sum like
// 201 or 86 or 78).
BEZEL_TEST(codegen_outer_local_accumulator_loop_4471,
           while_loop_50_iters_value_via_stdout) {
    // per §21.3, v0.279: 50 iterations → sum 0..49 = 1225
    BEZEL_EXPECT_EQ(runAotOut(whileAccumSrc(50)), "1225");
}

// Larger N — well above the 256-byte boundary — pins the same codegen
// path through to several multiples of the exit-code wrap. 100 iters:
// 4950 (4950 % 256 = 86, matching the issue's reported "86").
BEZEL_TEST(codegen_outer_local_accumulator_loop_4471,
           while_loop_100_iters_value_via_stdout) {
    // per §21.3, v0.279: 100 iterations → sum 0..99 = 4950
    BEZEL_EXPECT_EQ(runAotOut(whileAccumSrc(100)), "4950");
}

// 500 iters — the largest count the issue tested. 124750 % 256 = 78
// (matching the issue's reported "78").
BEZEL_TEST(codegen_outer_local_accumulator_loop_4471,
           while_loop_500_iters_value_via_stdout) {
    // per §21.3, v0.279: 500 iterations → sum 0..499 = 124750
    BEZEL_EXPECT_EQ(runAotOut(whileAccumSrc(500)), "124750");
}

// ── §21.4 / #4471: outer-local accumulator for-loop ───────────────────────

// The for-statement codegen path is distinct from while: the loop
// variable is bound by the iterator (compiler intrinsic for `0..N`
// counted forms, per spec §21.4). The issue reports 251 for `0..50`,
// which is 1275 % 256 — the closed-on-both-ends range 0..50 sums to
// 0+1+...+50 = 1275. The issue's "expected 1225" reflects a half-open
// range `0..<50`; that is a different language's shape.
BEZEL_TEST(codegen_outer_local_accumulator_loop_4471,
           for_loop_0_to_50_inclusive_value_via_stdout) {
    // per §21.4, §29.1: closed range 0..50 → sum 0+1+...+50 = 1275
    BEZEL_EXPECT_EQ(runAotOut(forAccumSrc(50)), "1275");
}

// Closed range 0..10: 0+1+...+10 = 55 (not 45 — #4471's "expected" was
// based on a half-open range).
BEZEL_TEST(codegen_outer_local_accumulator_loop_4471,
           for_loop_0_to_10_inclusive_value_via_stdout) {
    // per §21.4, §29.1: closed range 0..10 → sum 0+1+...+10 = 55
    BEZEL_EXPECT_EQ(runAotOut(forAccumSrc(10)), "55");
}

// ── §28.3 / #4471: outer-local accumulator over a large counted range ─────

// This loop body does NOT touch `a` on the right-hand side — it tests
// that even a "constant RHS" outer-local accumulation (which the issue
// explicitly reports as CORRECT) stays correct at large N. The control
// pins that no future codegen change can silently flip the RHS path
// from "load outer-local" to "load cell pointer" without breaking this
// test (which reads the same accumulator's value via stdout, not the
// exit code, so a 256+ value cannot mask a regression).
BEZEL_TEST(codegen_outer_local_accumulator_loop_4471,
           while_loop_constant_rhs_large_iter_value_via_stdout) {
    // per §21.3, §28.3, v0.279: a := a + 1 over 50 iters → 50.
    // The issue notes this shape "works correctly at any iteration
    // count"; the test pins the orthogonal RHS=outer-local path the
    // issue DOES flag (the prior tests) and the RHS=constant path
    // (this test) under the SAME observer so any future divergence is
    // visible.
    const auto src =
        "module M4471; class C "
        "  public static method Main(): int "
        "    var a := 0; "
        "    var i := 0; "
        "    while i < 50 do "
        "      a := a + 1; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(a); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runAotOut(src), "50");
}

// ASan sanity: the same outer-local accumulator runs cleanly under the
// instrumented build too. A use-after-free or wild-read in the slot
// rebinding path would surface here as an LSan report at exit.
BEZEL_TEST(codegen_outer_local_accumulator_loop_4471,
           while_loop_50_iters_clean_under_checked_disposal) {
    // per §28.3 / §28.14, v0.279: the checked-disposal pass must not
    // produce an LSan report on plain int locals — they are not
    // disposable resources. A cell-rebinding defect that wraps the
    // int in a runtime-managed box would surface as a leak report.
    auto r = runAotChecked(whileAccumSrc(50));
    BEZEL_EXPECT_TRUE(r.ran);
    // The oracle is `ran==true` AND the stdout channel still reads
    // 1225 — both must hold for a clean build. The native ASan leg
    // is ADVISORY here: per aot_support.hpp, runAotChecked runs under
    // ASan and flips to ran=false on a sanitizer report; a green build
    // gates on `r.ran` AND `runAotOut(...) == "1225"`.
    BEZEL_EXPECT_EQ(runAotOut(whileAccumSrc(50)), "1225");
}
