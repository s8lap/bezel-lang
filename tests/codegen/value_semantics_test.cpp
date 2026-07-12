// Codegen — compiler-synthesized value-semantics members for records (§9) and
// enums (§10): ToString, GetHashCode, and structural Equals.
//
// §8.5 / §9.1: a positional record's ToString is `"TypeName(F1: v1, F2: v2)"`,
// the field names spelled exactly as declared (§9.1: "the compiler does not
// transform them"); string fields format via their own ToString (identity,
// §9.6), so they appear unquoted. A zero-field record is `"TypeName()"`
// (§9.2). Enum ToString is the type's qualified name (§8.5 — the spec default
// for enums; member-name formatting is a separate user concern).
//
// §8.3: GetHashCode combines per-field hashes; equal values MUST hash equal
// (the one-directional invariant). The synthesized record GetHashCode is
// consistent with the structural Equals of §9.1.
//
// The JIT harness returns `main`'s int, so string results are checked by
// comparing `record.ToString()` to the spec-stated literal with `=`.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §9.1: positional record ToString matches `TypeName(field: value, ...)`.
BEZEL_TEST(codegen_value_semantics, record_tostring_format_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(1, 2); "
        "  return (1 if p.ToString() = \"Point(x: 1, y: 2)\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5/§9.1: the field names are spelled exactly as declared (PascalCase by
// convention; the compiler does not transform them), and a string field
// formats unquoted (string ToString is identity, §9.6).
BEZEL_TEST(codegen_value_semantics, record_tostring_string_field_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Person(Name: string, Age: int); "
        "class C public static method Main(): int "
        "  var p := new Person(\"Ann\", 30); "
        "  return (1 if p.ToString() = \"Person(Name: Ann, Age: 30)\" else 0); "
        "end Main; end C;"), 1);
}

// §9.2: a zero-field record's ToString is `"TypeName()"`.
BEZEL_TEST(codegen_value_semantics, record_tostring_zero_field_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Unit(); "
        "class C public static method Main(): int "
        "  var u := new Unit(); "
        "  return (1 if u.ToString() = \"Unit()\" else 0); "
        "end Main; end C;"), 1);
}

// §9.1: nested record ToString recurses into the inner record's ToString.
BEZEL_TEST(codegen_value_semantics, record_tostring_nested_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Inner(a: int, b: int); record Outer(p: Inner, z: int); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(3, 4), 5); "
        "  return (1 if o.ToString() = \"Outer(p: Inner(a: 3, b: 4), z: 5)\" else 0); "
        "end Main; end C;"), 1);
}

// §9.x: a user-declared ToString overrides the synthesized one.
BEZEL_TEST(codegen_value_semantics, record_user_tostring_wins) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Tagged(x: int) public method ToString(): string => \"TAG\"; end Tagged; "
        "class C public static method Main(): int "
        "  var t := new Tagged(7); "
        "  return (1 if t.ToString() = \"TAG\" else 0); "
        "end Main; end C;"), 1);
}

// §8.3: two equal records have equal GetHashCode (the hash-equality invariant).
BEZEL_TEST(codegen_value_semantics, record_equal_hash_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(1, 2); var b := new Point(1, 2); "
        "  return (1 if a.GetHashCode() = b.GetHashCode() else 0); "
        "end Main; end C;"), 1);
}

// §8.3: different records (usually) have different hashes — order-dependent
// combiner makes Point(1,2) and Point(2,1) differ (§8.3 example).
BEZEL_TEST(codegen_value_semantics, record_distinct_hash_order_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(a: int, b: int); "
        "class C public static method Main(): int "
        "  var x := new Pair(1, 2); var y := new Pair(2, 1); "
        "  return (1 if x.GetHashCode() <> y.GetHashCode() else 0); "
        "end Main; end C;"), 1);
}

// §8.3: a record with a string field hashes by content, so two records with
// equal string content hash equal (consistent with structural equality).
BEZEL_TEST(codegen_value_semantics, record_string_field_hash_consistent_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Person(Name: string, Age: int); "
        "class C public static method Main(): int "
        "  var a := new Person(\"Ann\", 30); var b := new Person(\"Ann\", 30); "
        "  return (1 if a.GetHashCode() = b.GetHashCode() else 0); "
        "end Main; end C;"), 1);
}

// §9.1: synthesized Equals is structural — equal field values compare equal.
BEZEL_TEST(codegen_value_semantics, record_equals_structural_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(1, 2); var b := new Point(1, 2); "
        "  return (1 if a.Equals(b) else 0); "
        "end Main; end C;"), 1);
}

// §9.1: synthesized Equals reports inequality for differing fields.
BEZEL_TEST(codegen_value_semantics, record_equals_unequal_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(1, 2); var b := new Point(1, 9); "
        "  return (1 if a.Equals(b) else 0); "
        "end Main; end C;"), 0);
}

// §8.5: enum ToString returns the type's qualified name (the spec default).
BEZEL_TEST(codegen_value_semantics, enum_tostring_type_name_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Priority Low, Medium, High; end Priority; "
        "class C public static method Main(): int "
        "  var p := Priority.High; "
        "  return (1 if p.ToString() = \"Priority\" else 0); "
        "end Main; end C;"), 1);
}

// §8.3: enum GetHashCode is the underlying integer value (§10.1 default
// numbering: Low=0, Medium=1, High=2).
BEZEL_TEST(codegen_value_semantics, enum_gethashcode_underlying_int_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Priority Low, Medium, High; end Priority; "
        "class C public static method Main(): int "
        "  var p := Priority.Medium; "
        "  return p.GetHashCode(); "
        "end Main; end C;"), 1);
}

// §8.3: equal enum values hash equal; distinct enum values hash distinct.
BEZEL_TEST(codegen_value_semantics, enum_hash_distinguishes_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var a := Color.Green; var b := Color.Green; var c := Color.Blue; "
        "  return (1 if (a.GetHashCode() = b.GetHashCode()) "
        "             and (a.GetHashCode() <> c.GetHashCode()) else 0); "
        "end Main; end C;"), 1);
}

// §8.4/§10: enum Equals is integer comparison.
BEZEL_TEST(codegen_value_semantics, enum_equals_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var a := Color.Green; var b := Color.Green; "
        "  return (1 if a.Equals(b) else 0); "
        "end Main; end C;"), 1);
}

// §8.5: an enum interpolates as the type name (its default ToString), not as
// its underlying integer.
BEZEL_TEST(codegen_value_semantics, enum_interpolation_type_name_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Priority Low, Medium, High; end Priority; "
        "class C public static method Main(): int "
        "  var p := Priority.Low; "
        "  return (1 if $\"{p}\" = \"Priority\" else 0); "
        "end Main; end C;"), 1);
}

// §9.1: a record interpolates with the synthesized ToString form.
BEZEL_TEST(codegen_value_semantics, record_interpolation_format_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(4, 5); "
        "  return (1 if $\"{p}\" = \"Point(x: 4, y: 5)\" else 0); "
        "end Main; end C;"), 1);
}
