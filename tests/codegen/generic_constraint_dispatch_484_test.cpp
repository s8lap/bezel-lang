// Codegen — generic method constraint-bound member dispatch (issue #484).
//
// §14.2: "When a type parameter T is constrained by `where T: ISome` (or
// `where T: Base`), a call `x.M()` where `x: T` must dispatch to the
// interface slot (itable dispatch) or the most-derived override (vtable
// dispatch), exactly as if the receiver had the constraint type as its
// declared type."
//
// Before the fix: `resolveMemberAccess` in ExprTypecheck_Calls.cpp had no
// path to look a member up against a type-parameter's constraint.  The target
// type was resolved as null (a bare type-param TypeRef, declaration==nullptr)
// and the entire if (!targetType) block had no constraint-bound look-up; the
// callee MemberAccessExpr received no exprSymbol; lowerCall returned nullptr;
// emitReturn emitted `ret i32 0` — the observed zero.
//
// Tests derive directly from §14.2:
//   (a) interface constraint `where T: INum`  → x.Val() returns 42.
//   (b) class constraint    `where T: Base`   → dispatches to Derived override
//       (returns 2, not 1).
//   (c) non-generic DirectGet returns 42      (control: already correct).
//   (d) constraint method result used in arithmetic.
//   (e) two type params each constrained.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── (a) interface constraint: where T: INum ──────────────────────────────────

// §14.2: a generic method constrained `where T: INum` calls `x.Val()` via the
// itable — resolves to Num.Val() which returns _n=42.
// Before the fix: 0 (no exprSymbol, lowerCall → nullptr, ret i32 0).
BEZEL_TEST(generic_constraint_dispatch_484, interface_constraint_val_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  method Val(): int; end INum; "
        "class Num : INum "
        "  public _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Val(): int => _n; "
        "  destructor end destructor; "
        "end Num; "
        "static class Utils "
        "  public static method GenGet<T>(x: T): int where T: INum "
        "    return x.Val(); "
        "  end GenGet; "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var n := new Num(42); "
        "  return Utils.GenGet(n); "
        "end Main; end C;"), 42);
}

// §14.2: expression-body form of the generic method (same constraint).
BEZEL_TEST(generic_constraint_dispatch_484, interface_constraint_expr_body_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  method Val(): int; end INum; "
        "class Num : INum "
        "  public _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Val(): int => _n; "
        "  destructor end destructor; "
        "end Num; "
        "static class Utils "
        "  public static method GenGet<T>(x: T): int where T: INum => x.Val(); "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var n := new Num(42); "
        "  return Utils.GenGet(n); "
        "end Main; end C;"), 42);
}

// ── (b) class constraint: where T: Base — dispatches to Derived override ─────

// §14.2: `where T: Base` — the call `x.Rank()` must reach the Derived
// override (Rank()=2) not the Base declaration (Rank()=1).
// Before the fix: 0 (same null-callee path).
BEZEL_TEST(generic_constraint_dispatch_484, class_constraint_dispatches_derived_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public constructor() end constructor; "
        "  public virtual method Rank(): int => 1; "
        "  destructor end destructor; "
        "end Base; "
        "class Derived : Base "
        "  public constructor() end constructor; "
        "  public override method Rank(): int => 2; "
        "  destructor end destructor; "
        "end Derived; "
        "static class Utils "
        "  public static method GetRank<T>(x: T): int where T: Base "
        "    return x.Rank(); "
        "  end GetRank; "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  return Utils.GetRank(d); "
        "end Main; end C;"), 2);
}

// ── (c) control: non-generic DirectGet already correct ───────────────────────

// §14.2 / §12: a non-generic method whose parameter has a concrete interface
// type must already dispatch correctly (this is CG35 itable dispatch, not the
// new fix). Verifies the test harness is sound and the fixture compiles.
BEZEL_TEST(generic_constraint_dispatch_484, non_generic_direct_get_control_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  method Val(): int; end INum; "
        "class Num : INum "
        "  public _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Val(): int => _n; "
        "  destructor end destructor; "
        "end Num; "
        "static class Utils "
        "  public static method DirectGet(x: INum): int => x.Val(); "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var n := new Num(42); "
        "  return Utils.DirectGet(n); "
        "end Main; end C;"), 42);
}

// ── (d) constraint method result used in arithmetic ───────────────────────────

// §14.2: the value returned through a constrained dispatch is a live integer
// and can be used in an arithmetic expression.
BEZEL_TEST(generic_constraint_dispatch_484, constraint_result_in_arithmetic_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  method Val(): int; end INum; "
        "class Num : INum "
        "  public _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Val(): int => _n; "
        "  destructor end destructor; "
        "end Num; "
        "static class Utils "
        "  public static method GenDouble<T>(x: T): int where T: INum "
        "    return x.Val() + x.Val(); "
        "  end GenDouble; "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var n := new Num(21); "
        "  return Utils.GenDouble(n); "
        "end Main; end C;"), 42);
}

// ── (e) two type params each constrained ─────────────────────────────────────

// §14.2: a generic method with two type parameters each constrained to a
// distinct interface — both dispatch paths must resolve.
BEZEL_TEST(generic_constraint_dispatch_484, two_constrained_type_params_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IVal  method Val(): int;  end IVal; "
        "interface IBase method Base(): int; end IBase; "
        "class A : IVal "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Val(): int => _v; "
        "  destructor end destructor; "
        "end A; "
        "class B : IBase "
        "  public _b: int := 0; "
        "  public constructor(b: int) _b := b; end constructor; "
        "  public method Base(): int => _b; "
        "  destructor end destructor; "
        "end B; "
        "static class Utils "
        "  public static method Sum<T, U>(x: T, y: U): int "
        "    where T: IVal where U: IBase "
        "    return x.Val() + y.Base(); "
        "  end Sum; "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var a := new A(30); "
        "  using var b := new B(12); "
        "  return Utils.Sum(a, b); "
        "end Main; end C;"), 42);
}
