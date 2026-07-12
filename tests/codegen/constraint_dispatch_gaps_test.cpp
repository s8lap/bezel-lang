// Codegen — two constraint-dispatch gaps (§14.2/§23).
//
// GAP R1.1 (§14.2): a NON-VIRTUAL method on a class-constraint type param
// emits malformed IR — "Call parameter type does not match function
// signature! call i32 @bz$Helper$Add(i32 ...)".
//
// Root: the unspecialized generic template body erases T to i32, so the
// receiver `args[0]` is i32. The non-virtual direct-call branch in
// emitMethodCall falls through to CreateCall(callee, args) without checking
// whether the receiver type matches the callee's first parameter type.
// The virtual-dispatch branch (issue #484) already has this guard
// ("!recv->getType()->isPointerTy() → return nullptr"); the non-virtual
// branch is missing it.
//
// GAP R1.2 (§14.2 / §23): a record property accessed via an interface
// constraint returns 0 instead of the correct value.
//
// Root: lowerVirtualPropertyGet returns nullptr early when recv is not a
// pointer (record value type); the constraint-bound property (IArea.Area)
// is an interface property with no body, so computedGetterOf/blockGetterOf
// both also return nullptr; the whole expression returns nullptr → 0.
// The method devirtualization in lowerCall (commit 738a3aa, issue record-
// constraint fix) only covered METHOD calls; PROPERTY access went through
// lowerVirtualPropertyGet which lacked the analogous devirtualization.
//
// Tests derive directly from §14.2 (generics/constraints) and §23
// (properties):
//   (a) R1.1: non-virtual class-constraint method → 7
//   (b) R1.1: abstract-class-constraint non-virtual method → 9
//   (c) R1.2: record property via interface constraint → 25
//   (d) R1.2: record property used in arithmetic via constraint → 50
//   (e) CONTROL: class property via constraint still works → 42
//   (f) CONTROL: virtual method via class constraint still works → 99
//   (g) CONTROL: record METHOD via constraint (702dede) still works → 7

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── (a) R1.1: non-virtual class-constraint method ────────────────────────────

// §14.2: `Helper.Add(a, b)` is a non-virtual instance method. In a generic
// body `G.Run<T>(h: T, ...) where T: Helper`, the receiver is erased to i32
// in the unspecialized stub. Before the fix: IR verifier crash
// ("Call parameter type does not match function signature!"). Should return 7.
BEZEL_TEST(constraint_dispatch_gaps, r1_1_nonvirtual_class_constraint_method_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Helper "
        "  public method Add(a: int, b: int): int => a + b; "
        "  destructor end destructor; "
        "end Helper; "
        "static class G "
        "  public static method Run<T>(h: T, a: int, b: int): int where T: Helper "
        "    return h.Add(a, b); "
        "  end Run; "
        "end G; "
        "class C public static method Main(): int "
        "  using var h := new Helper(); "
        "  return G.Run(h, 3, 4); "   // 3 + 4 = 7
        "end Main; end C;"), 7);
}

// §14.2: expression-body form of the generic method (same non-virtual class
// constraint). Exercises the expression-body lowering path in emitInlineGenericMethod.
BEZEL_TEST(constraint_dispatch_gaps, r1_1_nonvirtual_class_constraint_expr_body_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Helper "
        "  public method Add(a: int, b: int): int => a + b; "
        "  destructor end destructor; "
        "end Helper; "
        "static class G "
        "  public static method Run<T>(h: T, a: int, b: int): int where T: Helper "
        "    => h.Add(a, b); "
        "end G; "
        "class C public static method Main(): int "
        "  using var h := new Helper(); "
        "  return G.Run(h, 5, 2); "   // 5 + 2 = 7
        "end Main; end C;"), 7);
}

// ── (b) R1.1: abstract-class-constraint non-virtual method ───────────────────

// §14.2: an abstract class constraint with a non-virtual concrete method.
// The abstract class base declares a non-virtual method; the constraint
// `where T: Base` — concrete method call on the base should NOT dispatch
// through vtable (it is not virtual). Before the fix: same malformed IR.
BEZEL_TEST(constraint_dispatch_gaps, r1_1_abstract_class_nonvirtual_method_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Base "
        "  public method Square(x: int): int => x * x; "
        "end Base; "
        "class Derived : Base "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Derived; "
        "static class G "
        "  public static method Compute<T>(b: T, x: int): int where T: Base "
        "    return b.Square(x); "
        "  end Compute; "
        "end G; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  return G.Compute(d, 3); "  // 3 * 3 = 9
        "end Main; end C;"), 9);
}

// ── (c) R1.2: record property via interface constraint → 25 ──────────────────

