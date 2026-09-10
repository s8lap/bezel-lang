// Spec-derived regression tests for issue #4180 — `flow::CFG`'s real
// `visitTry` never routed an early `return`/`break`/`continue` out of a
// `try` body (or a catch arm) through an ACTIVE `finally` before reaching
// its real target. `ReturnStmt`/`BreakStmt`/`ContinueStmt` in `visitStmt`
// (`src/flow/CFG.cpp`) wired their edge straight to the real target
// (`cfg_.exit()` / a loop's `afterEnd`/`header`), never consulting
// `tryHandlers_` the way `ThrowStmt` already did via `exceptionTarget()`.
// `runInto`/`build` return `nullptr` for a body ending in one of these
// (the documented "returns nullptr when the body ends in a terminator"
// contract), so `visitTry`'s own `if (tryBodyEnd) { addEdge(...,
// finallyEntry) }` never ran — a `finally` reachable ONLY via an early exit
// could end up with NO predecessor reachable from `cfg_.entry()` at all, so
// the ownership dataflow (`MethodOwnership::runDataflow`, driven by
// `flow::buildCFG(body, treatTryAsOpaque=false)`, `Ownership.cpp:599` — the
// CFG's ONLY call site in the whole compiler) never visited it: every
// ownership check inside that `finally` was silently skipped.
//
// MEASURED (this fix's own commit): `try t:=t+Sink.Take(move x); return t;
// finally t:=t+Sink.Take(move x); end;` (Row 1 below, no lambda involved —
// the REAL per-method CFG, not the lambda-body pre-pass — is the exact
// shape #4180 reports) compiled with **rc=0 and zero diagnostics about `x`**
// on the pre-fix compiler, and heap-use-after-freed under
// `--sanitize=address` on first execution (`Sink.Take` reading `r.tag` on
// already-freed memory) — a double `move` of `x`, once in the try body and
// once (unreachable to the analysis) in the `finally`, both of which run at
// runtime because codegen lowers `finally` directly from the AST,
// independent of this analysis CFG.
//
// THE FIX: `Builder::routeExitThroughFinally` (CFG.cpp) walks the active
// `tryHandlers_` stack innermost-first and, for each try frame the exit
// actually LEAVES, routes through a per-(frame, ultimate-target) MEMOIZED
// CLONE of that frame's `finally` body before reaching the real target — so
// two different terminators needing the SAME (frame, target) hop reuse one
// clone rather than rebuilding the finally body per terminator site, and
// deeply nested finally chains stay linear in (nesting depth) x (distinct
// targets used), not combinatorial. A `return` (or a malformed break/
// continue reached with no active loop — sema-rejected in practice, kept
// defensive) routes through EVERY active frame's finally unconditionally,
// since it leaves the whole method; a `break`/`continue` routes through
// only the frames entered BEFORE the target loop (`loopDepthAtEntry >
// targetLoopIdx`) — a loop nested INSIDE the try never needs a hop, since
// exiting IT does not exit the try. `exceptionTarget()` (already consulted
// by `ThrowStmt`, and now also by a CATCH ARM's own synthetic
// `isArmContext` frame) gets the same per-frame finally hop for a `throw`
// that skips past a try whose OWN catch could not have caught it (arm
// bodies) — see CFG.cpp's `TryHandlerContext`/`FinallyCloneState` comments
// for the full design.
//
// SCOPE, EXPLICITLY: a `return`/`break`/`continue` DIRECTLY inside a
// `finally` block is a SEPARATE, PRE-EXISTING, ALREADY-ENFORCED sema
// rejection this fix does not touch or need to — verbatim, §21.7: "`return`
// inside a `finally` block is a compile error"; §21.5: "inside a `finally`,
// a `break` or `continue` — labeled or unlabeled — whose target loop lies
// OUTSIDE the `finally` is a compile error". Measured on this tree:
// `try return 1; finally return 2; end;` -> "`return` inside a `finally`
// block is a compile error (§31.2)"; a `break` targeting an enclosing loop
// from inside a `finally` -> "`break` inside a `finally` block may not
// target a loop that encloses the `finally` (§21.5/§31.2)". So neither
// shape ever reaches `flow::CFG` in a well-formed program — this fix's
// `routeExitThroughFinally` still handles a RETURN *inside a clone of* a
// finally (i.e. an OUTER finally reached via a chain, whose own body
// legitimately returns) correctly by construction (the recursive call
// naturally routes through any FURTHER enclosing frames, or straight to
// `cfg_.exit()`), but that shape is not separately pinned here since it
// collapses to the same "return leaves every active frame" case Row 4/5
// already exercise transitively.
//
// NOT FIXED HERE, filed separately: a `throw` INSIDE A CATCH ARM escaping
// its own try's finally was closed by this same commit (the `isArmContext`
// frame + `exceptionTarget()` extension) — see CFG.cpp. The pre-existing
// "throwable call mid-try-body approximated by ONE exception edge from the
// body's end" (`cfg_.addExceptionEdge(tryBodyEnd, catchDispatch)`) is
// skipped, unchanged, when the try body ends in a terminator — a
// pre-existing, narrower, adjacent gap this fix does not touch (a
// throwable call BEFORE an unconditional `return`/`break`/`continue` in the
// same try body gets no approximating exception edge at all); see the
// commit body / issue tracker for the follow-up filed against it.
//
// per §21.5, §21.7, §22.16 (n/a), §28.3, §30.5, §30.9, §31.2, v0.298, #4180

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"   // parseAndCheckOwnership, hasErrorMessage
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A destructor-counter Res + a Sink that consumes it — the exact idiom
// #4180's own issue repro and this project's sibling CFG-join tests
// (#4116/#4117/#4138) use, so this file's rows are directly comparable.
const char* kPrelude =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; "
    "end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "end Sink; ";

