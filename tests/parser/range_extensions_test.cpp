// Spec-derived tests for the §A.7 RangeExpr extensions — open-ended forms
// and the optional `step` modifier:
//
//   RangeExpr ::= Shift
//               | Shift (".." | "..<") Shift ("step" Shift)?
//               | Shift ".."                /* low-only  */
//               | (".." | "..<") Shift      /* high-only */
//               | ".."                      /* fully open */
//
// Slice 52 replaces the earlier `BinaryExpr op=".."/"..<"` representation
// with a dedicated `RangeExpr` node carrying nullable `lo`, `hi`, and
// `step`. The renderer represents missing operands as `<>`; tests assert
// against that compact form.

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

std::string render(const std::string& expr) {
    const std::string src = "module M; class C public method M() _ := " +
                            expr + "; end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto* e = extract(r);
    return e ? exprToString(*e) : std::string("<no-expr>");
}

} // namespace

BEZEL_TEST(parser_range_ext, both_bounds_inclusive) {
    BEZEL_EXPECT_EQ(render("0..10"), std::string("(0 .. 10)"));
}

BEZEL_TEST(parser_range_ext, both_bounds_exclusive) {
    BEZEL_EXPECT_EQ(render("0..<10"), std::string("(0 ..< 10)"));
}

BEZEL_TEST(parser_range_ext, low_only) {
    BEZEL_EXPECT_EQ(render("10.."), std::string("(10 .. <>)"));
}

BEZEL_TEST(parser_range_ext, high_only_inclusive) {
    BEZEL_EXPECT_EQ(render("..10"), std::string("(<> .. 10)"));
}

BEZEL_TEST(parser_range_ext, high_only_exclusive) {
    BEZEL_EXPECT_EQ(render("..<10"), std::string("(<> ..< 10)"));
}

BEZEL_TEST(parser_range_ext, fully_open) {
    BEZEL_EXPECT_EQ(render(".."), std::string("(<> .. <>)"));
}

BEZEL_TEST(parser_range_ext, step_modifier) {
    BEZEL_EXPECT_EQ(
        render("0..10 step 2"),
        std::string("(0 .. 10 step 2)"));
}

BEZEL_TEST(parser_range_ext, exclusive_with_step) {
    BEZEL_EXPECT_EQ(
        render("0..<100 step 10"),
        std::string("(0 ..< 100 step 10)"));
}

BEZEL_TEST(parser_range_ext, low_only_with_step) {
    BEZEL_EXPECT_EQ(
        render("1.. step 2"),
        std::string("(1 .. <> step 2)"));
}

BEZEL_TEST(parser_range_ext, high_only_with_step) {
    BEZEL_EXPECT_EQ(
        render("..10 step 2"),
        std::string("(<> .. 10 step 2)"));
}

BEZEL_TEST(parser_range_ext, range_inside_call_arg) {
    BEZEL_EXPECT_EQ(
        render("Range(0..10)"),
        std::string("Range((0 .. 10))"));
}

BEZEL_TEST(parser_range_ext, range_inside_collection_literal) {
    BEZEL_EXPECT_EQ(
        render("[0..10, 20..30]"),
        std::string("[(0 .. 10), (20 .. 30)]"));
}

BEZEL_TEST(parser_range_ext, regression_basic_inclusive) {
    // Confirm the prior `a .. b` shape still renders unchanged after the
    // BinaryExpr → RangeExpr refactor (slice 52 callout).
    BEZEL_EXPECT_EQ(render("a..b"), std::string("(a .. b)"));
}
