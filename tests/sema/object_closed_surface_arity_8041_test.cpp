// Regression tests for the root `Object`'s CLOSED SURFACE — its ARITY axis
// (#8041), its extension side (#8042), and its one STATIC member (#8043).
//
// SPEC (docs/Bezel_Language_Spec.md, v0.350).
//
// §8.1 *Definition* — the root `Object`'s declaration, and the sentence that
// closes it:
//
//     public class Object
//       public method GetType(): Type;
//       public virtual method Equals(other: Object?): bool;
//       public virtual method GetHashCode(): int;
//       public virtual method ToString(): string;
//       public static method ReferenceEquals(a: Object?, b: Object?): bool;
//     end Object;
//
//   "`Object` is a compiler intrinsic; method bodies are provided by the
//    runtime. The declaration above defines the public surface."
//
// §26.5 rule 1 *Resolution*: "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` — of any arity, applicability,
// or accessibility — instance resolution is committed and extension blocks
// are **never consulted** for that name on that receiver. If the committed
// instance lookup then fails (no applicable overload, §22.5/§22.6, or the
// member is inaccessible), the call is a compile error citing the instance
// member(s); it does not fall back to an extension (the diagnostic should
// note a same-named extension where one exists)."
//
// §22.6 *Overloading*: a call that binds no form of a member is a compile
// error — the arity axis the closed surface needs.
//
// §22.12 *Static members* (cf. C# CS0176): "a static member must be accessed
// through the type name, not an instance". §2.7 rule 1: a chain head that
// resolves to a TYPE resolves the rest of the chain against that type's
// members; one that resolves to a VALUE is member access. So §8.1's one STATIC
// member (`ReferenceEquals`) is reached through its TYPE
// (`Object.ReferenceEquals(a, b)`), never through a value.
//
// MEASURED on the pre-fix base (this lane's base `0b154fdc`): every reject
// row below COMPILED and exited 0. `o.Equals()` and `o.GetHashCode(1)`
// degraded the slot to a zero-initialised value (the #2434 warning) and
// `o.ToString(1)` was silently accepted; `o.ReferenceEquals(a, b)` compiled
// with only a codegen "cannot dispatch member 'ReferenceEquals' on receiver
// of type 'Object'" warning; and the wrong-arity extension call
// `o.Extra()` was refused with a message that asserted the OPPOSITE of the
// truth ("no extension on `Object` provides `Extra`" — the extension DOES
// provide `Extra`; the CALL has the wrong arity). Every control row below
// compiled and ran with the same output before and after the fix.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The ARITY axis's wording (#8041). The first clause is the #2446
// universal-arity wording, so the two sites that refuse a wrong-arity
// universal call say the same thing.
const char* kArityEquals =
    "the universal `Object.Equals` member takes 1 argument(s) but the call "
    "supplies 0 (\xC2\xA7""8.1)";
const char* kArityGetHashCode =
    "the universal `Object.GetHashCode` member takes 0 argument(s) but the "
    "call supplies 1 (\xC2\xA7""8.1)";
const char* kArityToString =
    "the universal `Object.ToString` member takes 0 argument(s) but the call "
    "supplies 1 (\xC2\xA7""8.1)";
const char* kArityGetType =
    "the universal `Object.GetType` member takes 0 argument(s) but the call "
    "supplies 1 (\xC2\xA7""8.1)";

// The extension side's wording (#8042): the ARITY is named, and the
// message does NOT claim the member's absence.
const char* kExtArity =
    "the extension method `Extra` on type `Object` takes 1 argument(s) but "
    "the call supplies 0 (\xC2\xA7""26.5)";
// The same refusal when the extension declares the name in TWO arities: the
// declared forms are named, and the member's absence is still not claimed.
const char* kExtArityTwoForms =
    "no form of the extension method `Extra` on type `Object` takes 0 "
    "argument(s) — its declared forms take 1 and 2 (\xC2\xA7""26.5)";
// The FALSE wording #8042 removes: an extension on `Object` DOES provide
// `Extra`, so a refusal may not say it does not.
const char* kFalseAbsence =
    "no extension on `Object` provides `Extra`";

// The static member's wording (#8043).
const char* kStaticThroughInstance =
    "cannot call the static member `ReferenceEquals` through an instance of "
    "`Object`";

// §8.2's reflection gate — `GetType` keeps its OWN diagnostic.
const char* kGetTypeNotImplemented = "reflection is not yet implemented";

