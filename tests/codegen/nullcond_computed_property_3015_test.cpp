// Codegen — issue #3015. `q?.P` on a nullable class receiver, where P is a
// computed property getter, SILENTLY DROPPED the whole enclosing statement —
// no diagnostic at any severity, exit 0. The `start` and `end` markers printed
// but the middle line (the access and its effect) vanished.
//
// Spec: §7.5 "Null-conditional member access" —
//
//   The null-conditional member access `e?.M` has type `T?` (always nullable).
//   If `e` is null, the result is null; otherwise the result is the member
//   access `e.M`.
//
// §23.2 "Expression-bodied properties": `public property P: T => expr;` is a
// computed read-only property whose getter is the expression body, evaluated
// with `this` bound to the receiver. §23.3 "Block-form properties":
// `get => do … end;` is the block-form getter. §11.3 / §12.1 make a property
// available on a derived class. v0.240.
//
// THE DEFECT. A computed property carries NO storage FieldSymbol — only an
// auto-property does. The `.` form evaluates the getter body
// (computedGetterOf / blockGetterOf in the MemberAccessExpr arm of lowerExpr);
// that arm never runs for a `?.` access (a NullCondMemberExpr is a distinct AST
// node), so the access reached the storage-field path of the NullCondMemberExpr
// arm, `storageMemberOf` returned null, and the `!ms` guard returned nullptr —
// silently dropping the enclosing statement. MEASURED on f386fda (one commit
// after the #2990 fix): `Console.WriteLine("p=" + (q?.Computed ?? "<null>"))`
// on a NON-NULL receiver printed nothing at all; `start` and `end` printed
// around the gap, exit 0. The same drop hit the block-form getter and a
// generic class's getter alike.
//
// This is #2990's failure mode in a different member kind: #2990 was the four
// §31.4 header members (Message / StackTrace / CauseChain / ParamName); #3015
// is the SAME lowering gap for a user-declared computed property getter. The
// #2990 lane found it while writing its negative control (the control was
// switched to a storage field because the property form was this separate bug).
//
// THE FIX routes the `?.` form through the SAME getter evaluation the `.` form
// uses — computedGetterOf / blockGetterOf are the one fact sema already
// computes that decides "this is a computed getter, call the body" — with a
// null-receiver short-circuit first per §7.5 (the always-nullable result is
// null when the receiver is null, else the getter value lifted to nullable).
// Virtual / interface properties are gated out (they dispatch dynamically and
// are tracked separately). See LlvmBackend_Exprs.cpp, NullCondMemberExpr arm.
//
// Every case below asserts the program's OUTPUT BYTES. "It compiled" and
// "exit 0" are not evidence for a silent-drop bug: without the fix every
// positive case here compiles clean and exits 0 while printing the WRONG thing
// (nothing — a line missing from the middle of the output). The measurement
// asserts the middle line IS present.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

bool prints(const std::string& program, const std::string& expected) {
    return runAotOut(program) == expected;
}

// A class with an expression-bodied getter (`§23.2`), a block-form getter
// (§23.3), a value-typed int getter, and a STORAGE field — the field is the
// negative control (it travelled the storage-member path and was never part of
// the defect). The getter bodies reference the field so a null/degraded read is
// distinguishable from a correct one.
const char* kHolder =
    "class H3015 "
    "  public store: string; "
    "  public constructor(s: string) store := s; end constructor; "
    "  public property Computed: string => \"comp-\" + store; "
    "  public property BlockGet: string "
    "    get => do return \"blk-\" + store; end; "
    "  end BlockGet; "
    "end H3015; ";

// A value-typed holder: the §7.5 result is the tagged {i8 hasValue; T} struct,
// not a pointer — exercising the value-member lift path.
const char* kIntHolder =
    "class N3015 "
    "  public n: int; "
    "  public constructor(n: int) this.n := n; end constructor; "
    "  public property Doubled: int => n * 2; "
    "end N3015; ";

}  // namespace

// ===========================================================================
// A. The issue's own repro. `start`/`end` markers bracket the access: without
// the fix the middle `p=` line is dropped entirely (the whole WriteLine call
// vanishes), so the output is "start\nend". With the fix it is the three-line
// "start\np=comp-zz\nend". The receiver is non-null.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_computed_property_3015,
           expression_getter_on_nullable_receiver_spec_23_2) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3015a; " +
        std::string(kHolder) +
        "class C public static method Main(): int "
        "  Console.WriteLine(\"start\"); "
        "  using var h := new H3015(\"zz\"); "
        "  var q: H3015? := h; "
        "  Console.WriteLine(\"p=\" + (q?.Computed ?? \"<null>\")); "
        "  Console.WriteLine(\"end\"); "
        "  return 0; "
        "end Main; end C;",
        "start\np=comp-zz\nend"));
}

