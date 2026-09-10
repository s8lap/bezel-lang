// Spec-derived regression tests for issue #4194 — `flow::CFG`'s real
// `visitTry` (`src/flow/CFG.cpp`) models "a throwable call somewhere in the
// try body" as a single coarse over-approximating exception edge from the
// body's fall-through tail (`tryBodyEnd`) into `catchDispatch`:
//
//   CFGBlock* tryBodyEnd = inner.runInto(s.body, tryEntry);
//   if (tryBodyEnd) {
//       cfg_.addEdge(tryBodyEnd, finallyEntry ? finallyEntry : afterTry);
//       cfg_.addExceptionEdge(tryBodyEnd, catchDispatch);   // the ONLY
//                                                            // representation
//                                                            // of "a call in
//                                                            // this body
//                                                            // might have
//                                                            // thrown"
//   }
//
// Both edges are gated on `tryBodyEnd` being non-null. #4180 (261baf1) fixed
// the FIRST edge's routing problem for a body ending in a terminator (an
// early return/break/continue was never even reaching its `finally`, since
// `runInto` returns null for a terminator-ended body and the `if (tryBodyEnd)`
// block never ran at all). #4194 is the adjacent, narrower gap #4180 explicitly
// declined to touch: the coarse EXCEPTION edge is gated on the exact same
// null check, so it is ALSO entirely absent whenever the try body ends in a
// terminator — a throwable call earlier in the body (before the return/
// break/continue) gets NO representation of "this might have thrown, landing
// at catchDispatch" at all when there is no explicit `throw` statement
// anywhere in the body (an explicit `throw` already gets its own real,
// unconditional edge via `exceptionTarget()`, untouched by this gap).
//
// MEASURED (this fix's own test file, Row 1): `var x:=new Res(5); try
// t:=t+Sink.Take(move x); return t; catch e:Exception => t:=t+Sink.Take(move
// x); end;` — a double move of `x`, once in the try body's normal path and
// once in the catch arm, reachable ONLY via the coarse exception-edge
// approximation (there is no explicit `throw` in the body) — compiled with
// **zero diagnostics about `x`** on the pre-fix compiler (the catch arm's own
// `Sink.Take(move x)` was never flagged: `catchDispatch` had ZERO
// predecessors at all pre-fix, so `MethodOwnership::runDataflow`'s
// `computeEntry` folds nothing in — `x` is simply absent from the catch arm's
// entry state, so its `move` is unchecked). Post-fix: correctly rejected with
// the real "already moved out" diagnostic.
//
// THE FIX: `Builder::addApproximateTryBodyExceptionEdge` (CFG.cpp) adds the
// SAME coarse `cfg_.addExceptionEdge(from, exceptionTarget())` at each
// `ReturnStmt`/`BreakStmt`/`ContinueStmt` site, scoped to a real (non-arm)
// try-body frame via `!isArmContext` (so it fires while still lexically
// inside a try's own body, at any nesting depth through if/while/nested-try,
// mirroring the scope `tryBodyEnd`'s own edge covers for the fall-through
// case) — see CFG.cpp's comment on that helper for the full reasoning,
// including why `exceptionTarget()` (not `routeExitThroughFinally`) is the
// right target: an exception from within a try's own body always lands at
// THAT try's own `catchDispatch` directly, with nothing being "exited" yet.
//
// SCOPE, EXPLICITLY: a throwable call inside a CATCH ARM's own body has
// never had this coarse modeling either (before or after this fix) — a
// separate, still-open, narrower-still gap, filed as #4201, not touched here.
//
// REVIEW FIX (still #4194, same commit as the review that found it): the
// FIRST version of `addApproximateTryBodyExceptionEdge`'s `!isArmContext`
// guard was itself broken by an existing #4180 structural detail — `visitTry`
// pushed the synthetic `isArmContext=true` frame onto a catch arm's Builder
// only `if (finallyEntry)`. For a try with catch arms and NO finally, no arm
// frame was pushed at all, so an arm's Builder inherited `tryHandlers_`
// UNCHANGED and `.back()` named the OUTER frame instead of "am I inside this
// arm" — the guard silently tested the wrong thing. MEASURED false reject:
// two SIBLING (mutually exclusive) catch arms on a nested try/no-finally,
// each independently `move`ing and returning the SAME local, were wrongly
// rejected as a double-move (compiles clean, runs correctly exit 1, at
// `3999278`; wrongly rejected at the first `a3ebff6` cut of this fix). THE
// FIX: push the arm frame UNCONDITIONALLY (`finallyEntry`/`finallyClones`
// null when there is none) — see visitTry's own comment on that push; both
// `exceptionTarget()` and `routeExitThroughFinally` already treat a
// null-`finallyEntry` frame as "nothing to hop through" and are unaffected.
// Row 7 below pins this mechanically.
//
// per §21.5, §21.7, §28.3, §30.5, §31.2, v0.298, #4194 (adjacent to #4180)