const std::string kAlreadyMovedMsg =
    "owned value in `x` was already moved out at this point (§28.3)";

const std::string kMaybeConsumedReadMsg =
    "local `x` may have already been consumed on another branch and may "
    "not be read here (maybe-consumed join, §28.3)";

const std::string kMaybeConsumedConsumeMsg =
    "cannot consume `x`: it may have already been consumed on another "
    "branch (maybe-consumed join, §28.3)";

}  // namespace

// ===========================================================================
// Reject-side rows (parseAndCheckOwnership) — the measured bug itself.
// FAIL-WITHOUT: each compiles with rc=0 and ZERO diagnostics about `x` on
// the pre-fix compiler (the finally's own move of `x` was analysis-
// unreachable); heap-use-after-frees under ASan on first execution (Row 1
// measured directly — see this fix's commit body for the ASan trace).
// PASS-WITH: correctly rejected at compile time, with a real location.
// ===========================================================================

// Row 1: `return` — the issue's own filed repro, non-lambda (the real
// per-method CFG, `treatTryAsOpaque=false`). The try body moves `x` and
// returns; `finally` also moves `x`. Before this fix `finallyEntry` had no
// predecessor reachable from `cfg_.entry()` at all (verified: neither the
// normal-exit edge `if (tryBodyEnd)` nor the `catches.empty()` pass-through
// through `catchDispatch` reaches it when the try body itself ends in
// `return` and no `throw`/throwable-call is textually present), so the
// finally's move went unanalyzed. per §21.7, §28.3, v0.298
BEZEL_TEST(cfg_try_finally_early_exit_4180,
           return_through_active_finally_rejects_double_move_21_7) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    try "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    finally "
        "      t := t + Sink.Take(move x); "
        "    end; "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kAlreadyMovedMsg));
}

