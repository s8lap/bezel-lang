// fix #8118 (§28.1 / §8.1 / §28.5 / §28.9, v0.355): a plain (non-override)
// `owned x: Object` / `owned x: Object?` parameter or field is ADMITTED.
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§28.1, v0.355, "Type requirement"):
//   "In the method-parameter, return-type, `out`-parameter, lambda-parameter,
//   choice-payload (§35.1), and function-type parameter/return slot positions
//   above, `owned` requires the declared type to be a **class type other than
//   `Type`** (§28.9: the process-interned type-identity token is runtime-managed
//   and carries no obligation to transfer), an interface type, or a bare type
//   parameter. `Object` is such a class type (§8.1), here and at an `owned`
//   field (§28.5): the marker transfers whatever value the position holds, and
//   a runtime-managed value there — a boxed value, a `string`, a function
//   value — carries no obligation, the discharge points disposing nothing for
//   it (§8.7)."
//
//   §28.5 (v0.355): "Only fields whose type is a class type or an interface
//   type take the `owned` field modifier as a marking of their own." §8.1
//   (v0.355): `Object` is the ROOT CLASS.
//
// THE BUG (#8118). `Resolver::checkOwnedDeclType`'s primitive arm classified
// `Object` — `TypeRef::Kind::Primitive` with `PrimitiveType::Object` — as
// "a primitive value type", so a plain `owned x: Object` was rejected with a
// statement the spec makes FALSE of `Object` in as many words. Only the
// OVERRIDE parameters were exempted (commit 312100a4, issue #6029), so the
// defect survived at every other `owned` position.
//
// THE FIX. The `Object` case is admitted at the type requirement, and the
// discharge is the runtime discrimination the class protocol needs: the value
// an `Object` slot holds is not statically decidable (§5.1 — `Object` is the
// universal reference type), so the discharge point asks the runtime which
// kind of value it holds and disposes exactly that.
//
// THE ORACLE. Every accept row compiles and RUNS (the `runAot` oracle) and
// pins the printed output, so a program that was admitted but then mis-
// discharged (a double dispose, a missing dispose, a wild free) is caught by
// the output and by the sanitizer leg. The leak row is the `runAotChecked`
// oracle (generated-code AddressSanitizer + LeakSanitizer) and is the row the
// issue asks for: an owned `Array<int>` passed through `Object?` in a 100-
// iteration loop, discharged exactly once.
//
// The REJECT rows are the controls: `owned x: int` keeps the §28.1 diagnostic
// with the SAME text, so the fix did not widen the marker to a real primitive.
//
// per §28.1, §8.1, §28.5, §28.9, v0.355

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasAutoDropWarning;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A class with a destructor, so the DISCHARGE is observable in the output:
// `Res.n` counts the destructions.
const char* kRes =
    "public class Res "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.n := Res.n + 1; end destructor; "
    "end Res; ";

// Accept rows: the program compiles, RUNS, and prints exactly `expected`.
void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// The §28.1 reject control: the marker is still refused on a real primitive,
// with the SAME text the issue quotes.
void expectRejectedPrimitive(const JitResult& r) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`owned` is not admissible on a parameter of type a primitive value type"));
    BEZEL_EXPECT_FALSE(r.ran);
}

}  // namespace

// ---------------------------------------------------------------------------
// ACCEPT — a plain (non-override) method parameter, `Object` and `Object?`.
// ---------------------------------------------------------------------------

BEZEL_TEST(owned_object_param_plain_8118,
           plain_method_object_parameter_is_admitted_spec_28_1_8_1) {
    JitResult r = runAot(
        "module M; " + std::string(kRes) +
        "public class App "
        "  public static method Take(owned x: Object): int "
        "    return 1; "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new Res())); "
        "    Console.WriteLine(Res.n); "
        "    return 0; "
        "  end Main; "
        "end App;");
    // The value the callee was given is disposed exactly once (the destructor
    // ran, and the program did not fault).
    expectRunOutput(r, "1\n1\n");
}

BEZEL_TEST(owned_object_param_plain_8118,
           plain_method_nullable_object_parameter_is_admitted_spec_28_1_8_1) {
    JitResult r = runAot(
        "module M; " + std::string(kRes) +
        "public class App "
        "  public static method Take(owned x: Object?): int "
        "    if x = null then return 0; end if; "
        "    return 1; "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new Res())); "
        "    Console.WriteLine(Res.n); "
        "    return 0; "
        "  end Main; "
        "end App;");
    expectRunOutput(r, "1\n1\n");
}