std::string mainBody(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// #8041 — the closed surface has an ARITY axis.
// ---------------------------------------------------------------------------

// `Equals(other: Object?)` is a ONE-argument member; a zero-argument call
// binds no declared form of it.
BEZEL_TEST(object_surface_arity_8041, equals_with_no_argument_is_refused) {
    auto r = runAot(mainBody("var o: Object := 5; var b := o.Equals();"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kArityEquals));
}

// `GetHashCode()` is a ZERO-argument member.
BEZEL_TEST(object_surface_arity_8041, get_hash_code_with_an_argument_is_refused) {
    auto r = runAot(mainBody("var o: Object := 5; var h := o.GetHashCode(1);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kArityGetHashCode));
}

// `ToString()` is a ZERO-argument member. This spelling is the one the
// member lookup never sees (the primitive-intrinsic branch types it and
// returns), so the refusal is drawn at the call.
BEZEL_TEST(object_surface_arity_8041, to_string_with_an_argument_is_refused) {
    auto r = runAot(mainBody("var o: Object := 5; var s := o.ToString(1);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kArityToString));
}

// `GetType()` is a ZERO-argument member.
BEZEL_TEST(object_surface_arity_8041, get_type_with_an_argument_is_refused) {
    auto r = runAot(mainBody("var o: Object := 5; var g := o.GetType(1);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kArityGetType));
}

// The `?.` twin (§7.5): the null-conditional spelling resolves its member
// against the same declared type, so it is refused the same way.
BEZEL_TEST(object_surface_arity_8041, null_conditional_twin_is_refused) {
    auto r = runAot(mainBody("var o: Object? := 5; var b := o?.Equals();"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kArityEquals));
}

// The `!` twin (§7.4): the unwrapped receiver is the root `Object`, so the
// surface — and its arity — is the same.
BEZEL_TEST(object_surface_arity_8041, force_unwrap_twin_is_refused) {
    auto r = runAot(mainBody("var o: Object? := 5; var s := o!.ToString(1);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kArityToString));
}

// THE CONTROL ROW: the DECLARED forms still bind and read the data. The
// refusal is the arity axis, not a closure of the surface.
BEZEL_TEST(object_surface_arity_8041, declared_forms_still_bind) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; "
        "  Console.WriteLine(o.ToString()); "
        "  Console.WriteLine(o.Equals(o)); "
        "  Console.WriteLine(o.GetHashCode()); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\nTrue\n5\n");
}

// §8.2's reflection gate keeps its OWN diagnostic for the zero-argument
// `GetType()` — the arity axis must not shadow it.
BEZEL_TEST(object_surface_arity_8041, get_type_keeps_its_own_diagnostic) {
    auto r = runAot(mainBody("var o: Object := 5; var g := o.GetType();"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kGetTypeNotImplemented));
}

// A CLASS receiver keeps its own surface: §8.1's declaration is the root
// `Object`'s, not every receiver's.
BEZEL_TEST(object_surface_arity_8041, class_receiver_keeps_its_own_members) {
    auto r = runAot(
        "module M; "
        "class Cell public X: int; public method Hi(): int => 7; end Cell; "
        "class C public static method Main(): int "
        "  using var c := new Cell(); "
        "  Console.WriteLine(c.Hi()); "
        "  Console.WriteLine(c.ToString()); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\nM.Cell\n");
}

// ---------------------------------------------------------------------------
// #8042 — a wrong-ARITY call to an EXTENSION member over `Object` gets a
// diagnostic that names the arity and does NOT claim the member's absence.
// ---------------------------------------------------------------------------

BEZEL_TEST(object_surface_arity_8042, extension_member_with_the_wrong_arity_is_refused) {
    auto r = runAot(
        "module M; "
        "extension Object public method Extra(x: int): int => x + 1; end; "
        "class C public static method Main(): int "
        "  var o: Object := 5; var v := o.Extra(); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kExtArity));
    // The refusal names the ARITY; it does not assert the opposite of the
    // truth (the extension DOES provide `Extra`).
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kFalseAbsence));
}

// THE CONTROL ROW: the extension member still binds at its declared arity.
BEZEL_TEST(object_surface_arity_8042, extension_member_at_its_arity_still_binds) {
    auto r = runAot(
        "module M; "
        "extension Object public method Extra(x: int): int => x + 1; end; "
        "class C public static method Main(): int "
        "  var o: Object := 5; Console.WriteLine(o.Extra(41)); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// TWO blocks over `Object` declare the same name in DIFFERENT arities: the
// refusal names BOTH declared forms, and still does not claim the member's
// absence.
BEZEL_TEST(object_surface_arity_8042, two_declared_forms_are_named) {
    auto r = runAot(
        "module M; "
        "extension Object public method Extra(x: int): int => x + 1; end; "
        "extension Object public method Extra(x: int, y: int): int => x + y; end; "
        "class C public static method Main(): int "
        "  var o: Object := 5; var v := o.Extra(); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kExtArityTwoForms));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kFalseAbsence));
}

// A `where` clause on an `extension Object` block does NOT filter the block
// out for the root `Object`: §26.5.1's applicability filter needs the
// block's type variable BOUND, and the root `Object` binds none (the block's
// head is the root `Object`, a PRIMITIVE head, so it carries no free type
// variable). The refusal is therefore the ARITY one — and it still does not
// claim the member's absence.
BEZEL_TEST(object_surface_arity_8042, where_clause_does_not_change_the_refusal) {
    auto r = runAot(
        "module M; "
        "extension Object where T: hashable "
        "  public method Extra(x: int): int => x + 1; end; "
        "class C public static method Main(): int "
        "  var o: Object := 5; var v := o.Extra(); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kExtArity));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kFalseAbsence));
}

// ---------------------------------------------------------------------------
// #8043 — §8.1's one STATIC member is reached through its TYPE.
// ---------------------------------------------------------------------------

BEZEL_TEST(object_surface_arity_8043, instance_spelling_of_the_static_member_is_refused) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; var p: Object := 6; "
        "  var b := o.ReferenceEquals(o, p); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStaticThroughInstance));
}

// The `?.` twin of the same refusal.
BEZEL_TEST(object_surface_arity_8043, null_conditional_instance_spelling_is_refused) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var o: Object? := 5; var p: Object? := 6; "
        "  var b := o?.ReferenceEquals(o, p); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStaticThroughInstance));
}

// THE ADMITTING TWIN: the static spelling keeps working — it is the only
// spelling §3.4's static-qualifier path admits.
BEZEL_TEST(object_surface_arity_8043, static_spelling_still_works) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var o: Object := 5; var p: Object := 6; "
        "  Console.WriteLine(Object.ReferenceEquals(o, o)); "
        "  Console.WriteLine(Object.ReferenceEquals(o, p)); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}
