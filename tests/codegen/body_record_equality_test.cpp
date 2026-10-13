// Codegen — §9.2 auto-equality for *body* records, and the §8.5 ToString rule.
//
// §9.2 "A body-only record receives auto-generated `=`, `<>`, `GetHashCode`,
// and `ToString` over its auto-properties (and only its auto-properties) when
// every member of the record is one of: an auto-property …; a method or
// expression-bodied property …; an overload of a non-equality operator; a
// constructor. Such a record is said to be auto-hashable." The
// participating components are the auto-properties in declaration order; the
// synthesized members reuse the positional-record machinery over that set.
//
// §9.2 "A body record is NOT auto-hashable … when it contains: an
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

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
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
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.2: differing auto-property values compare unequal.
BEZEL_TEST(codegen_body_record, equals_false_for_differing_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kPt +
        "class C public static method Main(): int "
        "  var a := new Pt(1, 2); var b := new Pt(1, 9); "
        "  return (if a.Equals(b) then 1 else 0); "
        "end Main; end C;"), 0);
}

// §8.3 via §9.2: equal body records hash equal (the hash-equality invariant,
// consistent with the synthesized structural Equals).
BEZEL_TEST(codegen_body_record, gethashcode_equal_for_equal_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kPt +
        "class C public static method Main(): int "
        "  var a := new Pt(1, 2); var b := new Pt(1, 2); "
        "  return (if a.GetHashCode() = b.GetHashCode() then 1 else 0); "
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
        "  return (if a.ToString() = \"Pt(X: 1, Y: 2)\" then 1 else 0); "
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
        "  return (if r.Equals(s) "
        "             and r.ToString() = \"Rectangle(Width: 3, Height: 4)\" "
        "             and r.Area() = 12 then 1 else 0); "
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
        "  return (if a.GetHashCode() = b.GetHashCode() "
        "             and a.ToString() = \"Person(Name: Ann, Age: 30)\" "
        "             then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.2: a non-equality operator overload does not disqualify a body record —
// it remains auto-hashable (Equals/ToString are synthesized).
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
        "  return (if a.Equals(b) and a.ToString() = \"V(X: 2, Y: 3)\" "
        "             then 1 else 0); "
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
        "  public method GetHashCode(): int => X; "
        "end Tagged; "
        "class C public static method Main(): int "
        "  var a := new Tagged(1); var b := new Tagged(9); "
        "  return (if a.Equals(b) then 1 else 0); "  // user Equals → true
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
        "  public method GetHashCode(): int => X; "
        "end Tagged; "
        "class C public static method Main(): int "
        "  var a := new Tagged(7); "
        // #2934 (§8.5, v0.252): was "Tagged"; a body record with a
        // manually-declared `Equals` is not auto-hashable (§9.2), so its
        // §8.5 default is the QUALIFIED type name.
        "  return (if a.ToString() = \"M.Tagged\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.2 / §8.5: a body record with an explicit field declaration (storage that
// is not an auto-property) is NOT auto-hashable — its ToString is the
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
        // #2934 (§8.5, v0.252): was "HasField" — same clause as above
        // (an explicit field declaration is §9.2's own disqualifier).
        "  return (if a.ToString() = \"M.HasField\" and a.X = 5 then 1 else 0); "
        "end Main; end C;"), 1);
}

// §8.5: an eligible body record interpolates with the synthesized property-
// listing ToString (`Console.WriteLine(person)` produces useful output).
BEZEL_TEST(codegen_body_record, interpolation_eligible_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kPt +
        "class C public static method Main(): int "
        "  var a := new Pt(4, 5); "
        "  return (if $\"{a}\" = \"Pt(X: 4, Y: 5)\" then 1 else 0); "
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
        // #2934 (§8.5, v0.252): was "HasField"; the §18.5 hole dispatches
        // the same default the explicit call above does.
        "  return (if $\"{a}\" = \"M.HasField\" then 1 else 0); "
        "end Main; end C;"), 1);
}

// ─── §9.2 v0.158: new admissible component kinds (value correctness) ─────────

// §9.2 v0.158 / §8.4: a `T?` (nullable-primitive) auto-property is an admissible
// component; the synthesized `=` uses the null-safe component comparison
// (both-present compares payloads; both-null equal; one-null unequal).
BEZEL_TEST(codegen_body_record, nullable_component_equality_spec_9_2_v0158) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R "
        "  public property V: int?; "
        "  public constructor(v: int?) V := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var a := new R(5); var b := new R(5); "
        "  var c := new R(9); "
        "  var n1 := new R(null); var n2 := new R(null); "
        "  if a = b and n1 = n2 and not (a = c) and not (a = n1) then "
        "    return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §9.2 v0.158 / §8.3: a non-owning inline-array (`[N]T`) component is admissible;
