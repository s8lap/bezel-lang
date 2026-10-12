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

// §18.5 (v0.334), issue #2250. The hole-delimitation rule: the lexer "skips
// nested string/char literals (recognized by their own opening quote and
// parsed as complete literals; they do not terminate the outer string)".
// The char literal below holds a LITERAL `}` — the character itself, not the
// escape `\x7D` — so without the fix that `}` closes the hole early, the rest
// of the literal becomes stray text, and the string never terminates. This is
// the discriminating row: it fails on the pre-fix lexer AND on the
// intermediate fix whose lookahead stopped at `}` (which cannot, by
// construction, ever see the very `}` it needs to protect).
BEZEL_TEST(parser_interp, nested_char_literal_with_closing_brace) {
    auto r = parseAll(
        R"(module M; class C public method M() _ := $"{'}'}"; end M; end C;)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same shape, but asserted on the split RESULT rather than on the
// absence of diagnostics: the char literal must survive into the hole's
// expression text intact, so the row cannot be satisfied by merely
// swallowing the offending characters.
BEZEL_TEST(parser_interp, nested_char_literal_survives_split) {
    BEZEL_EXPECT_EQ(render("$\"{'}'}\""), std::string("$\"{'}'}\""));
}

// §18.5 (v0.334), issue #2250: the issue's own repro, verbatim. `HH'` is a
// brace-free format spec that happens to END in an apostrophe. The format
// spec runs from the first top-level `:` to the hole's closing `}`, so the
// `'` is ordinary format content and the `}` closes the hole — exactly as
// before the fix. Note the NO space before the `'`, which is the form the
// issue reports; a lookahead that scans for a closing `'` PAST the hole's
// `}` swallows this string whole (the original #2250 defect, reverted in
// 9ee923ab).
BEZEL_TEST(parser_interp, format_spec_trailing_apostrophe) {
    auto r = parseAll(
        R"(module M; class C public method M() _ := $"{d:HH'}"; end M; end C;)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §18.5 (v0.334), issue #2250 — the spaced spelling of the same format spec.
// Pins the other direction of the "is this a complete char literal?" test:
// here the `'` is followed by a space, so `'` … `}` is not `'` CharContent
// `'`, and the apostrophe stays format content.
BEZEL_TEST(parser_interp, format_spec_trailing_apostrophe_spaced) {
    auto r = parseAll(
        R"(module M; class C public method M() _ := $"{d:HH' }"; end M; end C;)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §18.5 (v0.334), issue #2250 — DIFFERENTIAL CONTROL. A format spec
// carrying a MATCHED pair of apostrophes (`HH'mm'`) is still brace-free
// content running to the hole's `}`. A "skip to the next `'`" heuristic
// applied at the hole's top nesting level regardless of the format-spec
// boundary would cut this hole short and swallow the rest of the string.
BEZEL_TEST(parser_interp, format_spec_matched_apostrophe_pair) {
    auto r = parseAll(
        R"(module M; class C public method M() _ := $"{d:HH'mm'}"; end M; end C;)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §18.5 (v0.334), issue #2250 — DIFFERENTIAL CONTROL. A hole with no
// apostrophe anywhere must be untouched by the nested-literal skip, braces
// and all: the round-trip assertion pins that the expression is still split
// and re-rendered unchanged.
BEZEL_TEST(parser_interp, hole_without_apostrophe) {
    BEZEL_EXPECT_EQ(render("$\"{x + 1}\""), std::string("$\"{x + 1}\""));
}

// §18.5 (v0.334) / §18.7, issue #2250 — the verbatim spelling of the primary
// row. `@` suppresses *string* escapes, not §18.3's CharContent grammar, so
// a char literal in a `$@"…"` hole obeys the same complete-literal rule.
BEZEL_TEST(parser_interp, verbatim_nested_char_literal_with_closing_brace) {
    auto r = parseAll(
        R"(module M; class C public method M() _ := $@"{'}'}"; end M; end C;)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §18.5 (v0.334) / §18.7, issue #2250 — the verbatim differential control:
// the format spec's trailing apostrophe still ends at the hole's `}`.
BEZEL_TEST(parser_interp, verbatim_format_spec_trailing_apostrophe) {
    auto r = parseAll(
        R"(module M; class C public method M() _ := $@"{d:HH'}"; end M; end C;)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §18.5 (v0.334), issue #2250 — escape control. A char literal whose `}` is
// written as the escape `\x7D` never had a literal `}` for the hole scan to
// mis-delimit, so this row passes with and without the fix. It is here to
// pin that the escape arm of the complete-literal test does not mis-fire —
// in particular that `\x7D`'s hex digits are consumed as the CharContent
// rather than scanned for a stray `'` — NOT to demonstrate the bug.
BEZEL_TEST(parser_interp, nested_char_literal_escaped_brace) {
    const std::string src =
        "module M; class C public method M() _ := $\"{'\\x7D'}\"; end M; end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
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