#include <string>

#include "support/aot_support.hpp"        // runAotChecked
#include "support/ownership_support.hpp"  // parseAndCheckOwnership, hasErrorMessage
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// Same destructor-counter idiom #4180's own sibling file
// (cfg_try_finally_early_exit_4180_test.cpp) and #4116/#4117/#4138 use, so
// rows are directly comparable.
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
    "end Sink; "
    "class MyErr: Exception "
    "  public constructor() end constructor; "
    "end MyErr; ";

const std::string kAlreadyMovedMsg =
    "owned value in `x` was already moved out at this point (§28.3)";

// Same message constants as the sibling #4180 file
// (cfg_try_finally_early_exit_4180_test.cpp) — reused verbatim because Rows
// 2/3 below hit the SAME pre-existing `visitWhile` "both edges wired even for
// a literal condition" artifact that file's own Row 2/3 document: a `while`
// loop's back-edge / condition-false-skip-edge is modeled even when the
// condition can statically never take it, so a try nested in a loop reports
// this maybe-consumed FAMILY of diagnostics rather than the direct
// "already moved out" one Row 1 gets outside any loop.
const std::string kMaybeConsumedReadMsg =
    "local `x` may have already been consumed on another branch and may "
    "not be read here (maybe-consumed join, §28.3)";

const std::string kMaybeConsumedConsumeMsg =
    "cannot consume `x`: it may have already been consumed on another "
    "branch (maybe-consumed join, §28.3)";

// Count of ERROR-severity diagnostics containing `needle` as a substring —
// `hasErrorMessage` only answers "at least one", which is not enough for Row
// 3 (continue): the try body's OWN first `move x` already triggers ONE
// instance of `kMaybeConsumedConsumeMsg` pre-fix (the same unrelated
// `visitWhile` artifact noted above, independent of this fix — MEASURED via
// `build/bezelc` directly on the pre-fix binary). This fix adds a SECOND,
// independent instance — the catch arm's own re-move of `x`, reachable only
// via the new coarse exception edge — so the FAIL-WITHOUT signal for Row 3 is
// the COUNT rising from 1 to 2, not mere presence.
int countErrorMessage(const OwnershipResult& r, std::string_view needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) ++n;
    }
    return n;
}

}  // namespace

// ===========================================================================
// Reject-side rows (parseAndCheckOwnership) — the measured bug itself.
// MECHANICALLY VERIFIED (§3/§20), via both this file's own build and direct
// `build/bezelc` probes on equivalent multi-line source (real diagnostics
// carry real locations either way — see each row's own comment for exact
// pre-fix vs post-fix diagnostic text measured):
//   Row 1 (return): FAILS WITHOUT — pre-fix rc=0, ZERO diagnostics about `x`
//     at all (an unsound ACCEPT, exactly the issue's own prediction);
//     post-fix rejects with `kAlreadyMovedMsg` at a real location.
//   Row 2 (break): FAILS WITHOUT — pre-fix already rejects, but ONLY via the
//     unrelated `visitWhile`-artifact "would be dropped here without
//     disposal" diagnostic pointing at the DECLARATION (zero occurrences of
//     the maybe-consumed family); post-fix additionally rejects with
//     `kMaybeConsumedReadMsg`/`kMaybeConsumedConsumeMsg` pointing at the
//     actual double-move sites.
//   Row 3 (continue): FAILS WITHOUT on a NARROWER signal — the maybe-consumed
//     CONSUME message already fires ONCE pre-fix (the try body's own first
//     move, via the same unrelated artifact Row 2's comment names) but rises
//     to TWICE post-fix (the catch arm's own re-move, newly reachable via
//     this fix's edge) — see `countErrorMessage`'s comment.
// 3 of 3 reject rows fail without the fix, each on the specific signal named
// above (not a uniform "0 diagnostics before" claim — Rows 2/3 already
// reject pre-fix for an unrelated reason, per §20).
// ===========================================================================

