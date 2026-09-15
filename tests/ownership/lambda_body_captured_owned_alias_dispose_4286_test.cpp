// Spec-derived tests for fix #4286 — `disposeAliasSource`'s alias-detection
// (fix #4236, widened to `using var` by fix #4268 and to owned FIELDS by fix
// #4283) never fired for a local declared INSIDE A LAMBDA/CLOSURE BODY that
// aliases an *owned local captured from the enclosing scope* — for `var`,
// `using var`, and `const` alike. An explicit `.Dispose()` on the alias
// double-frees identically to the original #4236 bug (measured: a live
// `bz_array_free` SIGSEGV under ASan). Filed and investigated by the review
// of fix #4268 (LANE_RULES §15's over-rejection/adjacent-axis hunt); this
// fix closes it.
//
// §28.9 "Capturing an owned class-typed local" (v0.298): capturing an owned
// class-typed local from the enclosing scope is a BORROW of that scope — the
// same K3.13 "no silent consume" principle §28.3 "Ownership state of locals"
// applies to an unmarked read of a named owned binding at method scope. So
// `var b := a;` inside a lambda body, where `a` is a captured owned local,
// makes `b` a BORROW of `a` exactly as the method-level `var b := a;` shape
// already is (fix #4236) — and §28.6 "Who may call Dispose()" ("Dispose()
// through a borrow is a compile error") then rejects `b.Dispose()`
// identically.
//
// THE GAP: `applyUsingVarBorrowPinInLambdas` (#2166) — the structural
// pre-pass that runs `handleLocalDecl` for every `LocalDecl` inside a lambda
// body — CLEARS and rebuilds `states_` from scratch for each lambda body
// (a fresh, per-lambda scope). `handleLocalDecl`'s bare-identifier alias
// check (`isOwnedTrackedLocal`) reads THIS `states_`, so a captured owned
// local from an ENCLOSING frame (the method, or an outer lambda) is never a
// key of it and the check always missed. Independently, a `const` declared
// inside a lambda body reached NEITHER this check NOR the #4283 field-read
// check AT ALL — `applyUsingVarBorrowPinInLambdaStmt`'s `LocalConstDecl` arm
// previously did nothing but hunt for nested lambdas (there is no
// `handleLocalConstDecl` this call site could reuse, unlike the `LocalDecl`
// arm's reuse of `handleLocalDecl`).
//
// THE FIX: `handleLocalDecl` gains a `fromLambdaBody` parameter (defaulted
// `false`, so the method-level call site is untouched) and, when true and
// the primary own-scope check misses, retries via
// `isOwnedTrackedLocalInEnclosingFrame` — a NEW, narrow predicate asking "is
// this captured symbol's OWN declaration a fresh owned production"
// (`producerState` on the declaration's own initializer), deliberately NOT
// built on the existing `enclosingFrameOwnedLocalBorrowSource` (measured:
// that helper's underlying `localIsOwnedObligation` seed answers a
// dataflow-order-dependent question that is false for exactly this issue's
// repro — see `Ownership_impl.hpp`'s `isOwnedTrackedLocalInEnclosingFrame`
// declaration for the two independent, measured reasons). The lambda-body
// `LocalConstDecl` arm gains the SAME three-part check (own-frame identifier,
// enclosing-frame retry, #4283 field-read) `visitStmt`'s method-level
// `LocalConstDecl` arm already has, mirrored rather than reused (there is no
// shared function to reuse for this call site).
//
// SCOPE (LANE_RULES §6, deliberate, matching #4283's own fence): only the two
// DECLARATION sites are widened. `handleAssignment` and the lambda-body
// reassignment arm are left untouched — `fix #4272` (banked in this same
// batch) added reassignment-axis rows (`y := h.item; y.Dispose();`, and its
// lambda-body twin `axis_member_access_rebind_after_reclaim_no_crash_28_3`)
// that deliberately pin the CURRENT accept-and-runtime-protected reassignment
// behavior; widening the reassignment sites too would flip those rows to a
// compile-time rejection.
//
// DEFERRED: a captured `owned` PARAMETER (rather than a local) is a sibling
// gap this fix does NOT close — `isOwnedTrackedLocalInEnclosingFrame` is
// restricted to a `LocalSymbol` root. Measured still-open (issue #4304).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// REJECT arm — a captured owned LOCAL, all three declaration spellings.
// =====================================================================

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4286,
           plain_var_alias_of_captured_owned_local_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a := new Array<int>(2); a.Add(1); "
        "  var f := lambda() => do "
        "    var b := a; b.Dispose(); return 0; "
        "  end; "
        "  var r := f(); a.Dispose(); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4286,
           using_var_alias_of_captured_owned_local_dispose_rejected_28_10) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a := new Array<int>(2); a.Add(1); "
        "  var f := lambda() => do "
        "    using var b := a; b.Dispose(); return 0; "
        "  end; "
        "  var r := f(); a.Dispose(); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4286,
           const_alias_of_captured_owned_local_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a := new Array<int>(2); a.Add(1); "
        "  var f := lambda() => do "
        "    const b := a; b.Dispose(); return 0; "
        "  end; "
        "  var r := f(); a.Dispose(); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// REJECT arm — a lambda nested TWO levels deep. The capture chases through
