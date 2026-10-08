// fix #2429 (§22.4 "Inline `out` declarations" / §28.3 "Owned-local discharge
// requirement", spec v0.235): a constructor's `out owned var` recipient that
// is DROPPED ON THE FLOOR — never explicitly `.Dispose()`d, never moved,
// never returned, no throw involved — leaked SILENTLY, with NO compiler
// diagnostic at all. Confirmed live (LSan Direct leak) on main after #2411
// (constructor `out owned var` ICE fix) and #2430 (`out using var` ctor
// support) both landed; #2429 was filed during #2411's own adversarial
// self-pass (see that file's header, "KNOWN, FILED, NOT FIXED HERE").
//
// ROOT CAUSE: `MethodOwnership::recordMovesIn`'s `NewExpr` branch
// (src/ownership/Ownership_Checks.cpp) — unlike `recordMovesInArgs`, the
// CallExpr/method-call sibling, which finishes with
// `markOutOwnedArgs(args, callee);` — never called any out-owned-marking
// equivalent for a constructor's `out owned` formal(s). The recipient's
// LocalSymbol therefore never entered the ownership dataflow's `states_` map
// as Owned, so it was invisible to the §28.3 affine auto-drop end-of-scope
// check (Ownership.cpp, the `dynamic_cast<const ast::IdentifierExpr*>(
// ls->decl)` branch, #1387): no must-use warning, and — because codegen's
// `ensureCtorOutVarSlots` (LlvmBackend_Calls2.cpp, #2411) keys its
// normal-exit disposal choice off the SAME dataflow output
// (`locoo->autoDropAtScopeEnd`) — no scope-end disposal either. Codegen fell
// back to registering only an `unwindOnly` disposal (correct for a throw
// edge, inert on ordinary fall-off), so the value was never reclaimed on
// the normal-exit "dropped" path.
//
// SPEC-REQUIRED OUTCOME (verified against §28.3 "Owned-local discharge
// requirement" / "Auto-drop (affine)" / "Must-use warning", v0.235): a
// dropped owned local is NOT a hard error — "the compiler disposes the
// value at the end of its scope" (Auto-drop) and "emits a **warning** (not
// an error)" (Must-use warning). This is exactly the treatment the
// EQUIVALENT method-call shape (`Widget.TryMake(s, out owned var n)`)
// already receives (test coverage:
// owned_out_discard_throw_unwind_1833_test.cpp and the #860/#1387 lit
// tests) — the constructor path was simply missing the one call that wires
// its recipient into the same, already-correct machinery.
//
// THE FIX: `recordMovesIn`'s NewExpr branch now calls a new
// `markOutOwnedArgs(ne->args, ctorParams)` overload — sharing
// `markOutOwnedArgsCore` with the existing callee-based overload
// (`recordMovesInArgs` calls `markOutOwnedArgs(args, callee)`) rather than
// hand-duplicating its logic (the exact anti-pattern this issue's own root-
// cause narrative calls out as recurring). A constructor call has no
// "callee" Expr in the CallExpr sense, so the new overload takes the
// already-resolved constructor parameter list directly (`ctorParams`,
// resolved via `resolveCtorParams` earlier in the same NewExpr branch).
//
// Tests (each uses runAotChecked → generated code linked + run under
// ASan/LSan, so a leak, double-free, or use-after-free FAILS via
// r.ran=false; Res.alive — a live-object counter incremented in the
// constructor, decremented in the destructor — proves EXACTLY-ONCE
// disposal, not merely "did not crash"):
//   dropped_on_the_floor_disposes_exactly_once_spec_28_3 — the issue's own
//     repro: `n` from `new Widget(s, out owned var n)` is never discharged.
//     Pre-fix: compiles clean, but LSan flags a leak (r.ran=false). Post-fix:
//     compiles clean AND Res.alive settles at 0 (auto-drop reclaimed it).
//   FALSE-REJECT GUARD RAIL (the risk this fix must not introduce — a
//   recipient that already has a legitimate discharge path must stay
//   ACCEPTED, exactly as before this fix, and must still dispose exactly
//   once now that it is tracked by the dataflow):
//     explicit_dispose_still_accepted_disposes_once_spec_28_3
//     moved_into_owned_field_still_accepted_disposes_once_spec_28_8
//     passed_to_another_owned_sink_still_accepted_disposes_once_spec_28_8
//     multiple_recipients_one_dropped_one_explicit_both_dispose_once_spec_28_3
//
// per §22.4, §28.3, §28.8, #2411, #2429, #1387, #860

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic — guard
// so an environment gap does not masquerade as this fix's own regression
// (mirrors #1934 / the #1833 / #2411 tests' identical guard).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Res — owned instance, destructor decrements a static counter read after
// disposal: Res.alive == 0 at the end proves exactly-once disposal of every
// constructor-minted Res (not merely "no crash") on top of ASan/LSan's own
// double-free / leak detection.
const char* kRes =
    "class Res "
    "  public static alive: int := 0; "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; Res.alive := Res.alive + 1; "
    "    end constructor; "
    "  destructor Res.alive := Res.alive - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO: a constructor's `out owned var` recipient dropped