// Row 1: `return` ends the try body (tryBodyEnd == nullptr pre-fix) with NO
// explicit `throw` anywhere — the ONLY way `catchDispatch` could ever be
// reached is the coarse approximation this fix adds. The catch arm re-moves
// `x`, which the try body's normal path already moved. per §21.7, §28.3, v0.298
BEZEL_TEST(cfg_try_terminator_exception_edge_4194,
           return_ends_try_body_catch_arm_double_move_rejected) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var t := 0; "
        "    try "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    catch e: Exception => "
        "      t := t + Sink.Take(move x); "
        "    end; "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kAlreadyMovedMsg));
}

// Row 2: `break` ends the try body — the try is nested inside a loop, and the
// early exit is a `break` rather than a `return`. Same double-move shape,
// reached only via the coarse exception-edge approximation this fix adds
// (no explicit `throw`). MEASURED pre-fix (direct `build/bezelc` probe on an
// equivalent multi-line program): the compiler ALREADY rejects, but only via
// "owned value in `x` would be dropped here without disposal" pointing at
// the DECLARATION — zero occurrences of either maybe-consumed message.
// Post-fix: the maybe-consumed family additionally fires, pointing at the
// two actual move sites. per §21.5, §28.3, v0.298
BEZEL_TEST(cfg_try_terminator_exception_edge_4194,
           break_ends_try_body_catch_arm_double_move_rejected) {
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
        "      catch e: Exception => "
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

// Row 3: `continue` ends the try body — the loop condition then re-evaluates
// and the loop ends normally. Same double-move shape, reached only via the
// coarse exception-edge approximation this fix adds. MEASURED pre-fix: the
// try body's OWN first `move x` already triggers ONE instance of the
// maybe-consumed CONSUME message via the unrelated `visitWhile` artifact
// (independent of this fix — see `countErrorMessage`'s comment); post-fix a
// SECOND, independent instance appears — the catch arm's own re-move,
// reachable only via this fix's new edge. The FAIL-WITHOUT signal is the
// count rising from 1 to 2, not mere presence (bare presence is NOT a valid
// signal here — §18/§20). per §21.5, §28.3, v0.298
BEZEL_TEST(cfg_try_terminator_exception_edge_4194,
           continue_ends_try_body_catch_arm_double_move_rejected) {
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
        "      catch e: Exception => "
        "        t := t + Sink.Take(move x); "
        "      end; "
        "    end; "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(countErrorMessage(r, kMaybeConsumedConsumeMsg) >= 2);
}

// ===========================================================================
// Accept-side rows — false-reject guards (§15). Rows 4-6 use real execution
// (runAotChecked) to prove the new edge does not disturb legitimate code:
// Row 4 is the "no catches, pass-through to finally" shape (`s.catches.empty()`
// still routes through `catchDispatch`), Row 5 sweeps nested try, Row 6
// sweeps an ASYNC method (the block-order-sensitive suspend-locals /
// completion-edge annotation #4146's architectural note flags as at-risk on
// ANY CFG change).
// ===========================================================================

// Row 4: try/finally with NO catch arms, body ends in `return`, a throwable
// call precedes it — the new coarse edge routes through `catchDispatch`'s
// `catches.empty()` pass-through into `finallyEntry` in ADDITION to the
// (already #4180-fixed) normal-exit routing. The finally disposes a
// DIFFERENT, non-conflicting local; must compile clean and dispose exactly
// once. per §21.7, §28.3, §31.2, v0.298
BEZEL_TEST(cfg_try_terminator_exception_edge_4194,
           return_ends_try_body_no_catch_finally_disposes_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Helper(): int "
        "    var a := new Res(1); "
        "    a.Dispose(); "
        "    var b := new Res(2); "
        "    try "
        "      var u := Sink.Take(new Res(9)); "
        "      return u; "
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

// Row 5: a NESTED try, inner body ends in `return`, both inner and outer try
// have catch arms that do NOT touch the outer try's own resource — must
// compile clean and dispose it exactly once (the new edge on the INNER
// try's return must land at the INNER try's own catchDispatch, not leak
// into the outer one). per §21.7, §28.3, §31.2, v0.298
BEZEL_TEST(cfg_try_terminator_exception_edge_4194,
           nested_try_return_inner_catch_dispatch_scoped_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Helper(): int "
        "    var a := new Res(1); "
        "    try "
        "      try "
        "        var u := Sink.Take(new Res(9)); "
        "        return u; "
        "      catch inner: MyErr => "
        "        return -1; "
        "      end; "
        "    catch outer: Exception => "
        "      return -2; "
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

// Row 6: an ASYNC method with try/catch and an early `return`, a throwable
// call before it — sweeps the §30.5 suspend-locals / completion-edge
// annotation risk (block-order-sensitive per the #4146 architectural note).
// The `await` is INSIDE the try body, before the `return` — `x` is declared
// before the try and consumed AFTER the await, so its live range actually
// CROSSES a suspend point (an earlier revision of this row awaited only
// BEFORE the try, which never exercises the suspend-local state-machine
// path the row's own name implies — the try body ending in `return` was
// entirely synchronous there). Must compile clean and run correctly.
// per §21.7, §28.3, §30.1, §30.5, §30.7, v0.298
BEZEL_TEST(cfg_try_terminator_exception_edge_4194,
           async_method_return_ends_try_body_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static async method Helper(): Task<int> "
        "    var x := new Res(3); "
        "    try "
        "      await Task.Delay(0); "
        "      return Sink.Take(move x); "
        "    catch e: Exception => "
        "      return -1; "
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

// Row 7: REVIEW-FOUND false reject (see this file's header comment, "REVIEW
// FIX"). A nested try with NO finally on either level: the inner try
// (catches `MyErr`) and outer try (catches `Exception`) each have a catch
// arm that independently `move`s the SAME local `x` and returns — the two
// arms are MUTUALLY EXCLUSIVE (only one ever executes), so this is
// idiomatic sibling-cleanup code, not a real double-move. The first cut of
// this fix's `!isArmContext` guard (before `visitTry` pushed the arm frame
// unconditionally) tested the OUTER frame instead of "am I inside this arm"
// whenever there was no finally, and wrongly rejected this as a double-move.
// MEASURED (isolated worktree): compiles clean and returns 1 at `3999278`
// (baseline); wrongly rejected with "owned value in `x` was already moved
// out at this point (§28.3)" at the first `a3ebff6` cut of this fix; clean
// and returns 1 again after pushing the arm frame unconditionally.
// per §21.5, §28.3, §31.1, §31.2, v0.298
BEZEL_TEST(cfg_try_terminator_exception_edge_4194,
           sibling_catch_arms_no_finally_independent_move_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Helper(): int "
        "    var x := new Res(1); "
        "    var t := 0; "
        "    try "
        "      try "
        "        throw new MyErr(); "
        "      catch var e: MyErr => do "
        "        t := t + Sink.Take(move x); "
        "        return t; "
        "      end; "
        "      end; "
        "    catch var outer: Exception => do "
        "      t := t + Sink.Take(move x); "
        "      return t; "
        "    end; "
        "    end; "
        "  end Helper; "
        "  public static method Main(): int "
        "    return Helper(); "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
