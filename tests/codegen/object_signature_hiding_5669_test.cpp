// Sema/codegen — issue #5669, the HIDING half (§12.2, v0.338). `Object`'s
// three virtual members block a NON-override declaration at their own
// signature; the reject must fire on a class that declares it.
//
// Spec: §12.2 "Hiding is not allowed", v0.338 (docs/Bezel_Language_Spec.md
// line 2027), verbatim from the governing sentence —
//
//     `Object`'s three virtual members ([§8.1]: `Equals(other: Object?)`,
//     `GetHashCode()`, `ToString()`) block only a declaration at their **own
//     signature** — one whose parameter list matches `Object`'s up to the
//     nullability widening above — which `override` reaches and nothing else
//     does: a class may declare a non-`override` **instance** member named
//     `Equals`, `GetHashCode` or `ToString` at any other parameter list (...)
//
// and §8.1 (lines 1062-1068) declares `public virtual method Equals(other:
// Object?): bool;`, `public virtual method GetHashCode(): int;` and `public
// virtual method ToString(): string;`. The admission is `Object`'s alone and
// covers instance members only: "a static member of the three names stays
// blocked" (line 2027).
//
// The overload half (§22.6 resolution of `Equals(Point)` beside `Equals(
// Object?)`) is NOT pinned here; it is tracked on #5669 separately.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

// ─── Reject: a non-override member at Object's own signature ─────────────

// §12.2 line 2027: `Equals(other: Object?)` without `override` is hiding.
BEZEL_TEST(object_signature_hiding_5669,
           plain_equals_at_object_signature_rejected_spec_12_2) {
    auto r = runAot(
        "module M5669a; "
        "class C "
        "  public method Equals(other: Object?): bool => true; "
        "end C; "
        "class Program public static method Main(): int return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "hides the inherited `Object.Equals`"));
}

// §12.2 line 2027: "plain or `virtual`" — the `virtual` spelling is hiding too.
BEZEL_TEST(object_signature_hiding_5669,
           virtual_equals_at_object_signature_rejected_spec_12_2) {
    auto r = runAot(
        "module M5669b; "
        "class C "
        "  public virtual method Equals(other: Object?): bool => true; "
        "end C; "
        "class Program public static method Main(): int return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "hides the inherited `Object.Equals`"));
}

// §12.2 line 2027: `Equals(other: Object)` (non-nullable) is the same
// signature up to the nullability widening.
BEZEL_TEST(object_signature_hiding_5669,
           non_nullable_equals_at_object_signature_rejected_spec_12_2) {
    auto r = runAot(
        "module M5669c; "
        "class C "
        "  public method Equals(other: Object): bool => true; "
        "end C; "
        "class Program public static method Main(): int return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "hides the inherited `Object.Equals`"));
}

// §12.2 line 2027: `GetHashCode()` at Object's signature.
BEZEL_TEST(object_signature_hiding_5669,
           plain_gethashcode_at_object_signature_rejected_spec_12_2) {
    auto r = runAot(
        "module M5669d; "
        "class C "
        "  public method GetHashCode(): int => 1; "
        "end C; "
        "class Program public static method Main(): int return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "hides the inherited `Object.GetHashCode`"));
}

// §12.2 line 2027: `ToString()` at Object's signature.
BEZEL_TEST(object_signature_hiding_5669,
           plain_tostring_at_object_signature_rejected_spec_12_2) {
    auto r = runAot(
        "module M5669e; "
        "class C "
        "  public method ToString(): string => \"c\"; "
        "end C; "
        "class Program public static method Main(): int return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "hides the inherited `Object.ToString`"));
}

// §12.2 line 2027: "a static member of the three names stays blocked" — a
// static at Object's own signature is hiding as well.
BEZEL_TEST(object_signature_hiding_5669,
           static_equals_at_object_signature_rejected_spec_12_2) {
    auto r = runAot(
        "module M5669f; "
        "class C "
        "  public static method Equals(other: Object?): bool => true; "
        "end C; "
        "class Program public static method Main(): int return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "hides the inherited `Object.Equals`"));
}

// ─── Accept controls: the spellings §12.2 keeps admitted ─────────────────

// §12.2 line 2027: `override` reaches Object's slot, so the override at the
// signature is the legal spelling and must keep compiling and running.
BEZEL_TEST(object_signature_hiding_5669,
           override_equals_at_object_signature_accepted_spec_12_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M5669g; "
        "class C "
        "  public override method Equals(other: Object?): bool => true; "
        "  public override method GetHashCode(): int => 3; "
        "end C; "
        "class Program public static method Main(): int "
        "  var o: Object := new C(); "
        "  Console.WriteLine(o.Equals(o)); "
        "  return 0; "
        "end Main; end Program;"),
              "True");
}

// §12.2 line 2027: `Equals(a: K, b: K)` is "any other parameter list" — an
// overload that is admitted, not hiding (the IEqualityComparer<K> shape).
BEZEL_TEST(object_signature_hiding_5669,
           equals_two_params_is_admitted_overload_spec_12_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M5669h; "
        "class C "
        "  public method Equals(a: int, b: int): bool => a = b; "
        "  public method GetHashCode(value: int): int => value; "
        "end C; "
        "class Program public static method Main(): int "
        "  var c := new C(); "
        "  Console.WriteLine(c.Equals(1, 1)); "
        "  return 0; "
        "end Main; end Program;"),
              "True");
}

// §12.2 line 2027: `ToString(fmt: string)` is a different parameter list and
// is admitted beside the inherited `ToString()`.
BEZEL_TEST(object_signature_hiding_5669,
           tostring_with_format_param_is_admitted_overload_spec_12_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M5669i; "
        "class C "
        "  public method ToString(fmt: string): string => \"f\"; "
        "end C; "
        "class Program public static method Main(): int "
        "  var c := new C(); "
        "  Console.WriteLine(c.ToString(\"x\")); "
        "  return 0; "
        "end Main; end Program;"),
              "f");
}

// §12.2 line 2027: an `Equals` at the class's OWN type is the slot's second
// spelling, not Object's signature, and is not rejected by this half.
BEZEL_TEST(object_signature_hiding_5669,
           own_type_equals_not_object_signature_accepted_spec_12_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M5669j; "
        "class Point "
        "  public constructor(x: int) X := x; end constructor; "
        "  public readonly property X: int; "
        "  public method Equals(other: Point): bool => X = other.X; "
        "  public override method GetHashCode(): int => X; "
        "end Point; "
        "class Program public static method Main(): int "
        "  var p := new Point(1); "
        "  var q := new Point(1); "
        "  Console.WriteLine(p.Equals(q)); "
        "  return 0; "
        "end Main; end Program;"),
              "True");
}

// §12.2 line 2027: records keep their own value-kind rule (§8.3 / §10.5); the
// class-only hiding check does not reach a record's own-typed `Equals(other: R)`.
BEZEL_TEST(object_signature_hiding_5669,
           record_equals_at_object_signature_unchanged_spec_12_2) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M5669k; "
        "record R(X: int) "
        "  public method Equals(other: R): bool => true; "
        "  public method GetHashCode(): int => 1; "
        "end R; "
        "class Program public static method Main(): int "
        "  var r := new R(1); "
        "  Console.WriteLine(r.Equals(r)); "
        "  return 0; "
        "end Main; end Program;"),
              "True");
}
