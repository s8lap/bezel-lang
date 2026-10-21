// Spec-derived regression tests for issues #4312 and #4313 — the two
// remaining reachable state-pair cells `joinInto`'s "other mismatches"
// catch-all silently poisoned (no diagnostic), found while reviewing
// `fix #4247` (Borrowed ⊓ Disposed, e719d70), plus a consumer site that
// stayed gated Owned-only after that fix widened the flag's producer.
//
// `OwnershipState.hpp`'s own header comment documents the lattice as
// "intentionally NON-COMPLETE — the branch-join operation rejects mismatched
// merges instead of widening to the weaker state." Before this fix,
// `MethodOwnership::joinInto` (Ownership_Checks.cpp) named arms for
// same-state, PolyNull absorption, loop-only Uninitialized widening,
// `ownedBorrowedClash` (Owned⊓Borrowed), `ownedDischargedSplit`
// (Owned⊓{Disposed,MovedFrom}), and `borrowedDischargedSplit`
// (Borrowed⊓Disposed, fix #4247) — but NOT:
//
//   * MovedFrom ⊓ Disposed (issue 4312, repro 1) — an originally-Owned local
//     discharged via `move`/an owned-argument transfer on one branch,
//     `.Dispose()`d on the other.
//   * Borrowed ⊓ MovedFrom (issue 4312, repro 2) — the exact symmetric case
//     `fix #4247`'s own commit body named and explicitly deferred ("left for
//     a follow-up issue").
//
// Both fell through to the catch-all, which sets `OwnershipState::Poisoned`
// with NO diagnostic — silently disabling every later check on the local for
// the rest of the method, exactly as #4247 found for Borrowed⊓Disposed.
//
// Issue #4313 is a THIRD instance of the same shape at a CONSUMER, not a
// join, site: `fix #4247` widened `checkRead`'s `disposedOnSomePath` gate
// from `state == Owned` to also cover `state == Borrowed`, but left the
// lambda-capture read check (`reportMovedFromCapturesIn`'s
// `disposedOnSomePathAtLiteral`) gated `state == Owned` alone, with a new
// OwnershipState.hpp comment claiming the gap was "separately-filed" — no
// such issue existed. Both `disposedOnSomePathAtLiteral` and the sibling
// `maybeConsumedAtLiteral` gate (needed by this fix's own
// `borrowedMovedFromSplit`, below) are widened here too.
//
// MEASURED, NOT MERE HYPOTHESIS (lane C4-A, base f99d7c4 = r18-c4 cycle-4
// tip, which already carries fix #4247 = e719d70 and its two send-back
// corrections): all three repros below compiled with `build/bezelc -c`
// rc=0, ZERO diagnostics, on the pre-fix tree, and crashed under
// `scripts/agent_check.sh --probe --asan` at 200x loop amplification:
//   * MovedFrom⊓Disposed:   AddressSanitizer: SEGV, READ access, near-null
//                            address (matches issue 4312 repro 1 exactly).
//   * Borrowed⊓MovedFrom:   AddressSanitizer: heap-use-after-free, freed
//                            inside `Sink.Take` (matches issue 4312 repro 2).
//   * Lambda-capture UAF:   AddressSanitizer: SEGV inside the lambda body
//                            `lam$0` (matches issue 4313's own repro).
//
// THE MERGE OPERATOR FOR EACH NEW ARM, stated as the lattice argument:
//
//   * MovedFrom ⊓ Disposed -> merges to plain `MovedFrom` (a STATE choice,
//     not a boolean-flag OR/AND — but the same "favor soundness, pick the
//     stricter classification" posture `ownedBorrowedClash` and
//     `explicitDisposeReclaimedSlot`'s AND-merge already apply, one level
//     up). Neither side is Owned, so there is no live discharge obligation
//     to retain; both edges already fully discharged the local. A
//     subsequent read is unsafe on BOTH edges, but `checkRead`'s two plain-
//     state consumers diagnose at DIFFERENT severities — `state ==
//     MovedFrom` is an unconditional §28.3 ERROR (the value may be
//     physically freed by its new owner), `state == Disposed` is only a
//     §28.2 WARNING (the checked-mode disposed-flag backstop makes a
//     dominating use-after-dispose merely undefined-in-unchecked-mode, not
//     definitely-already-freed-by-someone-else). Merging to Disposed would
//     under-diagnose the MovedFrom edge's read with a mere warning; merging
//     to MovedFrom makes every consumer reject with ERROR regardless of
//     which edge is live. The only cost is a false reject of a post-join
//     repeat-`.Dispose()` that would have been the Disposed edge's own
//     idempotent no-op — but that shape is reachable ONLY TOGETHER WITH the
//     MovedFrom edge's genuine use-after-free danger in the same program, so
//     rejecting the whole program is sound, the identical asymmetric choice
//     `ownedBorrowedClash` already makes for Owned-vs-Borrowed.
//
//   * Borrowed ⊓ MovedFrom -> KEEPS the Borrowed side's record (so
//     `source`/`disposeAliasSource` survive the join, mirroring
//     `borrowedDischargedSplit`) and sets `maybeConsumed = true` — per
//     issue 4312's own suggested fix shape, `maybeConsumed` (the MOVED-FROM
//     split's use-after-free marker) rather than `disposedOnSomePath`,
//     because the danger is the MovedFrom side's use-after-free (physically
//     freed by the receiver), not the idempotent-Dispose-shaped danger
//     `disposedOnSomePath` carves an exception for. `maybeConsumed` had
//     never been populated on a Borrowed record before this fix, so setting
//     it is unconditional (no prior value to OR against on this specific
//     arm) — but making it VISIBLE required widening THREE consumer/merge
//     sites from `Owned`-only to also cover `Borrowed`:
//       (a) `checkRead`'s IdentifierExpr `maybeConsumed` arm;
//       (b) `reportMovedFromCapturesIn`'s `maybeConsumedAtLiteral` gate
//           (closing part of #4313's family too);
//       (c) `joinInto`'s OWN same-state OR-merge for `maybeConsumed`
//           (`st.state == o.state` branch) — without this, a NESTED if
//           whose outer join merges two Borrowed records (one
//           maybe-consumed via this new arm, one plain) would silently
//           drop the flag one level up, the identical nested-if gap
//           `borrowedDischargedSplit`'s own same-state OR-merge was already
//           widened to close for `disposedOnSomePath`. `markDisposedByName`
//           and `markMovedByName` already consult `maybeConsumed`
//           unconditionally on state, so neither needed a change.
//
// ON THE LOUD CATCH-ALL (brief's own question): `joinInto`'s "other
// mismatches" catch-all now carries an UNCONDITIONAL (not NDEBUG-stripped)
// internal-error check — `unhandledCoreOwnershipStateJoin`, matching the
// `unhandledAwaitScanExprKind` / `unhandledNullDeclClassConstraintSlot`
// convention already used elsewhere in this compiler (src/common/AwaitScan.cpp,
// src/sema/TypeResolution_Names.cpp) rather than a plain `assert()` (this
// build compiles `-DNDEBUG`, so `assert()` would be silently stripped) — that
// fires if BOTH sides of a mismatch are drawn from the four "core" states
// (Owned, Borrowed, MovedFrom, Disposed). This fix's own audit of the full
// 21-pair state matrix found every core-vs-core mismatch now has a named arm;
// every pair the catch-all is STILL meant to reach involves Uninitialized,
// PolyNull, or Poisoned, each safe by a documented, different mechanism (see
// `Ownership_Checks.cpp`'s catch-all comment). This is deliberately NOT a
// plain `assert()` (stripped by this build's `-DNDEBUG`) and deliberately
// scoped narrowly (not "abort on any catch-all hit") so it cannot fire on the
// three legitimate cases while still catching a REAL regression (an 8th
// OwnershipState value, or a later edit reopening one of the two gaps this
// fix closes) as an immediate, attributable crash instead of a silently
// re-poisoned local.
//
// §28.3 "Local variables, ownership, and borrows" (branch-join /
// conditional-discharge rules), §28.2 (`.Dispose()` idempotence). v0.298.
// #4312, #4313.
//
// Files touched: src/ownership/OwnershipState.hpp (`maybeConsumed` /
// `disposedOnSomePath` field comments), src/ownership/Ownership_Checks.cpp
// (`joinInto`'s same-state `maybeConsumed` OR-merge; new
// `movedFromDischargedSplit` / `borrowedMovedFromSplit` arms; the new
// `isCoreOwnershipStateForJoinAudit` / `unhandledCoreOwnershipStateJoin`
// helpers and the catch-all's loud audit call; `checkRead`'s `maybeConsumed`
// gate; `reportMovedFromCapturesIn`'s `maybeConsumedAtLiteral` /
// `disposedOnSomePathAtLiteral` gates).

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked — the AOT oracle
#include "support/aot_result_asserts.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// The MovedFrom-state plain read diagnostic — what a MovedFrom⊓Disposed
// merge's subsequent read now hits (checkRead's `state == MovedFrom` arm).
const char* kMovedFromReadNeedle =
    "was already moved out at this point";