// the synthesized `=` compares element-by-element and equal records hash equal
// (the hash-equality invariant over the new inline-array hash fold).
BEZEL_TEST(codegen_body_record, inline_array_component_equality_spec_9_2_v0158) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R(A: [3]int); "
        "class C public static method Main(): int "
        "  var a := new R([1, 2, 3]); var b := new R([1, 2, 3]); "
        "  var c := new R([1, 2, 9]); "
        "  if a.Equals(b) and not a.Equals(c) "
        "     and a.GetHashCode() = b.GetHashCode() then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §9.2 v0.158 / §8.3 (fix #1382): the same inline-array equality machinery in a
// BODY record whose auto-property is a `[N]T`.  Ctor takes a collection literal.
// Prior to fix: buildOrderedArgs silently bailed on CollectionLiteralExpr for an
// InlineArray parameter, leaving the slot uninitialised (garbage values).
BEZEL_TEST(codegen_body_record, body_inline_array_autoprop_equality_spec_9_2_v0158_fix1382) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt "
        "  public property A: [3]int; "
        "  public constructor(a: [3]int) A := a; end constructor; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var p := new Pt([1, 2, 3]); var q := new Pt([1, 2, 3]); "
        "  var r := new Pt([1, 2, 9]); "
        "  if p.Equals(q) and not p.Equals(r) "
        "     and p.GetHashCode() = q.GetHashCode() then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §9.2 / §15.1 (fix #1382): body record ctor stores a collection literal into
// an inline-array auto-property; element reads give the correct round-trip value.
BEZEL_TEST(codegen_body_record, body_inline_array_autoprop_ctor_literal_fix1382) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt "
        "  public property P: [3]int; "
        "  public constructor(p: [3]int) P := p; end constructor; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var pt := new Pt([10, 20, 30]); "
        "  return pt.P[0] + pt.P[1] + pt.P[2]; "
        "end Main; end C;"), 60);
}

// §9.5 (fix #1382): a `with`-expression that replaces a body record's inline-array
// auto-property via a collection literal correctly stores the new elements.
// Prior to fix: lowerWith returned null for CollectionLiteralExpr and silently
// dropped the write, leaving the copied source value intact (wrong result).
BEZEL_TEST(codegen_body_record, body_inline_array_autoprop_with_literal_fix1382) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt "
        "  public property P: [3]int; "
        "  public constructor(p: [3]int) P := p; end constructor; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var pt := new Pt([10, 20, 30]); "
        "  var pt2 := pt with(P := [1, 2, 9]); "
        "  return pt2.P[0] + pt2.P[1] + pt2.P[2]; "
        "end Main; end C;"), 12);
}

// §9.5 (fix #1382): the same `with`-literal fix applies to a POSITIONAL record.
// Prior to fix: the with-path's lowerExpr returned null for CollectionLiteralExpr
// and silently dropped the write.
BEZEL_TEST(codegen_body_record, positional_inline_array_with_literal_fix1382) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R(A: [3]int); "
        "class C public static method Main(): int "
        "  var r := new R([10, 20, 30]); "
        "  var r2 := r with(A := [1, 2, 9]); "
        "  return r2.A[0] + r2.A[1] + r2.A[2]; "
        "end Main; end C;"), 12);
}

// §9.2 / §15.1 (fix #1382): body record with a nullable inline-array auto-property
// correctly stores elements from a collection literal via the ctor.
BEZEL_TEST(codegen_body_record, body_inline_array_nullable_autoprop_ctor_literal_fix1382) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt "
        "  public property P: [3]int?; "
        "  public constructor(p: [3]int?) P := p; end constructor; "
        "end Pt; "
        "class C public static method Main(): int "
        "  var pt := new Pt([10, 20, 30]); "
        "  var s := 0; "
        "  if pt.P[0] <> null then s := s + pt.P[0]!; end; "
        "  if pt.P[1] <> null then s := s + pt.P[1]!; end; "
        "  if pt.P[2] <> null then s := s + pt.P[2]!; end; "
        "  return s; "
        "end Main; end C;"), 60);
}

// §9.2 v0.158 / §8.5: an `override ToString` alone keeps auto-equality — `=`
// still compares structurally, while `ToString` dispatches to the override.
BEZEL_TEST(codegen_body_record, tostring_override_keeps_equality_spec_9_2_v0158) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record R "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  public override method ToString(): string => \"custom\"; "
        "end R; "
        "class C public static method Main(): int "
        "  var a := new R(7); var b := new R(7); var c := new R(8); "
        "  if a = b and not (a = c) and a.ToString() = \"custom\" then "
        "    return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}
