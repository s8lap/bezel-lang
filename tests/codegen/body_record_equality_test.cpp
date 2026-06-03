// Codegen — §9.2 auto-equality for *body* records, and the §8.5 ToString rule.
//
// §9.2 "A body-only record receives auto-generated `=`, `<>`, `GetHashCode`,
// and `ToString` over its auto-properties (and only its auto-properties) when
// every member of the record is one of: an auto-property …; a method or
// expression-bodied property …; an overload of a non-equality operator; a
// constructor. Such a record is said to be auto-equality-bearing." The
// participating components are the auto-properties in declaration order; the
// synthesized members reuse the positional-record machinery over that set.
//
// §9.2 "A body record is NOT auto-equality-bearing … when it contains: an
// explicit field declaration … " and (§8.5) when it declares its own `Equals`
// / `GetHashCode` / `ToString`. An ineligible body record gets the type-name
// default for ToString (§8.5) and no auto equality.
//
// §8.5 "For body records that satisfy the auto-equality conditions … a
// representation of auto-properties, formatted as
// `"TypeName(Prop1: value1, Prop2: value2)"`."
//
// The JIT harness returns `main`'s int, so string results are checked by
// comparing `record.ToString()` to the spec-stated literal with `=`.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// An eligible body record: only auto-properties + a constructor that
// initializes them (§9.2 — the constructor performs only property
// initialization, so it does not disqualify).
const char* kPt =
    "record Pt "
    "  public property X: int; "
    "  public property Y: int; "
    "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
    "end Pt; ";
}  // namespace

// §9.2: an eligible body record's synthesized Equals is structural — equal
// auto-property values compare equal.
BEZEL_TEST(codegen_body_record, equals_true_for_equal_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kPt +
        "class C public static method Main(): int "
        "  var a := new Pt(1, 2); var b := new Pt(1, 2); "
        "  return (1 if a.Equals(b) else 0); "
        "end Main; end C;"), 1);
}

// §9.2: differing auto-property values compare unequal.
BEZEL_TEST(codegen_body_record, equals_false_for_differing_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kPt +
        "class C public static method Main(): int "
        "  var a := new Pt(1, 2); var b := new Pt(1, 9); "
        "  return (1 if a.Equals(b) else 0); "
        "end Main; end C;"), 0);
}

// §8.3 via §9.2: equal body records hash equal (the hash-equality invariant,
// consistent with the synthesized structural Equals).
BEZEL_TEST(codegen_body_record, gethashcode_equal_for_equal_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kPt +
        "class C public static method Main(): int "
        "  var a := new Pt(1, 2); var b := new Pt(1, 2); "
        "  return (1 if a.GetHashCode() = b.GetHashCode() else 0); "
        "end Main; end C;"), 1);
}

// §8.5: the eligible body record's ToString is
// `"TypeName(Prop1: v1, Prop2: v2)"` over the auto-properties in declaration
// order.
BEZEL_TEST(codegen_body_record, tostring_format_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kPt +
        "class C public static method Main(): int "
        "  var a := new Pt(1, 2); "
        "  return (1 if a.ToString() = \"Pt(X: 1, Y: 2)\" else 0); "
        "end Main; end C;"), 1);
}

// §9.2: the worked Rectangle example — auto-properties plus a *method*. The
// method does not contribute storage and does not disqualify; equality and
// ToString are over the auto-properties only.
BEZEL_TEST(codegen_body_record, rectangle_example_eligible_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Rectangle "
        "  public readonly property Width: int; "
        "  public readonly property Height: int; "
        "  public constructor(width: int, height: int) "
        "    Width := width; Height := height; end constructor; "
        "  public method Area(): int => Width * Height; "
        "end Rectangle; "
        "class C public static method Main(): int "
        "  var r := new Rectangle(3, 4); var s := new Rectangle(3, 4); "
        "  return (1 if r.Equals(s) "
        "             and r.ToString() = \"Rectangle(Width: 3, Height: 4)\" "
        "             and r.Area() = 12 else 0); "
        "end Main; end C;"), 1);
}

