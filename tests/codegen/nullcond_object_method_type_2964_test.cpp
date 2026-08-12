// Sema/codegen — issue #2964 (the `?.` typing half). The DOT form of #2964
// (`o.GetHashCode()` carrying no result type on a class receiver) was fixed by
// 6369649 — the plain-`.` last-resort typing in typeCall (keyed on
// MemberAccessExpr). The `?.` form — `x?.GetHashCode()` / `x?.Equals(o)` on a
// CLASS / Object / interface receiver with NO override — was the typing gap
// #2982 explicitly deferred: #2982 fixed the LOWERING (the call now runs and
// produces a value), but the result STILL carried no sema type, so an INLINE
// `??` on the value-typed results misread the {i8, T} nullable-value struct.
//
// MEASURED on 66d11f3 (HEAD, with #2982's lowering but before this fix):
//   Console.WriteLine(obj?.GetHashCode() ?? -1)   // obj: Object? := new Ovr(); Ovr.GetHashCode returns 42
// printed "1" — the i8 hasValue byte of the raw {i8, i32} struct, NOT 42 —
// compiling clean and exiting 0. And:
//   Console.WriteLine(on?.Equals(on) ?? false)    // on: Widget? := new Widget()
// CRASHED the IR verifier: `trunc { i8, i1 } %9 to i1` — lowerCoalesce, unable
// to see the nullable-value type (its `isNullableValueTy` arm needs the sema
// type), took the #1045 "non-pointer, non-nullable" fall-through and returned
// the RAW {i8, i1} struct, which the WriteLine(bool) lowering then truncated
// to i1.
//
// ROOT CAUSE. The `?.`-callee Object-value-method typing in typeCall (the
// NullCondMemberExpr arm) guarded on `valuePrim || valueNamed` — primitive /
// record / enum receivers ONLY. A CLASS / Object / interface receiver is a
// REFERENCE type, so the guard excluded it and the result type was never set
// (the plain-`.` last-resort does not fire for `?.` either: a `?.` callee is a
// NullCondMemberExpr, not a MemberAccessExpr). The fix removes that guard so
// the `?.` arm types ToString→string / GetHashCode→int / Equals→bool for ANY
// untyped nullcond call, mirroring the plain-`.` last-resort (no receiver-kind
// list — whether the call can LOWER on a given receiver is #2922's question,
// not sema's).
//
// THE TYPED-LOCAL CASE MASKED THE BUG. Binding the result to a TYPED local
// (`var h: int? := obj?.GetHashCode(); Console.WriteLine(h ?? -1)`) worked
// even WITHOUT this fix — the local's declared type drove the downstream
// coalesce — which is exactly why #2982's test (nullcond_object_method_2982)
// could pin the LOWERING without waiting on the typing gap. The discriminators
// here are the INLINE forms, which is where the missing sema type bites: the
// `??` has no declared target to fall back on, so lowerCoalesce reads the raw
// struct.
//
// Spec: §7.5 (null-conditional member access — the result is "lifted to a
// nullable"), §8.3 (GetHashCode: int), §8.5 (ToString: string), §8.1
// (Equals(Object?): bool), §9.6 (the universal Object surface on every class /
// Object / interface value). v0.240.
//
// Every case asserts the program's OUTPUT BYTES via runAotOut. "It compiled",
// "exit 0", and "no crash" are not evidence: without the fix the GetHashCode
// discriminator compiles clean and exits 0 while printing the WRONG value (the
// hasValue byte), and the Equals discriminator crashes the verifier (no binary
// at all). runAotOut returns "<did-not-run>" when the compile is rejected.

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
// discriminator observes a KNOWN value (42), not the §8.3 identity hash a
// no-override class yields (deterministic in a run, but not hardcodable).
const char* kPrelude =
    "class Widget public constructor() end constructor; end Widget; "
    "class Ovr public override method GetHashCode(): int "
    "  return 42; end GetHashCode; end Ovr; ";

std::string prog(const std::string& main) {
    return std::string("module M2964; ") + kPrelude +
           "class C public static method Main(): int " +
           main + " return 0; end Main; end C;";
}

}  // namespace

// ===========================================================================
// Discriminators — FAIL without the fix, PASS with it. All INLINE `??`
// (no typed local to mask the missing sema type).
// ===========================================================================

// D1. `obj?.GetHashCode() ?? -1` INLINE on an Object? receiver holding an
// override-bearing class (Ovr → 42). Per §7.5 the result is `int?`; per §8.3
// the value is 42. Without the fix the int? result carried no sema type, so
// lowerCoalesce's `isNullableValueTy` arm did not fire and it returned the RAW
// {i8, i32} struct — WriteLine(int) read the i8 hasValue byte and printed "1".
BEZEL_TEST(codegen_nullcond_object_method_type_2964,
           gethashcode_inline_on_object_receiver_spec_7_5_8_3) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var obj: Object? := new Ovr(); "
             "  Console.WriteLine(obj?.GetHashCode() ?? -1); "),
        "42"));
}

