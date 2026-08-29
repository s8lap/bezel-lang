// Codegen — issue #2251 (§19 / §19.3 / §19.4 / §30.2 / §30.7, v0.286), fixed
// as part of #3587 (CLAUDE.md §6 dedup: "four hand-written copies of 'does
// this contain an await?'").
//
// `ast::CaseExpr` was unreached by ALL FOUR of the pre-fix hand-written
// await-scanning walkers (`Emitter::exprContainsAwait` / `countAwaitsInExpr`,
// src/codegen/LlvmBackend_Async.cpp; `MethodOwnership::exprContainsAwait`,
// src/ownership/Ownership_Checks.cpp; `recordAwaitsInExpr`, src/flow/CFG.cpp)
// — none of them even looked at a CaseExpr's `scrutinee` or `arms`. Unlike
// the sibling `ast::BindExpr` gap (#3587), nothing in sema restricts `await`
// inside a case-expression's scrutinee, an arm's `when` guard (§19.4), an
// arm's expression body (§19.2), or a `RelOpPattern`'s embedded constant
// expression (§19.3) — so this is NOT latent: a real async method hits it.
//
// MECHANISM (why it hangs, not just mis-typechecks): codegen's
// `countAwaitsInExpr` sizes the async resume switch (§30.2's suspend-state
// count, driving `smNextSuspendState_`). An await inside a CaseExpr was never
// counted, so the switch had no case for the state the in-arm `await`
// actually suspends into. When a pool worker later fires that continuation
// (forced with `Task.Delay(40)` below, so the resume is a REAL cross-thread
// hop, not the same-thread synchronous-completion fast path that would mask
// the bug — see async_await_extra_kinds_2226_test.cpp's header for the same
// methodology note), the switch falls to its `default: return`, so
// `bz_task_complete` is never called and the outer `Task` never completes —
// the caller's `Task.Wait()`-style consumer (here, the process's own await
// chain reaching `Main`) blocks forever. The exact #441/#2226 hang shape.
//
// fail-without/pass-with: every non-control case below HUNG under the
// pre-fix compiler (`timeout 30`-bounded `runAot` -> `r.ran == false`,
// `exitCode == -1`, MEASURED via a standalone `timeout 10` probe binary at
// exit 124, stable across 3 repeats) and returns the expected value with the
// fix. The control case (repro 0) passed on both sides — it materializes the
// awaited value in a statement-level `var` BEFORE the `case`, so no await is
// ever nested inside the CaseExpr, and exists to show the `case` construct
// itself is not what's broken.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `r.ran ? r.exitCode : -999` — the standard "did it compile+run" idiom
// (async_own_task_across_suspend_1868_test.cpp). A HANG surfaces as -999 too
// (runAot's 30s per-program timeout leaves `ran == false`), which is
// indistinguishable here from a compile rejection — neither is expected for
// any case below, so -999 always means "investigate", not "expected".
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Class `C`'s async `Slow()` method returning 7, forcing a REAL cross-thread
// resume via `Task.Delay(40)` (see the file header). `body` is `C`'s Main
// method (and any other members); callers supply everything after `Slow`'s
// closing `end Slow;`.
std::string progWithSlowReturning7(const std::string& body) {
    return "module M; class C "
           "  public static async method Slow(): Task<int> "
           "    await Task.Delay(40); "
           "    return 7; "
           "  end Slow; " +
           body + " end C;";
}

}  // namespace

// Repro 0 (CONTROL): the awaited value is bound in a statement-level `var`
// BEFORE the `case`, so no `await` is nested inside the CaseExpr at all —
// isolates that the `case` construct itself is not what's broken. Value: `v`
// is 7 (Slow's result), doesn't match `1`, so the `_` arm selects 20.
BEZEL_TEST(case_expr_await_2251, control_await_materialized_before_case) {
    BEZEL_EXPECT_EQ(run(progWithSlowReturning7(
        "  public static async method Main(): Task<int> "
        "    var v := await Slow(); "
        "    var x := (case v of 1 => 10, _ => 20); "
        "    return x; "
        "  end Main; ")),
        20);
}

// Repro 1: `await` directly in the CaseExpr SCRUTINEE (§19.1's
// `case Expression? CaseArm...`). Same value shape as the control: 20.
BEZEL_TEST(case_expr_await_2251, await_in_scrutinee) {
    BEZEL_EXPECT_EQ(run(progWithSlowReturning7(
        "  public static async method Main(): Task<int> "
        "    var x := (case await Slow() of 1 => 10, _ => 20); "
        "    return x; "
        "  end Main; ")),
        20);
}

// Repro 2: `await` in an arm's expression BODY (§19.2 "Arms"). The scrutinee
// `7` matches the `7` arm, whose body is the await itself: value 7.
BEZEL_TEST(case_expr_await_2251, await_in_arm_expression_body) {
    BEZEL_EXPECT_EQ(run(progWithSlowReturning7(
        "  public static async method Main(): Task<int> "
        "    var x := (case 7 of 7 => await Slow(), _ => 0); "
        "    return x; "
        "  end Main; ")),
        7);
}

// Repro 3: `await` in an arm's `when` GUARD (§19.4). The scrutinee `3`
// matches `var n`'s pattern unconditionally, then the guard `n < (await
// Slow())` runs (3 < 7 -> true), selecting 100.
BEZEL_TEST(case_expr_await_2251, await_in_when_guard) {
    BEZEL_EXPECT_EQ(run(progWithSlowReturning7(
        "  public static async method Main(): Task<int> "
        "    var x := (case 3 of "
        "      var n when n < (await Slow()) => 100, "
        "      _ => 200); "
        "    return x; "
        "  end Main; ")),
        100);
}

// Repro 4: `await` embedded in a RelOpPattern's bound (§19.3 "Patterns" —
// `> EXPR`/`< EXPR`/... ). Reached only via `forEachAwaitSiteInCaseArm` ->
// `forEachAwaitSiteInPattern`, a deeper layer than repros 1-3: NONE of the
// four original walkers looked at `ast::CaseArm` at all, so this was equally
// unreached. The scrutinee `10` is compared against the pattern bound
// `await Slow()` (5, not 7 — a distinct `Slow` so this case's expected value
// can't be confused with the others'): `10 > 5` is true, selecting 100.
BEZEL_TEST(case_expr_await_2251, await_in_relop_pattern_bound) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Slow(): Task<int> "
        "    await Task.Delay(40); "
        "    return 5; "
        "  end Slow; "
        "  public static async method Main(): Task<int> "
        "    var x := (case 10 of > (await Slow()) => 100, _ => 200); "
        "    return x; "
        "  end Main; "
        "end C;"),
        100);
}
