// Spec-derived parser tests for the async-extended LocalDecl and ForStmt
// forms — §A.6:
//
//   LocalDecl ::= ... | "await" "using" "var" Identifier (":" Type)?
//                       ":=" Expression ";"
//   ForStmt   ::= ... | "await" "for" "var" Identifier (":" Type)? "in"
//                       Expression ("with" Expression)? "do" Statement* "end" ";"
//
// Sema enforces that these forms appear only inside `async` methods/lambdas
// (Appendix C.6); the parser admits them at any LocalDecl / ForStmt position.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

namespace {

// Wrap a single statement body in a class method and return the body
// rendered via memberSummary, so we can assert on the stmtSummary output
// directly.
std::string wrapAndRender(const std::string& body) {
    const std::string src =
        "module M; class C public method M() " + body +
        " end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto ms = memberSummaries(r);
    return ms.size() == 1 ? ms[0] : std::string("<wrong-count>");
}

} // namespace

BEZEL_TEST(parser_await_using, simple_await_using_var) {
    const std::string m = wrapAndRender(
        "await using var c := source.OpenAsync();");
    BEZEL_EXPECT_EQ(
        m,
        std::string(
            "public method M() { await using var c := "
            "source.OpenAsync(); }"));
}

BEZEL_TEST(parser_await_using, await_using_var_with_type) {
    const std::string m = wrapAndRender(
        "await using var c: IAsyncDisposable := source.OpenAsync();");
    BEZEL_EXPECT_EQ(
        m,
        std::string(
            "public method M() { await using var c: IAsyncDisposable "
            ":= source.OpenAsync(); }"));
}

BEZEL_TEST(parser_await_using, await_using_requires_initializer) {
    // Per A.6 the await-using and using-var forms both require `:=`. The
    // parser reports the absence as an error.
    auto r = parseAll(
        "module M; class C public method M() "
        "await using var c; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_await_for, simple_await_for) {
    const std::string m = wrapAndRender(
        "await for var x in src do _ := x; end;");
    BEZEL_EXPECT_EQ(
        m,
        std::string(
            "public method M() { await for var x in src do "
            "{ _ := x; } end; }"));
}

BEZEL_TEST(parser_await_for, await_for_with_type) {
    const std::string m = wrapAndRender(
        "await for var x: int in src do _ := x; end;");
    BEZEL_EXPECT_EQ(
        m,
        std::string(
            "public method M() { await for var x: int in src do "
            "{ _ := x; } end; }"));
}

BEZEL_TEST(parser_await_for, await_for_with_cancellation_token) {
    // §30.7 `with EXPR` cancellation-token clause.
    const std::string m = wrapAndRender(
        "await for var x in src with token do _ := x; end;");
    BEZEL_EXPECT_EQ(
        m,
        std::string(
            "public method M() { await for var x in src with token do "
            "{ _ := x; } end; }"));
}

BEZEL_TEST(parser_await_for, sync_for_rejects_with_clause) {
    // The `with` clause is async-only per §30.7; the parser flags it on a
    // synchronous `for`.
    auto r = parseAll(
        "module M; class C public method M() "
        "for var x in src with token do _ := x; end; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_await_for, plain_await_expression_still_parses) {
    // Statement-position `await expr;` should NOT be misparsed as an
    // await-using / await-for variant — the lookahead is strictly
    // `await using` / `await for`, anything else falls through to the
    // expression-statement path.
    const std::string m = wrapAndRender("_ := await task;");
    BEZEL_EXPECT_EQ(
        m,
        std::string(
            "public method M() { _ := (await task); }"));
}
