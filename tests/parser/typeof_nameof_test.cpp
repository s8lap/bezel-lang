// Spec-derived parser tests for typeof and nameof — §A.7.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

static std::string parseInitText(const std::string& expr) {
    auto r = parseAll(std::string("module M; class Foo public x: int := ") +
                      expr + std::string("; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public x: int := ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

BEZEL_TEST(parser_typeof_nameof, typeof_primitive) {
    BEZEL_EXPECT_EQ(parseInitText("typeof(int)"),
                    std::string("typeof(int)"));
}

BEZEL_TEST(parser_typeof_nameof, typeof_generic_type) {
    BEZEL_EXPECT_EQ(parseInitText("typeof(List<int>)"),
                    std::string("typeof(List<int>)"));
}

BEZEL_TEST(parser_typeof_nameof, typeof_nullable_type) {
    BEZEL_EXPECT_EQ(parseInitText("typeof(string?)"),
                    std::string("typeof(string?)"));
}

BEZEL_TEST(parser_typeof_nameof, nameof_identifier) {
    BEZEL_EXPECT_EQ(parseInitText("nameof(x)"),
                    std::string("nameof(x)"));
}

BEZEL_TEST(parser_typeof_nameof, nameof_qualified) {
    BEZEL_EXPECT_EQ(parseInitText("nameof(Foo.Bar)"),
                    std::string("nameof(Foo.Bar)"));
}

// Errors

BEZEL_TEST(parser_typeof_nameof, typeof_missing_open_paren_is_error) {
    auto r = parseAll(
        "module M; class Foo public x: int := typeof int; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_typeof_nameof, nameof_missing_close_paren_is_error) {
    auto r = parseAll(
        "module M; class Foo public x: int := nameof(x; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