// §9.2 / §8.5: a string auto-property hashes by content and formats unquoted
// (its own ToString is identity, §9.6) — same behavior as a positional
// record's string field, since the synthesis iterates auto-properties the same
// way. (Structural string *Equals* over body-record string auto-properties is
// covered by record_string_equality_test.cpp, which exercises the §8.3 content
// comparison emitStructEquality now performs for string fields.)
BEZEL_TEST(codegen_body_record, string_auto_property_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Person "
        "  public property Name: string; "
        "  public property Age: int; "
        "  public constructor(name: string, age: int) "
        "    Name := name; Age := age; end constructor; "
        "end Person; "
        "class C public static method Main(): int "
        "  var a := new Person(\"Ann\", 30); var b := new Person(\"Ann\", 30); "
        "  return (1 if a.GetHashCode() = b.GetHashCode() "
        "             and a.ToString() = \"Person(Name: Ann, Age: 30)\" "
        "             else 0); "
        "end Main; end C;"), 1);
}

// §9.2: a non-equality operator overload does not disqualify a body record —
// it remains auto-equality-bearing (Equals/ToString are synthesized).
BEZEL_TEST(codegen_body_record, non_equality_operator_eligible_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record V "
        "  public property X: int; "
        "  public property Y: int; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "  public static method operator +(a: V, b: V): V => "
        "    new V(a.X + b.X, a.Y + b.Y); "
        "end V; "
        "class C public static method Main(): int "
        "  var a := new V(2, 3); var b := new V(2, 3); "
        "  return (1 if a.Equals(b) and a.ToString() = \"V(X: 2, Y: 3)\" "
        "             else 0); "
        "end Main; end C;"), 1);
}

// §9.2: a body record that declares its own `Equals` is NOT auto-equality-
// bearing — the user's Equals is invoked (here it returns true even for
// differing values), and there is no synthesized structural equality.
BEZEL_TEST(codegen_body_record, manual_equals_ineligible_user_wins_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Tagged "
        "  public property X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public method Equals(other: Tagged): bool => true; "
        "end Tagged; "
        "class C public static method Main(): int "
        "  var a := new Tagged(1); var b := new Tagged(9); "
        "  return (1 if a.Equals(b) else 0); "  // user Equals → true
        "end Main; end C;"), 1);
}

// §8.5: a body record that declares its own `Equals` is ineligible, so its
// ToString is the type-name default (NOT the auto-property listing).
BEZEL_TEST(codegen_body_record, manual_equals_tostring_type_name_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Tagged "
        "  public property X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public method Equals(other: Tagged): bool => true; "
        "end Tagged; "
        "class C public static method Main(): int "
        "  var a := new Tagged(7); "
        "  return (1 if a.ToString() = \"Tagged\" else 0); "
        "end Main; end C;"), 1);
}

// §9.2 / §8.5: a body record with an explicit field declaration (storage that
// is not an auto-property) is NOT auto-equality-bearing — its ToString is the
// type-name default. The auto-property still reads correctly (storage layout
// includes both the field and the auto-property).
BEZEL_TEST(codegen_body_record, explicit_field_ineligible_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record HasField "
        "  public property X: int; "
        "  private _cache: int := 0; "
        "  public constructor(x: int) X := x; end constructor; "
        "end HasField; "
        "class C public static method Main(): int "
        "  var a := new HasField(5); "
        "  return (1 if a.ToString() = \"HasField\" and a.X = 5 else 0); "
        "end Main; end C;"), 1);
}

// §8.5: an eligible body record interpolates with the synthesized property-
// listing ToString (`Console.WriteLine(person)` produces useful output).
BEZEL_TEST(codegen_body_record, interpolation_eligible_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kPt +
        "class C public static method Main(): int "
        "  var a := new Pt(4, 5); "
        "  return (1 if $\"{a}\" = \"Pt(X: 4, Y: 5)\" else 0); "
        "end Main; end C;"), 1);
}

// §8.5: an ineligible body record (explicit field) interpolates with the
// type-name default, not the property listing.
BEZEL_TEST(codegen_body_record, interpolation_ineligible_type_name_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record HasField "
        "  public property X: int; "
        "  private _cache: int := 0; "
        "  public constructor(x: int) X := x; end constructor; "
        "end HasField; "
        "class C public static method Main(): int "
        "  var a := new HasField(5); "
        "  return (1 if $\"{a}\" = \"HasField\" else 0); "
        "end Main; end C;"), 1);
}
