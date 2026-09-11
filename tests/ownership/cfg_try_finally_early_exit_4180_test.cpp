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
// MECHANICALLY VERIFIED (§3/§20) AGAINST #4180's OWN PARENT COMMIT: reverting
// src/flow/CFG.cpp + src/ownership/Ownership.cpp to that commit's parent with
// this test file kept — 3 of these 4 rows FAIL WITHOUT the #4180 fix (Row 1
// `return`: both assertions fail, rc=0 with zero diagnostics about `x`
// pre-fix — Row 1 is also the shape measured directly under ASan,
// heap-use-after-free on first execution, see the #4180 commit body; Row 2
// `break`: at #4180 landing time the STRONGER `kAlreadyMovedMsg` assertion
// failed pre-#4180-fix, though the row already rejected for an unrelated,
// narrower reason; Row 4 catch-arm-throw: both assertions fail pre-fix). Row
// 3 `continue` passes IDENTICALLY on both sides of #4180 — NOT a
// fail-without row for #4180; see its own comment for why. PASS-WITH (at
// #4180 landing time): all 4 correctly rejected, with a real location.
//
// narrow #4194 (§21.5/§28.3, v0.298): Row 2's assertion was UPDATED (see its
// own comment) after #4194's fix changed WHICH diagnostic that one row's
// program gets — still `hasErrors()` either side of #4194, so this is not a
// soundness change, just a wording one this file's assertion now tracks.
// ===========================================================================

