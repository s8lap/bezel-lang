// Spec-derived parser tests for v0.167 (#1371) pattern alternatives spelled
// `|` — §19.3 / Appendix A.7 (AltPattern, PositionalPattern) / A.6
// (CatchPattern).
//
// Through v0.166 alternatives were comma-separated and disambiguated from the
// arm/slot separator by an `=>` lookahead. v0.167 gives alternatives their own
// separator `|`; the comma is now exclusively the arm/slot separator. These
// tests cover:
//   - `|` case-arm alternatives (constants, enum names, mixed, negatives),
//   - per-slot `|` alternatives inside a positional destructuring pattern,
//   - `catch` multi-type `|` alternatives with and without a binding,
//   - the old comma-alternative spelling now parses as two arms (a parse error
//     when the first has no body),
//   - `|` never collides with bitwise-or: relational operands and range bounds
//     parse at Shift level, below `|`.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::exprToString;
using bezel::test::parseAll;
using bezel::test::stmtSummary;

namespace {

const bezel::ast::Expr* extractExpr(const bezel::test::ParseResult& r) {
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

// Render a `case` expression to the parse_support debug form. The ConstantPattern
// alternatives print with `,` in this stringifier (arm separators print ` | `),
// so a `|`-source alternative list `1 | 2` round-trips to `1, 2` here.
std::string renderCase(const std::string& expr) {
    const std::string src = "module M; class C public method M() _ := " +
                            expr + "; end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto* e = extractExpr(r);
    return e ? exprToString(*e) : std::string("<no-expr>");
}

std::string renderBody(const std::string& body) {
    const std::string src =
        "module M; class C public method M() " + body + " end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return "<no-decl>";
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return "<no-member>";
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return "<no-stmt>";
    const auto* t = dynamic_cast<const bezel::ast::TryStmt*>(md->stmts[0].get());
    return t ? stmtSummary(*t) : std::string("<no-try>");
}

bool parses(const std::string& expr) {
    const std::string src = "module M; class C public method M() _ := " +
                            expr + "; end M; end C;";
    auto r = parseAll(src);
    return !r.diagnostics.hasErrors();
}

const bezel::ast::CaseExpr* firstCase(const bezel::test::ParseResult& r) {
    return dynamic_cast<const bezel::ast::CaseExpr*>(extractExpr(r));
}

} // namespace

// ---- §A.7 AltPattern: `|` case-arm alternatives ----------------------------

BEZEL_TEST(parser_alt_1371, two_int_alternatives_pipe) {
    // §19.3 example: `1 | 2 => "low"` matches 1 or 2.
    BEZEL_EXPECT_EQ(
        renderCase("case x of 1 | 2 => \"low\", 3 => \"high\" end"),
        std::string(
            "case x of { 1, 2 => \"low\" | 3 => \"high\" } end"));
}

BEZEL_TEST(parser_alt_1371, four_int_alternatives_pipe) {
    // §19.3 example: `2 | 3 | 4 | 5`.
    BEZEL_EXPECT_EQ(
        renderCase("case x of 2 | 3 | 4 | 5 => \"mid\", _ => \"other\" end"),
        std::string(
            "case x of { 2, 3, 4, 5 => \"mid\" | _ => \"other\" } end"));
}

BEZEL_TEST(parser_alt_1371, enum_name_alternatives_pipe) {
    BEZEL_EXPECT_EQ(
        renderCase(
            "case c of Color.Red | Color.Blue => \"cool\", _ => \"warm\" end"),
        std::string(
            "case c of { Color.Red, Color.Blue => \"cool\" "
            "| _ => \"warm\" } end"));
}

BEZEL_TEST(parser_alt_1371, mixed_literal_alternatives_pipe) {
    BEZEL_EXPECT_EQ(
        renderCase("case x of 1 | \"two\" | null => \"odd\", _ => \"n\" end"),
        std::string(
            "case x of { 1, \"two\", null => \"odd\" | _ => \"n\" } end"));
}

BEZEL_TEST(parser_alt_1371, negative_alternatives_pipe) {
    BEZEL_EXPECT_EQ(
        renderCase("case x of -1 | 0 | 1 => \"near\", _ => \"far\" end"),
        std::string(
            "case x of { (-1), 0, 1 => \"near\" | _ => \"far\" } end"));
}

BEZEL_TEST(parser_alt_1371, alternatives_with_when_guard_pipe) {
    BEZEL_EXPECT_EQ(
        renderCase("case x of 1 | 2 when x > 0 => \"pl\", _ => \"o\" end"),
        std::string(
            "case x of { 1, 2 when (x > 0) => \"pl\" | _ => \"o\" } end"));
}

// ---- §A.7 PositionalPattern: per-slot `|` alternatives ---------------------

BEZEL_TEST(parser_alt_1371, per_slot_alternatives_shape) {
    // §19.3: `TwoColor(Color.Red | Color.Blue, var x)` — the first slot is a
    // two-member alternatives ConstantPattern; the second is a binding. Verify
    // the AST directly (the flat debug render is ambiguous for a per-slot list).
    auto r = parseAll(
        "module M; class C public method M() "
        "_ := case p of TwoColor(Color.Red | Color.Blue, var x) => x, "
        "_ => 0 end; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* ce = firstCase(r);
    BEZEL_EXPECT_TRUE(ce != nullptr);
    if (!ce || ce->arms.empty()) return;
    const auto* dp = dynamic_cast<const bezel::ast::DestructuringPattern*>(
        ce->arms[0].pattern.get());
    BEZEL_EXPECT_TRUE(dp != nullptr);
    if (!dp) return;
    // Two positional slots: a `|` alternatives ConstantPattern, then a binding.
    BEZEL_EXPECT_EQ(dp->subPatterns.size(), static_cast<std::size_t>(2));
    if (dp->subPatterns.size() != 2) return;
    const auto* slot0 = dynamic_cast<const bezel::ast::ConstantPattern*>(
        dp->subPatterns[0].pattern.get());
    BEZEL_EXPECT_TRUE(slot0 != nullptr);
    if (slot0)
        BEZEL_EXPECT_EQ(slot0->constants.size(), static_cast<std::size_t>(2));
    // Second slot binds `x` (naked binding or `var x`).
    const bool slot1Binds =
        !dp->subPatterns[1].nakedBinding.empty() ||
        dynamic_cast<const bezel::ast::BindingPattern*>(
            dp->subPatterns[1].pattern.get()) != nullptr;
    BEZEL_EXPECT_TRUE(slot1Binds);
}

BEZEL_TEST(parser_alt_1371, positional_comma_is_still_slot_separator) {
    // A destructuring with two ordinary slots — the comma separates slots, and
    // no alternatives are formed.
    auto r = parseAll(
        "module M; class C public method M() "
        "_ := case p of Point(x, y) => x, _ => 0 end; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* ce = firstCase(r);
    if (!ce || ce->arms.empty()) return;
    const auto* dp = dynamic_cast<const bezel::ast::DestructuringPattern*>(
        ce->arms[0].pattern.get());
    BEZEL_EXPECT_TRUE(dp != nullptr);
    if (dp) BEZEL_EXPECT_EQ(dp->subPatterns.size(),
                            static_cast<std::size_t>(2));
}

// ---- §A.6 CatchPattern: `|` multi-type alternatives ------------------------

BEZEL_TEST(parser_alt_1371, catch_multi_type_no_binding_pipe) {
    BEZEL_EXPECT_EQ(
        renderBody(
            "try risky(); catch IOException | TimeoutException => log(); end;"),
        std::string(
            "try { risky(); } "
            "catch IOException | TimeoutException => log(); end;"));
}

BEZEL_TEST(parser_alt_1371, catch_multi_type_with_binding_pipe) {
    BEZEL_EXPECT_EQ(
        renderBody(
            "try risky(); catch var e: IOException | TimeoutException "
            "=> log(e); end;"),
        std::string(
            "try { risky(); } "
            "catch var e: IOException | TimeoutException => log(e); end;"));
}

// ---- old comma-alternative spelling is no longer an alternatives list ------

BEZEL_TEST(parser_alt_1371, old_comma_alternative_is_a_parse_error) {
    // `1, 2 => "low"` now reads as arm `1` (no body → error) then arm
    // `2 => "low"`. The comma is the arm separator; `1` lacks its `=> body`.
    BEZEL_EXPECT_FALSE(parses("case x of 1, 2 => \"low\", _ => \"o\" end"));
}

BEZEL_TEST(parser_alt_1371, old_catch_comma_list_no_longer_one_arm) {
    // `catch A, B => ...` now reads as arm `A` (no body → error) then `B`.
    auto r = parseAll(
        "module M; class C public method M() "
        "try risky(); catch IOException, TimeoutException => log(); end; "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- §19.3 no-collision note: `|` parses below Shift -----------------------

BEZEL_TEST(parser_alt_1371, pipe_after_constant_is_alternative_not_bitwise) {
    // `100 | 101` is two alternative constants, NOT the bitwise-or `100 | 101`.
    auto r = parseAll(
        "module M; class C public method M() "
        "_ := case x of 100 | 101 => \"a\", _ => \"b\" end; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* ce = firstCase(r);
    if (!ce || ce->arms.empty()) return;
    const auto* cp = dynamic_cast<const bezel::ast::ConstantPattern*>(
        ce->arms[0].pattern.get());
    BEZEL_EXPECT_TRUE(cp != nullptr);
    if (cp) BEZEL_EXPECT_EQ(cp->constants.size(), static_cast<std::size_t>(2));
}

BEZEL_TEST(parser_alt_1371, relational_operand_is_shift_level) {
    // `> a * 3` — the operand parses at Shift level (below `|`), so a trailing
    // arm separator is unaffected and the operand is the full `a * 3`.
    BEZEL_EXPECT_TRUE(parses(
        "case x of > a * 3 => \"hi\", _ => \"lo\" end"));
}

BEZEL_TEST(parser_alt_1371, leading_range_bound_is_shift_level) {
    // `..< a + 10` — the open-range upper bound parses at Shift level.
    BEZEL_EXPECT_TRUE(parses(
        "case x of ..< a + 10 => \"lo\", _ => \"hi\" end"));
}