// on the floor — never discharged, no throw. Pre-fix: LSan-confirmed leak
// (r.ran flips false under runAotChecked, since the ASan-instrumented
// generated code leaks the constructor-minted Res). Post-fix: the §28.3
// affine auto-drop reclaims it at scope end, Res.alive settles at 0.
// ===========================================================================
BEZEL_TEST(ctor_out_owned_var_dropped_2429,
           dropped_on_the_floor_disposes_exactly_once_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(9); "
        "  end constructor; "
        "end Widget; "
        "class C "
        // §28.3: the auto-drop of `n` runs as Build's own scope-end
        // epilogue — AFTER Build's `return 0;` expression is evaluated but
        // BEFORE control returns to the caller. Reading `Res.alive` in the
        // SAME statement as the drop (`return Res.alive;`) would race the
        // read against the drop and observe the PRE-drop count; routing
        // through this wrapper call means Main's own read happens strictly
        // after Build's frame (and its auto-drop) has fully unwound.
        "  public static method Build(): int "
        "    var w := new Widget(\"x\", out owned var n); "
        "    w.Dispose(); "
        "    return 0; "
        // `n` is never touched again — dropped on the floor.
        "  end Build; "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    _ := C.Build(); "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    // Pre-fix: LSan flags the undischarged `n` as a Direct leak, so the
    // ASan-checked run itself fails (r.ran == false) even though the
    // ordinary (non-ASan) compile succeeded with no diagnostic at all.
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    // Res.alive settles at 0: +1 mint (the constructor's `new Res(9)`),
    // -1 auto-drop disposal of `n` at Build's scope end.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// FALSE-REJECT GUARD RAIL — explicit `.Dispose()`. A recipient that IS
// disposed must stay accepted (it already was, pre-fix — this locks in that
// the new markOutOwnedArgs wiring does not turn a legitimate discharge into
// a spurious diagnostic) and must still dispose exactly once now that the
// dataflow tracks it.
// ===========================================================================
BEZEL_TEST(ctor_out_owned_var_dropped_2429,
           explicit_dispose_still_accepted_disposes_once_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(1); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var w := new Widget(\"x\", out owned var n); "
        "    n.Dispose(); "
        "    w.Dispose(); "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// FALSE-REJECT GUARD RAIL — moved into an owned field via the call-site
// `owned` transfer marker (§22.3/§28.8). A recipient that is transferred
// onward must stay accepted and dispose exactly once (through the new
// owner, `Holder`, not through Main's own scope-end pass).
// ===========================================================================
BEZEL_TEST(ctor_out_owned_var_dropped_2429,
           moved_into_owned_field_still_accepted_disposes_once_spec_28_8) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(2); "
        "  end constructor; "
        "end Widget; "
        "class Holder "
        "  public owned item: Res; "
        "  public constructor(owned i: Res) "
        "    item := i; "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var w := new Widget(\"x\", out owned var n); "
        "    w.Dispose(); "
        "    var h := new Holder(move n); "
        "    h.Dispose(); "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// FALSE-REJECT GUARD RAIL — passed on as the `owned` argument of another
// call. Mirrors the field case above but through a plain method's `owned`
// parameter instead of a field store.
// ===========================================================================
BEZEL_TEST(ctor_out_owned_var_dropped_2429,
           passed_to_another_owned_sink_still_accepted_disposes_once_spec_28_8) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(s: string, out owned n: Res) "
        "    n := new Res(3); "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Consume(owned item: Res): int "
        "    item.Dispose(); "
        "    return 0; "
        "  end Consume; "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    var w := new Widget(\"x\", out owned var n); "
        "    w.Dispose(); "
        "    _ := C.Consume(move n); "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// FALSE-REJECT / INDEPENDENT-TRACKING GUARD RAIL — two `out owned var`
// recipients from ONE constructor call, only ONE explicitly disposed, the
// OTHER dropped on the floor. Both must dispose exactly once (the explicit
// one via `.Dispose()`, the dropped one via the new §28.3 auto-drop this fix
// wires up) — a regression guard that `markOutOwnedArgsCore`'s per-argument
// loop tracks each recipient independently rather than aliasing/skipping.
// ===========================================================================
BEZEL_TEST(
    ctor_out_owned_var_dropped_2429,
    multiple_recipients_one_dropped_one_explicit_both_dispose_once_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kRes +
        "class Widget "
        "  public constructor(out owned a: Res, out owned b: Res) "
        "    a := new Res(4); "
        "    b := new Res(5); "
        "  end constructor; "
        "end Widget; "
        "class C "
        // §28.3: see the wrapper-call rationale on the drop-on-the-floor
        // test above — `y`'s auto-drop runs as Build's own scope-end
        // epilogue, so Res.alive is read from Main, strictly after Build's
        // frame (and its auto-drop) has fully unwound.
        "  public static method Build(): int "
        "    var w := new Widget(out owned var x, out owned var y); "
        "    w.Dispose(); "
        "    x.Dispose(); "
        "    return 0; "
        // `y` is never touched again — dropped on the floor.
        "  end Build; "
        "  public static method Main(): int "
        "    Res.alive := 0; "
        "    _ := C.Build(); "
        "    return Res.alive; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    // Res.alive settles at 0: +1/+1 mint (a, b), -1 explicit (x==a), -1
    // auto-drop (y==b).
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
