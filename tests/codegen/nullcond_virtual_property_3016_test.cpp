// Codegen — issue #3016. `q?.VProp` on a nullable class receiver, where VProp
// is a VIRTUAL (§12.1) or INTERFACE (§13) property getter, SILENTLY DROPPED the
// whole enclosing statement — no diagnostic at any severity, exit 0. The
// `start`/`end` markers printed but the middle line (the access and its effect)
// vanished. This is the gated-out sibling of #3015 (non-virtual computed
// getters); #3015 explicitly EXCLUDED virtual/interface properties because
// statically inlining the declaring getter would miscompile an override.
//
// Spec: §7.5 "Null-conditional member access" —
//
//   The null-conditional member access `e?.M` has type `T?` (always nullable).
//   If `e` is null, the result is null; otherwise the result is the member
//   access `e.M`.
//
// §12.1 "Members are non-virtual by default": a method/property/indexer marked
// `virtual` dispatches through the vtable; an `override` in a derived class
// replaces the slot. §13 "Visibility" + the itable dispatch (CG35): an interface
// property dispatches through the itable. §23.2 (expression-bodied properties)
// and §23.3 (block-form properties) define the getter forms; a `virtual` or
// interface property's getter runs through the slot, not a static inline. v0.240.
//
// THE DEFECT. The `.` form dispatches a virtual/interface property through
// lowerVirtualPropertyGet (vtable/itable slot load + call). A NullCondMemberExpr
// (`?.`) is a distinct AST node, so that arm never runs for `?.`. The #3015 fix
// routed `?.` through the computed-getter evaluation (computedGetterOf /
// blockGetterOf) for NON-VIRTUAL properties but explicitly GATED OUT the
// virtual/interface case (`propGetSlot_` / `ifacePropertyGetIndex_`). So a
// virtual/interface `?.` access reached the storage-field path (FINDING [3]),
// `storageMemberOf` returned null (a virtual property carries no storage
// FieldSymbol), and the `!ms` guard returned nullptr — dropping the statement.
// MEASURED on d2a3d3e (before this fix):
// `Console.WriteLine("p=" + (q?.VProp ?? "<null>"))` on a NON-NULL receiver
// printed nothing at all; `start`/`end` around the gap, exit 0.
//
// THE FIX routes `?.` through the SAME vtable/itable slot dispatch the `.` form
// uses (the slot load + call mirror lowerVirtualPropertyGet verbatim), with a
// §7.5 null-receiver short-circuit FIRST (loading the slot from a null receiver
// is the crash). Because the dispatch goes through the RUNTIME type's vtable, an
// OVERRIDDEN getter runs — the property is NOT statically inlined. See
// LlvmBackend_Exprs.cpp, NullCondMemberExpr arm, the `ncVirtSym` block.
//
// Every case below asserts the program's OUTPUT BYTES. "It compiled" and
// "exit 0" are not evidence for a silent-drop bug: without the fix every
// positive case here compiles clean and exits 0 while printing the WRONG thing
// (nothing — a line missing from the middle of the output). The measurement
// asserts the middle line IS present AND carries the OVERRIDDEN value.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

bool prints(const std::string& program, const std::string& expected) {
    return runAotOut(program) == expected;
}

}  // namespace

// ===========================================================================
// A. THE HEADLINE: a virtual property OVERRIDDEN in a derived class, accessed
// through `?.` on a nullable BASE-typed receiver holding a DERIVED instance.
// The OVERRIDDEN getter must run (the reason #3015 gated this out: a static
// inline of the declaring getter body would have printed "BASE"). Without the
// fix the whole `p=` line is dropped; with it the override dispatches and
// prints "DERIVED". `start`/`end` markers bracket the access.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_virtual_property_3016,
           virtual_override_on_nullable_receiver_spec_12_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3016a; "
        "class Base "
        "  public virtual property VProp: string => \"BASE\"; "
        "  destructor end destructor; "
        "end Base; "
        "class Derived : Base "
        "  public override property VProp: string => \"DERIVED\"; "
        "  destructor end destructor; "
        "end Derived; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"start\"); "
        "  using var d := new Derived(); "
        "  var q: Base? := d; "
        "  Console.WriteLine(\"p=\" + (q?.VProp ?? \"<null>\")); "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;",
        "start\np=DERIVED\nend"));
}

// ===========================================================================
// B. Virtual property on a BASE object (no override in play) — the BASE getter
// runs through the vtable. This is the control for A: proves the dispatch
// resolves the RUNTIME type's slot (Base here, Derived in A), not a hardcoded
// one. Without the fix the line is dropped.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_virtual_property_3016,
           virtual_base_object_on_nullable_receiver_spec_12_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3016b; "
        "class Base "
        "  public virtual property VProp: string => \"BASE\"; "
        "  destructor end destructor; "
        "end Base; "
        "class Derived : Base "
        "  public override property VProp: string => \"DERIVED\"; "
        "  destructor end destructor; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var b := new Base(); "
        "  var q: Base? := b; "
        "  Console.WriteLine(\"p=\" + (q?.VProp ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "p=BASE"));
}

