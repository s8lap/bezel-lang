// Ownership — `MethodOwnership::checkRead`'s plain `CallExpr` argument loop
// recursed into EVERY argument value as a plain read, with no carve-out for a
// `move`-operator argument — unlike its own `NewExpr` ctor-argument arm (and
// every arm the #3971 fix added), which routes operands through the shared
// `readOperand` helper that DECLINES a top-level consuming form
// (`isTopLevelConsumingForm`: `move x` / `await x`) so `recordMovesIn` is the
// sole authority that validates and marks it. The bare call's argument loop
// was the one position that still ran the unguarded `checkRead(*a.value,
// false)` — so `checkRead`'s own (correct, for a plain read) Warning-and-
// poison side effect fired FIRST and masked `recordMovesIn`'s more precise
// Error.
//
// Spec, v0.294:
//   §28.3 "Local variables, ownership, and borrows" — the move/disposal
//     state machine `markMovedByName` enforces ("a `Disposed` local may not
//     be moved from").
//   §28.2 "The `IDisposable` contract" — the use-after-dispose Warning
//     `checkRead`'s `IdentifierExpr` arm reports for a PLAIN read of a
//     disposed local (correct there; wrong diagnostic for a `move` site).
//
// THE BUG (measured pre-fix on r15-c3-batch base, carrying #4003 fix 7b37af7
// and #4028 fix 4d40c6f):
//
//   using var h := new Holder(move x);   // x already .Dispose()d
//     -> REJECTED: "cannot move from disposed local `x` (§28.3)".
//   var y := Factory.Make(move x);       // byte-identical move, as a BARE
//                                        // call's argument — no wrapper
//     -> ACCEPTED (only the unrelated §28.2 Warning + the auto-drop
//        must-discharge Warning on `y`, both non-fatal). Compiled and run
//        under `--sanitize=address`: SEGV (null READ) inside
//        `bz$M.Holder$ctor`, called from `bz$M.Factory$Make` — the disposed
//        `x`'s freed storage read as if valid. Measured (this lane):
//        `/var/tmp/bezel-scratch/r15/4023/logs/l6-4023-probe-pre.log`.
//
// ROOT CAUSE: `checkRead`'s `CallExpr` arm (Ownership_Checks.cpp) walks the
// WHOLE initializer expression BEFORE `recordMovesIn` runs (documented
// ordering: `handleLocalDecl` calls `checkRead` first, then
// `recordMovesIn`). For a `move x` argument, the arm's blanket
// `checkRead(*a.value, false)` recurses through the generic `UnaryExpr` arm
// into the IDENTIFIER `x`, whose `IdentifierExpr` arm sees `state ==
// Disposed`, reports the §28.2 WARNING (correct for a plain read; wrong
// diagnostic for a move site) and then — since this runs in the final,
// unsuppressed pass — POISONS `x`'s tracked state
// (`OwnershipState::Poisoned`). By the time `recordMovesIn` ->
// `recordMovesInArgs` -> the bare-`move`-identifier branch ->
// `markMovedByName("x", ...)` runs, `x`'s state is no longer `Disposed` —
// `markMovedByName` has arms only for `MovedFrom` and `Disposed`; `Poisoned`
// matches neither, so it falls through SILENTLY (poisoning is a deliberate
// cascade-suppressor, by design silent — see its own comment) and the
// precise "cannot move from disposed local `x`" ERROR never fires.
//
// THE FIX: the `CallExpr` arm's argument loop gained the SAME
// `isTopLevelConsumingForm` carve-out `readOperand` already applies to every
// other read position `checkRead` walks (the `NewExpr` ctor-argument arm,
// and every arm #3971 added) — skip the read-check entirely for a `move`/
// `await` top-level argument, deferring wholly to `recordMovesIn`, which
// already correctly reports on the ORIGINAL (unpoisoned) Disposed/MovedFrom
// state via `markMovedByName`. Only the `move` half is exercised below
// (`await` shares the identical `isTopLevelConsumingForm` predicate and is
// already covered at its other four call sites — Ownership.cpp
// `handleLocalDecl`/`handleAssignment`'s RHS checks and this file's
// `readOperand` — constructing a fresh, spec-correct `async`/`Task` program
// to exercise the identical code path here would not add coverage of a
// DIFFERENT mechanism, so it is declined per CLAUDE.md's decline-with-
// reasons discipline rather than bolted on).
//
// per §28.2, §28.3 K3.4, v0.294, #3971, #3941, #4023

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot — the strong AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

constexpr char kDisposedMoveError[] =
    "cannot move from disposed local `x` (§28.3)";
constexpr char kUseAfterDisposeWarningFragment[] =
    "after a dominating explicit Dispose() call";

