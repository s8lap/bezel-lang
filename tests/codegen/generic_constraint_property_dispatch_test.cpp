// Codegen — generic method constraint-bound PROPERTY dispatch (issue #486).
//
// §14.2: "When a type parameter T is constrained by `where T: ISome` (or
// `where T: Base`), a member access `x.Prop` where `x: T` must dispatch to
// the implementing type's property accessor — through the itable (interface
// constraint) or vtable (abstract/virtual class constraint) — exactly as for
// virtual method calls."
//
// This is the PROPERTY analog of issue #484 (method dispatch).  Before the
// fix, codegen for `x.Prop` in a constraint-bound generic method body would:
//   1. Call lowerVirtualPropertyGet, which returns nullptr because the
//      receiver `x: T` lowers to i32 (T's fallback) in the unspecialized body.
//   2. Fall through to computedGetterOf / blockGetterOf on the constraint
//      type's PropertySymbol — returns nullptr (interface / abstract have no body).
//   3. Fall through to fieldAddr, which calls lowerExpr on the receiver
//      (returns i32), then emits a StructGEP with the i32 as base pointer —
//      "GEP base pointer is not a vector" → IR verifier crash, or in the
//      specialized body an itable/vtable load with a wrong receiver type →
//      "Cannot emit physreg copy instruction" (exit 134).
//
// Tests derive directly from §14.2:
//   (a) interface constraint `where T: IHasName` → x.Name returns "Alice" (1).
//   (b) abstract-class constraint `where T: Shape` → x.Area dispatches override.
//   (c) virtual-(non-abstract)-class constraint → control, already worked with #481.
//   (d) property result used in arithmetic (int property).
//   (e) property result used in comparison (string property returns 1/0).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── (a) interface constraint: where T: IHasName  → string property ────────────

// §14.2: a generic method constrained `where T: IHasName` reads `x.Name` via
// the itable — resolves to Person.Name which returns _name = "Alice".
// Before the fix: crash exit 134 ("Cannot emit physreg copy instruction").
BEZEL_TEST(generic_constraint_property_dispatch, interface_constraint_string_property_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IHasName  property Name: string; end IHasName; "
        "class Person : IHasName "
        "  public _name: string; "
        "  public constructor(n: string) _name := n; end constructor; "
        "  public property Name: string => _name; "
        "  destructor end destructor; "
        "end Person; "
        "static class Namer "
        "  public static method GetName<T>(x: T): string where T: IHasName "
        "    return x.Name; "
        "  end GetName; "
        "end Namer; "
        "class C public static method Main(): int "
        "  using var p := new Person(\"Alice\"); "
        "  var n := Namer.GetName(p); "
        "  return (if n = \"Alice\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// §14.2: expression-body form of the generic method (interface constraint,
// string property).
BEZEL_TEST(generic_constraint_property_dispatch, interface_constraint_expr_body_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IHasName  property Name: string; end IHasName; "
        "class Person : IHasName "
        "  public _name: string; "
        "  public constructor(n: string) _name := n; end constructor; "
        "  public property Name: string => _name; "
        "  destructor end destructor; "
        "end Person; "
        "static class Namer "
        "  public static method GetName<T>(x: T): string where T: IHasName => x.Name; "
        "end Namer; "
        "class C public static method Main(): int "
        "  using var p := new Person(\"Bob\"); "
        "  var n := Namer.GetName(p); "
        "  return (if n = \"Bob\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// ── (b) abstract-class constraint: where T: Shape → int property ─────────────

// §14.2: `where T: Shape` — `s.Area` dispatches to Square's override (49),
// not Shape's abstract declaration (which has no body and would crash).
// Before the fix: crash exit 134.
BEZEL_TEST(generic_constraint_property_dispatch, abstract_class_constraint_int_property_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Shape  public abstract property Area: int; end Shape; "
        "class Square : Shape "
        "  public _s: int; "
        "  public constructor(s: int) _s := s; end constructor; "
        "  public override property Area: int => _s * _s; "
        "  destructor end destructor; "
        "end Square; "
        "static class Calc "
        "  public static method GetArea<T>(s: T): int where T: Shape => s.Area; "
        "end Calc; "
        "class C public static method Main(): int "
        "  using var sq := new Square(7); "
        "  return Calc.GetArea(sq); "
        "end Main; end C;"), 49);
}