// Row 2: `break` — the same shape, early exit is a `break` out of the
// enclosing loop instead of a `return`. Measured pre-fix: REJECTED, but for
// the WRONG (narrower) reason — "owned value in `x` would be dropped here
// without disposal", pointing at the declaration, not the finally's
// maybe-consumed move (visitWhile wires both a true- and false-condition
// edge even for a literal `while true`, so the unreachable false edge's
// pre-loop state pollutes the after-loop merge — a separate, narrower
// artifact #4180's issue flagged as "circumstantial", not independently
// root-caused there). Post-fix: correctly rejected with BOTH the definite
// "already moved out" (the finally's real double-move) and the discharge
// diagnostic. This row pins the STRONGER, now-correct diagnostic; it does
// not assert the pre-fix wording (already superseded, not a FAIL-WITHOUT
// this row needs — see Row 1 for the mechanically proven fail-without).
// per §21.5, §28.3, v0.298
BEZEL_TEST(cfg_try_finally_early_exit_4180,
           break_through_active_finally_rejects_double_move_21_5) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    while true do "
        "      try "
        "        t := t + Sink.Take(move x); "
        "        break; "
        "      finally "
        "        t := t + Sink.Take(move x); "
        "      end; "
        "    end; "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kAlreadyMovedMsg));
}

// Row 3: `continue` — same shape, early exit is `continue` (the loop
// condition then re-evaluates false and the loop ends normally). Post-fix:
// correctly rejected via the maybe-consumed join (the continue-routed
// finally-clone's move joins with the loop header's own entry state).
// per §21.5, §28.3, v0.298
BEZEL_TEST(cfg_try_finally_early_exit_4180,
           continue_through_active_finally_rejects_double_move_21_5) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    var i := 0; "
        "    while i < 1 do "
        "      i := i + 1; "
        "      try "
        "        t := t + Sink.Take(move x); "
        "        continue; "
        "      finally "
        "        t := t + Sink.Take(move x); "
        "      end; "
        "    end; "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedReadMsg) ||
                       hasErrorMessage(r, kMaybeConsumedConsumeMsg));
}

// Row 3b: a CATCH ARM (not the try body) that `move`s `x` and re-throws —
// the issue's own text calls out "a try body (OR A CATCH ARM)" ending in a
// terminator as the bug's scope. Before this fix a catch arm's Builder
// never had ITS OWN try's frame in `tryHandlers_` at all (by design — a
// throw inside a catch handler must not re-enter its OWN try's catch), so
// there was no channel at all for the arm's exit to reach the finally; this
// fix's `isArmContext` frame supplies `finallyEntry` without disturbing
// `exceptionTarget()`'s existing (correct, unchanged) "skip this try's own
// catch" answer for a throw escaping the arm. per §28.3, §31.1, §31.2, v0.298
BEZEL_TEST(cfg_try_finally_early_exit_4180,
           catch_arm_throw_through_active_finally_rejects_double_move) {
    std::string src =
        std::string(kPrelude) +
        "class MyErr: Exception "
        "  public constructor() end constructor; "
        "end MyErr; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    try "
        "      throw new MyErr(); "
        "    catch var e: MyErr => do "
        "      t := t + Sink.Take(move x); "
        "      throw; "
        "    end "
        "    finally "
        "      t := t + Sink.Take(move x); "
        "    end; "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kAlreadyMovedMsg));
}

// ===========================================================================
// Accept-side rows — false-reject guards (§15): a CFG that now sees MORE
// paths must not newly reject legitimate code. Rows 4-5 use real execution
// (runAotChecked) to prove the routed finally actually disposes its own,
// NON-conflicting resource (not merely "it compiled"); Rows 6-8 sweep the
// three at-risk analyses the #4146 architectural note names (§28.3 owned
// discharge / exception-path discharge, §30.5 async suspend-locals) plus
// the labeled-break-to-an-outer-loop and using-var-spanning-the-try shapes.
// ===========================================================================