// ---------------------------------------------------------------------------
// ACCEPT — the other §28.1 positions.
// ---------------------------------------------------------------------------

BEZEL_TEST(owned_object_param_plain_8118,
           constructor_object_parameter_is_admitted_spec_28_1_8_1) {
    JitResult r = runAot(
        "module M; " + std::string(kRes) +
        "public class App "
        "  public constructor(owned c: Object?) end constructor; "
        "  public static method Main(): int "
        "    var a := new App(new Res()); "
        "    _ := a; "
        "    Console.WriteLine(Res.n); "
        "    return 0; "
        "  end Main; "
        "end App;");
    expectRunOutput(r, "1\n");
}

BEZEL_TEST(owned_object_param_plain_8118,
           lambda_object_parameter_is_admitted_spec_28_1_8_1) {
    JitResult r = runAot(
        "module M; " + std::string(kRes) +
        "public class App "
        "  public static method Main(): int "
        "    var f := lambda(owned l: Object?) => 1; "
        "    Console.WriteLine(f(new Res())); "
        "    return 0; "
        "  end Main; "
        "end App;");
    expectRunOutput(r, "1\n");
}

BEZEL_TEST(owned_object_param_plain_8118,
           extension_method_object_parameter_is_admitted_spec_28_1_8_1) {
    JitResult r = runAot(
        "module M; " + std::string(kRes) +
        "extension Object "
        "  public method Take(owned x: Object?): int "
        "    return 2; "
        "  end Take; "
        "end; "
        "public class App "
        "  public static method Main(): int "
        "    var o: Object := new Res(); "
        "    Console.WriteLine(o.Take(new Res())); "
        "    return 0; "
        "  end Main; "
        "end App;");
    expectRunOutput(r, "2\n");
}

BEZEL_TEST(owned_object_param_plain_8118,
           function_type_slot_object_parameter_is_admitted_spec_28_1_8_1) {
    // The FUNCTION-TYPE parameter slot carries the marker (§28.1's "function-
    // type parameter/return slot positions").
    JitResult r = runAot(
        "module M; " + std::string(kRes) +
        "public class App "
        "  public static method Take(g: (owned Object?) -> ()): int "
        "    g(new Res()); "
        "    return 1; "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(lambda(owned l: Object?) => ())); "
        "    return 0; "
        "  end Main; "
        "end App;");
    expectRunOutput(r, "1\n");
}

BEZEL_TEST(owned_object_param_plain_8118,
           owned_object_field_is_admitted_spec_28_5_8_1) {
    // §28.5: "Only fields whose type is a class type or an interface type take
    // the `owned` field modifier as a marking of their own" — and `Object` is
    // a class type (§8.1). The field is auto-disposed when the containing
    // object is disposed, so the destructor runs exactly once.
    JitResult r = runAot(
        "module M; " + std::string(kRes) +
        "public class Holder "
        "  private owned _o: Object; "
        "  public constructor() "
        "    _o := new Res(); "
        "  end constructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    _ := h; "
        "    Console.WriteLine(Res.n); "
        "    return 0; "
        "  end Main; "
        "end App;");
    expectRunOutput(r, "1\n");
}

BEZEL_TEST(owned_object_param_plain_8118,
           out_owned_object_parameter_is_admitted_spec_28_1_8_1) {
    // §28.1's `out`-parameter position. The callee WRITES the value into the
    // recipient, so the RECIPIENT owns it and discharges it at its scope end —
    // exactly once (the destructor runs once, and the program does not fault).
    JitResult r = runAot(
        "module M; " + std::string(kRes) +
        "public class App "
        "  public static method Fill(out owned x: Object?): int "
        "    x := new Res(); "
        "    return 1; "
        "  end Fill; "
        "  public static method Run(): int "
        "    var v: Object? := null; "
        "    Console.WriteLine(Fill(out v)); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    Run(); "
        "    Console.WriteLine(Res.n); "
        "    return 0; "
        "  end Main; "
        "end App;");
    // The value the recipient was given is disposed exactly once, at the
    // recipient's scope end (the `Run` frame's end, before `Main` reads the
    // counter).
    expectRunOutput(r, "1\n1\n");
}

