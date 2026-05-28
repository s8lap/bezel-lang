// Spec-derived parser tests for LocalConstDecl — §A.6:
//
//   LocalConstDecl ::= "const" Identifier (":" Type)? ":=" ConstExpr ";"
//
// Statement-position const declaration (no visibility, no static — those
// belong to A.4 ConstDecl at member level). The right-hand side must be a
// ConstExpr — a compile-time evaluable expression. The parser admits any
// Expression here; sema enforces compile-time evaluability.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::stmtSummary;

namespace {

std::string renderBody(const std::string& body) {
    const std::string src =
        "module M; class C public method M() " + body +
        " end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    if (r.sourceFile->topDecls.empty()) return "<no-topdecls>";
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return "<no-member>";
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return "<no-stmt>";
    return stmtSummary(*md->stmts[0]);
}

} // namespace

BEZEL_TEST(parser_local_const, simple_with_type) {
    BEZEL_EXPECT_EQ(
        renderBody("const N: int := 42;"),
        std::string("const N: int := 42;"));
}

BEZEL_TEST(parser_local_const, without_type) {
    BEZEL_EXPECT_EQ(
        renderBody("const N := 42;"),
        std::string("const N := 42;"));
}

BEZEL_TEST(parser_local_const, string_value) {
    BEZEL_EXPECT_EQ(
        renderBody("const Greeting: string := \"hello\";"),
        std::string("const Greeting: string := \"hello\";"));
}

BEZEL_TEST(parser_local_const, expression_value) {
    BEZEL_EXPECT_EQ(
        renderBody("const Sum := 1 + 2 + 3;"),
        std::string("const Sum := ((1 + 2) + 3);"));
}

BEZEL_TEST(parser_local_const, missing_initializer_is_error) {
    // ConstExpr is required per the grammar; no `:=` is a syntax error.
    auto r = parseAll(
        "module M; class C public method M() const N: int; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_local_const, multiple_locals_with_const) {
    auto r = parseAll(
        "module M; class C public method M() "
        "const Factor: int := 4; "
        "var x: int := 10; "
        "x := x * Factor; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
