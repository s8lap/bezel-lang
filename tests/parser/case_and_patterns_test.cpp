// Spec-derived parser tests for case expressions and patterns — §A.7:
//
//   CaseExpr ::= "case" Expression "of" CaseArm ("," CaseArm)* ","? "end"
//   CaseArm  ::= Pattern ("when" Expression)? "=>" ArmBody
//   ArmBody  ::= Expression
//              | Statement                  /* valid only in stmt-pos case
//                                              or catch arm */
//              | "do" Statement* "end"
//
// v0.166 (§19.1, #1370): the `of` keyword delimits the scrutinee from the
// first arm. Because the boundary is now explicit, the scrutinee is a FULL
// Expression (no precedence restriction, no parenthesization rule); a missing
// `of` is a syntax error naming the delimiter. The new-behavior tests live at
// the end of this file.
//
//   Pattern  ::= "_"
//              | "null"
//              | "var" Identifier (":" Type)?
//              | Constant ("," Constant)*
//              | RangeExpr
//              | RelOpPattern Expression
//              | Type
//              | QualifiedName "(" PositionalPatternList ")"
//
// Slice 38 supports the scrutinee form (`case EXPR …`) with arms that have
// Expression bodies. Coverage of patterns: `_`, `null`, `var IDENT (: T)?`,
// single literal Constant, single QualifiedName (`NamedPattern` — sema
// resolves Constant-vs-Type), primitive-type TypePattern, with-lower-bound
// RangePattern (`a..b` / `a..<b`), and Destructuring.
//
// Deferred (own slice):
//   - Bare-case form (`case CaseArm* end` without a scrutinee).
//   - ConstantPattern alternatives (`1, 2, 3`).
//   - RelOpPattern (`< EXPR`) — fundamentally conflicts with greedy
//     scrutinee parsing without backtracking; needs the bare form to be
//     unambiguous.
//   - Open-ended RangePatterns (`..b`, `a..`, `..`).
//   - Statement-form / `do … end` ArmBody (stmt-position case).
//   - `case` as a statement-position construct.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::exprToString;
using bezel::test::parseAll;

namespace {

const bezel::ast::Expr* extract(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return nullptr;
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        md->stmts[0].get());
    if (!ds) return nullptr;
    return ds->value.get();
}

std::string renderCase(const std::string& expr) {
    const std::string src = "module M; class C public method M() _ := " +
                            expr + "; end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto* e = extract(r);
    return e ? exprToString(*e) : std::string("<no-expr>");
}

// v0.166 (§19.1): does parsing `expr` fire the missing-`of` diagnostic
// ("expected `of` after the case scrutinee")?
bool missingOfDiagnostic(const std::string& expr) {
    const std::string src = "module M; class C public method M() _ := " +
                            expr + "; end M; end C;";
    auto r = parseAll(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("expected `of` after the case scrutinee") !=
                std::string::npos)
            return true;
    return false;
}

} // namespace

BEZEL_TEST(parser_case, two_literal_arms) {
    BEZEL_EXPECT_EQ(
        renderCase("(case x of 1 => \"one\", 2 => \"two\")"),
        std::string("case x of { 1 => \"one\" | 2 => \"two\" } end"));
}

BEZEL_TEST(parser_case, discard_default_arm) {
    BEZEL_EXPECT_EQ(
        renderCase("(case x of 1 => \"one\", _ => \"other\")"),
        std::string("case x of { 1 => \"one\" | _ => \"other\" } end"));
}

BEZEL_TEST(parser_case, null_pattern) {
    BEZEL_EXPECT_EQ(
        renderCase("(case x of null => 0, var y => y)"),
        std::string("case x of { null => 0 | var y => y } end"));
}

BEZEL_TEST(parser_case, var_pattern_with_type) {
    BEZEL_EXPECT_EQ(
        renderCase("(case x of var n: int => n, _ => 0)"),
        std::string("case x of { var n: int => n | _ => 0 } end"));
}

BEZEL_TEST(parser_case, when_guard) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "(case x of var n: int when n > 0 => \"pos\", _ => \"non-pos\")"),
        std::string(
            "case x of { var n: int when (n > 0) => \"pos\" "
            "| _ => \"non-pos\" } end"));
}

BEZEL_TEST(parser_case, named_pattern_qualified) {
    // `Color.Red` parses as NamedPattern (sema resolves Constant-vs-Type-test
    // per §A.7).
    BEZEL_EXPECT_EQ(
        renderCase("(case x of Color.Red => 1, Color.Blue => 2)"),
        std::string("case x of { Color.Red => 1 | Color.Blue => 2 } end"));
}

BEZEL_TEST(parser_case, primitive_type_pattern) {
    BEZEL_EXPECT_EQ(
        renderCase("(case x of int => 1, string => 2, _ => 0)"),
        std::string(
            "case x of { int => 1 | string => 2 | _ => 0 } end"));
}

BEZEL_TEST(parser_case, range_pattern_with_lower_bound) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "(case x of 0..10 => \"small\", 10..<100 => \"mid\", _ => \"big\")"),
        std::string(
            "case x of { (0 .. 10) => \"small\" "
            "| (10 ..< 100) => \"mid\" "
            "| _ => \"big\" } end"));
}

BEZEL_TEST(parser_case, destructuring_pattern) {
    BEZEL_EXPECT_EQ(
        renderCase("(case x of Some(v) => v, None => 0)"),
        std::string(
            "case x of { Some(v) => v | None => 0 } end"));
}

