// Spec-derived regression tests for issue #4201 — `flow::CFG`'s `visitTry`
// (`src/flow/CFG.cpp`) never modeled "a throwable call inside a CATCH ARM's
// own body" as a possible exception source at all, before or after #4180
// (261baf1) or #4194 (0a0d8ef). #4194 extended the coarse "a throwable call
// somewhere in this body might have thrown" over-approximation
// (`Builder::addApproximateTryBodyExceptionEdge`) to a TRY body's own
// terminator-ending statements, but deliberately excluded `isArmContext`
// frames — a catch arm's own body had NO exception-edge modeling for a
// throwable call that was not an explicit `throw`, in either shape:
//
//   CFGBlock* armEnd = armEntry;
//   if (arm.body) armEnd = armInner.visitStmt(*arm.body, armEntry);
//   if (armEnd) {
//       cfg_.addEdge(armEnd, finallyEntry ? finallyEntry : afterTry);  // the
//                                                                       // ONLY
//                                                                       // edge
//   }
//
// (fall-through tail — no exception edge at all) and every `ReturnStmt` /
// `BreakStmt` / `ContinueStmt` site inside the arm body, where
// `addApproximateTryBodyExceptionEdge` early-returned on
// `tryHandlers_.back().isArmContext`.
//
// THE FIX (two halves, both landing together — a partial model covering only
// the terminator case, or only the fall-through case, would have been an
// inconsistent half-fix): `addApproximateTryBodyExceptionEdge` no longer
// excludes `isArmContext` frames (it already computed the CORRECT target for
// them via `exceptionTarget()`, which #4180 built specifically so a throw
// escaping an arm hops through that try's own finally before landing at the
// OUTER target — the guard was the only thing stopping it from firing here);
// and the arm-building loop in `visitTry` adds the mirror image of
// `tryBodyEnd`'s own fall-through exception edge for `armEnd`:
// `cfg_.addExceptionEdge(armEnd, armInner.exceptionTarget())`.
//
// THE CORE INVARIANT this preserves (unchanged by this fix, #4180's own
// design): a throw escaping a catch arm's own body is NEVER caught by that
// arm's OWN try — it propagates to the enclosing one (or the function exit,
// if there is none). Both halves of this fix route to `exceptionTarget()`,
// which already encodes exactly that.
//
// per §21.5, §21.7, §28.3, §31.2, v0.298, #4201 (closes the gap #4194 and
// #4180 both deliberately declined to touch)

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

// Same destructor-counter idiom the #4180/#4194 sibling files use, so rows
// are directly comparable.
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

}  // namespace

// ===========================================================================
// Reject-side row — the measured bug, fall-through half.
// ===========================================================================

// Row 1: the FALL-THROUGH half of the fix. Same nested-try shape as the
// sibling #4194 file's Row 7 (`sibling_catch_arms_no_finally_independent_
// move_rejected_4201`, updated in that file's own commit for this same
// issue) — an inner try whose OWN body is an explicit `throw new MyErr()`
// (so the inner try's own `tryBodyEnd` is null and contributes nothing to
// the outer try's coarse edge, keeping this row's signal isolated to the
// ARM'S OWN fall-through tail) — but here BOTH catch arms FALL THROUGH
// (no `return`) instead of ending in one, so control reaches the coarse
// exception edge added at `armEnd` (the second half of the fix), not the
// one added at a `ReturnStmt` site (the first half, already pinned by the
// #4194 file's updated Row 7).
//
// MEASURED (isolated worktree, direct `build/bezelc` probe on this exact
// multi-line source): pre-fix compiles CLEAN, `Main` returns `Res.live == 0`
// (an unsound ACCEPT — the inner arm's own `Sink.Take(move x)` has no CFG
// representation of "this might have thrown before falling off the arm's
// end", so the outer arm's own `Sink.Take(move x)` is never flagged as a
// second move of the same local); post-fix rejects with `kAlreadyMovedMsg`
// at the outer arm's move site. FAILS WITHOUT / PASSES WITH.
// per §21.5, §28.3, §31.1, §31.2, v0.298, #4201
BEZEL_TEST(cfg_catch_arm_exception_edge_4201,
           nested_sibling_catch_arms_fallthrough_double_move_rejected) {
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
        "      end; "
        "      end; "
        "    catch var outer: Exception => do "
        "      t := t + Sink.Take(move x); "
        "    end; "
        "    end; "
        "    return t; "
        "  end Helper; "
        "  public static method Main(): int "
        "    return Helper(); "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kAlreadyMovedMsg));
}

// ===========================================================================
// Accept-side controls (§15 — a fix that makes the compiler reject more
// must be swept against the legitimate shapes adjacent to what it now
// rejects, or a false-reject would hide in an all-reject test file).
// ===========================================================================

// Row 2: sibling catch arms that do NOT share a resource — each constructs
// and immediately consumes its OWN local `Res`, so there is no possible
// double-move regardless of how conservatively the coarse "might have
// thrown" edge is modeled. Must compile clean and dispose exactly twice
// across the two REAL constructions that occur along the one real run
// (inner arm's own construction; the outer arm's is dead code on every real
// execution here, but still needs no diagnostic — it disposes whatever it
// constructs on its own path). per §21.5, §28.3, §31.2, v0.298
BEZEL_TEST(cfg_catch_arm_exception_edge_4201,
           nested_sibling_catch_arms_independent_resources_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Helper(): int "
        "    var t := 0; "
        "    try "
        "      try "
        "        throw new MyErr(); "
        "      catch var e: MyErr => do "
        "        t := t + Sink.Take(new Res(1)); "
        "      end; "
        "      end; "
        "    catch var outer: Exception => do "
        "      t := t + Sink.Take(new Res(2)); "
        "    end; "
        "    end; "
        "    return t; "
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

// Row 3: a SINGLE-LEVEL try/catch (no enclosing try, no finally) whose catch
// arm falls through after a throwable call that consumes a resource created
// INSIDE the arm — exercising the new `armEnd` exception edge with
// `exceptionTarget()` resolving to `cfg_.exit()` directly (there is no
// outer frame to hop through or land at), which is also the
// core-invariant boundary case: the arm's own coarse "might have thrown"
// must NOT loop back to this try's OWN `catchDispatch` (that would be
// unsound in the other direction — an arm catching its own escape). A
// separate, unrelated resource `w`, declared before the try and used only
// AFTER it, must be unaffected. Must compile clean and dispose both exactly
// once. per §21.7, §28.3, §31.2, v0.298
BEZEL_TEST(cfg_catch_arm_exception_edge_4201,
           single_level_catch_arm_fallthrough_no_self_catch_accepted) {
    std::string src =
        std::string(kPrelude) +
        "class C "
        "  public static method Helper(): int "
        "    var w := new Res(4); "
        "    var t := 0; "
        "    try "
        "      var z := new Res(3); "
        "      t := t + Sink.Take(move z); "
        "    catch e: Exception => do "
        "      t := t + Sink.Take(new Res(5)); "
        "    end; "
        "    end; "
        "    t := t + Sink.Take(move w); "
        "    return t; "
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
