// fix #2944 / §12.1 "Members are non-virtual by default" / §14.1 "Generic
// declarations" / §23 "Properties and Indexers" / §2.8.3 rule 4, v0.240.
//
// #2928 put the per-instantiation body in the per-instantiation vtable slot
// for a virtual PROPERTY whose return type DOES NOT MOVE under substitution
// (`Item: int` on `GBase<T>` — both template and spec0 return i32). #2944 is
// the SAME DEFECT, ONE TYPE PARAMETER OVER: a virtual PROPERTY whose declared
// return type IS a generic base's TYPE PARAMETER (`Item: T`) and is overridden
// at the concrete type (`Item: string`). The slot's spec0 body returns the
// concrete type (`string` = ptr) while the call site — `g.Item` where
// `g: GBase<string>`, dynamic type `D2<int>` — computed its EXPECTED return
// type from the AST `pd->type` (the base `T`, which `lowerType` resolves to
// the i32 ERASED PLACEHOLDER because no `genericSubst_` is active at the
// dispatch site). Mismatch aborts the build with a raw IR-verifier report
// (no source location):
//
//     GEP base pointer is not a vector or a vector of pointers
//       %22 = getelementptr inbounds nuw %bz.string, i32 %17, 0, 1
//     Load operand must be a pointer.
//       %str.len = load i64, i32 %22, align 4
//     Function return type does not match operand type of return inst!
//       ret i32 %2
//
// THE METHOD TWIN (§2906) was already correct because the only expression in
// its template body is `return v;`, which routes through `emitReturn`'s
// `emittingGenericTemplateBody_` guard and substitutes a type-correct `ret
// null` (LlvmBackend_Setup.cpp ~10818). The PROPERTY EXPRESSION GETTER
// (`ensurePropertyAccessorFn`, LlvmBackend_Assign.cpp) bypasses emitReturn
// and calls `CreateRet` directly, so the body also needed an analogous guard
// at its `CreateRet` site to keep the template body IR-valid when the spec
// signature moves away from i32 — both pieces were required to land the
// program end-to-end.
//
// THE FIX (NARROWER than the previous lane's commit c5974ef). Two parts:
//   1. Call-site signature derivation in `lowerVirtualPropertyGet`
//      (LlvmBackend_Exprs.cpp): prefer the sema-resolved `exprType(e)` —
//      which has already applied the static type's `T → string` binding —
//      over the AST `pd->type`, which resolves to i32 when no
//      `genericSubst_` is active. This makes the indirect-call signature
//      match the per-instantiation slot's actual signature.
//   2. Template-body coercion in `ensurePropertyAccessorFn`
//      (LlvmBackend_Assign.cpp): mirror `emitReturn`'s
//      `emittingGenericTemplateBody_` guard at the property getter's body
//      emission sites — the expression-getter's `CreateRet` arm and the
//      block-getter's body emitter.
//
// NARROWER PREDICATE. Both arms gate on
// `accessorSpecTagNow() == 0 && !cls->genericParams.empty()` — i.e. only the
// shared/erased template body of a GENERIC class. The previous lane's commit
// (`c5974ef`) gated only on `accessorSpecTagNow() == 0`, which is the same
// flag-0 predicate as the §2759 residual's path. The §2759 residual test
// (`virtual_expr_getter_still_unbuildable_issue_2759`) is on a NON-GENERIC
// class (`Base`) with `public virtual property O: Object => 0;`, and it is
// deliberately pinned as a BUILD FAILURE: setupBoxDescriptors runs after
// declareVtables, so the §8.6 box vtable is empty inside the vtable accessor's
// body, and `boxForTarget` is inert. Moving the coercion arm out of that path
// preserves the residual. Restricting to generic classes is the natural
// boundary — the substitution-erased-template shape that needs coercion ONLY
// exists when the property's declaring class carries generic parameters (so
// its bodies may reach type-parameter fields with no active substitution).
//
// SCOPE NOTES:
//   * The shared/erased vtable is used only for UNSPECIALIZED generic
//     instances (`class D2<U>` referenced without binding U); for any concrete
//     `D2<int>` the spec0 vtable installed by `specializedVtableFor`
//     holds the correctly-typed spec body, and its callers route through
//     THAT slot.
//   * The importable analog (cross-module) is independent — separate vtable
//     machinery, separate test path — and is not exercised here.
//   * The block-bodied (`get => do return v; end;`) variant goes through
//     `ensurePropertyAccessorFn`'s BLOCK arm (the `else if (blockGet)` branch
//     a few lines below the expression getter), which the narrow predicate
//     ALSO arms for generic classes. The non-virtual block-getter path
//     (`ensurePropertyBlockGetter`, called from `lowerVirtualPropertyGet`'s
//     sibling) is UNCHANGED by this fix — those bodies are not on the
//     shared-template emission path.
//
// Every case runs end to end under generated-code AddressSanitizer /
// LeakSanitizer and asserts the VALUE, so a fix that only makes the verifier
// happy is not evidence the value is right.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// MAINTENANCE NOTE — the property type `T` in `GBase<T>` must resolve to a
// DIFFERENT LLVM representation when bound to `string` (ptr) than to the
// template's i32 erased placeholder. The two `T`-typed fields (v, v2) before
// the `a: int` make the layout move under substitution; without that move,
// `specializedVtableFor` would not build a spec0 vtable (its predicate is
// `instanceLayoutDiffersFromErasedTemplate`, false for layout-stable
// instantiations), and condition (d) would never install a spec body — the
// shared template would still be called, and the test would no longer pin
// the defect.
const char* kGBase =
    "class GBase<T> "
    "  public v: T; "
    "  public v2: T; "
    "  public a: int := 41; "
    "  public virtual property Item: T "
    "    get => v; "
    "  end; "
    "end GBase; ";

}  // namespace

