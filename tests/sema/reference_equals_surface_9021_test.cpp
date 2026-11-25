// Regression tests for issue #9021 (v0.359) — the four #3079 holes the
// `ReferenceEquals` surface still had on r30-c1.
//
// SPEC (docs/Bezel_Language_Spec.md, v0.359).
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
//   "The declaration above defines the public surface."
//
//   EXACTLY ONE form, TWO parameters. §22.6 *Overloading*: "If no candidate
//   is applicable at all ... the call is a compile error with no candidates
//   cited, because no overload was reachable" — a call that binds no declared
//   form of a member is a compile error.
//
// §11.7 *Static classes*: "`Object`'s **static** `ReferenceEquals` (§8.1) is
// inherited by a static class as by every class". §22.12 *Static members*: "A
// static member declared on a base class is a member of every class derived
// from it ... and may be named through a derived type". §2.7 rule 1 resolves
// a chain head that is a TYPE against "that type's members, declared or
// inherited".
//
// §8.1 also says who does NOT inherit it: "`Object` is the universal root.
// Every type derives from it: classes ... directly through the inheritance
// chain, with `string`, interfaces, and function types reaching `Object`
// similarly via their reference-type heap representation; records, enums,
// choices, primitives, inline arrays, and the unit type derive via boxing
// (§8.6) when used in `Object`-typed positions". §11.2: an interface declares
// no member named `GetType` or `ReferenceEquals`.
//
// §26.5 rule 2 *Resolution*: "Through a type name (§2.7 rule 1) the chain
// resolves against the type's nested types, enum constants and static
// members, declared or inherited — and a call or read `T.M` that none of them
// supplies names no member and is the same compile error".
//
// §27.8: "Type aliases — generic or not — see through to the aliased type for
// identity, equality, method dispatch, and overload resolution" — so
// `KA.ReferenceEquals(a, b)` (with `type KA := K;`) is the same call as
// `K.ReferenceEquals(a, b)`.
//
// MEASURED on the pre-fix base (r30-c1 @ c00f5a63f): EVERY row below
// COMPILED and exited 0, printing NOTHING (the #3079 "the argument of this
// `Console.WriteLine` failed to lower to a value" warning), except the alias
// row, which was refused with a FALSE diagnostic — "cannot call the static
// member `ReferenceEquals` through an instance of `K`" — although `KA` is a
// TYPE NAME (§2.7 rule 1), not an instance.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The ARITY axis's wording — the #2446 universal-arity wording, the same one
// the QUALIFIED root spelling (`System.Object.ReferenceEquals(a)`, §8.1:
// "`Object`'s module is `System`") already draws.
const char* kArityReferenceEquals =
    "the universal `Object.ReferenceEquals` member takes 2 argument(s) but "
    "the call supplies 1 (\xC2\xA7""8.1)";
const char* kArityReferenceEqualsThree =
    "the universal `Object.ReferenceEquals` member takes 2 argument(s) but "
    "the call supplies 3 (\xC2\xA7""8.1)";

// The NO-MEMBER wording of a type name that supplies no such member
// (§26.5 rule 2).
const char* kNoMemberOnTypeName =
    "no member `ReferenceEquals` on type `R` (\xC2\xA7""11.3, \xC2\xA7""12.1, "
    "\xC2\xA7""26.5 rule 2)";
const char* kNoMemberOnEnumTypeName =
    "no member `ReferenceEquals` on type `E` (\xC2\xA7""11.3, \xC2\xA7""12.1, "
    "\xC2\xA7""26.5 rule 2)";
const char* kNoMemberOnChoiceTypeName =
    "no member `ReferenceEquals` on type `Ch` (\xC2\xA7""11.3, \xC2\xA7""12.1, "
    "\xC2\xA7""26.5 rule 2)";
const char* kNoMemberOnInterfaceTypeName =
    "no member `ReferenceEquals` on type `I` (\xC2\xA7""11.3, \xC2\xA7""12.1, "
    "\xC2\xA7""26.5 rule 2)";
// The remedy the refusal names.
const char* kWriteObjectSpelling =
    "write `Object.ReferenceEquals(a, b)`";

