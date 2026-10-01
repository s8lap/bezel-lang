// Spec-derived tests for §19.3 / Appendix C.1 (v0.313, #5188 residual): a
// concrete value-typed `case` scrutinee (a primitive, record, enum, choice,
// inline array or the unit type — by declared type, or its nullable form)
// against a type pattern, bare or binding, naming a value type OTHER than its
// own or its own nullable form, not a type parameter, is a compile error: the
// test could never succeed (§12.7, §8.2). Where a type parameter stands on
// either side the arm is decided per instantiation and admitted. A choice's
// case names and destructures, constants and literal patterns are not type
// patterns (§35.5, §19.3) and stay admitted. The is/as sibling is #5175
// (§12.7); both share one "different value type" decision.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasOtherValueTypeError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("naming a different value type") !=
                std::string::npos &&
            d.message.rfind("internal:", 0) != 0)
            return true;
    }
    return false;
}

constexpr const char* kDecls =
    "module M;\n"
    "record Pt(x: int, y: int);\n"
    "record Q(x: int, y: int);\n"
    "enum Color Red, Green; end Color;\n"
    "choice Outcome Fetched(body: int), NotModified; end Outcome;\n";

std::string prog(const std::string& body) {
    return std::string(kDecls) + "class Program\n" + body + "end Program;\n";
}

std::string mainBody(const std::string& stmts) {
    return "  public static method Main(): int\n" + stmts +
           "  end Main;\n";
}

}  // namespace

// ---- REJECT rows ----------------------------------------------------------

// §19.3 / C.1: `n: int` against `long` (primitive keyword -> TypePattern).
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, int_vs_long_bare_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var n: int := 5;\n"
        "    return (case n of long => 1, _ => 9);\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, int_vs_long_binding_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var n: int := 5;\n"
        "    return (case n of var x: long => 1, _ => 9);\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

// The issue's headline: `case 5 of Color` (an enum type name, not a constant).
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, int_vs_enum_bare_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var n: int := 5;\n"
        "    return (case n of Color => 1, _ => 9);\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, int_vs_enum_binding_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var n: int := 5;\n"
        "    return (case n of var c: Color => 1, _ => 9);\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, record_vs_other_record_bare_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var p := new Pt(1, 2);\n"
        "    return (case p of Q => 1, _ => 9);\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, record_vs_other_record_binding_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var p := new Pt(1, 2);\n"
        "    return (case p of var q: Q => 1, _ => 9);\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, inline_array_length_mismatch_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var a: [3]int := [1, 2, 3];\n"
        "    return (case a of var b: [4]int => 1, _ => 9);\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, nullable_int_vs_long_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var n: int? := 5;\n"
        "    return (case n of long => 1, _ => 9);\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

// The pattern's own nullable form of a DIFFERENT type is still a different type.
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, int_vs_nullable_long_binding_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var n: int := 5;\n"
        "    return (case n of var x: long? => 1, _ => 9);\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

// CaseStmt form (the statement site is a separate code path).
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, stmt_form_int_vs_long_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var n: int := 5;\n"
        "    case n of\n"
        "      long => return 1;\n"
        "      _ => return 9;\n"
        "    end;\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, stmt_form_record_vs_other_record_rejected) {
    auto r = parseSemaAll(prog(mainBody(
        "    var p := new Pt(1, 2);\n"
        "    case p of\n"
        "      Q => return 1;\n"
        "      _ => return 9;\n"
        "    end;\n")));
    BEZEL_EXPECT_TRUE(hasOtherValueTypeError(r));
}

// ---- ADMIT rows -----------------------------------------------------------

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, own_type_admitted) {
    auto r = parseSemaAll(prog(mainBody(
        "    var n: int := 5;\n"
        "    var p := new Pt(1, 2);\n"
        "    return (case n of int => 1, _ => 9) + "
        "(case p of Pt => 1, _ => 9);\n")));
    BEZEL_EXPECT_FALSE(hasOtherValueTypeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(case_value_scrutinee_other_value_type_5188, own_nullable_admitted) {
    auto r = parseSemaAll(prog(mainBody(
        "    var n: int? := 5;\n"
        "    var m: int := 5;\n"
        "    return (case n of var x: int => x, _ => 9) + "
        "(case m of var y: int? => 1, _ => 9);\n")));
    BEZEL_EXPECT_FALSE(hasOtherValueTypeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §35.5: choice case names and destructures are not type patterns.
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, choice_case_patterns_admitted) {
    auto r = parseSemaAll(prog(mainBody(
        "    var c := Outcome.Fetched(7);\n"
        "    return (case c of Fetched(var b) => b, NotModified => 0);\n")));
    BEZEL_EXPECT_FALSE(hasOtherValueTypeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.3/§19.8: an enum constant pattern (bare and qualified) is a constant.
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, enum_constant_patterns_admitted) {
    auto r = parseSemaAll(prog(mainBody(
        "    var c := Color.Red;\n"
        "    return (case c of Red => 1, Color.Green => 2);\n")));
    BEZEL_EXPECT_FALSE(hasOtherValueTypeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.3: a literal pattern is not a type pattern.
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, literal_pattern_admitted) {
    auto r = parseSemaAll(prog(mainBody(
        "    var n: int := 5;\n"
        "    return (case n of 5 => 1, _ => 9);\n")));
    BEZEL_EXPECT_FALSE(hasOtherValueTypeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.3 / C.1: a type parameter on the scrutinee side is decided per
// instantiation; the arm is admitted.
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, type_param_scrutinee_vs_long_admitted) {
    auto r = parseSemaAll(prog(
        "  public static method Pick<T>(v: T): int\n"
        "    return (case v of long => 1, _ => 9);\n"
        "  end Pick;\n"
        "  public static method Main(): int\n"
        "    return Program.Pick<int>(5);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_FALSE(hasOtherValueTypeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A type parameter on the pattern side likewise.
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, type_param_pattern_over_int_admitted) {
    auto r = parseSemaAll(prog(
        "  public static method Pick<T>(p: int): int\n"
        "    return (case p of T => 1, _ => 9);\n"
        "  end Pick;\n"
        "  public static method Main(): int\n"
        "    return Program.Pick<long>(5);\n"
        "  end Main;\n"));
    BEZEL_EXPECT_FALSE(hasOtherValueTypeError(r));
}

// §12.7 "declared type": an Object carrier type-narrowed to int still has the
// reference declared type, so a later `long` pattern is not a value-vs-value
// reject.
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, narrowed_object_carrier_admitted) {
    auto r = parseSemaAll(prog(mainBody(
        "    var o: Object := 5;\n"
        "    if o is int then\n"
        "      return (case o of long => 1, _ => 9);\n"
        "    end;\n"
        "    return 0;\n")));
    BEZEL_EXPECT_FALSE(hasOtherValueTypeError(r));
}

// Reference scrutinee: out of scope for this rule.
BEZEL_TEST(case_value_scrutinee_other_value_type_5188, reference_scrutinee_admitted) {
    auto r = parseSemaAll(prog(mainBody(
        "    var s: string := \"a\";\n"
        "    return (case s of string => 1, _ => 9);\n")));
    BEZEL_EXPECT_FALSE(hasOtherValueTypeError(r));
}