// D2. `on?.Equals(on) ?? false` INLINE on a Widget? receiver (no override).
// Per §8.3 reference identity is true for the same object. Without the fix the
// bool? result carried no sema type, so lowerCoalesce returned the RAW {i8, i1}
// struct and the WriteLine(bool) lowering emitted `trunc { i8, i1 } to i1` —
// an LLVM verifier error, so runAotOut returns "<did-not-run>".
BEZEL_TEST(codegen_nullcond_object_method_type_2964,
           equals_inline_on_class_receiver_spec_7_5_8_3) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var on: Widget? := new Widget(); "
             "  Console.WriteLine(on?.Equals(on) ?? false); "),
        "True"));
}

// D3. `it?.GetHashCode() ?? -1` INLINE on an INTERFACE? receiver holding an
// override-bearing class (Thing → 99). An interface-typed receiver carries NO
// MethodSymbol, so (like D1) this travelled the untyped path. Without the fix
// the i8 hasValue byte printed instead of 99. Completes the receiver-kind axis
// (class / Object / interface all share §9.6's universal surface).
BEZEL_TEST(codegen_nullcond_object_method_type_2964,
           gethashcode_inline_on_interface_receiver_spec_7_5_9_6_13) {
    BEZEL_EXPECT_TRUE(prints(
        "module M2964d3; "
        "interface IThing end IThing; "
        "class Thing : IThing "
        "  public override method GetHashCode(): int "
        "    return 99; end GetHashCode; "
        "  public constructor() end constructor; "
        "end Thing; "
        "class C public static method Main(): int "
        "  var it: IThing? := new Thing(); "
        "  Console.WriteLine(it?.GetHashCode() ?? -1); "
        "  return 0; end Main; end C;",
        "99"));
}

// ===========================================================================
// Controls — PASS both ways (pre-empt). They do not depend on the fix.
// ===========================================================================

// C1. `w?.ToString() ?? "<null>"` INLINE on a Widget? receiver. ToString's
// result is a REFERENCE type (string → a pointer); lowerCoalesce's pointer
// null-check arm is mechanical and does not need the nullable-value sema type,
// so this worked BEFORE this fix too (the #2982 test pins the same path). The
// observable here is that this fix does not REGRESS the pointer arm now that
// ToString's type IS set (string → string?).
BEZEL_TEST(codegen_nullcond_object_method_type_2964,
           tostring_inline_pointer_path_unaffected_spec_8_5_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var w: Widget? := new Widget(); "
             "  Console.WriteLine(w?.ToString() ?? \"<null>\"); "),
        "Widget"));
}

// C2. `o?.GetHashCode() ?? -1` INLINE on an Ovr? receiver that DECLARES its own
// override. The override resolves a real MethodSymbol, so the call was ALREADY
// typed (the `!exprType(ce)` guard in the fix skips it). 42 before and after —
// proves the fix targets exactly the no-override gap.
BEZEL_TEST(codegen_nullcond_object_method_type_2964,
           gethashcode_own_override_inline_unaffected_spec_12_1) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var o: Ovr? := new Ovr(); "
             "  Console.WriteLine(o?.GetHashCode() ?? -1); "),
        "42"));
}

// C3. `obj?.GetHashCode() ?? -1` bound to a TYPED local first. The local's
// DECLARED type (`int?`) drove the downstream coalesce even without this fix
// (which is how #2982's test pinned the lowering pre-typing-fix). 42 before
// and after — proves the discriminators above are specifically the INLINE
// form, not the call/lowering itself.
BEZEL_TEST(codegen_nullcond_object_method_type_2964,
           gethashcode_typed_local_unaffected_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var obj: Object? := new Ovr(); "
             "  var h: int? := obj?.GetHashCode(); "
             "  Console.WriteLine(h ?? -1); "),
        "42"));
}

// D4. NULL receiver short-circuit: `n?.GetHashCode() ?? -1` INLINE on a null
// Widget?. Per §7.5 a null receiver yields a null `int?` (hasValue=0), so the
// coalesce returns -1. This is a DISCRIMINATOR too, not a control: without the
// type the {i8, i32} struct (hasValue=0, value undef) is returned RAW by the
// #1045 fall-through and WriteLine(int) misreads it — it failed on the reverted
// src alongside D1–D3. With the fix lowerCoalesce sees int?, reads hasValue=0,
// and takes the rhs (-1).
BEZEL_TEST(codegen_nullcond_object_method_type_2964,
           gethashcode_inline_null_short_circuits_spec_7_5) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  var n: Widget? := null; "
             "  Console.WriteLine(n?.GetHashCode() ?? -1); "),
        "-1"));
}