// ===========================================================================
// C. A VALUE-TYPED virtual property (returns int) OVERRIDDEN in the derived
// class. The §7.5 result is the tagged {i8 hasValue; i32} struct — a DIFFERENT
// lift path than a reference-typed getter. The override must run (returns 84,
// not 42). Without the fix the line is dropped.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_virtual_property_3016,
           value_typed_virtual_override_lifts_to_nullable_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3016c; "
        "class Base "
        "  public virtual property Num: int => 42; "
        "  destructor end destructor; "
        "end Base; "
        "class Derived : Base "
        "  public override property Num: int => 84; "
        "  destructor end destructor; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  var q: Base? := d; "
        "  Console.WriteLine(\"n=\" + (q?.Num ?? 0).ToString()); "
        "  return 0; "
        "end Main; end C;",
        "n=84"));
}

// ===========================================================================
// D. §7.5 null-receiver short-circuit for a VIRTUAL property: a null receiver
// yields null, so the coalesce supplies the default. This proves the null check
// runs at all (a path that loaded the vtable slot unconditionally would fault
// dereferencing the null receiver).
// ===========================================================================

BEZEL_TEST(codegen_nullcond_virtual_property_3016,
           null_receiver_virtual_short_circuits_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3016d; "
        "class Base "
        "  public virtual property VProp: string => \"BASE\"; "
        "  destructor end destructor; "
        "end Base; "
        "class C public static method Main(): int "
        "  var q: Base? := null; "
        "  Console.WriteLine(\"p=\" + (q?.VProp ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "p=<null>"));
}

// Null-receiver short-circuit for a VALUE-TYPED virtual property: the lifted
// result's hasValue is false, so the coalesce supplies the default.
BEZEL_TEST(codegen_nullcond_virtual_property_3016,
           null_receiver_value_typed_virtual_short_circuits_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3016e; "
        "class Base "
        "  public virtual property Num: int => 42; "
        "  destructor end destructor; "
        "end Base; "
        "class C public static method Main(): int "
        "  var q: Base? := null; "
        "  Console.WriteLine(\"n=\" + (q?.Num ?? -1).ToString()); "
        "  return 0; "
        "end Main; end C;",
        "n=-1"));
}

// ===========================================================================
// E. An INTERFACE property (§13) accessed through `?.` on a nullable INTERFACE-
// typed receiver. The dispatch goes through the itable. Without the fix the
// line is dropped.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_virtual_property_3016,
           interface_property_on_nullable_receiver_spec_13) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3016f; "
        "interface INamed readonly property Label: string; end INamed; "
        "class Widget : INamed public property Label: string => \"WIDGET\"; "
        "  destructor end destructor; "
        "end Widget; "
        "class C public static method Main(): int "
        "  using var w := new Widget(); "
        "  var q: INamed? := w; "
        "  Console.WriteLine(\"p=\" + (q?.Label ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "p=WIDGET"));
}

// Interface property null-receiver short-circuit: a null interface receiver
// yields null. Proves the null check runs before the itable load.
BEZEL_TEST(codegen_nullcond_virtual_property_3016,
           interface_property_null_receiver_short_circuits_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3016g; "
        "interface INamed readonly property Label: string; end INamed; "
        "class Widget : INamed public property Label: string => \"WIDGET\"; "
        "  destructor end destructor; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var q: INamed? := null; "
        "  Console.WriteLine(\"p=\" + (q?.Label ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "p=<null>"));
}

// ===========================================================================
// F. A value-typed INTERFACE property (returns int) through `?.` — exercises
// the itable dispatch + tagged-struct lift together.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_virtual_property_3016,
           value_typed_interface_property_lifts_to_nullable_spec_13) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3016h; "
        "interface IShape readonly property Size: int; end IShape; "
        "class Square : IShape public property Size: int => 9; "
        "  destructor end destructor; "
        "end Square; "
        "class C public static method Main(): int "
        "  using var sq := new Square(); "
        "  var q: IShape? := sq; "
        "  Console.WriteLine(\"s=\" + (q?.Size ?? 0).ToString()); "
        "  return 0; "
        "end Main; end C;",
        "s=9"));
}

// ===========================================================================
// G. NEGATIVE CONTROL / REGRESSION PRE-EMPT — the `.` form (non-null receiver)
// on a virtual property already worked (lowerVirtualPropertyGet). This test
// passes BOTH with and without the fix; it guards against a regression in the
// `.` path caused by the new `?.` block.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_virtual_property_3016,
           dot_form_virtual_override_unaffected_spec_12_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3016i; "
        "class Base "
        "  public virtual property VProp: string => \"BASE\"; "
        "  destructor end destructor; "
        "end Base; "
        "class Derived : Base "
        "  public override property VProp: string => \"DERIVED\"; "
        "  destructor end destructor; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  var q: Base := d; "
        "  Console.WriteLine(\"p=\" + q.VProp); "
        "  return 0; "
        "end Main; end C;",
        "p=DERIVED"));
}