// ===========================================================================
// T0 — THE ISSUE'S OWN REPRO. §12.1 / §14.1 / §23. Pre-fix: raw IR-verifier
// abort on a legal single-module program. Post-fix: 4 (`"bbbb".Length`).
// The override says `Item: string`, the base `Item: T` is bound to `string`
// by the base clause, and dispatch goes through the spec0 vtable for the
// concrete `D2<int>` instance. This is the EXPRESSION-GETTER path, fixed by
// the `CreateRet` arm in `ensurePropertyAccessorFn` (NARROW predicate).
// ===========================================================================
BEZEL_TEST(virtual_property_generic_base_type_param_concrete_override_2944,
           issue_repro_spec_12_1_14_1_23) {
    auto r = runAotChecked(
        std::string("module H; ") + kGBase +
        "class D2<U> : GBase<string> "
        "  public dv: U; "
        "  public own: int := 5; "
        "  public override property Item: string "
        "    get => v2; "
        "  end; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  d.v := \"aa\"; "
        "  d.v2 := \"bbbb\"; "
        "  var g: GBase<string> := d; "
        "  return g.Item.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// ===========================================================================
// T1 — THE METHOD TWIN OF #2944 (already worked via §2906 + §817's
// `emitReturn` template guard). Pinning the value so any regression in the
// shape #2944 is fixing does not silently break the working twin.
// Pre-fix: probed at 48 (issue #2906 measured). Post-#2944-fix: still 5.
// ===========================================================================
BEZEL_TEST(virtual_property_generic_base_type_param_concrete_override_2944,
           method_twin_still_works_spec_12_1) {
    auto r = runAotChecked(
        "module H; "
        "class GBase<T> "
        "  public v: T; "
        "  public virtual method Get(): T "
        "    return v; "
        "  end Get; "
        "end GBase; "
        "class D2<U> : GBase<string> "
        "  public override method Get(): string "
        "    return v; "
        "  end Get; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  d.v := \"hello\"; "
        "  var g: GBase<string> := d; "
        "  var s: string := g.Get(); "
        "  return s.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// T2 — A NON-GENERIC OVERRIDE of a non-type-param property. Sema accepts and
// the codegen path is identical to the non-virtual property read. Ensures
// the call-site fix did not regress a shape that did not need it.
// ===========================================================================
BEZEL_TEST(virtual_property_generic_base_type_param_concrete_override_2944,
           non_generic_concrete_override_unaffected_spec_12_1) {
    auto r = runAotChecked(
        "module H; "
        "class GBase "
        "  public v: string := \"hello\"; "
        "  public virtual property Item: string "
        "    get => v; "
        "  end; "
        "end GBase; "
        "class D2 : GBase "
        "  public override property Item: string "
        "    get => \"world!\"; "
        "  end; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2(); "
        "  var g: GBase := d; "
        "  return g.Item.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);  // "world!".Length
}

// ===========================================================================
// T3 — THE BLOCK-FORM TWIN. The block-bodied property getter goes through
// `ensurePropertyAccessorFn`'s BLOCK arm (`else if (blockGet)` branch),
// which the narrow predicate ALSO arms for generic classes. Pinning both
// shapes so a regression that moves only one is caught.
// ===========================================================================
BEZEL_TEST(virtual_property_generic_base_type_param_concrete_override_2944,
           block_form_fixed_spec_23_3) {
    auto r = runAotChecked(
        "module H; "
        "class GBase<T> "
        "  public v: T; "
        "  public virtual property Item: T "
        "    get => do return v; end; "
        "  end; "
        "end GBase; "
        "class D2<U> : GBase<string> "
        "  public override property Item: string "
        "    get => do return v; end; "
        "  end; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2<int>(); "
        "  d.v := \"x\"; "
        "  var g: GBase<string> := d; "
        "  return g.Item.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// T4 — §2759 RESIDUAL, PINNED. // per §12 / §8.6, issue #2759
// A VIRTUAL `O: Object` on a NON-GENERIC `Base`. Same mismatch shape as
// #2944 (body i32, function ptr), but the class has no generic params, so
// the narrow predicate does NOT arm the coercion — and this test stays as
// the deliberate build failure §2759's residual was pinned as. If a future
// patch re-narrows this (by arming the flag for non-generic classes too),
// this test goes GREEN and the regression is loud.
// ===========================================================================
BEZEL_TEST(virtual_property_generic_base_type_param_concrete_override_2944,
           section_2759_residual_still_unbuildable_spec_12_8_6) {
    JitResult r = runAot(
        "module M; "
        "class Base "
        "  public virtual property O: Object => 0; "
        "end Base; "
        "class Derived : Base "
        "  public override property O: Object => 7; "
        "end Derived; "
        "class P public static method Main(): int "
        "  var d := new Derived(); "
        "  var b: Base := d; "
        "  var o := b.O; "
        "  if o is int then return 41; end; "
        "  return 1; "
        "end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