// §14.2 / §23: `Square.Area` is a computed property declared on a record
// implementing `IArea`. In a generic body `GetArea<T>(x: T) where T: IArea`,
// the body reads `x.Area` — which was resolved by sema to the INTERFACE
// property `IArea.Area`. Before the fix: lowerVirtualPropertyGet returned
// nullptr (recv is a record struct, not a pointer); fell through to
// computedGetterOf(IArea.Area) → nullptr (no body); returned 0.
// Should return 25 (5 * 5).
BEZEL_TEST(constraint_dispatch_gaps, r1_2_record_property_via_interface_constraint_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IArea  property Area: int; end IArea; "
        "record Square(side: int) : IArea "
        "  public property Area: int => side * side; "
        "end Square; "
        "static class Util "
        "  public static method GetArea<T>(x: T): int where T: IArea "
        "    return x.Area; "
        "  end GetArea; "
        "end Util; "
        "class C public static method Main(): int "
        "  var s := new Square(5); "
        "  return Util.GetArea(s); "   // 5 * 5 = 25
        "end Main; end C;"), 25);
}

// §14.2 / §23: expression-body form of the generic method (same record
// property constraint). Exercises the lowerVirtualPropertyGet fix from
// within the emitInlineGenericMethod expression-body path.
BEZEL_TEST(constraint_dispatch_gaps, r1_2_record_property_interface_constraint_expr_body_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IArea  property Area: int; end IArea; "
        "record Rect(w: int, h: int) : IArea "
        "  public property Area: int => w * h; "
        "end Rect; "
        "static class Util "
        "  public static method GetArea<T>(x: T): int where T: IArea => x.Area; "
        "end Util; "
        "class C public static method Main(): int "
        "  var r := new Rect(4, 6); "
        "  return Util.GetArea(r); "   // 4 * 6 = 24
        "end Main; end C;"), 24);
}

// ── (d) R1.2: record property used in arithmetic via constraint → 50 ─────────

// §14.2 / §23: the value returned through a record's property (dispatched via
// interface constraint) is a live integer usable in arithmetic. The generic
// body reads `x.Area` twice and adds. Before the fix: 0 + 0 = 0.
BEZEL_TEST(constraint_dispatch_gaps, r1_2_record_property_in_arithmetic_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IArea  property Area: int; end IArea; "
        "record Square(side: int) : IArea "
        "  public property Area: int => side * side; "
        "end Square; "
        "static class Util "
        "  public static method DoubleArea<T>(x: T): int where T: IArea "
        "    return x.Area + x.Area; "
        "  end DoubleArea; "
        "end Util; "
        "class C public static method Main(): int "
        "  var s := new Square(5); "
        "  return Util.DoubleArea(s); "   // 25 + 25 = 50
        "end Main; end C;"), 50);
}

// ── (e) CONTROL: class property via constraint still works → 42 ──────────────

// §14.2 / §23: a *class* type implementing an interface property — the itable
// dispatch path (already tested in generic_constraint_property_dispatch_test)
// must still return the correct value after the record devirtualization fix.
BEZEL_TEST(constraint_dispatch_gaps, control_class_property_via_constraint_spec_14_2) {
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
        "  public static method GetCode<T>(x: T): int where T: ICode "
        "    return x.Code; "
        "  end GetCode; "
        "end Check; "
        "class C public static method Main(): int "
        "  using var t := new Tag(42); "
        "  return Check.GetCode(t); "   // 42
        "end Main; end C;"), 42);
}

// ── (f) CONTROL: virtual method via class constraint still works → 99 ────────

// §14.2 / §12.1: a *class* type with a virtual method dispatched via class
// constraint — the vtable path must still work after the non-virtual receiver
// guard is added to emitMethodCall's direct-call branch.
BEZEL_TEST(constraint_dispatch_gaps, control_virtual_method_class_constraint_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public constructor() end constructor; "
        "  public virtual method Val(): int => 1; "
        "  destructor end destructor; "
        "end Base; "
        "class Sub : Base "
        "  public constructor() end constructor; "
        "  public override method Val(): int => 99; "
        "  destructor end destructor; "
        "end Sub; "
        "static class G "
        "  public static method Get<T>(x: T): int where T: Base "
        "    return x.Val(); "
        "  end Get; "
        "end G; "
        "class C public static method Main(): int "
        "  using var s := new Sub(); "
        "  return G.Get(s); "   // 99 (dispatch to Sub.Val via vtable)
        "end Main; end C;"), 99);
}

// ── (g) CONTROL: record METHOD via constraint (702dede fix) still works → 7 ──

// §14.2: record T implementing an interface, calling a METHOD (not property)
// via the constraint — the 702dede devirtualization. Must be unaffected by
// the R1.1 non-virtual receiver guard and the R1.2 property devirtualization.
BEZEL_TEST(constraint_dispatch_gaps, control_record_method_via_constraint_spec_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface INum  method Val(): int; end INum; "
        "record Point(X: int, Y: int) : INum "
        "  public method Val(): int => X + Y; "
        "end Point; "
        "static class Calc "
        "  public static method GetVal<T>(x: T): int where T: INum "
        "    return x.Val(); "
        "  end GetVal; "
        "end Calc; "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); "
        "  return Calc.GetVal(p); "   // 3 + 4 = 7
        "end Main; end C;"), 7);
}
