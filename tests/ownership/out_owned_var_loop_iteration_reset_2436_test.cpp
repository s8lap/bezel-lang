// fix #2436 (§22.4 "Inline `out` declarations" / §28.3 "Owned-local discharge
// requirement", spec v0.235) — REJECT-DIRECTION guard rails.
//
// See tests/codegen/out_owned_var_loop_iteration_reset_2436_test.cpp for the
// full root-cause narrative and the ACCEPT-direction (runtime-verified)
// coverage. This file locks in the other half of the fix's safety
// obligation: loosening the loop-body state reseed must NOT weaken any
// genuine rejection.
//
// §28.3 (v0.235) states an undischarged owned local is disposed implicitly
// "at the end of its scope" with a must-use WARNING, not an error (the
// affine auto-drop rule) — EXCEPT the while-loop-CONDITION `out owned var`
// form, which keeps the hard discharge ERROR because its physical storage
// slot is shared across every iteration (`ensureOutVarSlots`'s
// `alreadyMapped`): auto-drop only reclaims the FINAL iteration's value, so
// an intermediate iteration's un-disposed value would leak silently. This
// file confirms that carve-out (`loopCondOutOwnedSyms_`,
// src/ownership/Ownership.cpp ~line 2219-2230) is UNCHANGED by #2436's fix:
// the fix only touches WHEN the recipient's Owned state is (re)seeded, not
// whether an undischarged occurrence is a hard error or a soft warning.
//
// per §22.4, §28.3, §28.11, #2436, #892, #1387, #1466

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

bool diagContainsWithSeverity(const bezel::test::OwnershipResult& r,
                              bezel::Severity sev, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != sev) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

const char* kRes =
    "class Res "
    "  public static alive: int := 0; "
    "  public constructor() Res.alive := Res.alive + 1; end constructor; "
    "  destructor Res.alive := Res.alive - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// ACCEPT, NO SPURIOUS DIAGNOSTIC (either severity) — the ISSUE'S OWN REPRO 1
// (constructor form): discharge-then-redeclare on every `for`-loop
// iteration. `parseAndCheckOwnership` captures every diagnostic regardless
// of severity, unlike the AOT harness's JitResult (whose `.diagnostics`
// only carries synthetic infra-failure entries on a successful compile) —
// so this is the test that actually observes a WARNING-level regression,
// not just a hard-reject one. Pre-fix (defects 1+2): rejected with either
// "owned value in `n` was already moved out at this point" or "cannot move
// from disposed local `n`", depending on which of the two compounding
// defects' diagnostic fires first.
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436,
           ctor_form_for_loop_no_spurious_diagnostic_of_any_severity) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; "
        "class Widget "
        "  public constructor(s: string, out owned n: Item) "
        "    n := new Item(); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public method M() "
        "    for var i in 0..3 do "
        "      var w := new Widget(\"x\", out var n); "
        "      w.Dispose(); "
        "      n.Dispose(); "
        "    end for; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagContains(r, "already moved out"));
    BEZEL_EXPECT_FALSE(diagContains(r, "cannot move from disposed"));
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics().empty());
}

// ===========================================================================
// ACCEPT, NO SPURIOUS DIAGNOSTIC (either severity) — the ISSUE'S OWN REPRO 2
// (plain method-call form): discharge-then-redeclare on every `for`-loop
// iteration. Pre-fix (defect 1 alone — the method form never hits defects 2
// or 3, which are constructor-path-specific): a WARNING, "use of `n` after
// a dominating explicit Dispose() call" — invisible to the AOT-harness
// tests (see the codegen test file's comment on this exact gap), which is
// why this diagnostic-text assertion belongs here.
//
// fix #3842 (§22.3 / §22.4, v0.289): `out owned var` is RETIRED.
// Migrated from `out owned var` to `out var` (the adoption form). The
// read-check skip in recordMovesIn (consumed=true for `out var` at `out owned`
// formal) prevents the spurious "use after Dispose" warning on loop revisit.
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436,
           method_form_for_loop_no_spurious_use_after_dispose_warning) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; "
        "class Widget "
        "  public static method TryMake(s: string, out owned n: Item): bool "
        "    n := new Item(); "
        "    return true; "
        "  end TryMake; "
        "end Widget; "
        "class C "
        "  public method M() "
        "    for var i in 0..3 do "
        "      var ok := Widget.TryMake(\"x\", out var n); "
        "      n.Dispose(); "
        "    end for; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(
        diagContains(r, "after a dominating explicit Dispose() call"));
    BEZEL_EXPECT_TRUE(r.diagnostics.diagnostics().empty());
}

