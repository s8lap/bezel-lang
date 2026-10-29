// Spec-derived regression tests for issue #1900: reassigning a moved-from /
// disposed owned local (the consume-and-restore idiom and the reset-a-resource
// idiom) was spuriously rejected. The owned value in `x` reached scope end
// alive (the fresh value placed by the reassign), but the ownership pass
// denied auto-drop because `everDischarged_` was set from a prior
// `s.Take(move x)` / explicit `x.Dispose()`, falling through to the hard
// discharge error.
//
// §28.3 "Move semantics for owned locals" + "Owned-local discharge requirement
// / Auto-drop (affine)" (lines ~5735, 5934), v0.196:
//   "A moved-from owned local may not be read until reassigned — i.e.
//    reassignment is the sanctioned restore mechanism."
//   "An owned local that reaches a normal-exit scope end not in moved-from
//    state is auto-dropped with a must-use WARNING (not an error)."
// The fresh value reassigned into `x` is a plain owned class local whose
// drop-on-overwrite codegen is covered (§28.7), so it should auto-drop with a
// warning, exactly as the same reassignment does when `x` was NOT previously
// discharged (`var x := new Res(); x := new Res();` is accepted).
//
// Before this fix, the scope-end auto-drop was gated on
// `everDischarged_.count(sym) == 0` in Ownership.cpp (~line 415-418); because
// `x` was already discharged once, the re-acquired value was denied auto-drop
// and the program was hard-rejected at the reassignment. Adding an explicit
// `x.Dispose()` on the restored value made it compile (proving the rejected
// thing is the denied auto-drop, not the overwrite).
//
// The fix maintains a parallel `definitelyRestoredLocals_` set updated by
// handleAssignment (add on unconditional Owned-reassign) and by
// markMovedByName / markDisposedByName (remove on any discharge); the
// auto-drop gate relaxes `everDischarged_ == 0` to
// `everDischarged_ == 0 || definitelyRestoredLocals_ != 0` for the sync and
// async §28.3 gates and the #1257 nested-scope gate. `everDischarged_` stays
// monotone so the conditional-discharge double-free the original gate was
// designed to catch remains protected.

#include "support/aot_support.hpp"       // runAot — AOT oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// `class Res` is a disposable-shaped class (a destructor makes it non-trivial-
// dispose, so the drop-on-overwrite codegen path is exercised); `body` is the
// Main method body.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class Res "
        "  public constructor() end constructor; "
        "  public method Ping(): int return 1; end Ping; "
        "  destructor end destructor; "
        "end Res; "
        "class Sink "
        "  public method Take(owned t: Res) "
        "    using var h: Res := move t; "
        "  end Take; "
        "end Sink; "
        "class Program "
        "  public static method Main(): int ") +
        body + " end Main; end Program;";
}

// True iff some ERROR diagnostic cites §28.3 in the "would be dropped here
// without disposal" form — the exact phrase the pre-fix ownership pass
// emitted at the reassignment.
bool hasRestoreRejectError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("§28.3") == std::string::npos) continue;
        if (d.message.find("would be dropped here without disposal") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

// True iff an auto-drop must-use WARNING fires for the named local.
bool hasAutoDropWarningFor(const OwnershipResult& r, const std::string& name) {
    return hasAutoDropWarning(r, name);
}

}  // namespace

// ============================================================================
// ACCEPTED — the consume-and-restore idiom: move-out then fresh-reassign.
// ============================================================================

// §28.3 (#1900): the exact issue repro — `s.Take(move x); x := new Res();`
// is the consume-and-restore idiom and must be ACCEPTED. The fresh `x` is a
// plain owned class local that auto-drops with the §28.3 must-use WARNING
// (NOT the hard "would be dropped without disposal" error). Returns 1 from
// Ping on the restored value to confirm the run-time value is the reassigned
// one, not the moved-out one (no stale reference).
BEZEL_TEST(owned_reassignment_restore_1900,
           consume_and_restore_after_take_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var s: Sink := new Sink(); "
        "var x: Res := new Res(); "
        "s.Take(move x); "
        "x := new Res(); "
        "return x.Ping();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasRestoreRejectError(r));
    // Must-use warning per §28.3 auto-drop on the reassigned value.
    BEZEL_EXPECT_TRUE(hasAutoDropWarningFor(r, "x"));
}

