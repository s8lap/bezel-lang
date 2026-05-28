// Spec-derived parser tests for case expressions and patterns — §A.7:
//
//   CaseExpr ::= "case" Expression? CaseArm ("," CaseArm)* ","? "end"
//   CaseArm  ::= Pattern ("when" Expression)? "=>" ArmBody
//   ArmBody  ::= Expression
//              | Statement                  /* valid only in stmt-pos case
//                                              or catch arm */
//              | "do" Statement* "end"
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

} // namespace

BEZEL_TEST(parser_case, two_literal_arms) {
    BEZEL_EXPECT_EQ(
        renderCase("case x 1 => \"one\", 2 => \"two\" end"),
        std::string("case x of { 1 => \"one\" | 2 => \"two\" } end"));
}

BEZEL_TEST(parser_case, discard_default_arm) {
    BEZEL_EXPECT_EQ(
        renderCase("case x 1 => \"one\", _ => \"other\" end"),
        std::string("case x of { 1 => \"one\" | _ => \"other\" } end"));
}

BEZEL_TEST(parser_case, null_pattern) {
    BEZEL_EXPECT_EQ(
        renderCase("case x null => 0, var y => y end"),
        std::string("case x of { null => 0 | var y => y } end"));
}

BEZEL_TEST(parser_case, var_pattern_with_type) {
    BEZEL_EXPECT_EQ(
        renderCase("case x var n: int => n, _ => 0 end"),
        std::string("case x of { var n: int => n | _ => 0 } end"));
}

BEZEL_TEST(parser_case, when_guard) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "case x var n: int when n > 0 => \"pos\", _ => \"non-pos\" end"),
        std::string(
            "case x of { var n: int when (n > 0) => \"pos\" "
            "| _ => \"non-pos\" } end"));
}

BEZEL_TEST(parser_case, named_pattern_qualified) {
    // `Color.Red` parses as NamedPattern (sema resolves Constant-vs-Type-test
    // per §A.7).
    BEZEL_EXPECT_EQ(
        renderCase("case x Color.Red => 1, Color.Blue => 2 end"),
        std::string("case x of { Color.Red => 1 | Color.Blue => 2 } end"));
}

BEZEL_TEST(parser_case, primitive_type_pattern) {
    BEZEL_EXPECT_EQ(
        renderCase("case x int => 1, string => 2, _ => 0 end"),
        std::string(
            "case x of { int => 1 | string => 2 | _ => 0 } end"));
}

BEZEL_TEST(parser_case, range_pattern_with_lower_bound) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "case x 0..10 => \"small\", 10..<100 => \"mid\", _ => \"big\" end"),
        std::string(
            "case x of { (0 .. 10) => \"small\" "
            "| (10 ..< 100) => \"mid\" "
            "| _ => \"big\" } end"));
}

BEZEL_TEST(parser_case, destructuring_pattern) {
    BEZEL_EXPECT_EQ(
        renderCase("case x Some(v) => v, None => 0 end"),
        std::string(
            "case x of { Some(v) => v | None => 0 } end"));
}

BEZEL_TEST(parser_case, destructuring_with_nested_patterns) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "case x Pair(var a, var b) => a + b, _ => 0 end"),
        std::string(
            "case x of { Pair(var a, var b) => (a + b) "
            "| _ => 0 } end"));
}

BEZEL_TEST(parser_case, destructuring_naked_binding) {
    // A naked binding (`v` not `var v`) is valid inside destructuring;
    // the spec admits the bare Identifier as a PositionalPattern shape.
    BEZEL_EXPECT_EQ(
        renderCase("case x Some(v) => v end"),
        std::string("case x of { Some(v) => v } end"));
}

BEZEL_TEST(parser_case, trailing_comma_allowed) {
    BEZEL_EXPECT_EQ(
        renderCase("case x 1 => \"a\", 2 => \"b\", end"),
        std::string("case x of { 1 => \"a\" | 2 => \"b\" } end"));
}

BEZEL_TEST(parser_case, case_inside_method_compiles) {
    // Regression check that a `case` expression survives the method body's
    // statement/expression dispatch (DiscardStmt assigning a CaseExpr).
    auto r = parseAll(
        "module M; class C "
        "public method Pick(): string "
        "  return case 1 1 => \"a\", _ => \"b\" end; "
        "end Pick; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