// ===========================================================================
// B. Block-form getter (§23.3) through `?.` — same defect, distinct lowering
// (a standalone getter function call, not an inline body).
// ===========================================================================

BEZEL_TEST(codegen_nullcond_computed_property_3015,
           block_getter_on_nullable_receiver_spec_23_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3015b; " +
        std::string(kHolder) +
        "class C public static method Main(): int "
        "  using var h := new H3015(\"zz\"); "
        "  var q: H3015? := h; "
        "  Console.WriteLine(\"b=\" + (q?.BlockGet ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "b=blk-zz"));
}

// ===========================================================================
// C. Value-typed getter (§23.2, returns int). The §7.5 result is the tagged
// {i8 hasValue; i32} struct — a DIFFERENT lift path than a reference-typed
// getter. Without the fix the line is dropped; with it the getter runs and the
// value wraps correctly. Asserted via .ToString() on the lifted result.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_computed_property_3015,
           value_typed_getter_lifts_to_nullable_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3015c; " +
        std::string(kIntHolder) +
        "class C public static method Main(): int "
        "  using var h := new N3015(21); "
        "  var q: N3015? := h; "
        "  Console.WriteLine(\"d=\" + (q?.Doubled ?? 0).ToString()); "
        "  return 0; "
        "end Main; end C;",
        "d=42"));
}

// ===========================================================================
// D. §7.5 null-receiver short-circuit: a null receiver yields null, so the
// coalesce supplies the default. This is the arm that distinguishes `?.` from
// `.` and proves the null check runs at all (a path that called the getter
// unconditionally would fault dereferencing the null `this`).
// ===========================================================================

BEZEL_TEST(codegen_nullcond_computed_property_3015,
           null_receiver_short_circuits_to_null_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3015d; " +
        std::string(kHolder) +
        "class C public static method Main(): int "
        "  var q: H3015? := null; "
        "  Console.WriteLine(\"p=\" + (q?.Computed ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "p=<null>"));
}

// Null-receiver short-circuit for a VALUE-typed getter: the lifted result's
// hasValue is false, so the coalesce to 0 supplies the default.
BEZEL_TEST(codegen_nullcond_computed_property_3015,
           null_receiver_value_typed_short_circuits_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3015e; " +
        std::string(kIntHolder) +
        "class C public static method Main(): int "
        "  var q: N3015? := null; "
        "  Console.WriteLine(\"d=\" + (q?.Doubled ?? -1).ToString()); "
        "  return 0; "
        "end Main; end C;",
        "d=-1"));
}

// ===========================================================================
// E. A generic class's computed getter through `?.`. The getter body reads a
// `T`-typed field; the fix's generic-substitution setup (mirroring the `.`
// form) lets `T` resolve to the receiver's concrete type arg. Without it the
// line is dropped; with it the getter runs with the right type.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_computed_property_3015,
           generic_class_getter_on_nullable_receiver_spec_14_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3015f; "
        "class Box<T> "
        "  public v: T; "
        "  public constructor(v: T) this.v := v; end constructor; "
        "  public property Label: string => \"box-\" + v.ToString(); "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<int>(21); "
        "  var q: Box<int>? := b; "
        "  Console.WriteLine(\"g=\" + (q?.Label ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "g=box-21"));
}

// ===========================================================================
// F. NEGATIVE CONTROL — a STORAGE field on the same receiver. The `?.` access
// reads the declared field through the existing storage-member path (FINDING
// [3]), NOT the new getter block. This passes WITHOUT the fix as well as with
// it; that is what makes it a control rather than a pin.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_computed_property_3015,
           storage_field_control_unaffected_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        "module M3015g; " +
        std::string(kHolder) +
        "class C public static method Main(): int "
        "  using var h := new H3015(\"zz\"); "
        "  var q: H3015? := h; "
        "  Console.WriteLine(\"s=\" + (q?.store ?? \"<null>\")); "
        "  return 0; "
        "end Main; end C;",
        "s=zz"));
}
