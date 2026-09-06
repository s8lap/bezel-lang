// Spec-derived regression tests for issue #3937 — a `string`-typed `out var`
// recipient DECLARED DIRECTLY IN A DESTRUCTOR BODY was never released at scope
// end: both allocations (the `bz_string` header and its payload) leaked, per
// disposal. The byte-identical call in an ORDINARY method was already clean
// (fix #2641 / #3305, v0.268) — the destructor-body venue was the residual gap.
//
// THE MECHANISM (established by inspecting `BEZEL_DUMP_IR=1` output for the
// issue's own repro, HEAD before this fix): `bz$M.Holder$DisposeCore` called
// `bz$M.W$Make(..., %2)` and later LOADED `%2` to read `.Length` — but emitted
// no `bz_string_release` on ANY edge (normal or unwind). The release was NEVER
// EMITTED (not emitted-into-dead-code, not emitted-against-the-wrong-owner) —
// `registerOutVarStringRelease` / `registerCtorOutVarStringRelease`
// (`LlvmBackend_Calls2.cpp`) each had a "gate 5" —
// `if (disposeStack_.empty() || emittingDisposerBody_) return;` — that bailed
// UNCONDITIONALLY whenever the recipient was declared inside ANY destructor
// body, regardless of whether that body's string-local traffic had actually
// been analyzed.
//
// WHY THAT EXCLUSION WAS STALE, NOT SOUND. Fix #2852 (v0.240, predating fix
// #2641's v0.268 introduction of `registerOutVarStringRelease` itself) already
// made `emitDisposerCore` (`LlvmBackend_Disposal.cpp`) run
// `collectStringReleaseEligible(dd->body)` and pin
// `stringReleaseEligibleFn_ = fn` for the destructor body BEFORE
// `emittingDisposerBody_` is even set — which is exactly what lets a plain
// `var` string LOCAL declared directly in a destructor body release at scope
// end today (`armStringLocalRefcount`, gated on nothing but that pin plus
// `disposeStack_`, with NO `emittingDisposerBody_` check at all). An `out var`
// recipient is analyzed by the SAME walk
// (`outVarStringReleaseEligible`/`stringReleaseAnalyzedBody_`, gate 4) and
// registered into the SAME `disposeStack_`, which `lowerScopedBlock` already
// drains correctly inside a destructor body (proven by the `var`-local case).
// So gate 5's `emittingDisposerBody_` half was redundant with — and stricter
// than — gate 3's `currentFn_ == stringReleaseEligibleFn_` check, which
// already correctly answers "was this body's string traffic analyzed" for a
// disposer body. THE FIX drops the `emittingDisposerBody_` half of gate 5 in
// both functions; gate 3's existing per-body pin is the sole authority left
// (CLAUDE.md rule 6 — no new predicate introduced).
//
// RELATIONSHIP TO #3936 (checked explicitly, per the issue brief). #3936 is a
// state-restoration defect: `emitDisposerCore` leaves `emittingDisposerBody_`
// / `disposerEntryExc_` reset to a NEUTRAL value (not the caller's) after a
// NESTED disposer emission mid-outer-body, so a statement AFTER a nested
// generic disposer call silently flips from disposer-body codegen to
// ordinary-method codegen. #3937's own repro (`Holder`'s destructor calls only
// a plain static method, `W.Make`, and constructs no generic instantiation at
// all) never enters `emitDisposerCore` a second time, so
// `emittingDisposerBody_` is simply, correctly, `true` for the whole body —
// there is no nested emission and nothing is left unrestored. #3937 is
// therefore NOT the #3936 defect: it is a plain over-broad gate that fires
// even when the (correctly-set) flag it reads should not have disqualified
// the registration in the first place, given gate 3's pin. Fixed independently
// of #3936, which remains open and latent.
//
// DOUBLE-FREE CHECK. Newly releasing a recipient risks a double-release if
// something else already released it. `loop_reused_out_var_slot_in_
// destructor_is_leak_free` below amplifies the #2629-style loop-reused-slot
// shape (`nullSlotAfterRelease`'s load-bearing case) to 5 calls per disposal x
// 500 disposals = 2500 releases INSIDE a destructor body, and the whole-file
// verification ran the single-call shape at 500 iterations under ASan/LSan
// (`scripts/agent_check.sh --probe --asan`, generated-code sanitizer, NOT a
// one-shot) — clean on both counts. See the commit body for the full
// measurement.
//
// per §22.3, §22.4, §28.4, §28.9, v0.294

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ASan + LSan oracle: a leak / double-free / UAF report at exit surfaces as
// ran=false, which this flips to the -999 sentinel.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Plain (non-sanitized) oracle — value only.
int run(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// -----------------------------------------------------------------------------
// Issue #3937's own repro, loop-amplified: 500 constructions/disposals of
// `Holder`, whose destructor body calls a plain static method with a `string`
// `out var` recipient it reads (`nm.Length`) and discards. §28.4 step 1 runs
// the destructor body before the (empty, here) owned-field cascade; the
// recipient's scope-end release must fire at the END of the destructor body,
// not be skipped because the body IS a destructor body. Pre-fix: LSan reports
// 20500 B / 1000 allocations (measured — the issue's own number). Post-fix:
// clean, `t == 500`.
// per §22.3, §22.4, §28.4, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(destructor_out_var_string_release_3937,
           single_call_out_var_string_recipient_in_destructor_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class Holder "
        "  public tag: int; "
        "  public constructor() tag := 0; end constructor; "
        "  destructor "
        "    var okv := W.Make(\"abc\", out var nm); "
        "    _ := okv; "
        "    tag := nm.Length; "
        "  end destructor; "
        "end Holder; "
        "class C "
        "  public static method Run(): int "
        "    do using var h := new Holder(); end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return t; "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// CONTROL — the byte-identical call in an ORDINARY method (not a destructor
// body). This was already clean before this fix (fix #2641); pinned here so a
// future regression that reintroduces a destructor-body-only gate is caught by
// contrast with the test above, exactly as the issue's own repro isolated the
// venue.
// per §22.3, §22.4, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(destructor_out_var_string_release_3937,
           control_ordinary_method_out_var_string_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class C "
        "  public static method Run(): int "
        "    var okv := W.Make(\"abc\", out var nm); "
        "    _ := okv; "
        "    return nm.Length - 8; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return t; "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// DOUBLE-FREE HUNT — a LOOP inside the destructor body reuses the recipient's
// entry-block-hoisted slot across iterations (the #2629 shape,
// `Disposal::nullSlotAfterRelease`'s load-bearing case, now exercised INSIDE a
// disposer body for the first time). 5 out-var calls per disposal x 500
// disposals = 2500 release/null cycles. If the slot were not nulled after
// release, the next loop iteration's callee-side load in `lowerStringRefParamStore`
// would read already-freed memory — a double-release ASan reports rather than
// tolerates. LSan-clean AND value-correct is the whole bar.
// per §22.3, §22.4, §28.4, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(destructor_out_var_string_release_3937,
           loop_reused_out_var_slot_in_destructor_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class Holder "
        "  public tag: int; "
        "  public constructor() tag := 0; end constructor; "
        "  destructor "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 5 do "
        "      var okv := W.Make(\"x\", out var nm); "
        "      _ := okv; "
        "      total := total + nm.Length; "
        "      i := i + 1; "
        "    end; "
        "    tag := total; "
        "  end destructor; "
        "end Holder; "
        "class C "
        "  public static method Run(): int "
        "    do using var h := new Holder(); end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return t; "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// CONSTRUCTOR SIBLING — `registerCtorOutVarStringRelease` (the `new T(out var
// n)` form, fix #3305) had the identical gate-5 defect: a `string` `out var`
// recipient of a CONSTRUCTOR call, declared directly in a destructor body,
// also leaked pre-fix. `W` is trivial-dispose (no destructor, no owned
// fields), so the discharge-obligation ceremony for the `new W(...)` value
// itself is not in play; only the `out var` recipient's own release is under
// test.
// per §22.3, §22.4, §28.4, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(destructor_out_var_string_release_3937,
           ctor_out_var_string_recipient_in_destructor_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-tail\"; "
        "  end constructor; "
        "end W; "
        "class Holder "
        "  public tag: int; "
        "  public constructor() tag := 0; end constructor; "
        "  destructor "
        "    var w := new W(\"abc\", out var nm); "
        "    _ := w; "
        "    tag := nm.Length; "
        "  end destructor; "
        "end Holder; "
        "class C "
        "  public static method Run(): int "
        "    do using var h := new Holder(); end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return t; "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// GENERIC CLASS — the destructor of a generic instantiation (`Holder<int>`)
// goes through the same `emitDisposerCore`/`emitSpecializedDisposer` path;
// pinned here so a per-instantiation specialization does not silently regress
// the fix.
// per §22.3, §22.4, §28.4, §28.9, §14.1
// -----------------------------------------------------------------------------
BEZEL_TEST(destructor_out_var_string_release_3937,
           generic_class_destructor_out_var_string_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class Holder<T> "
        "  public payload: T; "
        "  public tag: int; "
        "  public constructor(payload: T) this.payload := payload; tag := 0; "
        "  end constructor; "
        "  destructor "
        "    var okv := W.Make(\"abc\", out var nm); "
        "    _ := okv; "
        "    tag := nm.Length; "
        "  end destructor; "
        "end Holder; "
        "class C "
        "  public static method Run(): int "
        "    do using var h := new Holder<int>(1); end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return t; "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// ESCAPE GUARD, INSIDE A DESTRUCTOR BODY — the recipient is stored into a
// class FIELD, which (per §28.9's "runtime-managed fields require no
// disposal") is NEVER separately released by `Dispose()`; the escape gate
// (`outVarStringReleaseEligible`, gate 4, an AST-only walk this fix does not
// touch) must still decline the LOCAL's scope-end release, or the field would
// be left holding a reference nothing minted. Plain oracle: this shape is a
// deliberate, pre-existing leak (leak-over-UAF; unaffected by this fix either
// way), so the assertion is the observed VALUE, not LSan-clean — mirrors
// `field_stored_out_var_recipient_is_not_released` (issue #2641) transplanted
// into a destructor body.
// per §28.9, §28.4
// -----------------------------------------------------------------------------
BEZEL_TEST(destructor_out_var_string_release_3937,
           field_stored_out_var_recipient_in_destructor_is_not_corrupted) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class Holder "
        "  public kept: string := \"\"; "
        "  public tag: int; "
        "  public constructor() tag := 0; end constructor; "
        "  destructor "
        "    var okv := W.Make(\"abc\", out var nm); "
        "    _ := okv; "
        "    kept := nm; "
        "    tag := kept.Length; "
        "  end destructor; "
        "end Holder; "
        "class C "
        "  public static method Run(): int "
        "    do using var h := new Holder(); end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run(); end; "
        "    return t; "
        "  end Main; end C;"), 0);
}