// The maybe-consumed join diagnostic — what a Borrowed⊓MovedFrom merge's
// subsequent read now hits (checkRead's widened `maybeConsumed` arm).
const char* kMaybeConsumedNeedle =
    "may have already been consumed on another branch";

// The lambda-capture disposed-on-some-path diagnostic (#4313's own gate).
const char* kCaptureDisposedNeedle =
    "closure captures";

const char* kCountingPrelude =
    "module M; "
    "class Counter "
    "  public static Dropped: int := 0; "
    "end Counter; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor Counter.Dropped := Counter.Dropped + 1; end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int "
    "    var v := r.tag; "
    "    r.Dispose(); "
    "    return v; "
    "  end Take; "
    "end Sink; "
    "class C "
    "  public static method Main(): int ";

const char* kEpilogue =
    "  end Main; "
    "end C; ";

std::string wrapCounting(const std::string& body) {
    return std::string(kCountingPrelude) + body + kEpilogue;
}

}  // namespace

// ============================================================================
// PRIMARY REGRESSION — the issues' own repro shapes, verbatim (bool-
// parameterized Main, matching the issue text), checked via the ownership
// pass directly (compile-time rejection, no execution needed).
// ============================================================================

// Issue #4312 repro 1 — MovedFrom ⊓ Disposed.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           issue_4312_repro1_movedfrom_disposed_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    if cond then "
        "      x.Dispose(); "
        "    else "
        "      var s := Sink.Take(move x); "
        "    end; "
        "    var t := x.tag; "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMovedFromReadNeedle));
}