// The diagnostic text of the result, for the leak row's assertions.
std::string diagnosticText(const bezel::test::JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes.
void assertNoLeak(const bezel::test::JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes,
                                                   leakedAllocs);
    BEZEL_EXPECT_EQ(sawSummary ? leakedAllocs : 0ull, 0ull);
    BEZEL_EXPECT_EQ(sawSummary ? leakedBytes : 0ull, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

// The ISSUE'S REPRO for hole 1: the ROOT spelling with ONE argument.
std::string rootSpellingOneArgument() {
    return "module M; class K "
           "end K; "
           "class C "
           "  public static method Main(): int "
           "    var k := new K(); "
           "    Console.WriteLine(Object.ReferenceEquals(k)); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// HOLE 1 — the wrong-ARITY call to the ONE static §8.1 declares.
// ---------------------------------------------------------------------------

// §8.1 declares EXACTLY ONE form of `ReferenceEquals`, with TWO parameters;
// §22.6 refuses a call that binds no declared form of it. The ROOT spelling
// is exempt from the #2446 universal-arity check (that check skips every
// PRIMITIVE-typed receiver, and `Object` IS one), so no arity gate saw it:
// the call compiled, printed NOTHING and exited 0.
BEZEL_TEST(reference_equals_surface_9021,
           root_spelling_with_one_argument_is_refused) {
    auto r = runAot(rootSpellingOneArgument());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kArityReferenceEquals));
}

// The same call with THREE arguments binds no declared form either.
BEZEL_TEST(reference_equals_surface_9021,
           root_spelling_with_three_arguments_is_refused) {
    auto r = runAot(
        "module M; class K "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    Console.WriteLine(Object.ReferenceEquals(k, k, k)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kArityReferenceEqualsThree));
}

// The CLASS type name spelling of the SAME inherited static (§11.7 / §22.12)
// has the same declared form, so the same refusal.
BEZEL_TEST(reference_equals_surface_9021,
           class_type_name_with_one_argument_is_refused) {
    auto r = runAot(
        "module M; class K "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    Console.WriteLine(K.ReferenceEquals(k)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kArityReferenceEquals));
}

// ---------------------------------------------------------------------------
// HOLE 2 — the RECORD / ENUM / CHOICE / INTERFACE type-name spellings.
//
// §8.1: only a CLASS inherits the root's static; a record, an enum and a
// choice reach `Object` by BOXING, and §11.2 gives an interface no member
// named `ReferenceEquals` at all. §26.5 rule 2's answer is the compile error.
// ---------------------------------------------------------------------------

BEZEL_TEST(reference_equals_surface_9021,
           record_type_name_spelling_is_refused) {
    auto r = runAot(
        "module M; record R(X: int); "
        "class C "
        "  public static method Main(): int "
        "    var r := new R(1); "
        "    var r2 := new R(1); "
        "    Console.WriteLine(R.ReferenceEquals(r, r2)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoMemberOnTypeName));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kWriteObjectSpelling));
}

BEZEL_TEST(reference_equals_surface_9021,
           enum_type_name_spelling_is_refused) {
    auto r = runAot(
        "module M; enum E "
        "  A, B; "
        "end E; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(E.ReferenceEquals(E.A, E.B)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoMemberOnEnumTypeName));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kWriteObjectSpelling));
}

BEZEL_TEST(reference_equals_surface_9021,
           choice_type_name_spelling_is_refused) {
    auto r = runAot(
        "module M; choice Ch One(i: int), Two(s: string); end Ch; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(Ch.ReferenceEquals(Ch.One(1), Ch.Two(\"a\"))); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoMemberOnChoiceTypeName));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kWriteObjectSpelling));
}

BEZEL_TEST(reference_equals_surface_9021,
           interface_type_name_spelling_is_refused) {
    auto r = runAot(
        "module M; interface I "
        "  method Foo(): int; "
        "end I; "
        "class Impl: I "
        "  public method Foo(): int => 1; "
        "end Impl; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(I.ReferenceEquals(new Impl(), new Impl())); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, kNoMemberOnInterfaceTypeName));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kWriteObjectSpelling));
}

// ---------------------------------------------------------------------------
// HOLE 4 — a type ALIAS is a TYPE NAME (§2.7 rule 1 / §27.8), not an
// instance: `KA.ReferenceEquals(a, b)` is the call `K.ReferenceEquals(a, b)`
// is.
// ---------------------------------------------------------------------------

// The ISSUE'S REPRO, verbatim: the alias spelling of the inherited static.
// On the base it was refused with a FALSE diagnostic — "cannot call the
// static member `ReferenceEquals` through an instance of `K`" — because the
// #8776 gate read the receiver's EXPRESSION TYPE, and an alias carries the
// ALIASED type there.
BEZEL_TEST(reference_equals_surface_9021,
           type_alias_spelling_answers_the_identity_compare) {
    auto r = runAot(
        "module M; class K "
        "end K; "
        "type KA := K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var k2 := new K(); "
        "    Console.WriteLine(KA.ReferenceEquals(k, k)); "
        "    Console.WriteLine(KA.ReferenceEquals(k, k2)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// The alias spelling called 200 times, under the CHECKED oracle (the
// sanitizer leg really runs — a toolchain skip fails the row instead of
// passing it silently) and with the leak assertion: the alias spelling is the
// ROOT's identity compare (§8.4), so it must buy no allocation of its own.
BEZEL_TEST(reference_equals_surface_9021,
           type_alias_spelling_loop_200_no_leak) {
    auto r = runAotChecked(
        "module M; class K "
        "end K; "
        "type KA := K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    var k2 := new K(); "
        "    var hits := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      if KA.ReferenceEquals(k, k) then hits := hits + 1; end; "
        "      if KA.ReferenceEquals(k, k2) then hits := hits + 1; end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(hits); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
    assertNoLeak(r);
}
