// Ownership tests for issue #3337 — v0.273 §28.14 bound 2(b) (loop-case
// freeze extension). The v0.273 binding narrows the freeze interval from
// "rest of method" (v1) to "L's scope exit" (v0.273), and bound 2(b)
// further widens the interval for the loop case: when L's block sits
// inside a loop and the captured source is declared OUTSIDE that loop,
// the freeze persists to the end of the innermost enclosing loop,
// because a lift at L's block end would re-admit the capture over a
// source the previous iteration consumed.
//
// Spec authority: §28.14 v0.273 bound 2(b): "where `L`'s block sits
// inside a **loop** and the captured source is declared outside that
// loop, the freeze persists to the end of that innermost enclosing
// loop, since a lift there would re-admit the capture over a source the
// previous iteration consumed."
//
// The compiler-side change: the pre-scan pushes the LOOP's `span.end`
// onto a parallel `scopedLoopEndStack_` while scanning a `while` or
// `for` body, and the freeze-interval upper bound for a `using var`
// declared inside the loop body becomes max(top of block-end stack, top
// of loop-end stack) — the loop's end, not the inner block's end. The
// v1 method-wide freeze and the v0.273 block-scoped freeze both
// coincided for flat bodies and for `do … end` lifts outside a loop;
// this lane surfaces the loop-specific divergence.
//
// PINS THE BUG: the REJECT tests use `Prog.Sink(owned res)` (a move
// action), because `checkScopedFreeze` only fires on move / dispose /
// reassign — a member call like `res.Mutate()` would not trigger the
// diagnostic under either v1 or v0.273. (Same pattern as #3320 Test 1
// — the move-action shape is the only parse-time surface of the freeze
// interval's upper bound.)

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Same fixture as `scoped_freeze_3320_test.cpp`: Res + Runner +
// Prog.Main. Runner is the scoped-storage fixture so the freeze map is
// populated via `Install`; Prog.Sink(owned x) is the freeze action.
std::string prog(const std::string& body) {
    return std::string(
        "module M; "
        "class Res "
        "  public method Ping() end Ping; "
        "  public method Mutate() end Mutate; "
        "  destructor end; "
        "end Res; "
        "class Runner "
        "  private scoped _cb: (() -> ())?; "
        "  public constructor(scoped cb: () -> ()) _cb := cb; end; "
        "  public method Install(scoped cb: () -> ()) _cb := cb; end Install; "
        "  public method Fire() "
        "    if _cb <> null then _cb(); end; "
        "  end Fire; "
        "  destructor end; "
        "end Runner; "
        "class Prog "
        "  public static method Sink(owned x: Res) x.Dispose(); end Sink; "
        "  public static method Main(): int ") + body +
        "  end Main; "
        "end Prog;";
}

}  // namespace