// `hasErrorMessage` (ownership_support.hpp) deliberately scans ERROR-severity
// diagnostics only (its own header comment: "auto-drop warnings ... and
// use-after-dispose warnings (§28.2) are filtered out"). The §28.2
// use-after-dispose diagnostic this file's accept-side sweep needs to
// confirm STILL fires is a WARNING (see `checkRead`'s `IdentifierExpr` arm),
// so scan Warning-severity diagnostics directly, mirroring
// `hasAutoDropWarning`'s own shape for the sibling warning family.
bool hasWarningMessage(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// `Res` is a plain disposable class; `Holder`'s constructor consumes an
// `owned r: Res` and disposes it; `Factory.Make` forwards a `move`d owned
// argument through a bare (unwrapped) call — the issue's own repro shape,
// with NO structural wrapper (`checkRead` reaches this `move` through its
// own `CallExpr` arm's argument loop, the site this fix touches).
constexpr char kResHolderFactory[] =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Holder "
    "  public v: int; "
    "  public constructor(owned r: Res) this.v := r.tag; r.Dispose(); "
    "end constructor; "
    "  destructor end destructor; "
    "end Holder; "
    "class Factory "
    "  public static method Make(owned r: Res): owned Holder? "
    "    return new Holder(move r); "
    "  end Make; "
    "end Factory; ";

std::string program(const std::string& classesAndMain) {
    return kResHolderFactory + classesAndMain;
}

}  // namespace

// =============================================================================
// CONTROL — the pre-existing `NewExpr` ctor-argument spelling was already
// correctly rejected before this fix (its `move` never reaches `checkRead`
// at all — `checkRead` has no `NewExpr` arm) and must stay rejected with the
// IDENTICAL diagnostic text.
// =============================================================================
BEZEL_TEST(checkread_call_arg_move_order_4023,
           direct_new_ctor_move_of_disposed_local_rejected_control) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    using var h := new Holder(move x); "
        "    return h.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

// =============================================================================
// NEGATIVE (issue #4023's own repro) — a BARE call's `move` argument (no
// wrapper at all), reached through `checkRead`'s own `CallExpr` arm.
// FAIL-WITHOUT: `r.diagnostics.hasErrors()` is FALSE (only Warnings) and
// `hasErrorMessage(r, kDisposedMoveError)` is FALSE — measured pre-fix.
// PASS-WITH: both flip true, with the SAME diagnostic text the control case
// gets (proving the precise diagnostic is no longer masked, not merely that
// *some* error appears).
// =============================================================================
BEZEL_TEST(checkread_call_arg_move_order_4023,
           bare_call_move_of_disposed_local_rejected) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := Factory.Make(move x); "
        "    return y!.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kDisposedMoveError));
}

// The same case through the STRONG AOT oracle (real `bezelc` + link + exec):
// proves not merely that a diagnostic fired, but that NO EXECUTABLE was ever
// produced (the pre-fix program compiled to a real, SEGV-on-run binary).
BEZEL_TEST(checkread_call_arg_move_order_4023,
           aot_bare_call_move_of_disposed_local_rejected_no_program) {
    JitResult r = runAot(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := Factory.Make(move x); "
        "    return y!.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// =============================================================================
// ACCEPT-SIDE SWEEP (LANE_RULES §15) — the fix changes WHEN a diagnostic
// fires (deferring a `move` argument's validation entirely to
// `recordMovesIn`), so the FAIL-WITHOUT/PASS-WITH pair above proves only
// that this one negative row moved. These rows pin the adjacent shapes that
// must KEEP COMPILING (or keep warning exactly as before), so a false
// reject / lost diagnostic in the new skip would show here.
// =============================================================================

// A fresh, never-disposed owned source moved into the SAME bare-call
// position must compile clean — the skip must not itself manufacture a
// rejection.
BEZEL_TEST(checkread_call_arg_move_order_4023,
           accept_bare_call_move_of_live_local_compiles_clean) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); "
        "    var y := Factory.Make(move x); "
        "    return y!.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `move` argument to a bare call reached via a `using var` binding (the
// idiom the issue's own program uses one line later, `return y!.v` folded
// into a `using var`) must also stay accepted for a live source.
BEZEL_TEST(checkread_call_arg_move_order_4023,
           accept_using_var_bare_call_move_of_live_local_compiles_clean) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(2); "
        "    using var y := Factory.Make(move x); "
        "    return y!.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A NON-move (plain, borrow-position) argument to a bare call over an
// already-disposed local must still get the EXISTING §28.2 use-after-dispose
// Warning — proving the fix's `isTopLevelConsumingForm` gate is narrow (only
// a top-level `move`/`await` is skipped) and did not silently widen to
// swallow the ordinary read-check path for every argument.
BEZEL_TEST(checkread_call_arg_move_order_4023,
           accept_bare_call_plain_read_of_disposed_local_still_warns) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Reader "
        "  public static method Peek(r: Res): int return r.tag; end Peek; "
        "end Reader; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(3); x.Dispose(); "
        "    var y := Reader.Peek(x); "
        "    return y; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasWarningMessage(r, kUseAfterDisposeWarningFragment));
}

// A `move` argument to a bare call reached through a straight-line loop
// (adjacent to the pre-existing `out owned var` loop-back-edge carve-out
// documented immediately above this fix's arm) must still compile clean for
// a fresh producer minted each iteration — the new skip must not interact
// with that unrelated carve-out's own loop-state handling.
BEZEL_TEST(checkread_call_arg_move_order_4023,
           accept_bare_call_move_in_loop_of_fresh_producer_each_iteration) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 2 do "
        "      var x := new Res(i); "
        "      var y := Factory.Make(move x); "
        "      total := total + y!.v; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
