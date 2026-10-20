// fix #2436 (§22.4 "Inline `out` declarations" / §28.3 "Owned-local discharge
// requirement", spec v0.235): an inline `out owned var` recipient declared at
// a call site INSIDE a loop body, and explicitly `.Dispose()`d on EVERY
// iteration, was spuriously REJECTED starting on the second iteration —
// "already moved out" (constructor form, #2429's own subject) or "used after
// dispose" (plain method-call form) — even though the program is legal: the
// recipient is a FRESH binding minted by its declaring call every time
// control reaches it, exactly like an ordinary `var w := new Widget();`
// inside a loop body (which handleLocalDecl already resets on every visit).
//
// ROOT CAUSE (three compounding defects in src/ownership/Ownership_Checks.cpp,
// all in the constructor / method-call argument-classification machinery
// shared by `MethodOwnership::markOutOwnedArgsCore` and its ctor-specific
// NewExpr-argument-loop sibling):
//
//   1. `markOutOwnedArgsCore`'s `"out owned var"` branch seeded the
//      recipient's `LocalOwnership` Owned state behind a
//      `states_.find(sym) == states_.end()` guard — "only if not already
//      present." A loop back-edge's entry-state merge (`Ownership.cpp`'s
//      `computeEntry`/`joinInto`, the O3b loop fixpoint) is a UNION-KEY meet
//      that carries a symbol present on only one incoming edge through
//      VERBATIM — so a recipient discharged (Disposed) at the end of
//      iteration N was carried, unchanged, into the block's entry state for
//      iteration N+1 (the declaring call sits inside the loop body, or — for
//      the while-CONDITION `TryPop(out owned var item)` idiom, §28.3/#892 —
//      inside the loop-header block itself, both revisited every iteration
//      via the back edge). The guard then read that carried-forward stale
//      state as "already seeded" and skipped the reseed. FIX: seed
//      unconditionally on every visit, mirroring handleLocalDecl's
//      unconditional `states_[lsym] = lo;` and seedOwnedIterVar's
//      per-iteration reseed of a for-loop's owned iteration variable.
//
//   2. The constructor NewExpr argument loop's `ctorOwnedSink` classifier
//      (added by #2429) incorrectly included `"out owned"` alongside
//      `"owned"` as an owned-IN sink — diverging from its OWN documented
//      analogue, `recordMovesInArgs`'s `userOwned` check, which explicitly
//      excludes `"out owned"` ("an `out owned` formal is the owned-OUT
//      direction ... NOT an owned-IN sink"). This routed a FRESH inline
//      `out owned var n` recipient identifier through `markMovedByName` as
//      if `n` were an EXISTING owned value being moved INTO the constructor
//      — harmless on a first straight-line occurrence (`n` not yet tracked,
//      so `lookupState` finds nothing), but on a loop's second iteration,
//      where `n`'s prior-iteration Disposed state is now present in
//      `states_` (bug 1's precondition), this reported a spurious
//      "cannot move from disposed local `n`". FIX: `ctorOwnedSink` now
//      checks only `"owned"`, matching the method-call path.
//
//   3. `checkRead`'s `CallExpr` branch recurses into EVERY argument
//      uniformly, with no carve-out for an `out owned var` / builtin-out-
//      owned WRITE-target argument — unlike `handleDischarge`'s CallExpr
//      branch, which already documents and implements exactly this
//      carve-out ("an `out owned` arg is a WRITE target ... Skip
//      read-checking it (a stale Disposed state from a prior loop iteration
//      would otherwise produce a spurious use-after-dispose warning)") for
//      the bare-statement-call form. `checkRead` is reached from
//      `handleLocalDecl` (the `var ok := Widget.TryMake(..., out owned var
//      n);` / assignment-RHS forms) BEFORE the declaring call's own
//      `markOutOwnedArgs` reseed runs, so on a loop's second iteration it
//      read `n`'s stale carried-forward Disposed state and reported a
//      spurious use-after-dispose warning. FIX: mirror handleDischarge's
//      carve-out inside checkRead's CallExpr argument loop.
//
// Tests use `runAotChecked` (generated code linked + run under ASan/LSan, so
// a leak, double-free, or use-after-free fails via r.ran=false) with a
// live-object counter (constructor increments, destructor decrements) to
// prove EXACTLY-ONCE disposal per iteration — not merely "did not crash" or
// "compiled clean."
//
// per §22.4, §28.3, §28.11, #2436, #2429, #2411, #2430, #860, #1387

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 7 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is still recognised as an environment condition rather than a
// product failure, but it now MARKS THE ROW FAILED instead of hiding it.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

// Res — owned instance, destructor decrements a static counter: Res.alive ==
// 0 at the end proves exactly-once disposal of every constructor-minted Res
// across ALL loop iterations (not merely "no crash") on top of ASan/LSan's
// own double-free / leak / use-after-free detection.
const char* kRes =
    "class Res "
    "  public static alive: int := 0; "
    "  public constructor() Res.alive := Res.alive + 1; end constructor; "
    "  destructor Res.alive := Res.alive - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO 1 — constructor form. `n` is redeclared and
// explicitly disposed on EVERY iteration of a `for` loop. Pre-fix: rejected
// on the 2nd iteration ("cannot move from disposed local `n`" via defect 2,
// or "already moved out" depending on which defect's diagnostic fires
// first). Post-fix: accepted, Res.alive settles at 0 (3 mints, 3 disposes).
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436,
           ctor_form_for_loop_discharge_then_redeclare_each_iteration) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    for var i in 0..3 do "
        "      var w := new Widget(\"x\", out var n); "
        "      w.Dispose(); "
        "      n.Dispose(); "
        "    end for; "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// THE ISSUE'S OWN REPRO 2 — plain method-call form (pre-existing, not
