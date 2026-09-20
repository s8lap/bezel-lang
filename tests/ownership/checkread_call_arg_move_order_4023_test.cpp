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
// diagnostic.
//
// Spec, v0.294 → v0.303:
//   §28.3 "Local variables, ownership, and borrows" — the move/disposal
//     state machine `markMovedByName` enforces ("a `Disposed` local may not
//     be moved from" — UNCHANGED at the kernel level; v0.303 changes only
//     the SEVERITY of the DOMINATING-kill arm from ERROR to §28.2 WARNING).
//   §28.2 "The `IDisposable` contract" — the use-after-dispose Warning
//     `checkRead`'s `IdentifierExpr` arm reports for a PLAIN read of a
//     disposed local.  v0.303 / #3975 (option 1) extends this regime to
//     the consume side: under a DOMINATING `Dispose()`, EVERY use — read,
//     borrow, AND consume — draws the §28.2 WARNING (the consume-side
//     ruling is the new contract; the conditional-kill arms stay §28.3
//     errors per `maybeConsumed`).
//
// THE BUG (measured pre-#4023 on r15-c3-batch base, carrying #4003 fix
// 7b37af7 and #4028 fix 4d40c6f):
//
//   using var h := new Holder(move x);   // x already .Dispose()d
//     -> DIAGNOSTIC FIRED (precise §28.3 ERROR pre-#4450, §28.2 WARNING
//        post-#4450 — the verdict MOVED; the diagnostic still fires).
//   var y := Factory.Make(move x);       // byte-identical move, as a BARE
//                                        // call's argument — no wrapper
//     -> NO DIAGNOSTIC (only the unrelated §28.2 read-side Warning + the
//        auto-drop must-discharge Warning on `y`, both non-fatal). Compiled
//        and run under `--sanitize=address`: SEGV (null READ) inside
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
// Disposed`, reports the §28.2 WARNING (correct for a plain read; but the
// wrong diagnostic for a move site) and then — since this runs in the
// final, unsuppressed pass — POISONS `x`'s tracked state
// (`OwnershipState::Poisoned`). By the time `recordMovesIn` ->
// `recordMovesInArgs` -> the bare-`move`-identifier branch ->
// `markMovedByName("x", ...)` runs, `x`'s state is no longer `Disposed` —
// `markMovedByName`'s `Disposed` arm either (pre-#4450) reports its
// §28.3 ERROR, or (post-#4450) reports its §28.2 consume-side WARNING,
// but the cascade-poisoning means the warning is the §28.2 read-side
// one from `checkRead`, with `markMovedByName`'s OWN diagnostic suppressed.
// The `Poisoned` state matches NEITHER `MovedFrom` NOR `Disposed` cleanly
// for the consume arm's verification — poisoning is a deliberate
// cascade-suppressor, by design silent — so `markMovedByName` falls
// through SILENTLY (this is the cascade-poisoning hazard #4023 fixed).
//
// THE FIX (#4023): the `CallExpr` arm's argument loop gained the SAME
// `isTopLevelConsumingForm` carve-out `readOperand` already applies to every
// other read position `checkRead` walks (the `NewExpr` ctor-argument arm,
// and every arm #3971 added) — skip the read-check entirely for a `move`/
// `await` top-level argument, deferring wholly to `recordMovesIn`, which
// already correctly reports on the ORIGINAL (unpoisoned) Disposed/MovedFrom
// state via `markMovedByName`.  Post-#4450 the consume-side ruling per
// v0.303 is what `markMovedByName`'s `Disposed` arm reports (a §28.2
// consume-side WARNING, distinct from `checkRead`'s read-side wording, so
// the consume shape stays visible at a glance).  Only the `move` half is
// exercised below (`await` shares the identical `isTopLevelConsumingForm`
// predicate and is already covered at its other four call sites —
// Ownership.cpp `handleLocalDecl`/`handleAssignment`'s RHS checks and this
// file's `readOperand` — constructing a fresh, spec-correct `async`/`Task`
// program to exercise the identical code path here would not add coverage
// of a DIFFERENT mechanism, so it is declined per CLAUDE.md's
// decline-with-reasons discipline rather than bolted on).
//
// CHECKED-MODE BACKSTOP (§28.2): the §28.2 ruling admits these programs
// with a WARNING only because the language's checked-disposal mode is the
// deterministic runtime backstop — every member access through a
// logically-disposed instance raises `DisposedAccessException`.  The AOT
// row below exercises this for the bare-call shape; the catch arm observes
// the exception and returns 1, pinning "warns, then crashes at runtime"
// as the §28.2 verdict (so "warns, runs to completion" is not a hole).
//
// per §28.2, §28.3 K3.4/K1.3/K7, v0.294 → v0.303, #3971, #3941, #4023, #4450

