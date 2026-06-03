// Codegen — a NULLABLE ENUM (`enum?`) has a real null representation (§7, §10;
// issue #364).
//
// A non-nullable enum is a value type backed by an integer (§10) and lowers to
// its underlying integer (i32). Before this fix `enum?` ALSO stayed i32, so the
// `null` literal was indistinguishable from the enum member whose value is 0 —
// a representation/correctness gap. A nullable enum now lowers, like the other
// nullable value types (int?, bool?, …, §18.9), to the tagged struct
// {i8 hasValue; i32 value}, so `null` is representable and distinct from every
// enum value. These tests exercise the null literal, the null-safe `=`/`<>`
// comparisons of §8.4 (against `null` and against an enum value), the
// null-forgiving `!` of §17.12, the `??` coalesce of §17.1, and the §8.3/§8.5
// value-semantics (equality / ToString) of a record field of this type.
//
// `Color` is deliberately declared with `Red := 0` so the core bug — a null
// `enum?` colliding with the value-0 member — is directly observable: a null
// `Color?` must be `<> Color.Red`, not `=`.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& enumDecl, const std::string& body) {
    auto r = runJit(
        "module M; " + enumDecl +
        " class C public static method Main(): int " + body +
        " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Red is value 0 on purpose: this is the member a naive i32 null would alias.
const char* kColor = "enum Color Red := 0, Green := 1, Blue := 2; end Color;";

}  // namespace

// §7 / §10: a `null`-initialized `enum?` compares equal to `null`.
BEZEL_TEST(codegen_nullable_enum, null_literal_equals_null_spec_7_10) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := null; return int(c = null);"), 1);
}

// §8.4: a value-initialized `enum?` is NOT null — `= null` is false.
BEZEL_TEST(codegen_nullable_enum, value_not_equal_null_spec_8_4) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := Color.Green; return int(c = null);"), 0);
}

// §8.4: `<> null` is true for a value-initialized `enum?`.
BEZEL_TEST(codegen_nullable_enum, value_neq_null_spec_8_4) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := Color.Green; return int(c <> null);"), 1);
}

// §8.4: a value-initialized `enum?` compares equal to that enum value (the
// null-safe `=` reduces to the inner enum's value comparison when present).
BEZEL_TEST(codegen_nullable_enum, value_equals_enum_value_spec_8_4) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := Color.Green; return int(c = Color.Green);"), 1);
}

// §8.4: a value-initialized `enum?` is NOT equal to a different enum value.
BEZEL_TEST(codegen_nullable_enum, value_neq_other_enum_value_spec_8_4) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := Color.Green; return int(c = Color.Blue);"), 0);
}

// CORE BUG (issue #364): Red has value 0, but a null `enum?` is distinct from
// it — `(c: Color? := null) <> Color.Red` is TRUE. A null aliasing the value-0
// member (the pre-fix behavior) would make this 0.
BEZEL_TEST(codegen_nullable_enum, null_distinct_from_value_zero_member_364) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := null; return int(c <> Color.Red);"), 1);
}

// CORE BUG (issue #364): correspondingly, a null `enum?` does NOT equal the
// value-0 member.
BEZEL_TEST(codegen_nullable_enum, null_not_equal_value_zero_member_364) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := null; return int(c = Color.Red);"), 0);
}

// §17.12: postfix `!` on a non-null `enum?` yields the inner enum value (here
// Green := 1, so `int(c!)` is 1).
BEZEL_TEST(codegen_nullable_enum, bang_deref_non_null_spec_17_12) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := Color.Green; return int(c!);"), 1);
}

// §17.1: `??` on a null `enum?` yields the right operand (Red := 0).
BEZEL_TEST(codegen_nullable_enum, coalesce_null_yields_rhs_spec_17_1) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := null; return int(c ?? Color.Red);"), 0);
}

// §17.1: `??` on a non-null `enum?` yields the inner value (Blue := 2), not the
// right operand.
BEZEL_TEST(codegen_nullable_enum, coalesce_value_yields_lhs_spec_17_1) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := Color.Blue; return int(c ?? Color.Red);"), 2);
}

// §8.3 / §9.1: a record with a nullable-enum field — two records with null
// fields are equal (the null-safe `=` of §8.4: null = null is true).
BEZEL_TEST(codegen_nullable_enum, record_field_both_null_equal_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        std::string(kColor) + " record Tag(C: Color?);",
        "var a := new Tag(null); var b := new Tag(null); "
        "return int(a = b);"), 1);
}

// §8.3 / §8.4: a record null-field vs value-field is NOT equal (Red is value 0,
// so this also exercises the core null-vs-value-0 distinction inside a record).
BEZEL_TEST(codegen_nullable_enum, record_field_null_vs_value_neq_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        std::string(kColor) + " record Tag(C: Color?);",
        "var a := new Tag(null); var b := new Tag(Color.Red); "
        "return int(a = b);"), 0);
}

// §8.3 / §9.1: records with equal non-null nullable-enum fields are equal.
BEZEL_TEST(codegen_nullable_enum, record_field_same_value_equal_spec_8_3) {
    BEZEL_EXPECT_EQ(run(
        std::string(kColor) + " record Tag(C: Color?);",
        "var a := new Tag(Color.Green); var b := new Tag(Color.Green); "
        "return int(a = b);"), 1);
}

// §8.5: a null nullable-enum field renders as "null" in the synthesized record
// ToString (value-semantics for the enum case, mirroring #363).
BEZEL_TEST(codegen_nullable_enum, record_field_null_tostring_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        std::string(kColor) + " record Tag(C: Color?);",
        "var a := new Tag(null); var s := $\"{a}\"; "
        "return int(s.Contains(\"null\"));"), 1);
}

// §8.5: a non-null nullable-enum field renders as the enum TYPE NAME (the
// enum default ToString), not its underlying integer, in the record ToString.
BEZEL_TEST(codegen_nullable_enum, record_field_value_tostring_spec_8_5) {
    BEZEL_EXPECT_EQ(run(
        std::string(kColor) + " record Tag(C: Color?);",
        "var a := new Tag(Color.Green); var s := $\"{a}\"; "
        "return int(s.Contains(\"Color\"));"), 1);
}

// §8.5: a standalone nullable-enum interpolates as "null" when null.
BEZEL_TEST(codegen_nullable_enum, interpolate_null_spec_8_5) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := null; var s := $\"{c}\"; "
        "return int(s = \"null\");"), 1);
}

// §8.5: a standalone nullable-enum interpolates as the type name when present.
BEZEL_TEST(codegen_nullable_enum, interpolate_value_spec_8_5) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := Color.Green; var s := $\"{c}\"; "
        "return int(s = \"Color\");"), 1);
}

// §7: reassigning a null `enum?` to a value (and back) tracks the tag through
// the assignment path (the local-slot wrap), so the null state is observable.
BEZEL_TEST(codegen_nullable_enum, reassign_tracks_null_state_spec_7) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c: Color? := null; c := Color.Blue; "
        "var x := int(c = Color.Blue); c := null; "
        "return x + int(c = null);"), 2);
}