// specific to #2429's constructor path; defect 1 alone). Pre-fix: compiles
// and runs (a WARNING, not a hard error), but wrongly reports "use of `n`
// after a dominating explicit Dispose() call" starting on the 2nd
// iteration, even though `n` is a fresh value each pass; the runtime
// dispose count is unaffected either way (a warning does not change
// codegen), so this test's real proof is the dispose-count/no-leak/no-UAF
// verification, not the warning's absence — see the companion diagnostic-
// text assertion, out_owned_var_loop_iteration_reset_2436.
// method_form_for_loop_no_spurious_use_after_dispose_warning, in
// tests/ownership/out_owned_var_loop_iteration_reset_2436_test.cpp
// (parseAndCheckOwnership captures ALL diagnostics including warnings;
// runAot/runAotChecked's JitResult.diagnostics only carries synthetic
// infra-failure diagnostics on a SUCCESSFUL compile, so it cannot observe a
// warning here — checked directly, this is not a guess: aot_support.hpp's
// runAot only calls r.diagnostics.report(...) on an infra failure or a
// CompileOutcome != Ok; a successful compile's own warning text is
// discarded, never attached to JitResult).
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436,
           method_form_for_loop_discharge_then_redeclare_each_iteration) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public static method TryMake(s: string, out owned n: Res): bool "
        "    n := new Res(); "
        "    return true; "
        "  end TryMake; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    for var i in 0..3 do "
        "      var ok := Widget.TryMake(\"x\", out var n); "
        "      n.Dispose(); "
        "    end for; "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// `while` as well as `for` — the ctor form's body-position recipient inside
// a `while` loop, discharged every pass.
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436,
           ctor_form_while_loop_body_discharge_each_iteration) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var i: int := 0; "
        "    while i < 3 do "
        "      var w := new Widget(\"x\", out var n); "
        "      w.Dispose(); "
        "      n.Dispose(); "
        "      i := i + 1; "
        "    end; "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// Nested loops — the recipient's declaring call is reached by TWO back
// edges (inner loop's, then outer loop's), stressing the union-key merge
// at two levels.
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436, nested_loops_discharge_each_iteration) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    for var i in 0..3 do "
        "      for var j in 0..2 do "
        "        var w := new Widget(\"x\", out var n); "
        "        w.Dispose(); "
        "        n.Dispose(); "
        "      end for; "
        "    end for; "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// `break` / `continue` inside the loop, each discharging the recipient
// before leaving — the back edge is reached from multiple different
// predecessor points, not just the block's natural fall-through.
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436, break_and_continue_still_discharge_each_iteration) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    for var i in 0..5 do "
        "      var w := new Widget(\"x\", out var n); "
        "      if i = 1 then "
        "        w.Dispose(); "
        "        n.Dispose(); "
        "        continue; "
        "      end; "
        "      if i = 3 then "
        "        w.Dispose(); "
        "        n.Dispose(); "
        "        break; "
        "      end; "
        "      w.Dispose(); "
        "      n.Dispose(); "
        "    end for; "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// Multiple recipients per iteration — one from the ctor form, one from the
// method form, both discharged every pass. Guards that the per-argument
// classification fix (defect 2) and the per-symbol reseed (defect 1) do not
// alias or cross-contaminate independent recipients.
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436, multiple_recipients_per_iteration_both_discharge) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(); "
        "  end constructor; "
        "  public static method TryMake(s: string, out owned n: Res): bool "
        "    n := new Res(); "
        "    return true; "
        "  end TryMake; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    for var i in 0..3 do "
        "      var w := new Widget(\"x\", out var n1); "
        "      var ok := Widget.TryMake(\"y\", out var n2); "
        "      w.Dispose(); "
        "      n1.Dispose(); "
        "      n2.Dispose(); "
        "    end for; "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// while-CONDITION form (§28.3/§892's `while s.TryPop(out owned var item) do
// item?.Dispose(); end` idiom) — a DIFFERENT recipient position (inside the
// loop HEADER, not the body) that shares defect 1's fix. Pre-fix this
// compiled clean but with a SPURIOUS "disposed again" no-op warning starting
// on the 2nd iteration (item's carried-forward Disposed state made the
// idempotent-dispose warning fire on a value that was actually freshly
// re-minted by that pass's TryPop). Res.alive proves the REAL disposal count
// is correct either way; this test additionally locks in that ONLY the two
// genuinely-popped items are ever live (see also the companion lit test,
// tests/lit/sema/out_owned_condition_892__while_drain_dispose_body_accepted.bzl,
// which asserts the warning's ABSENCE post-fix).
// ===========================================================================
BEZEL_TEST(out_owned_var_loop_iteration_reset_2436, while_condition_form_drain_disposes_exactly_once_each) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    using var s := new OwningStack<Res>(); "
        "    s.Push(move (new Res())); "
        "    s.Push(move (new Res())); "
        "    s.Push(move (new Res())); "
        "    while s.TryPop(out var item) do "
        "      item?.Dispose(); "
        "    end; "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    // 3 mints, 3 pops each disposed — settles at 0. A double-dispose (the
    // pre-fix symptom's runtime twin) would still settle at 0 too (Dispose
    // is idempotent), so this test's real value is the ACCEPT itself
    // (r.ran) plus the companion lit test pinning the warning's absence.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
