// Spec-derived sema tests for nested-class upcast to base class / interface.
//
// Authority: §11.6 (nested types are full types with all type-system
// properties of their kind), §5.1 (a value of a derived type is implicitly
// convertible to its base type or an implemented interface).
//
// A nested class that declares a base class or implements an interface must
// be assignable to that base / interface exactly like an equivalent top-level
// class.  The bug being fixed: sema's §5.1 assignability check walked the
// TypeSymbol::baseTypes chain, which was never populated for nested types
// (the TypeResolution pass searched only module-level children when finding
// the TypeSymbol to attach base information to).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

// ── (a) nested class → base class ─────────────────────────────────────────

// §11.6 / §5.1: var-init — a nested class assigned to its base class via a
// local variable declaration must compile without error.
BEZEL_TEST(sema_nested_upcast, var_init_nested_to_base_spec_11_6_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Base; "
        "class Outer "
        "  public class Child : Base "
        "    public constructor() end constructor; "
        "    destructor end destructor; "
        "  end Child; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var c := new Outer.Child(); "
        "  var b: Base := c; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.6 / §5.1: assignment statement — nested class assigned to a base-typed
// variable after its declaration must compile without error.
BEZEL_TEST(sema_nested_upcast, assignment_nested_to_base_spec_11_6_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Base; "
        "class Outer "
        "  public class Child : Base "
        "    public constructor() end constructor; "
        "    destructor end destructor; "
        "  end Child; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var c := new Outer.Child(); "
        "  var b: Base := new Base(); "
        "  b := c; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.6 / §5.1: return — a method returning the base type must accept a
// nested-class value without error.
BEZEL_TEST(sema_nested_upcast, return_nested_to_base_spec_11_6_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Base; "
        "class Outer "
        "  public class Child : Base "
        "    public constructor() end constructor; "
        "    destructor end destructor; "
        "  end Child; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Outer; "
        "class C "
        "  public static method MakeBase(): Base "
        "    using var c := new Outer.Child(); "
        "    return c; "
        "  end MakeBase; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §11.6 / §5.1: argument — a nested-class value passed as a base-typed
// argument must compile without error.
BEZEL_TEST(sema_nested_upcast, argument_nested_to_base_spec_11_6_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Base; "
        "class Outer "
        "  public class Child : Base "
        "    public constructor() end constructor; "
        "    destructor end destructor; "
        "  end Child; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Outer; "
        "class C "
        "  public static method Accept(b: Base) end Accept; "
        "  public static method Main(): int "
        "    using var c := new Outer.Child(); "
        "    Accept(c); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── (b) nested class → implemented interface ───────────────────────────────

// §11.6 / §5.1: a nested class implementing an interface is assignable to
// that interface type via var-init.
BEZEL_TEST(sema_nested_upcast, var_init_nested_to_interface_spec_11_6_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo "
        "  method Value(): int; "
        "end IFoo; "
        "class Outer "
        "  public class Child : IFoo "
        "    public constructor() end constructor; "
        "    destructor end destructor; "
        "    public method Value(): int => 42; "
        "  end Child; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var c := new Outer.Child(); "
        "  var x: IFoo := c; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── (c) control: nested class → UNRELATED type is REJECTED ────────────────

// §5.1: a nested class must NOT be assignable to an unrelated type.
// This control case verifies we did not weaken the rejection of genuinely
// unrelated types.
BEZEL_TEST(sema_nested_upcast, nested_to_unrelated_type_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Unrelated "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Unrelated; "
        "class Outer "
        "  public class Child "
        "    public constructor() end constructor; "
        "    destructor end destructor; "
        "  end Child; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var c := new Outer.Child(); "
        "  var u: Unrelated := c; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── (d) control: top-level class → base still COMPILES ────────────────────

// §5.1: top-level derived→base upcast must continue to compile (regression).
BEZEL_TEST(sema_nested_upcast, toplevel_to_base_still_ok_spec_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Base; "
        "class Derived : Base "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  var b: Base := d; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── (e) nested class extending another nested class → grandbase ───────────

// §11.6 / §5.1: a nested class that extends another nested class must be
// transitively assignable to the grandbase class.
BEZEL_TEST(sema_nested_upcast, nested_nested_to_grandbase_spec_11_6_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class GrandBase "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end GrandBase; "
        "class Outer "
        "  public class Mid : GrandBase "
        "    public constructor() end constructor; "
        "    destructor end destructor; "
        "  end Mid; "
        "  public class Leaf : GrandBase "
        "    public constructor() end constructor; "
        "    destructor end destructor; "
        "  end Leaf; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var leaf := new Outer.Leaf(); "
        "  var gb: GrandBase := leaf; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── extra: nested class → base where base is itself a nested class ─────────

// §11.6 / §5.1: two nested types in the same outer, one inheriting the
// other; the child should be assignable to the parent (both nested).
BEZEL_TEST(sema_nested_upcast, nested_child_to_nested_base_spec_11_6_5_1) {
    auto r = parseSemaAll(
        "module M; "
        "class Outer "
        "  public class NestedParent "
        "    public constructor() end constructor; "
        "    destructor end destructor; "
        "  end NestedParent; "
        "  public class NestedChild : NestedParent "
        "    public constructor() end constructor; "
        "    destructor end destructor; "
        "  end NestedChild; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var child := new Outer.NestedChild(); "
        "  var parent: Outer.NestedParent := child; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