// ===========================================================================
// REJECT — while-CONDITION form (§892's drain idiom), the recipient NEVER
// discharged on any iteration. This is the loopCondOutOwnedSyms_ hard-error
// carve-out — must stay a genuine compile ERROR (not the soft auto-drop
// warning the body-position form gets, and not a silent accept) both before
// and after #2436's fix, since #2436 only changes WHEN the recipient is
// re-seeded Owned, not the auto-drop-vs-hard-error classification.
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436,
           while_condition_form_never_discharged_stays_hard_reject) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kRes +
        "class C "
        "  public method M() "
        "    using var s := new OwningStack<Res>(); "
        "    s.Push(move (new Res())); "
        "    while s.TryPop(out var item) do "
        // `item` dropped without disposal on every iteration.
        "    end; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Error,
        "would be dropped here without disposal"));
}

// ===========================================================================
// ACCEPT-WITH-WARNING (not silent, not hard reject) — the BODY-position
// form, never explicitly discharged on any iteration. Unlike the
// while-condition form above, a body-position recipient's storage is
// re-alloca'd fresh at the top of every loop-body visit (it is not a
// shared-slot condition-position recipient), so the existing §28.3 affine
// auto-drop machinery reclaims EACH iteration's value at that iteration's
// own scope end — a must-use WARNING, exactly as the non-loop case
// (#2429's own `dropped_on_the_floor` test) already establishes. This locks
// in that #2436's unconditional-reseed fix does not turn this warning into
// either a silent accept or a hard error.
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436,
           body_form_never_discharged_in_loop_still_warns_not_silent_not_hard_error) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; "
        "class Widget "
        "  public constructor(s: string, out owned n: Item) "
        "    n := new Item(); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public method M() "
        "    for var i in 0..3 do "
        "      var w := new Widget(\"x\", out var n); "
        "      w.Dispose(); "
        // `n` dropped without disposal on every iteration.
        "    end for; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Warning,
        "is implicitly disposed at the end of its scope"));
}

// ===========================================================================
// REJECT — a conditional discharge on only ONE branch, per iteration, still
// carries an obligation on the other branch: §28.3's affine rule means this
// is ACCEPT-with-warning (the undischarged path's value is auto-dropped),
// not a hard reject and not a silent accept — locking in that #2436's fix
// does not change this K7.4-style per-branch obligation tracking. (Compare
// the while-condition form above, which stays hard-reject regardless of
// branching, because ITS carve-out is about the shared slot, not the
// per-branch join.)
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436,
           conditional_discharge_one_branch_per_iteration_warns_not_silent) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; "
        "class Widget "
        "  public constructor(s: string, out owned n: Item) "
        "    n := new Item(); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public method M() "
        "    for var i in 0..4 do "
        "      var w := new Widget(\"x\", out var n); "
        "      w.Dispose(); "
        "      if i mod 2 = 0 then "
        "        n.Dispose(); "
        "      end; "
        // `n` reaches the loop-body's own scope end undischarged on the
        // odd-`i` branch.
        "    end for; "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContainsWithSeverity(
        r, bezel::Severity::Warning,
        "is implicitly disposed at the end of its scope"));
}

// ===========================================================================
// REJECT — a value genuinely moved out and then moved AGAIN, straight-line
// (no loop involved) — the ordinary, non-loop §28.3 double-move rejection
// that must keep working unmodified. A baseline / non-regression control:
// #2436's fix touches only the loop-carried reseed of a FRESH inline
// `out owned var` recipient's OWN declaring call; it must not weaken the
// general already-moved-out check for a value that is genuinely consumed
// twice.
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436,
           straightline_double_move_of_recipient_stays_reject) {
    auto r = parseAndCheckOwnership(
        std::string("module M; class Item end Item; "
        "class Sink public static method Take(owned x: Item) end Take; end Sink; "
        "class Widget "
        "  public constructor(s: string, out owned n: Item) "
        "    n := new Item(); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public method M() "
        "    var w := new Widget(\"x\", out var n); "
        "    w.Dispose(); "
        "    Sink.Take(move n); "
        "    Sink.Take(move n); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "already moved out"));
}