// Issue #4312 repro 2 — Borrowed ⊓ MovedFrom.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           issue_4312_repro2_borrowed_movedfrom_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "      var s := Sink.Take(move y); "
        "    end; "
        "    var t := y.tag; "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedNeedle));
}

// Issue #4313 — lambda-capture read of a Borrowed+disposedOnSomePath local.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           issue_4313_lambda_capture_borrowed_disposed_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "      y.Dispose(); "
        "    end; "
        "    var f := lambda(): int => y.tag; "
        "    var t := f(); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kCaptureDisposedNeedle));
}

// A lambda-capture read of a Borrowed+maybeConsumed local — the sibling of
// the row above, exercising `maybeConsumedAtLiteral`'s own widening (this
// fix's OTHER half of #4313's family, needed by `borrowedMovedFromSplit`).
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           lambda_capture_borrowed_movedfrom_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "      var s := Sink.Take(move y); "
        "    end; "
        "    var f := lambda(): int => y.tag; "
        "    var t := f(); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedNeedle));
}

// ============================================================================
// AXIS GRID — which merge shapes reach each new mismatched join. Each row
// disposes/moves `x` inside the SAME branch (not after the join) to isolate
// just this fix's own diagnostic from the pre-existing #4225 consume-freeze
// check, mirroring `borrowed_disposed_branch_join_4247_test.cpp`'s own grid.
// ============================================================================

// MovedFrom⊓Disposed via a `case` arm — §19/§28.8's arm-to-arm merge routes
// through the same `joinInto` machinery.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           axis_movedfrom_disposed_case_arm_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    case cond of "
        "      true => do x.Dispose(); end, "
        "      _ => do var s := Sink.Take(move x); end "
        "    end case; "
        "    var t := x.tag; "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMovedFromReadNeedle));
}

