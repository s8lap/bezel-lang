// Spec-derived parser tests for IncrementStmt, DiscardStmt, and YieldStmt
// — all from §A.6.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

static std::string parseBodyText(const std::string& body) {
    auto r = parseAll(std::string("module M; class Foo public method run() ") +
                      body + std::string(" end run; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public method run() ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

// --- IncrementStmt (§17.6 / A.6) -------------------------------------------

BEZEL_TEST(parser_inc_discard, increment_local) {
    BEZEL_EXPECT_EQ(parseBodyText("counter++;"),
                    std::string("{ counter++; }"));
}

BEZEL_TEST(parser_inc_discard, decrement_local) {
    BEZEL_EXPECT_EQ(parseBodyText("counter--;"),
                    std::string("{ counter--; }"));
}

BEZEL_TEST(parser_inc_discard, increment_field) {
    BEZEL_EXPECT_EQ(parseBodyText("this.count++;"),
                    std::string("{ this.count++; }"));
}

BEZEL_TEST(parser_inc_discard, increment_indexed) {
    BEZEL_EXPECT_EQ(parseBodyText("arr[i]++;"),
                    std::string("{ arr[i]++; }"));
}

// --- DiscardStmt (A.6) ------------------------------------------------------

BEZEL_TEST(parser_inc_discard, discard_call_result) {
    BEZEL_EXPECT_EQ(parseBodyText("_ := f(x);"),
                    std::string("{ _ := f(x); }"));
}

BEZEL_TEST(parser_inc_discard, discard_expression) {
    BEZEL_EXPECT_EQ(parseBodyText("_ := a + b;"),
                    std::string("{ _ := (a + b); }"));
}

// --- YieldStmt (A.6) --------------------------------------------------------

BEZEL_TEST(parser_inc_discard, yield_return_value) {
    BEZEL_EXPECT_EQ(parseBodyText("yield return x;"),
                    std::string("{ yield return x; }"));
}

BEZEL_TEST(parser_inc_discard, yield_return_complex_value) {
    BEZEL_EXPECT_EQ(parseBodyText("yield return a + b;"),
                    std::string("{ yield return (a + b); }"));
}

BEZEL_TEST(parser_inc_discard, yield_break) {
    BEZEL_EXPECT_EQ(parseBodyText("yield break;"),
                    std::string("{ yield break; }"));
}

// --- Errors ----------------------------------------------------------------

BEZEL_TEST(parser_inc_discard, yield_without_return_or_break_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "yield x; end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_inc_discard, increment_missing_semicolon_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "x++ y++; end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
