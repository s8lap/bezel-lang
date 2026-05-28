// Spec-derived tests for interpolated string literals — §18.5 / §18.7:
//
//   InterpString ::= '$"' (InterpPart | StringContent)* '"'
//                  | '$@"' (InterpPart | VerbatimContent)* '"'
//   InterpPart   ::= "{" Expression ("," "-"? IntLiteral)? (":" FormatSpec)? "}"
//
// Slice 51 lexes each `$"..."` (or `$@"..."`) as a single
// `InterpolatedString` / `InterpolatedVerbatimString` token and admits it
// as a primary expression. Splitting the lexeme into alternating fragments
// and `{ Expression }` parts is a follow-up slice; this slice's contract
// is "scan the whole interp literal correctly, balance-aware".

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

BEZEL_TEST(parser_interp, plain_no_parts) {
    BEZEL_EXPECT_EQ(render("$\"hello\""), std::string("$\"hello\""));
}

BEZEL_TEST(parser_interp, single_simple_part) {
    BEZEL_EXPECT_EQ(
        render("$\"Hello, {name}!\""),
        std::string("$\"Hello, {name}!\""));
}

BEZEL_TEST(parser_interp, multiple_parts) {
    BEZEL_EXPECT_EQ(
        render("$\"{a} + {b} = {a + b}\""),
        std::string("$\"{a} + {b} = {a + b}\""));
}

BEZEL_TEST(parser_interp, part_with_format_spec) {
    // `{value:F2}` — the `:F2` after the expression is a FormatSpec.
    BEZEL_EXPECT_EQ(
        render("$\"x = {value:F2}\""),
        std::string("$\"x = {value:F2}\""));
}

BEZEL_TEST(parser_interp, part_with_alignment_and_format) {
    BEZEL_EXPECT_EQ(
        render("$\"x = {value,-10:F2}\""),
        std::string("$\"x = {value,-10:F2}\""));
}

BEZEL_TEST(parser_interp, literal_brace_escape) {
    // `{{` and `}}` are literal-brace escapes in non-verbatim interp.
    BEZEL_EXPECT_EQ(
        render("$\"a {{ literal {x} }}\""),
        std::string("$\"a {{ literal {x} }}\""));
}

BEZEL_TEST(parser_interp, escaped_quote_inside) {
    BEZEL_EXPECT_EQ(
        render("$\"says \\\"hi\\\" to {who}\""),
        std::string("$\"says \\\"hi\\\" to {who}\""));
}

BEZEL_TEST(parser_interp, nested_quote_inside_part) {
    // A string literal inside the interpolation `{ ... }` doesn't terminate
    // the outer interp; the scanner skips nested strings as a unit. The
    // Bezel source here is: $"got {Lookup("x")} back" — the inner "x"
    // is a regular string literal inside the interpolation expression.
    BEZEL_EXPECT_EQ(
        render("$\"got {Lookup(\"x\")} back\""),
        std::string("$\"got {Lookup(\"x\")} back\""));
}

BEZEL_TEST(parser_interp, verbatim_interp_multiline) {
    BEZEL_EXPECT_EQ(
        render("$@\"line1\nline2 {x}\""),
        std::string("$@\"line1\nline2 {x}\""));
}

BEZEL_TEST(parser_interp, verbatim_doubled_quote) {
    BEZEL_EXPECT_EQ(
        render("$@\"says \"\"hi\"\" to {who}\""),
        std::string("$@\"says \"\"hi\"\" to {who}\""));
}

BEZEL_TEST(parser_interp, in_method_body) {
    auto r = parseAll(
        "module M; class C "
        "public method Greet(name: string): string => "
        "  $\"Hello, {name}!\"; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_interp, unterminated_diagnosed) {
    auto r = parseAll(
        "module M; class C "
        "public method M() _ := $\"unterminated; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