// BOTH frames: `a` lives at the OUTERMOST method scope, and the alias is
// declared inside the INNER lambda, two frames in. `isOwnedTrackedLocalInEnclosingFrame`
// resolves straight to `a`'s own declaration regardless of how many lambda
// frames sit between it and the alias — no per-depth special-casing needed.
// =====================================================================

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4286,
           nested_two_levels_deep_alias_of_captured_owned_local_rejected_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a := new Array<int>(2); a.Add(1); "
        "  var outer := lambda() => do "
        "    var inner := lambda() => do "
        "      var b := a; b.Dispose(); return 0; "
        "    end; "
        "    return inner(); "
        "  end; "
        "  var r := outer(); a.Dispose(); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// =====================================================================
// REJECT arm — a lambda capturing a FIELD-rooted owned value (`this._arr`).
// `var`/`using var` already reached this via fix #4283's widening of
// `handleLocalDecl` (which IS reused for lambda bodies) — a REGRESSION PIN,
// not new here. `const` is NEW: the lambda-body `LocalConstDecl` arm gained
// the field-read check for the first time in this fix (previously it ran
// NEITHER the identifier NOR the field-read check for ANY lambda-body
// `const`).
// =====================================================================

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4286,
           var_alias_of_captured_owned_field_dispose_rejected_28_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); end constructor; "
        "  public method Run(): int "
        "    var f := lambda() => do "
        "      var b := this._arr; b.Dispose(); return 0; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); return h.Run(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `_arr`"));
}

BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4286,
           const_alias_of_captured_owned_field_dispose_rejected_28_3) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private owned _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(2); end constructor; "
        "  public method Run(): int "
        "    var f := lambda() => do "
        "      const b := this._arr; b.Dispose(); return 0; "
        "    end; "
        "    return f(); "
        "  end Run; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); return h.Run(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `_arr`"));
}

// =====================================================================
// ACCEPT-side controls (LANE_RULES §15) — programs that MUST keep compiling
// and running leak/UAF-free; a false-reject or a regression here would be
// worse than #4286 itself.
// =====================================================================

// A lambda that produces its OWN fresh value and disposes it: the
// initializer is a NewExpr, never an IdentifierExpr, so neither the primary
// nor the new enclosing-frame-retry alias check ever engages, by
// construction — the genuinely-owning case must stay legal.
BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4286,
           lambda_owns_and_disposes_its_own_value_still_compiles_28_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var f := lambda() => do "
        "    var b := new Array<int>(2); b.Add(1); b.Dispose(); return 0; "
        "  end; "
        "  return f(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A capture that is only READ (no alias declared at all, and no `.Dispose()`
// anywhere in the lambda) must stay accepted and must not affect the
// captured local's own disposal at the enclosing scope. Loop-amplified
// (LANE_RULES §3/§30): a one-shot leak/UAF row can pass on both sides.
BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4286,
           captured_local_read_only_no_alias_frees_exactly_once_28_9) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    var a := new Array<int>(2); a.Add(1); "
        "    var f := lambda() => do return a.Count; end; "
        "    var n := f(); "
        "    a.Dispose(); "
        "    i := i + n - n; i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A captured owned local aliased inside the lambda (`var b := a;`) but the
// alias is NEVER disposed — merely SETTING `disposeAliasSource` must be
// inert at runtime (codegen never consults it; only the dispose-call-site
// check does), and the captured local's own disposal at the enclosing scope
// must still free exactly once. Loop-amplified.
BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4286,
           captured_local_alias_never_disposed_frees_exactly_once_28_3) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    var a := new Array<int>(2); a.Add(1); "
        "    var f := lambda() => do var b := a; return b.Count; end; "
        "    var n := f(); "
        "    a.Dispose(); "
        "    i := i + n - n; i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The FENCED reassignment axis, one scope further in than the method-level
// #4272 row: `y := h.item; y.Dispose();` inside a LAMBDA BODY, mirroring the
// already-banked
// `lambda_reassign_explicit_dispose_reclaimed_slot_4270.axis_member_access_rebind_after_reclaim_no_crash_28_3`.
// This fix touches NEITHER `handleAssignment` NOR the lambda-body
// reassignment arm, so this shape must stay compiling and runtime-protected
// exactly as it already is. Loop-amplified, more modestly than the banked
// row (which already carries the full 150-iteration measurement) — this row
// exists so a reader of #4286's OWN test file sees the fence held, without
// needing to cross-reference #4270/#4272.
BEZEL_TEST(lambda_body_captured_owned_alias_dispose_4286,
           fenced_member_access_reassignment_inside_lambda_stays_accepted_28_3) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Holder "
        "  public item: Res; "
        "  public constructor(r: Res) this.item := r; end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 50 do "
        "    var f := lambda(): int => do "
        "      var x := new Res(5); "
        "      var y := x; "
        "      var h := new Holder(x); "
        "      y := h.item; "
        "      y.Dispose(); "
        "      x.Dispose(); "
        "      return 0; "
        "    end; "
        "    var n := f(); "
        "    i := i + n - n; i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}