// MovedFrom⊓Disposed via a loop back-edge (`unionKeys` merge) — the
// discharge is conditional INSIDE the loop body, so the loop header's own
// entry/exit meet is the mismatched join.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           axis_movedfrom_disposed_loop_header_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var i := 0; "
        "    while i < 3 do "
        "      if i = 1 then "
        "        x.Dispose(); "
        "      else "
        "        var s := Sink.Take(move x); "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    var t := x.tag; "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Borrowed⊓MovedFrom via an if-WITHOUT-else — the core repro shape,
// isolated (no `x` reassignment after the join).
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           axis_borrowed_movedfrom_if_without_else_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "      var s := Sink.Take(move y); "
        "    end; "
        "    var t := y.tag; "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedNeedle));
}

// Borrowed⊓MovedFrom via a `case` arm.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           axis_borrowed_movedfrom_case_arm_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    case cond of "
        "      true => do "
        "        y := new Res(1); "
        "        var s := Sink.Take(move y); "
        "      end, "
        "      _ => () "
        "    end case; "
        "    var t := y.tag; "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedNeedle));
}

// Borrowed⊓MovedFrom via a loop back-edge.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           axis_borrowed_movedfrom_loop_header_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    var i := 0; "
        "    while i < 3 do "
        "      if i = 1 then "
        "        y := new Res(1); "
        "        var s := Sink.Take(move y); "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    var t := y.tag; "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedNeedle));
}

// Borrowed⊓MovedFrom via a NESTED if — the outer join sees Borrowed (outer
// not-taken arm) merged against Borrowed+maybeConsumed (outer taken arm,
// produced by the INNER if's own `borrowedMovedFromSplit`). This is the row
// that pins the same-state OR-merge widening for `maybeConsumed`: without
// it, the outer join's `st.state == o.state` branch would silently drop the
// flag one level up.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           axis_borrowed_movedfrom_nested_if_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(outerCond: bool, innerCond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if outerCond then "
        "      if innerCond then "
        "        y := new Res(1); "
        "        var s := Sink.Take(move y); "
        "      end; "
        "    end; "
        "    var t := y.tag; "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedNeedle));
}

// ============================================================================
// ACCEPT-SIDE CONTROLS (§15) — the expensive direction. Every row below must
// keep compiling AND (where it executes) run ASan/LSan-clean.
// ============================================================================

// B1 — if-without-else, MOVES on the taken path, never touched again after
// the join. The disposal-obligation warning for `x` never firing (it WAS
// discharged on both paths) is the only diagnostic difference from a plain
// Owned local; nothing about a mismatched-join rejection.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           accept_movedfrom_disposed_no_further_touch_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    if cond then "
        "      x.Dispose(); "
        "    else "
        "      var s := Sink.Take(move x); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// B2 — if-without-else, BOTH arms `.Dispose()` (same-state Disposed⊓Disposed
// — does not reach either new arm at all). A repeat post-join read must
// still be flagged by the PRE-EXISTING use-after-dispose WARNING, not an
// ERROR, and must NOT hit either new arm's needle.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           accept_both_arms_dispose_stays_disposed_not_movedfrom_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    if cond then "
        "      x.Dispose(); "
        "    else "
        "      x.Dispose(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// B3 — if-without-else, BOTH arms `move` (same-state MovedFrom⊓MovedFrom —
// does not reach either new arm). Accepted with no further touch.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           accept_both_arms_move_stays_movedfrom_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    if cond then "
        "      var s1 := Sink.Take(move x); "
        "    else "
        "      var s2 := Sink.Take(move x); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// B4 — Borrowed side, if-without-else, REBIND to a fresh owned value with NO