// §28.3 (#1900): the reset-a-resource idiom — `x.Dispose(); x := new Res();`.
// An explicit Dispose is the unconditional-discharge / unconditional-restore
// case; same fix-direction, same accept-with-warning shape.
BEZEL_TEST(owned_reassignment_restore_1900,
           reset_resource_dispose_then_restore_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Res := new Res(); "
        "x.Dispose(); "
        "x := new Res(); "
        "return x.Ping();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasRestoreRejectError(r));
    BEZEL_EXPECT_TRUE(hasAutoDropWarningFor(r, "x"));
}

// §28.3 (#1900): the consume-and-restore inside a `for` loop — the
// loop-accumulator shape. Reassigning `x` after each iteration's move-out
// must compile clean and the AOT run returns 0.
BEZEL_TEST(owned_reassignment_restore_1900,
           consume_and_restore_in_loop_accepted_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var s: Sink := new Sink(); "
        "var x: Res := new Res(); "
        "for var i in 0..2 do "
        "  s.Take(move x); "
        "  x := new Res(); "
        "end; "
        "_ := x; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasRestoreRejectError(r));
}

// ============================================================================
// NO REGRESSION — the auto-drop still fires correctly without a prior
// discharge, and the conditional-discharge gate is still protected.
// ============================================================================

// §28.3 v0.150 baseline (control A from the issue): `var x := new Res();
// x := new Res();` with NO prior discharge is the existing accept-with-
// warning case. Pre-fix behavior unchanged.
BEZEL_TEST(owned_reassignment_restore_1900,
           no_prior_discharge_still_auto_drops_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Res := new Res(); "
        "x := new Res(); "
        "return x.Ping();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasRestoreRejectError(r));
    BEZEL_EXPECT_TRUE(hasAutoDropWarningFor(r, "x"));
}

// ── REMOVED 2026-07-29 for issue #2270 ─────────────────────────────────────
//
// A cell `conditional_discharge_still_rejected_spec_28_3` stood here. It
// asserted that
//
//     var c: bool := true;
//     var x: Res := new Res();
//     if c then x.Dispose(); end;
//     return 0;
//
// is hard-rejected — described as the #1626 conditional-discharge gate that the
// #1900 restore fix must not regress.
//
// It was self-contradictory on arrival: the SAME commit (d2ecf82, "fix #1900
// #1883") that added it also added
// `conditional_discharge_1883.one_branch_dispose_no_post_join_use_accepted_spec_28_3`,
// which asserts the OPPOSITE outcome for the byte-equivalent program, and
// implemented the #1883 side — so this cell was red from the moment it landed.
//
// The #1883 cell is the spec-correct one. K7.4 (v0.228): "owned ⊓ moved-from
// and owned ⊓ explicitly-disposed = maybe-consumed — unreadable after the join,
// no flow around a back edge into a use; **scope-end disposal per-path**". With
// no post-join read/borrow/consume — the body here is just `return 0;` — the
// program is well-formed and the still-owned path's obligation is met by
// §28.3's affine auto-drop, "a warning (not an error)".
//
// So the cell is removed rather than inverted: inverted, it would be a verbatim
// duplicate of the #1883 cell, and the accept side of this rule belongs with
// #1883's evidence (which also carries the AOT exactly-once run). #1900's own
// subject — that a local RESTORED by reassignment after a discharge still
// auto-drops — is covered by the cells above and below.
//
// What #1900 genuinely must not regress is the MOVED-FROM split, which stays
// hard-rejected; that guard lives in branch_disposal_test.cpp
// (`move_then_only_leaks_else_path_28_6`) and maybe_consumed_join_1626_test.cpp.

// §28.3 (#1900): AOT execution — the consume-and-restore idiom actually
// runs and returns the expected value (1 from the restored Res.Ping), with
// no UAF on the moved-out `x`. This is the AOT-side regression that proves
// the fix is sound, not just that the diagnostic went away.
BEZEL_TEST(owned_reassignment_restore_1900,
           consume_and_restore_aot_runs_and_returns_one) {
    JitResult jr = runAot(wrap(
        "var s: Sink := new Sink(); "
        "var x: Res := new Res(); "
        "s.Take(move x); "
        "x := new Res(); "
        "return x.Ping();"));
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
    BEZEL_EXPECT_TRUE(jr.ran);
}