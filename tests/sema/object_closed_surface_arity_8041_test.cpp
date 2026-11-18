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

// ---------------------------------------------------------------------------
// #7270 — the same rule for a FUNCTION value (§8.1 v0.359, §22.12).
//
// §8.1: function types reach `Object` "via their reference-type heap
// representation", so a function value HAS `ReferenceEquals` — as the one
// `static` member of §8.1's block, reached through the type name (§22.12:
// "a static member must be accessed through the type name, not an
// instance"). MEASURED on base b279e8daf: `c.ReferenceEquals(c, c)` compiled
// with only the #3079 "failed to lower" warning, printed nothing, leaked 64 B.
// ---------------------------------------------------------------------------

BEZEL_TEST(object_surface_arity_7270, function_value_instance_spelling_of_the_static_member_is_refused) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var c := lambda(): int => 42; "
        "  Console.WriteLine(c.ReferenceEquals(c, c)); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot call the static member `ReferenceEquals` through a "
           "function value"));
}

// ---------------------------------------------------------------------------
// #8776 — the same rule for an INSTANCE of a declared class, record or
// interface (§8.1 v0.359, §22.12).
//
// §8.1: classes derive from `Object` "directly through the inheritance
// chain" and records via boxing, so they reach the one `static` member,
// which §22.12 requires through the type name, not an instance. The spec has
// no sentence naming "static through an instance of a class" (docs handoff
// #8774); the rule follows the #8043 / #7270 precedent. MEASURED on base
// 29d38200e: each spelling below compiled with only the #3079 "failed to
// lower" degrade warning and printed nothing. The admitting twins
// (`Object.ReferenceEquals(k, k2)`, a differently named instance method) are
// in the conformance-corpus candidates, not here.
// ---------------------------------------------------------------------------

namespace {
const char* kStaticThroughClassInstance =
    "cannot call the static member `ReferenceEquals` through an instance of "
    "`K`: `ReferenceEquals` is a static member of the root `Object` (\xC2\xA7""8.1)";
}  // namespace

BEZEL_TEST(object_surface_arity_8776, class_instance_spelling_of_the_static_member_is_refused) {
    auto r = runAot(
        "module M; class K end K; class C public static method Main(): int "
        "  var k := new K(); var k2 := new K(); "
        "  Console.WriteLine(k.ReferenceEquals(k, k2)); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStaticThroughClassInstance));
}

BEZEL_TEST(object_surface_arity_8776, record_instance_spelling_of_the_static_member_is_refused) {
    auto r = runAot(
        "module M; record K(X: int); class C public static method Main(): int "
        "  var k := new K(1); var k2 := new K(1); "
        "  Console.WriteLine(k.ReferenceEquals(k, k2)); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStaticThroughClassInstance));
}

BEZEL_TEST(object_surface_arity_8776, interface_instance_spelling_of_the_static_member_is_refused) {
    auto r = runAot(
        "module M; interface K method Foo(): int; end K; "
        "class Impl: K public method Foo(): int => 1; end Impl; "
        "class C public static method Take(i: K, j: K): bool => "
        "  i.ReferenceEquals(i, j); "
        "  public static method Main(): int "
        "  Console.WriteLine(C.Take(new Impl(), new Impl())); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStaticThroughClassInstance));
}

BEZEL_TEST(object_surface_arity_8776, null_conditional_and_this_spellings_are_refused) {
    auto q = runAot(
        "module M; class K end K; class C public static method Main(): int "
        "  var k: K? := new K(); var k2 := new K(); "
        "  Console.WriteLine(k?.ReferenceEquals(k, k2)); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(q.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(q));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(q, kStaticThroughClassInstance));
    auto t = runAot(
        "module M; class K "
        "  public method Same(o: K): bool => this.ReferenceEquals(this, o); "
        "end K; class C public static method Main(): int "
        "  var k := new K(); Console.WriteLine(k.Same(k)); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(t.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(t));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(t, kStaticThroughClassInstance));
}
