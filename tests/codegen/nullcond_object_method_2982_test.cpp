// Codegen — issue #2982. `x?.GetHashCode()` / `x?.Equals(o)` / `x?.ToString()`
// on a nullable CLASS / Object / interface receiver with NO override silently
// DROPPED the whole enclosing statement — no diagnostic at any severity, exit 0.
// The `start`/`end` markers printed but the middle line (the call and its
// effect) vanished. MEASURED on 0bddcb7:
//   Console.WriteLine(w?.ToString() ?? "<null>")   // w: Widget? non-null
// printed nothing at all; "start"/"end" around the gap.
//
// This is the method-call member of the `?.` family — the fifth member kind
// after the four property/header forms landed:
//   #2990 (§31.4 exception header) · #3015 (computed property) ·
//   #3016 (virtual / interface property) · and the storage-field path (FINDING
//   [3]). Each routed a NullCondMemberExpr through the SAME lowering the `.`
//   form uses. #2982 is the METHOD-CALL member: `x?.M(...)` parses as
//   CallExpr{callee=NullCondMemberExpr{...}, args=[...]} and lowers through
//   lowerNullCondCall — a DIFFERENT entry point than the member-access arm the
//   property fixes touched.
//
// Spec: §7.5 (null-conditional member access) — "a null-conditional access
// evaluates to null if the receiver is null; otherwise the member is evaluated
// normally, lifted to a nullable result". §8.3 (Object's default hash/equality)
// — a class WITHOUT an override uses the reference-identity / identity-hash
// default; a class WITH an override dispatches it. §9.6 — ToString / Equals /
// GetHashCode are the universal Object surface, available on every class /
// Object / interface value. v0.240.
//
// THE DEFECT. The universal Object members carry NO MethodSymbol on a receiver
// whose static type has no override — the `.` form dispatches them through
// lowerObjectGetHashCode / lowerObjectEquals / lowerObjectToString (the §9.6
// vtable[kSlot] / string-content dispatch in lowerCall). That dispatch never
// runs for `?.`: lowerNullCondCall resolves `ms = exprSymbol(callee)` as a
// MethodSymbol, and for a no-override class / Object / interface receiver `ms`
// is null, so the value-type block (isNullableValueTy — a reference type is NOT
// a nullable VALUE type) did not fire and the `if (!ms ...) return nullptr` bail
// dropped the statement. (A class WITH an override resolved `ms` and travelled
// the normal emitMethodCall path — that is the pre-empt proving the fix targets
// exactly the no-override gap.)
//
// THE FIX routes `?.` through the SAME object-surface dispatch the `.` form
// uses — lowerObjectGetHashCode / lowerObjectEquals / lowerObjectToString —
// with a §7.5 null-receiver short-circuit FIRST (the call is skipped entirely
// when the receiver is null; the Equals argument is lowered inside the
// non-null block for the same reason). The §7.5 nullable lift mirrors
// lowerNullCondValueSemCall (the value-type sibling, #1075): ToString → a
// reference result (string ptr, null when the receiver is null); GetHashCode →
// i32 / Equals → i1, wrapped into the tagged {i8 hasValue; T} struct. See
// LlvmBackend_Calls2.cpp, the new reference-type arm in lowerNullCondCall.
//
// NOTE on #2964 (typing gap, NOT fixed here): GetHashCode / Equals on a CLASS
// receiver carry NO sema type on the `?.` result, so an INLINE `??`/`= null`
// test on those results misreads the tagged struct (the pointer-shaped ToString
// result is unaffected — its null check is mechanical). Each value-typed case
// below binds the result to a TYPED local first (giving the slot an explicit
// nullable type) so the downstream coalesce is correct regardless of #2964 —
// the observable this test pins is the LOWERING (does the call run and produce
// the right value?), which is #2982's scope.
//
// Every case asserts the program's OUTPUT BYTES via runAotOut. "It compiled",
// "exit 0", and "no crash" are not evidence for a silent-drop bug: without the
// fix every positive case here compiles clean and exits 0 while printing the
// WRONG thing (a dropped line, or a degraded value).
//
// Mechanical fail-without: see the commit message. With `src/` reverted to
// 0bddcb7 and this file kept, every discriminator case below fails (the `?.`
// line is dropped or the local degrades to null, so the expected output does
// not match) and the control cases pass — they are controls precisely because
// they do not depend on the fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

bool prints(const std::string& program, const std::string& expected) {
    return runAotOut(program) == expected;
}

// A nullable `Widget?` (no override) and a nullable `Ovr?` whose GetHashCode
// override returns 42 deterministically, so the Object-typed-receiver
// discriminator (case G) observes a known value, not the nondeterministic
// §8.3 identity hash a no-override class yields.
const char* kPrelude =
    "class Widget public constructor() end constructor; end Widget; "
    "class Ovr public override method GetHashCode(): int "
    "  return 42; end GetHashCode; end Ovr; ";

std::string prog(const std::string& main) {
    return std::string("module M2982; ") + kPrelude +
           "class C public static method Main(): int " +
           main + " return 0; end Main; end C;";
}

}  // namespace

// ===========================================================================
// Discriminators — FAIL without the fix, PASS with it.
// ===========================================================================