// Row 1: `return` — the issue's own filed repro, non-lambda (the real
// per-method CFG, `treatTryAsOpaque=false`). The try body moves `x` and
// returns; `finally` also moves `x`. Before this fix `finallyEntry` had no
// predecessor reachable from `cfg_.entry()` at all (verified: neither the
// normal-exit edge `if (tryBodyEnd)` nor the `catches.empty()` pass-through
// through `catchDispatch` reaches it when the try body itself ends in
// `return` and no `throw`/throwable-call is textually present), so the
// finally's move went unanalyzed. per §21.7, §28.3, v0.298
//
// ⚠ KNOWN GAP, filed/closed as issue 4211 (this row's half of it): this row
// no longer DISCRIMINATES a regression in #4180's OWN clone-based routing
// mechanism (`routeExitThroughFinally`/`getOrMakeFinallyClone`) either —
// same root cause as Row 2's own note below, confirmed by MEASUREMENT (not
// just the issue's own "reasoned suspicion") with the identical 3-variant
// methodology: reverting `ReturnStmt`'s call to `routeExitThroughFinally`
// to its pre-#4180 direct edge (`cfg_.addEdge(from, cfg_.exit())`), while
// keeping #4194's `addApproximateTryBodyExceptionEdge` intact, leaves this
// row's assertions passing UNCHANGED. Reason: for this shape (single,
// unnested try/finally, no catch arms), #4194's coarse edge from the
// `return`'s own block ALWAYS reaches `catchDispatch`, and since
// `s.catches.empty()`, `catchDispatch`'s unconditional pass-through ALWAYS
// then reaches the REAL (non-clone) `finallyEntry` too — carrying the SAME
// already-moved state as the `routeExitThroughFinally` edge, because both
// edges originate from the SAME block. This is structural to the shape, not
// a defect in this row: see `tests/flow/cfg_finally_clone_routing_4211_test.cpp`
// (fix #4211) for a STRUCTURAL CFG-edge test that isolates
// `routeExitThroughFinally` specifically, immune to this contamination.
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
// enclosing loop instead of a `return`. Measured pre-#4180 (this commit's own
// parent): REJECTED, but for the WRONG (narrower) reason — only "owned value
// in `x` would be dropped here without disposal", pointing at the
// declaration, not the finally's move. This WAS a FAIL-WITHOUT row against
// that baseline: `kAlreadyMovedMsg` (the finally's real double-move, reached
// via the break's own `routeExitThroughFinally` CLONE of the finally body)
// was NOT among the pre-#4180 diagnostics — mechanically confirmed at the
// time by reverting src/flow/CFG.cpp + src/ownership/Ownership.cpp to this
// commit's parent with this test file kept (see the commit body).
//
// narrow #4194 (§21.5/§28.3, v0.298) UPDATED this row's assertion: #4194's
// fix additionally makes the try's OWN (non-clone) `finallyEntry` reachable
// for the FIRST time in this exact shape too — via `catchDispatch`'s
// pre-existing `catches.empty()` pass-through, now fed by #4194's new coarse
// exception edge on the `break` statement — so the finally body is now
// analyzed via TWO independent CFG paths: the #4180 CLONE (reached directly
// from the break's own block, carrying a clean already-moved `x`) and the
// REAL finally (reached via catchDispatch, carrying the SAME already-moved
// `x`, but whose own exit — `afterTry`, unlike the clone's `afterEnd` target
// — feeds the enclosing `while true`'s back edge). That back edge is the
// SAME pre-existing `visitWhile` "both edges wired even for a literal
// condition" artifact Row 2/3 already named (#4180's issue called it
// "circumstantial"): it carries the real finally's post-move state of `x`
// around into the try body's OWN first `move x` on the (statically dead, but
// CFG-modeled) next iteration, downgrading BOTH sites from the CLONE's clean
// "already moved out" to the weaker maybe-consumed family. MEASURED (this
// exact program, both binaries built from an isolated worktree): pre-#4194
// (i.e. #4180 alone) — `kAlreadyMovedMsg` at the finally's move + "would be
// dropped" at the declaration, no maybe-consumed text at all; post-#4194 —
// `kAlreadyMovedMsg` and "would be dropped" are BOTH GONE, replaced entirely
// by `kMaybeConsumedReadMsg`/`kMaybeConsumedConsumeMsg` at the try body's
// first move AND (twice — once per CFG copy of the finally body) at the
// finally's own move. Still `hasErrors()` either way — this is a
// diagnostic-WORDING change on an already-rejecting program, not a
// soundness regression: assert the family Row 3 already uses instead of the
// single message that no longer fires in THIS shape.
//
// ⚠ KNOWN GAP, closed as issue 4211 by accepting the narrower claim (this
// row's own suggested option 2): this row no longer DISCRIMINATES a
// regression in #4180's OWN clone-based routing mechanism
// (`routeExitThroughFinally`/`getOrMakeFinallyClone`) specifically.
// MEASURED (three isolated `CFG.cpp` variants built against this exact
// program, `/var/tmp/bezel-scratch/r18/iso-4211-neutered`): with #4180's
// `break` routing reverted to its pre-#4180 direct edge (so the clone does
// nothing) but #4194's new coarse exception edge kept, the SAME
// maybe-consumed diagnostics still fire — #4194's edge alone is now
// sufficient to make the real (non-clone) finally reachable and trip the
// SAME unrelated back-edge artifact. If #4180's clone routing regressed
// tomorrow, this row would not catch it — it would still report the same
// maybe-consumed diagnostics via #4194's edge alone. This row (and Row 1,
// see its own note above — measured the same way, same root cause) now
// primarily pins "the try body + finally still reject a real double-move in
// this shape", not "#4180's specific routing mechanism".
// `tests/flow/cfg_finally_clone_routing_4211_test.cpp` (fix #4211) carries a
// DIFFERENT, uncontaminated regression test for `routeExitThroughFinally`
// specifically — a direct structural assertion on the CFG's own edges,
// immune to #4194's coarse-edge contamination (its target block carries no
// `stmts`, so it can never be mistaken for a finally clone's entry). per
// §21.5, §28.3, v0.298
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
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedReadMsg) ||
                       hasErrorMessage(r, kMaybeConsumedConsumeMsg));
}

// Row 3: `continue` — same shape, early exit is `continue` (the loop
// condition then re-evaluates false and the loop ends normally). NOT a
// FAIL-WITHOUT row (measured, do not assume): this exact maybe-consumed
// diagnostic already fires on the pre-fix compiler too — the SAME
// `visitWhile` "both edges wired even for a literal condition" artifact
// noted on Row 2 incidentally produces a (differently-reasoned, but
// textually matching one of these two messages) maybe-consumed rejection
// for the continue shape specifically, independent of this fix. This is a
// reject-side row that passes IDENTICALLY on both sides (§20): kept as a
// control pinning that the now-correctly-routed finally continues to
// produce a real diagnostic here, not a regression check for this
// specific fix. per §21.5, §28.3, v0.298
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