BEZEL_TEST(parser_case, destructuring_with_nested_patterns) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "(case x of Pair(var a, var b) => a + b, _ => 0)"),
        std::string(
            "case x of { Pair(var a, var b) => (a + b) "
            "| _ => 0 } end"));
}

BEZEL_TEST(parser_case, destructuring_naked_binding) {
    // A naked binding (`v` not `var v`) is valid inside destructuring;
    // the spec admits the bare Identifier as a PositionalPattern shape.
    BEZEL_EXPECT_EQ(
        renderCase("(case x of Some(v) => v)"),
        std::string("case x of { Some(v) => v } end"));
}

BEZEL_TEST(parser_case, trailing_comma_allowed) {
    BEZEL_EXPECT_EQ(
        renderCase("(case x of 1 => \"a\", 2 => \"b\",)"),
        std::string("case x of { 1 => \"a\" | 2 => \"b\" } end"));
}

BEZEL_TEST(parser_case, case_inside_method_compiles) {
    // Regression check that a `case` expression survives the method body's
    // statement/expression dispatch (DiscardStmt assigning a CaseExpr).
    auto r = parseAll(
        "module M; class C "
        "public method Pick(): string "
        "  return (case 1 of 1 => \"a\", _ => \"b\"); "
        "end Pick; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// v0.166 (§19.1 / A.7 CaseExpr, #1370): the `of` delimiter.
// ---------------------------------------------------------------------------

// §19.1: a `case` scrutinee not followed by `of` is a syntax error at the
// first arm token; the diagnostic names the delimiter.
BEZEL_TEST(parser_case_of, missing_of_is_a_syntax_error_spec_19_1) {
    BEZEL_EXPECT_TRUE(
        missingOfDiagnostic("(case x 1 => \"one\", _ => \"other\")"));
    // A bare scrutinee with no `of` and no arm token still names the delimiter.
    BEZEL_EXPECT_TRUE(missingOfDiagnostic("(case x)"));
}

// §19.1: with `of` present, the same construct parses cleanly.
BEZEL_TEST(parser_case_of, of_present_parses_cleanly_spec_19_1) {
    BEZEL_EXPECT_EQ(
        renderCase("(case x of 1 => \"one\", _ => \"other\")"),
        std::string("case x of { 1 => \"one\" | _ => \"other\" } end"));
}

// §19.1: the scrutinee is a FULL Expression — an additive expression needs no
// parentheses now that `of` marks the boundary (`case x - 1 of`).
BEZEL_TEST(parser_case_of, additive_scrutinee_unparenthesized_spec_19_1) {
    BEZEL_EXPECT_EQ(
        renderCase("(case x - 1 of 0..3 => \"lo\", _ => \"hi\")"),
        std::string(
            "case (x - 1) of { (0 .. 3) => \"lo\" | _ => \"hi\" } end"));
}

// §19.1: a bitwise scrutinee is admitted unparenthesized (`case flags & mask
// of`) — the old multiplicative-level restriction is gone.
BEZEL_TEST(parser_case_of, bitwise_scrutinee_unparenthesized_spec_19_1) {
    BEZEL_EXPECT_EQ(
        renderCase("(case flags & mask of 0 => \"none\", _ => \"some\")"),
        std::string(
            "case (flags & mask) of { 0 => \"none\" | _ => \"some\" } end"));
}

// §19.1: a range scrutinee is admitted unparenthesized (`case lo..hi of`), and
// a first pattern may still begin with `..` / `..<` with no ambiguity.
BEZEL_TEST(parser_case_of, range_scrutinee_unparenthesized_spec_19_1) {
    BEZEL_EXPECT_EQ(
        renderCase("(case lo..hi of _ => \"r\")"),
        std::string("case (lo .. hi) of { _ => \"r\" } end"));
}

// §19.1: the canonical spec example — a first pattern that is a leading
// negative range (`-10..-6`) is recognized as a pattern, never absorbed into
// the scrutinee, because `of` marks the boundary.
BEZEL_TEST(parser_case_of, leading_negative_range_first_pattern_spec_19_1) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "(case delta of -10..-6 => \"strongly down\", ..<0 => \"down\", "
            "0 => \"flat\", _ => \"up\")"),
        std::string(
            "case delta of { ((-10) .. (-6)) => \"strongly down\" "
            "| (<> ..< 0) => \"down\" | 0 => \"flat\" | _ => \"up\" } end"));
}

// §19.1: a trailing comma before `end` is still accepted with `of` present.
BEZEL_TEST(parser_case_of, trailing_comma_still_allowed_spec_19_1) {
    BEZEL_EXPECT_EQ(
        renderCase("(case x of 1 => \"a\", 2 => \"b\",)"),
        std::string("case x of { 1 => \"a\" | 2 => \"b\" } end"));
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired. This test previously verified that the `when` expression
// form is unaffected by `case`'s `of` requirement — now it verifies that `when`
// is correctly rejected.
BEZEL_TEST(parser_case_of, when_construct_unaffected_by_of_spec_19_1) {
    auto r = parseAll(
        "module M; class C public method M(score: int): string "
        "  return (when "
        "    score >= 90 => \"A\", "
        "    score >= 80 => \"B\", "
        "    _ => \"F\"); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §B (contextual): `of` is not a reserved word, so it still works as an
// ordinary identifier away from the case-scrutinee-delimiter position —
// including as a `case` scrutinee itself (`case of of ...`).
BEZEL_TEST(parser_case_of, of_still_usable_as_identifier_spec_B) {
    auto r = parseAll(
        "module M; class C public method M(): int "
        "  var of := 42; "
        "  return (case of of 42 => 1, _ => 0); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