// A. `?.ToString()` on a NON-NULL class receiver → the type name. Without the
// fix the whole WriteLine vanished (start/end around the gap); with it the
// synthesized class ToString dispatches and prints "Widget".
BEZEL_TEST(codegen_nullcond_object_method_2982,
           tostring_on_nullable_class_receiver_spec_7_5_9_6) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var w: Widget? := new Widget(); "
             "  Console.WriteLine(\"start\"); "
             "  Console.WriteLine(w?.ToString() ?? \"<null>\"); "
             "  Console.WriteLine(\"end\"); "),
        "start\nWidget\nend"));
}

// B. `?.ToString()` on a NULL receiver → the §7.5 null short-circuit yields a
// null string?, coalesced to "<null>". Without the fix the WriteLine vanished
// (the call returned nullptr and the statement dropped); with it the
// short-circuit runs and "<null>" prints.
BEZEL_TEST(codegen_nullcond_object_method_2982,
           tostring_on_null_receiver_short_circuits_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var n: Widget? := null; "
             "  Console.WriteLine(\"start\"); "
             "  Console.WriteLine(n?.ToString() ?? \"<null>\"); "
             "  Console.WriteLine(\"end\"); "),
        "start\n<null>\nend"));
}

// D. `?.Equals(self)` on a NON-NULL class receiver → §8.3 reference identity is
// true for the same object. Without the fix the bool? local degraded to null
// (the #2434 zero-init) so `eq ?? false` printed "EQ-FALSE"; with it the vtable
// [kEqualsSlot] dispatch runs and "EQ-TRUE" prints.
BEZEL_TEST(codegen_nullcond_object_method_2982,
           equals_on_nullable_class_receiver_spec_7_5_8_3) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var same: Widget? := new Widget(); "
             "  var eq: bool? := same?.Equals(same); "
             "  if eq ?? false then Console.WriteLine(\"EQ-TRUE\"); "
             "  else Console.WriteLine(\"EQ-FALSE\"); end; "),
        "EQ-TRUE"));
}

// G. `?.GetHashCode()` on an Object? receiver holding an override-bearing
// class → the §9.6 vtable [kGetHashCodeSlot] dispatch reaches Ovr's override
// (42). This is the issue's exact method (GetHashCode) with a deterministic
// value: an Object-typed receiver has NO MethodSymbol, so without the fix the
// int? local degraded to null and `h ?? -1` printed "-1"; with it the override
// dispatches and "42" prints.
BEZEL_TEST(codegen_nullcond_object_method_2982,
           gethashcode_on_nullable_object_receiver_spec_7_5_8_3) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var obj: Object? := new Ovr(); "
             "  var h: int? := obj?.GetHashCode(); "
             "  Console.WriteLine(h ?? -1); "),
        "42"));
}

// H. `?.GetHashCode()` on an INTERFACE? receiver holding an override-bearing
// class → the §13 itable / §9.6 vtable dispatch reaches Thing's override (99).
// An interface-typed receiver has NO MethodSymbol, so (like G) this travels the
// fix's dispatch: without it the int? local degraded to null ("-1"); with it
// the override dispatches and "99" prints. Completes the method × receiver-kind
// axis (class / Object / interface all share the §9.6 dispatch).
BEZEL_TEST(codegen_nullcond_object_method_2982,
           gethashcode_on_nullable_interface_receiver_spec_7_5_13) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2982h; "
        "interface IThing end IThing; "
        "class Thing : IThing "
        "  public override method GetHashCode(): int "
        "    return 99; end GetHashCode; "
        "  public constructor() end constructor; "
        "end Thing; "
        "class C public static method Main(): int "
        "  var it: IThing? := new Thing(); "
        "  var h: int? := it?.GetHashCode(); "
        "  Console.WriteLine(h ?? -1); "
        "  return 0; end Main; end C;",
        "99"));
}

// ===========================================================================
// Controls — PASS both ways (pre-empt). They do not depend on the fix.
// ===========================================================================

// C. `?.GetHashCode()` on a class that DECLARES its own override → resolves a
// real MethodSymbol (`ms`), so the call travels the normal emitMethodCall path
// UNCHANGED by this fix. 42 before and after.
BEZEL_TEST(codegen_nullcond_object_method_2982,
           gethashcode_own_override_unaffected_spec_12_1) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var o: Ovr? := new Ovr(); "
             "  var h: int? := o?.GetHashCode(); "
             "  Console.WriteLine(h ?? -1); "),
        "42"));
}

// E. `?.GetHashCode()` on a DERIVED nullable whose BASE declares the override →
// the inherited override resolves `ms`, so the call travels the normal path
// UNCHANGED. 7 before and after.
BEZEL_TEST(codegen_nullcond_object_method_2982,
           gethashcode_inherited_override_unaffected_spec_12_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2982e; "
        "class Base public override method GetHashCode(): int "
        "  return 7; end GetHashCode; end Base; "
        "class Derived : Base public constructor() end constructor; end Derived; "
        "class C public static method Main(): int "
        "  var d: Derived? := new Derived(); "
        "  var h: int? := d?.GetHashCode(); "
        "  Console.WriteLine(h ?? -1); "
        "  return 0; end Main; end C;",
        "7"));
}

// F. `?.GetHashCode()` on a NULL override-bearing receiver → the §7.5
// short-circuit yields a null int?, coalesced to -1. Without the fix the local
// degraded to null the same way; with it the short-circuit produces null
// properly. -1 before and after (a control, not a discriminator).
BEZEL_TEST(codegen_nullcond_object_method_2982,
           gethashcode_on_null_override_receiver_short_circuits_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var no: Ovr? := null; "
             "  var h: int? := no?.GetHashCode(); "
             "  Console.WriteLine(h ?? -1); "),
        "-1"));
}