// §14.2: block-body form of the generic method (abstract class constraint).
BEZEL_TEST(generic_constraint_property_dispatch, abstract_class_constraint_block_body_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Shape  public abstract property Area: int; end Shape; "
        "class Circle : Shape "
        "  public _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public override property Area: int => _r * _r; "
        "  destructor end destructor; "
        "end Circle; "
        "static class Calc "
        "  public static method GetArea<T>(s: T): int where T: Shape "
        "    return s.Area; "
        "  end GetArea; "
        "end Calc; "
        "class C public static method Main(): int "
        "  using var c := new Circle(3); "
        "  return Calc.GetArea(c); "
        "end Main; end C;"), 9);
}

// ── (c) virtual (non-abstract) class constraint → control ────────────────────

// §14.2 / §12.1: `where T: Base` with a virtual (not abstract) property —
// the override returns 2, not 1.  This is the class-vtable path (parallel to
// the itable path for interface constraints).
BEZEL_TEST(generic_constraint_property_dispatch, virtual_class_constraint_property_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public constructor() end constructor; "
        "  public virtual property Value: int => 1; "
        "  destructor end destructor; "
        "end Base; "
        "class Child : Base "
        "  public constructor() end constructor; "
        "  public override property Value: int => 2; "
        "  destructor end destructor; "
        "end Child; "
        "static class Utils "
        "  public static method GetValue<T>(x: T): int where T: Base => x.Value; "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var c := new Child(); "
        "  return Utils.GetValue(c); "
        "end Main; end C;"), 2);
}

// ── (d) property result used in arithmetic ────────────────────────────────────

// §14.2: the value returned through an interface-constrained property dispatch
// is a live integer that can be used in arithmetic.
BEZEL_TEST(generic_constraint_property_dispatch, constraint_property_result_in_arithmetic_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  property Val: int; end INum; "
        "class Num : INum "
        "  public _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Val: int => _n; "
        "  destructor end destructor; "
        "end Num; "
        "static class Utils "
        "  public static method Double<T>(x: T): int where T: INum "
        "    return x.Val + x.Val; "
        "  end Double; "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var n := new Num(21); "
        "  return Utils.Double(n); "
        "end Main; end C;"), 42);
}

// ── (e) interface constraint — two type params each constrained ───────────────

// §14.2: a generic method with two type parameters each constrained to a
// distinct interface — both property dispatch paths must resolve.
BEZEL_TEST(generic_constraint_property_dispatch, two_constrained_type_params_property_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IVal   property Val: int;  end IVal; "
        "interface IBase  property Base: int; end IBase; "
        "class A : IVal "
        "  public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property Val: int => _v; "
        "  destructor end destructor; "
        "end A; "
        "class B : IBase "
        "  public _b: int := 0; "
        "  public constructor(b: int) _b := b; end constructor; "
        "  public property Base: int => _b; "
        "  destructor end destructor; "
        "end B; "
        "static class Utils "
        "  public static method Sum<T, U>(x: T, y: U): int "
        "    where T: IVal where U: IBase "
        "    return x.Val + y.Base; "
        "  end Sum; "
        "end Utils; "
        "class C public static method Main(): int "
        "  using var a := new A(30); "
        "  using var b := new B(12); "
        "  return Utils.Sum(a, b); "
        "end Main; end C;"), 42);
}

// ── (f) interface constraint with property result in comparison ───────────────

// §14.2: the property dispatched through an interface constraint produces a
// live value suitable for comparison.  The int property returns 42; compare to
// verify it is not 0 (the dead-stub no-op value).
BEZEL_TEST(generic_constraint_property_dispatch, interface_constraint_int_property_compare_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface ICode  property Code: int; end ICode; "
        "class Tag : ICode "
        "  public _c: int := 0; "
        "  public constructor(c: int) _c := c; end constructor; "
        "  public property Code: int => _c; "
        "  destructor end destructor; "
        "end Tag; "
        "static class Check "
        "  public static method IsFortyTwo<T>(x: T): int where T: ICode "
        "    return (if x.Code = 42 then 1 else 0); "
        "  end IsFortyTwo; "
        "end Check; "
        "class C public static method Main(): int "
        "  using var t := new Tag(42); "
        "  return Check.IsFortyTwo(t); "
        "end Main; end C;"), 1);
}
