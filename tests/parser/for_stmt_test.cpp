// Spec-derived parser tests for the synchronous ForStmt — §A.6:
// `for var Identifier (":" Type)? in Expression do Statement* end ";"`.
// The async `await for ... with` variant lands with the async slice.

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

BEZEL_TEST(parser_for_stmt, simple_for_over_identifier) {
    BEZEL_EXPECT_EQ(
        parseBodyText("for var x in items do log(x); end;"),
        std::string(
            "{ for var x in items do { log(x); } end; }"));
}

BEZEL_TEST(parser_for_stmt, with_type_annotation) {
    BEZEL_EXPECT_EQ(
        parseBodyText("for var x: int in numbers do x; end;"),
        std::string(
            "{ for var x: int in numbers do { x; } end; }"));
}

BEZEL_TEST(parser_for_stmt, over_range) {
    BEZEL_EXPECT_EQ(
        parseBodyText("for var i in 0 .. 10 do x := i; end;"),
        std::string(
            "{ for var i in (0 .. 10) do { x := i; } end; }"));
}

BEZEL_TEST(parser_for_stmt, over_call_result) {
    BEZEL_EXPECT_EQ(
        parseBodyText("for var x in source.values() do f(x); end;"),
        std::string(
            "{ for var x in source.values() do { f(x); } end; }"));
}

BEZEL_TEST(parser_for_stmt, empty_body_is_legal) {
    // Statement* admits zero.
    BEZEL_EXPECT_EQ(parseBodyText("for var x in items do end;"),
                    std::string(
                        "{ for var x in items do {  } end; }"));
}

BEZEL_TEST(parser_for_stmt, nested_for) {
    BEZEL_EXPECT_EQ(
        parseBodyText(
            "for var r in rows do for var c in cols do f(r, c); end; end;"),
        std::string(
            "{ for var r in rows do { for var c in cols do "
            "{ f(r, c); } end; } end; }"));
}

BEZEL_TEST(parser_for_stmt, break_inside_for) {
    BEZEL_EXPECT_EQ(
        parseBodyText("for var x in items do if done then break; end; end;"),
        std::string(
            "{ for var x in items do { if done then { break; } end; } "
            "end; }"));
}

// Errors

BEZEL_TEST(parser_for_stmt, missing_var_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "for x in items do f(x); end; end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_for_stmt, missing_in_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "for var x items do f(x); end; end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_for_stmt, missing_do_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "for var x in items f(x); end; end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