// Row 4: early `return` through a `finally` that discharges a DIFFERENT,
// non-conflicting owned local (the "discharge inside the finally" grid
// cell) — must compile clean and, once the callee fully returns (its
// finally chain complete), leave BOTH `a` and `b` disposed exactly once.
// per §21.7, §28.3, v0.298
BEZEL_TEST(cfg_try_finally_early_exit_4180,
           return_through_finally_disposes_nonconflicting_local_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Helper(): int "
        "    var a := new Res(1); "
        "    a.Dispose(); "
        "    var b := new Res(2); "
        "    try "
        "      return 42; "
        "    finally "
        "      b.Dispose(); "
        "    end; "
        "  end Helper; "
        "  public static method Main(): int "
        "    var r := Helper(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 5: NESTED try/finally, early `return` from the innermost body — both
// the inner and outer finally must be reachable and each discharge its own
// resource (the "nested trys, both finally must run" shape). Checked via
// a separate caller so the return value proves the CALLEE's entire
// finally chain (inner AND outer) completed before Res.live is read.
// per §21.7, §28.3, §31.2, v0.298
BEZEL_TEST(cfg_try_finally_early_exit_4180,
           nested_try_finally_both_reachable_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Helper(): int "
        "    var a := new Res(1); "
        "    try "
        "      var b := new Res(2); "
        "      try "
        "        return 7; "
        "      finally "
        "        b.Dispose(); "
        "      end; "
        "    finally "
        "      a.Dispose(); "
        "    end; "
        "  end Helper; "
        "  public static method Main(): int "
        "    var r := Helper(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 6: a LABELED `break` targeting a loop OUTSIDE an inner try, whose
// `finally` disposes an owned resource — the try is nested inside the
// INNER loop, so per `routeExitThroughFinally`'s `loopDepthAtEntry`
// eligibility test the break must hop through the inner try's finally
// (loop entered before the try) on its way to the OUTER (labeled) loop's
// `afterEnd`. Must compile clean and dispose `x` exactly once.
// per §21.5, §28.3, v0.298
BEZEL_TEST(cfg_try_finally_early_exit_4180,
           labeled_break_through_finally_to_outer_loop_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Helper(): int "
        "    outer: while true do "
        "      while true do "
        "        var x := new Res(9); "
        "        try "
        "          break outer; "
        "        finally "
        "          x.Dispose(); "
        "        end; "
        "      end; "
        "    end outer; "
        "    return 0; "
        "  end Helper; "
        "  public static method Main(): int "
        "    var r := Helper(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 7: an ASYNC method (non-iterator) with a `try`/`finally` and an early
// `return` — sweeps the §30.5/§30.9 suspend-locals / completion-edge
// annotation risk the #4146 note names (block-order-sensitive; a `case`
// that used to contribute zero blocks now contributes N+1 was THAT note's
// risk — the analogous risk here is the new finally-clone blocks). No
// `await` crosses the try in this row (the resource's live range never
// crosses a suspend point), so this is the synchronous-suspend-local
// classification path; must compile clean.
// per §21.7, §28.3, §30.1, §30.5, v0.298
BEZEL_TEST(cfg_try_finally_early_exit_4180,
           async_method_try_finally_early_return_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static async method Helper(): Task<int> "
        "    await Task.Delay(0); "
        "    var x := new Res(3); "
        "    try "
        "      return Sink.Take(move x); "
        "    finally "
        "      var y := new Res(4); "
        "      y.Dispose(); "
        "    end; "
        "  end Helper; "
        "  public static async method Main(): Task<int> "
        "    var t := Helper(); "
        "    var v := await t; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 8: a `using var` established BEFORE the `try`, spanning it — the
// try's own early `return` must NOT disturb the using-var's own
// scope-bound discharge (established/torn-down independently of the
// finally clone chain per §29.6's `using var` ≡ `try`/`finally`
// equivalence and §21.6's "one establishment-ordered stack"). Compiles
// clean and disposes exactly once.
// per §21.7, §28.3, §28.10, §29.6, v0.298
BEZEL_TEST(cfg_try_finally_early_exit_4180,
           using_var_spanning_try_early_return_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Helper(): int "
        "    using var u := new Res(6); "
        "    try "
        "      return u.tag; "
        "    finally "
        "      var y := new Res(8); "
        "      y.Dispose(); "
        "    end; "
        "  end Helper; "
        "  public static method Main(): int "
        "    var r := Helper(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