#include <string>

#include "support/aot_support.hpp"         // runAotCheckedDisposal — the strong AOT oracle (§28.2 backstop)
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotCheckedDisposal;

namespace {

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

// v0.303 / #4450 (the #3975 option-1 ruling): under a DOMINATING
// `Dispose()`, every use of the source local — read, borrow, AND consume —
// is §28.2's use-after-dispose WARNING, NOT §28.3's consume-table ERROR.
// The consume-side ruling is the new contract; the OLD §28.3 ERROR text
// ("cannot move from disposed local `x`") is GONE.  These two helpers are
// the inclusive matcher (both wording variants — the read-side wording
// `checkRead` produces and the consume-side wording `markMovedByName`
// produces post-ruling) and the negative guard that the OLD §28.3 ERROR
// must NOT have reappeared (catches any over-relaxation of the
// conditional-kill arms).
//
// per §28.2, §28.3 K1.3/K7, v0.303, #3975 (option 1), #4450
inline bool hasUseAfterDisposeWarning(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("use of `x` after a dominating explicit "
                           "Dispose() call") != std::string::npos) return true;
        if (d.message.find("move from a local `x` already disposed at "
                           "this point") != std::string::npos) return true;
    }
    return false;
}
inline bool hasCannotMoveFromDisposedError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("cannot move from disposed local `x`") !=
                std::string::npos) return true;
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
// detected before this fix (its `move` never reaches `checkRead` at all —
// `checkRead` has no `NewExpr` arm); the original `recordMovesIn` path was
// the one reporting the precise §28.3 ERROR.  v0.303 / #4450 demotes that
// arm's verdict to §28.2's consume-side WARNING — the ruling is the OTHER
// way for a DOMINATING-kill consume.  The preemption invariant #3971 owned
// (this row is consumed by `recordMovesIn`'s OWN path, NOT by `checkRead`'s
// cascade) is unchanged: `checkRead` does not also report, so the verdict
// is a single §28.2 consume-side warning, with the OLD §28.3 ERROR text
// GONE.
// =============================================================================
BEZEL_TEST(checkread_call_arg_move_order_4023,
           direct_new_ctor_move_of_disposed_local_warns_28_2_4450_control) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    using var h := new Holder(move x); "
        "    return h.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(hasCannotMoveFromDisposedError(r));
}

// =============================================================================
// NEGATIVE (issue #4023's own repro) — a BARE call's `move` argument (no
// wrapper at all), reached through `checkRead`'s own `CallExpr` arm.
// v0.303 / #4450: under a DOMINATING Dispose(), the consume side draws
// §28.2's WARNING rather than §28.3's ERROR (the ruling is option 1 of
// #3975).  The pre-fix #4023 measurement is no longer the correct verdict
// for this shape — but the cascade-preemption invariant IS still load-bearing:
// the consume-side warning is reported by `markMovedByName` (NOT masked by
// `checkRead`'s read-side cascade), and the OLD §28.3 ERROR text is GONE.
// `Factory.Make`'s body calls `new Holder(move r)`, whose ctor reads
// `r.tag` and calls `r.Dispose()` — the `r.tag` read is the member access
// through the disposed-and-moved-from instance that triggers checked-mode's
// `DisposedAccessException`, observed in the backstop row below.
// =============================================================================
BEZEL_TEST(checkread_call_arg_move_order_4023,
           bare_call_move_of_disposed_local_warns_28_2_4450) {
    auto r = parseAndCheckOwnership(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := Factory.Make(move x); "
        "    return y!.v; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUseAfterDisposeWarning(r));
    BEZEL_EXPECT_FALSE(hasCannotMoveFromDisposedError(r));
}

// CHECKED-MODE BACKSTOP (§28.2 runtime backstop) — the same shape under
// AOT with checked disposal: `Factory.Make`'s ctor-side `r.tag` read inside
// `Holder`'s body is the member access through the logically-disposed `r`
// that fires `DisposedAccessException`.  Static analysis admits the program
// with a WARNING (the row above); runtime flags the access deterministically.
// "warns, then crashes at runtime" is the §28.2 verdict; this row pins it
// so "warns, runs to completion" is not a hole in the contract.
BEZEL_TEST(checkread_call_arg_move_order_4023,
           aot_bare_call_move_of_disposed_local_warns_28_2_4450_checked_backstop) {
    JitResult jr = runAotCheckedDisposal(program(
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    try "
        "      var y := Factory.Make(move x); "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
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