BEZEL_TEST(owned_object_param_plain_8118,
           owned_object_return_is_admitted_spec_28_1_8_1) {
    // §28.1's return-type position. The callee's obligation ends at the
    // return and the CALLER's binding owns the value, so the caller's scope
    // end discharges it exactly once.
    JitResult r = runAot(
        "module M; " + std::string(kRes) +
        "public class App "
        "  public static method Make(): owned Object "
        "    return new Res(); "
        "  end Make; "
        "  public static method Run(): int "
        "    using var o := Make(); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Run()); "
        "    Console.WriteLine(Res.n); "
        "    return 0; "
        "  end Main; "
        "end App;");
    expectRunOutput(r, "0\n1\n");
}

// ---------------------------------------------------------------------------
// REJECT — the controls. The marker is still refused where §28.1 refuses it,
// with the SAME text.
// ---------------------------------------------------------------------------

BEZEL_TEST(owned_object_param_plain_8118,
           owned_int_parameter_is_still_rejected_spec_28_1) {
    JitResult r = runAot(
        "module M; "
        "public class App "
        "  public static method Take(owned x: int): int "
        "    return 1; "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(3)); "
        "    return 0; "
        "  end Main; "
        "end App;");
    expectRejectedPrimitive(r);
}

BEZEL_TEST(owned_object_param_plain_8118,
           owned_string_parameter_is_still_rejected_spec_28_1) {
    JitResult r = runAot(
        "module M; "
        "public class App "
        "  public static method Take(owned x: string): int "
        "    return 1; "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(\"s\")); "
        "    return 0; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`owned` is not admissible on a parameter of type `string` (a "
        "runtime-managed reference type)"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// The ASan / LeakSanitizer row the issue asks for: an owned Array passed
// through `Object?` in a 100-iteration loop, discharged exactly once.
// ---------------------------------------------------------------------------

BEZEL_TEST(owned_object_param_plain_8118,
           owned_array_through_object_is_disposed_exactly_once_spec_28_1_8_7) {
    // `runAotChecked` is the generated-code AddressSanitizer + LeakSanitizer
    // oracle (`detect_leaks=1`): the row fails if the array is NOT freed
    // (LSan) and if it is freed twice (ASan heap-use-after-free /
    // double-free). 100 iterations so a one-shot mistake cannot hide.
    JitResult r = runAotChecked(
        "module M; "
        "public class App "
        "  public static method Take(owned x: Object?): int "
        "    if x = null then return 0; end if; "
        "    return 1; "
        "  end Take; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 100 do "
        "      total := total + Take(new Array<int>()); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end App;");
    // The skip must be LOUD: a silent skip would report a clean pass having
    // run no sanitizer at all (the #3733 shape).
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(owned_object_param_plain_8118,
           owned_owning_array_through_object_disposes_its_elements_spec_28_11) {
    // The OTHER half of the array case: an OWNING array (§28.11 — the owning
    // family owns its class elements) passed through an `Object?` slot. The
    // discharge must run the array's ELEMENT cascade — the element's
    // destructor fires exactly once — and free the backing exactly once.
    JitResult r = runAotChecked(
        "module M; "
        "public class Res "
        "  public static n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Res.n := Res.n + 1; end destructor; "
        "end Res; "
        "public class App "
        "  public static method Take(owned x: Object?): int "
        "    if x = null then return 0; end if; "
        "    return 1; "
        "  end Take; "
        "  public static method Main(): int "
        "    var a := new OwningArray<Res>(); "
        "    a.Add(new Res()); "
        "    Console.WriteLine(Take(move a)); "
        "    Console.WriteLine(Res.n); "
        "    return 0; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The element's destructor ran exactly once (a second disposal would be an
    // ASan double-free / use-after-free, which the same run reports).
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n1\n");
}

// ---------------------------------------------------------------------------
// The ownership-pass limb: the parameter carries the §28.3 obligation, so
// the auto-drop warning names it (the same warning a class-typed `owned`
// parameter gets).
// ---------------------------------------------------------------------------
BEZEL_TEST(owned_object_param_plain_8118,
           owned_object_parameter_carries_the_auto_drop_obligation_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "public class Res "
        "  public constructor() end constructor; "
        "end Res; "
        "public class App "
        "  public static method Take(owned x: Object?): int "
        "    return 1; "
        "  end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine(Take(new Res())); "
        "    return 0; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "x"));
}
