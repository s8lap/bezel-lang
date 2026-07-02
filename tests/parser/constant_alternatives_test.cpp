// Spec-derived parser tests for ConstantPattern alternatives — §A.7
// `Pattern ::= Constant ("," Constant)*` line. Alternatives are admitted
// only for Constant patterns; `Type` and other Pattern forms don't support
// the `,` extension (the grammar's `,` after them belongs to the enclosing
// CaseArm separator).
//
// Slice 38 introduced single-Constant patterns. Slice 44 adds the
// alternatives form, covering:
//   - literal alternatives:           `1, 2, 3`
//   - QualifiedName alternatives:     `Color.Red, Color.Blue`
//   - mixed kinds:                    `1, "two", null`
//   - alternatives interaction with arm separator (no false positives)

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

BEZEL_TEST(parser_const_alt, two_int_alternatives) {
    BEZEL_EXPECT_EQ(
        renderCase("case x 1, 2 => \"low\", _ => \"high\" end"),
        std::string(
            "case x of { 1, 2 => \"low\" | _ => \"high\" } end"));
}

BEZEL_TEST(parser_const_alt, three_int_alternatives) {
    BEZEL_EXPECT_EQ(
        renderCase("case x 1, 2, 3 => \"low\", _ => \"high\" end"),
        std::string(
            "case x of { 1, 2, 3 => \"low\" | _ => \"high\" } end"));
}

BEZEL_TEST(parser_const_alt, qualified_name_alternatives) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "case c Color.Red, Color.Green => \"warm\", "
            "Color.Blue => \"cool\" end"),
        std::string(
            "case c of { Color.Red, Color.Green => \"warm\" "
            "| Color.Blue => \"cool\" } end"));
}

BEZEL_TEST(parser_const_alt, mixed_literal_alternatives) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "case x 1, \"two\", null => \"odd\", _ => \"normal\" end"),
        std::string(
            "case x of { 1, \"two\", null => \"odd\" "
            "| _ => \"normal\" } end"));
}

BEZEL_TEST(parser_const_alt, boolean_alternatives) {
    BEZEL_EXPECT_EQ(
        renderCase("case b true, false => \"either\" end"),
        std::string(
            "case b of { true, false => \"either\" } end"));
}

BEZEL_TEST(parser_const_alt, single_literal_no_extra_comma) {
    // Regression: a single-constant pattern followed by `,` should NOT
    // consume the `,` as an alternative when what follows is not a
    // Constant (e.g. `_`, `var`, `null` is also not interpreted unless
    // following a Constant `,` — but `_` IS not a constant so it stops).
    BEZEL_EXPECT_EQ(
        renderCase("case x 1 => \"one\", _ => \"other\" end"),
        std::string(
            "case x of { 1 => \"one\" | _ => \"other\" } end"));
}

BEZEL_TEST(parser_const_alt, alternatives_with_when_guard) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "case x 1, 2 when x > 0 => \"pos-low\", _ => \"other\" end"),
        std::string(
            "case x of { 1, 2 when (x > 0) => \"pos-low\" "
            "| _ => \"other\" } end"));
}

// §A.8 / #1341 (v0.158): Constant admits "-"? (IntLiteral | FloatLiteral),
// so negative numeric literals must be accepted in ConstantPattern
// alternatives lists.

BEZEL_TEST(parser_const_alt, single_negative_int) {
    // A single negative constant as the whole arm pattern.
    BEZEL_EXPECT_EQ(
        renderCase("case x -1 => \"neg\", _ => \"other\" end"),
        std::string(
            "case x of { (-1) => \"neg\" | _ => \"other\" } end"));
}

BEZEL_TEST(parser_const_alt, two_negative_int_alternatives) {
    // #1341: `-2, -3` must parse as two ConstantPattern alternatives.
    BEZEL_EXPECT_EQ(
        renderCase("case x -2, -3 => \"small neg\", _ => \"other\" end"),
        std::string(
            "case x of { (-2), (-3) => \"small neg\" "
            "| _ => \"other\" } end"));
}

BEZEL_TEST(parser_const_alt, mixed_positive_and_negative_alternatives) {
    // A mix of positive and negative constants in one alternatives list.
    BEZEL_EXPECT_EQ(
        renderCase(
            "case x -1, 0, 1 => \"near zero\", _ => \"other\" end"),
        std::string(
            "case x of { (-1), 0, 1 => \"near zero\" "
            "| _ => \"other\" } end"));
}

BEZEL_TEST(parser_const_alt, negative_first_arm_then_positive_arm) {
    // Each arm is independent; verify the arm boundary is correct.
    BEZEL_EXPECT_EQ(
        renderCase(
            "case x -1 => \"minus one\", 1 => \"one\", _ => \"other\" end"),
        std::string(
            "case x of { (-1) => \"minus one\" | 1 => \"one\" "
            "| _ => \"other\" } end"));
}