// discharge at all in the taken arm (stays Owned, not Disposed/MovedFrom —
// this is the PRE-EXISTING `ownedBorrowedClash`-adjacent same-state-Borrowed
// case, untouched by this fix). A read after the join must be accepted as a
// plain borrow read (`y` never became non-Borrowed on the taken arm either,
// since `y := new Res(1);` alone, no discharge, leaves the fresh value
// simply Owned until IT is itself disposed — but the MERGE here is actually
// Owned(taken)⊓Borrowed(not-taken), i.e. `ownedBorrowedClash`, which SHOULD
// reject; this row exists to confirm that PRE-EXISTING rejection is
// unaffected by this fix, not to assert acceptance).
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           control_rebind_no_discharge_hits_preexisting_clash_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      y := new Res(1); "
        "    end; "
        "    var t := y.tag; "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "branch ownership conflict"));
}

// B5 — Borrowed side, a REBIND to ANOTHER BORROW in the taken arm, no
// dispose/move anywhere (stays Borrowed/Borrowed — same-state, never a
// Borrowed/MovedFrom split). Accepted, runs clean.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           accept_rebind_to_another_borrow_no_discharge_28_3) {
    JitResult jr = runAotChecked(wrapCounting(
        "var cond := true; "
        "var x := new Res(5); "
        "var z := new Res(7); "
        "var y := x; "
        "if cond then "
        "  y := z; "
        "end; "
        "x.Dispose(); "
        "z.Dispose(); "
        "return Counter.Dropped;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 2);  // x and z, independently disposed
}

// B6 — NOT an accept-side control (moved here from that section after
// measurement corrected the premise it was written under — see below).
// MovedFrom⊓Disposed, REBOUND to a fresh owned value after the join, no
// further USE of the fresh value beyond its own `.Dispose()`.
//
// The ORIGINAL premise for this row was "a rebind clears `maybeConsumed`/
// state regardless of the prior merge (handleAssignment's unconditional
// reset), so this must compile clean and run ASan-clean on BOTH sides" —
// i.e. a genuine accept-side control. MEASURED, that premise was WRONG for
// the pre-fix side: run against the reverted (pre-fix) tree, this exact
// program is a SECOND, INDEPENDENT crash — `AddressSanitizer:
// heap-use-after-free`, freed inside `bz$M.Sink$Take`, READ in `main` at
// the `x := new Res(9);` REASSIGNMENT statement itself (not at any read of
// `x`'s value afterward, which this program never does). So the pre-fix
// silent-poison defect is not only a missed compile-time diagnostic on a
// later READ (the shape #4312/#4313's own repros exercise) — poisoning `x`
// ALSO corrupts what codegen emits for the very REASSIGNMENT that would
// otherwise discharge the danger, independent of whether the poisoned
// value is ever read again. This fix's `movedFromDischargedSplit` arm
// resolves BOTH: post-fix this program runs ASan-clean, 100x amplified,
// with the exact drop count below. Kept as a REGRESSION GUARD (not an
// accept-side control): it is EXPECTED, not a false-reject worry, that this
// row fails pre-fix.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           regression_movedfrom_disposed_rebind_after_join_asan_100x_28_3) {
    JitResult jr = runAotChecked(wrapCounting(
        "var i := 0; "
        "while i < 100 do "
        "  var cond := (i mod 2 = 0); "
        "  var x := new Res(5); "
        "  if cond then "
        "    x.Dispose(); "
        "  else "
        "    var s := Sink.Take(move x); "
        "  end; "
        "  x := new Res(9); "
        "  x.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return Counter.Dropped;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    BEZEL_EXPECT_TRUE(jr.ran);
    // Every iteration discharges exactly TWO Res objects: Res(5) once
    // (`.Dispose()` on the cond-true arm, or `Sink.Take(move x)`'s own
    // internal `.Dispose()` on the cond-false arm — either way, exactly one
    // drop), then Res(9) once via the post-join rebind's own `.Dispose()`.
    // 100 iterations * 2 drops = 200.
    BEZEL_EXPECT_EQ(jr.exitCode, 200);
}

// B7 — Borrowed⊓MovedFrom, disposes `x` (the borrow SOURCE, not `y`) after
// the join, `y` never touched again. Executes under the strict AOT oracle,
// ASan/LSan, 100x loop-amplified.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           accept_borrowed_movedfrom_dispose_x_no_y_touch_asan_100x_28_3) {
    JitResult jr = runAotChecked(wrapCounting(
        "var i := 0; "
        "while i < 100 do "
        "  var cond := (i mod 2 = 0); "
        "  var x := new Res(5); "
        "  var y := x; "
        "  if cond then "
        "    y := new Res(1); "
        "    var s := Sink.Take(move y); "
        "  end; "
        "  x.Dispose(); "
        "  i := i + 1; "
        "end; "
        "return Counter.Dropped;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(jr));
    BEZEL_EXPECT_TRUE(jr.ran);
    // 100 iterations: 50 taken-arm iterations discharge 2 objects (Res(1)
    // via Sink.Take's internal Dispose + Res(5) via x.Dispose()), 50
    // not-taken-arm iterations discharge 1 (Res(5) only) = 50*2 + 50*1 = 150.
    BEZEL_EXPECT_EQ(jr.exitCode, 150);
}

// B8 — nested if, Borrowed⊓MovedFrom on the innermost taken path, never
// touched again anywhere. Pins that the same-state OR-merge widening for
// `maybeConsumed` does not itself introduce a spurious rejection when there
// is genuinely no later use.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           accept_nested_if_borrowed_movedfrom_no_further_touch_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(outerCond: bool, innerCond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if outerCond then "
        "      if innerCond then "
        "        y := new Res(1); "
        "        var s := Sink.Take(move y); "
        "      end; "
        "    end; "
        "    x.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// B9 — NEITHER arm discharges `y` at all; `y` is read as a plain borrow both
// inside the `if` and after the join. No MovedFrom/Disposed state anywhere.
BEZEL_TEST(joinInto_core_state_matrix_4312_4313, accept_neither_discharges_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(cond: bool): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    if cond then "
        "      var t1 := y.tag; "
        "    end; "
        "    var t2 := y.tag; "
        "    x.Dispose(); "
        "    return t2; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// RUNTIME SEVERITY REGRESSION GUARDS — the pre-fix defect's own measured
// crashes, loop-amplified 200x, pinned as compile-time REJECTIONS (matching
// `borrowed_disposed_branch_join_4247_test.cpp`'s own guard row: the crash
// itself was already measured directly via `scripts/agent_check.sh --probe
// --asan` — see the file header — so re-deriving it here would require
// reverting the fix, out of place in a regression test).
// ============================================================================

BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           axis_movedfrom_disposed_loop_amplified_rejected_not_a_crash_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var sum := 0; "
        "    while i < 200 do "
        "      var cond := (i mod 2 = 0); "
        "      var x := new Res(5); "
        "      if cond then "
        "        x.Dispose(); "
        "      else "
        "        var s := Sink.Take(move x); "
        "      end; "
        "      var t := x.tag; "
        "      sum := sum + t; "
        "      i := i + 1; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMovedFromReadNeedle));
}

BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           axis_borrowed_movedfrom_loop_amplified_rejected_not_a_crash_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public static method Take(owned r: Res): int "
        "    var v := r.tag; "
        "    r.Dispose(); "
        "    return v; "
        "  end Take; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var sum := 0; "
        "    while i < 200 do "
        "      var x := new Res(5); "
        "      var y := x; "
        "      var cond := true; "
        "      if cond then "
        "        y := new Res(1); "
        "        var s := Sink.Take(move y); "
        "      end; "
        "      x := new Res(9); "
        "      var t := y.tag; "
        "      x.Dispose(); "
        "      sum := sum + t; "
        "      i := i + 1; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kMaybeConsumedNeedle));
}

BEZEL_TEST(joinInto_core_state_matrix_4312_4313,
           issue_4313_lambda_capture_loop_amplified_rejected_not_a_crash_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var sum := 0; "
        "    while i < 200 do "
        "      var cond := true; "
        "      var x := new Res(5); "
        "      var y := x; "
        "      if cond then "
        "        y := new Res(1); "
        "        y.Dispose(); "
        "      end; "
        "      var f := lambda(): int => y.tag; "
        "      var t := f(); "
        "      x.Dispose(); "
        "      sum := sum + t; "
        "      i := i + 1; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kCaptureDisposedNeedle));
}