// ===========================================================================
// Test 1 (REJECT — pins the bug): a `using var run` declared inside a
// `while` loop body, capturing a source `res` declared OUTSIDE the
// loop. Per v0.273 bound 2(b), the freeze extends to the LOOP's end,
// so a reassignment of `res` BEFORE the back-edge (still inside the
// loop, after L's block but before the loop closes) is rejected.
// The mutation site is `Prog.Sink(owned res)` placed inside a `do …
// end` block INSIDE the loop and AFTER the inner block's `end` — the
// `do … end` lift would otherwise unfreeze `res` per v0.273 bound 2(a)
// in flat bodies, but the loop case widens the interval to the loop's
// end. Pre-fix: the freeze's upper bound was the loop body's end (the
// inner block end was pushed but the loop wasn't tracked), so the
// v0.273 loop-case widening was a no-op and this test would FAIL with
// no error — wrong. Post-fix: the loop's `span.end` is on the parallel
// loop-end stack and the freeze upper bound becomes the loop's end,
// so the mutation at the do-end lift site is rejected.
// // per §28.14 (v0.273, bound 2(b))
BEZEL_TEST(scoped_freeze_3337, mutation_after_inner_block_inside_loop_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    while true do "
        "      do "
        "        using var run := new Runner(lambda() => res.Ping()); "
        "      end; "
        "      Prog.Sink(owned res); "
        "      break; "
        "    end; "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// ===========================================================================
// Test 2 (REJECT — flat-body control inside a loop): a `using var run`
// declared directly in the loop body (no inner `do … end`) capturing
// `res` declared outside. A move of `res` later in the SAME loop
// iteration (still before the loop's `end`) is rejected — the freeze's
// upper bound is the loop's end, not the body of the iteration. //
// per §28.14 (v0.273, bound 2(b))
BEZEL_TEST(scoped_freeze_3337, mutation_in_loop_body_after_admission_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    while true do "
        "      using var run := new Runner(lambda() => res.Ping()); "
        "      Prog.Sink(owned res); "
        "      break; "
        "    end; "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// ===========================================================================
// Test 3 (REJECT — for-loop, do-block, PINS THE BUG): the for-loop
// analogue of Test 1. The `using var run` is declared inside a `do …
// end` block that is itself inside a `for` loop, capturing `res`
// declared OUTSIDE the loop. The `Prog.Sink(owned res)` sits between
// the do-block's `end` and the for-loop's `end`.
//
// Pre-fix: scopeEndLoc = do-block end (the block stack's top), so the
// Sink is past the freeze interval → ACCEPT (wrong — bound 2(b)
// unsatisfied).
// Post-fix: scopeEndLoc = max(do-end, for-end) = for-end, so the Sink
// is still in the freeze interval → REJECT (correct). // per §28.14
// (v0.273, bound 2(b))
//
// Note: `xs` is an `Array<int>` (declared inline) — its iteration
// variable `x` is a value-typed `int`, so this loop is just a shape
// carrier for the freeze diagnostic. The captured source is `res`,
// declared OUTSIDE the loop.
BEZEL_TEST(scoped_freeze_3337, mutation_after_do_block_in_for_loop_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    var xs: Array<int> := new Array<int>(); "
        "    for var x in xs do "
        "      do "
        "        using var run := new Runner(lambda() => res.Ping()); "
        "      end; "
        "      Prog.Sink(owned res); "
        "    end; "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// ===========================================================================
// Test 4 (REJECT — control, source INSIDE the loop): the basic §28.14
// freeze is in effect for `res` (the source is a per-iteration local,
// but the freeze is structural — recorded against the AST node's
// symbol, which has one entry per body, not per iteration). The move
// is inside the same iteration body, before the loop's end. The
// bound 2(b) extension doesn't change this outcome — the basic block-
// scoped freeze already rejects this. (Verifies the fix doesn't
// inadvertently OVER-extend or break the in-body case.) // per §28.14
// (v0.273, bound 2(b))
BEZEL_TEST(scoped_freeze_3337, mutation_inside_iteration_source_local_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var xs: Array<int> := new Array<int>(); "
        "    for var x in xs do "
        "      var res := new Res(); "
        "      using var run := new Runner(lambda() => res.Ping()); "
        "      Prog.Sink(owned res); "
        "    end; "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// ===========================================================================
// Test 5 (ACCEPT — pins the bug): mutation of the captured source AFTER
// the loop closes — outside the loop entirely. The freeze's upper
// bound is the loop's end (post-fix), so a move past the loop's `end`
// is admitted.
//
// Pre-fix: scopeEndLoc = methodEnd (no inner block, no loop tracking
// beyond the inner block), so the Sink at offset < methodEnd stays
// inside the freeze interval → REJECT (wrong — bound 2(b) would
// correctly lift the freeze at the loop's end).
// Post-fix: scopeEndLoc = loopEnd (the loop's span.end is the LATEST
// of the two stacks' tops), so the Sink at offset > loopEnd lifts
// the freeze → ACCEPT (correct). // per §28.14 (v0.273, bound 2(b))
BEZEL_TEST(scoped_freeze_3337, mutation_after_loop_end_accepted) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    while true do "
        "      using var run := new Runner(lambda() => res.Ping()); "
        "      break; "
        "    end; "
        "    Prog.Sink(move res); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
